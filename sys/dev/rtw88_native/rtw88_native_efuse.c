/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Gamer100Dev
 * All rights reserved.
 *
 * Native RTL8822B USB physical/logical eFuse reader. The record format,
 * header decoding and field offsets follow the vendored RTW88 RTL8822B
 * implementation; all register transactions use FreeBSD USB APIs.
 */

#include <sys/param.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/systm.h>

#include "if_rtw88_native.h"

#define RTW88_NATIVE_EFUSE_PHY_SIZE	1024U
#define RTW88_NATIVE_EFUSE_LOG_SIZE	768U
#define RTW88_NATIVE_EFUSE_PROTECT	96U

#define REG_EFUSE_CTRL			0x0030
#define REG_LDO_EFUSE_CTRL		0x0034
#define REG_LDO_EFUSE_CTRL_BYTE3	(REG_LDO_EFUSE_CTRL + 3)

#define EFUSE_BIT_FLAG			(1U << 31)
#define EFUSE_ADDR_SHIFT		8
#define EFUSE_ADDR_MASK			0x3ffU
#define EFUSE_DATA_MASK			0xffU
#define EFUSE_BANK_MASK			((1U << 8) | (1U << 9))
#define EFUSE_LDO25_ENABLE		(1U << 7)

#define RTW88_NATIVE_EFUSE_POLL_TRIES	16

static int
rtw88_native_efuse_dump_physical(struct rtw88_native_softc *sc,
    uint8_t *map, size_t map_size)
{
	uint32_t ctrl, value;
	uint8_t ldo_before, ldo_now;
	size_t addr;
	int tries, error;

	if (map == NULL || map_size < RTW88_NATIVE_EFUSE_PHY_SIZE)
		return (EINVAL);

	/* Select the Wi-Fi eFuse bank (bank index zero). */
	if (rtw88_native_reg_read32(sc, REG_LDO_EFUSE_CTRL, &value) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	value &= ~EFUSE_BANK_MASK;
	if (rtw88_native_reg_write32(sc, REG_LDO_EFUSE_CTRL, value) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	/* Disable the eFuse 2.5-V LDO for the read sequence, then restore it. */
	if (rtw88_native_reg_read8(sc, REG_LDO_EFUSE_CTRL_BYTE3,
	    &ldo_before) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	ldo_now = ldo_before & ~EFUSE_LDO25_ENABLE;
	if (rtw88_native_reg_write8(sc, REG_LDO_EFUSE_CTRL_BYTE3, ldo_now) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	error = 0;
	if (rtw88_native_reg_read32(sc, REG_EFUSE_CTRL, &ctrl) !=
	    USB_ERR_NORMAL_COMPLETION) {
		error = EIO;
		goto restore_ldo;
	}

	for (addr = 0; addr < RTW88_NATIVE_EFUSE_PHY_SIZE; addr++) {
		ctrl &= ~(EFUSE_DATA_MASK | (EFUSE_ADDR_MASK << EFUSE_ADDR_SHIFT));
		ctrl |= ((uint32_t)addr & EFUSE_ADDR_MASK) << EFUSE_ADDR_SHIFT;
		ctrl &= ~EFUSE_BIT_FLAG;
		if (rtw88_native_reg_write32(sc, REG_EFUSE_CTRL, ctrl) !=
		    USB_ERR_NORMAL_COMPLETION) {
			error = EIO;
			break;
		}

		for (tries = 0; tries < RTW88_NATIVE_EFUSE_POLL_TRIES; tries++) {
			if (rtw88_native_reg_read32(sc, REG_EFUSE_CTRL, &value) !=
			    USB_ERR_NORMAL_COMPLETION) {
				error = EIO;
				break;
			}
			if (value & EFUSE_BIT_FLAG)
				break;
			pause("rtw88ef", MAX(1, hz / 1000));
		}
		if (error != 0)
			break;
		if (tries == RTW88_NATIVE_EFUSE_POLL_TRIES) {
			error = ETIMEDOUT;
			break;
		}
		map[addr] = (uint8_t)(value & EFUSE_DATA_MASK);
		ctrl = value;
	}

restore_ldo:
	if (rtw88_native_reg_read8(sc, REG_LDO_EFUSE_CTRL_BYTE3,
	    &ldo_now) == USB_ERR_NORMAL_COMPLETION) {
		ldo_now = (ldo_now & ~EFUSE_LDO25_ENABLE) |
		    (ldo_before & EFUSE_LDO25_ENABLE);
		if (rtw88_native_reg_write8(sc, REG_LDO_EFUSE_CTRL_BYTE3,
		    ldo_now) != USB_ERR_NORMAL_COMPLETION && error == 0)
			error = EIO;
	} else if (error == 0) {
		error = EIO;
	}

	return (error);
}

static int
rtw88_native_efuse_decode_logical(const uint8_t *physical, size_t physical_size,
    size_t protect_size, uint8_t *logical, size_t logical_size)
{
	size_t phy_idx, log_idx, limit;
	uint8_t hdr1, hdr2, block, word_en;
	int word;

	if (physical == NULL || logical == NULL || protect_size >= physical_size)
		return (EINVAL);

	limit = physical_size - protect_size;
	memset(logical, 0xff, logical_size);

	for (phy_idx = 0; phy_idx < limit;) {
		hdr1 = physical[phy_idx];
		hdr2 = (phy_idx + 1 < limit) ? physical[phy_idx + 1] : 0xff;

		/* 0xff or an extended-header terminator ends the packed map. */
		if (hdr1 == 0xff || ((hdr1 & 0x1f) == 0x0f && hdr2 == 0xff))
			break;

		if ((hdr1 & 0x1f) == 0x0f) {
			if (phy_idx + 1 >= limit)
				return (EINVAL);
			block = (uint8_t)(((hdr2 & 0xf0) >> 1) |
			    ((hdr1 >> 5) & 0x07));
			word_en = hdr2 & 0x0f;
			phy_idx += 2;
		} else {
			block = (hdr1 & 0xf0) >> 4;
			word_en = hdr1 & 0x0f;
			phy_idx++;
		}

		for (word = 0; word < 4; word++) {
			if (word_en & (1U << word))
				continue;
			log_idx = ((size_t)block << 3) + ((size_t)word << 1);
			if (phy_idx + 1 >= limit || log_idx + 1 >= logical_size)
				return (EINVAL);
			logical[log_idx] = physical[phy_idx];
			logical[log_idx + 1] = physical[phy_idx + 1];
			phy_idx += 2;
		}
	}
	return (0);
}

static bool
rtw88_native_valid_ether_addr(const uint8_t addr[6])
{
	static const uint8_t zero[6] = { 0, 0, 0, 0, 0, 0 };
	static const uint8_t ones[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	return ((addr[0] & 1) == 0 &&
	    memcmp(addr, zero, sizeof(zero)) != 0 &&
	    memcmp(addr, ones, sizeof(ones)) != 0);
}

static void
rtw88_native_efuse_parse_8822bu(struct rtw88_native_softc *sc,
    const uint8_t *logical)
{
	struct rtw88_native_efuse *efuse = &sc->efuse;

	/*
	 * Offsets are from struct rtw8822b_efuse's packed USB union in
	 * sys/contrib/dev/rtw88/rtw8822b.h (the vendor struct comments include
	 * the corresponding logical-map offsets).
	 */
	efuse->usb_mode = logical[0x06];
	efuse->channel_plan = logical[0xb8];
	efuse->crystal_cap = logical[0xb9];
	efuse->thermal_meter = logical[0xba];
	efuse->pa_type = logical[0xbc];
	efuse->lna_type_2g = logical[0xbd];
	efuse->lna_type_5g = logical[0xbf];
	efuse->rf_board_option = logical[0xc1];
	efuse->bt_setting = logical[0xc3];
	efuse->rf_antenna_option = logical[0xc9];
	/*
	 * struct rtw8822b_efuse stores four packed struct rtw_txpwr_idx
	 * records at logical offset 0x10. Each record is 15 bytes of 2.4-GHz
	 * power indices followed by 23 bytes of 5-GHz power indices. The
	 * native RTL8822BU path supports the first two RF chains (A and B).
	 */
	memcpy(efuse->txpwr_idx, logical + 0x10,
	    sizeof(efuse->txpwr_idx));
	efuse->rfe_option = logical[0xca];
	efuse->country_code[0] = logical[0xcb];
	efuse->country_code[1] = logical[0xcc];
	memcpy(efuse->macaddr, logical + 0x107, sizeof(efuse->macaddr));

	/* Apply the driver's documented defaults for erased optional fields. */
	if (efuse->channel_plan == 0xff)
		efuse->channel_plan = 0x7f;
	if (efuse->crystal_cap == 0xff)
		efuse->crystal_cap = 0;
	if (efuse->pa_type == 0xff)
		efuse->pa_type = 0;
	if (efuse->lna_type_2g == 0xff)
		efuse->lna_type_2g = 0;
	if (efuse->lna_type_5g == 0xff)
		efuse->lna_type_5g = 0;
	if (efuse->rf_board_option == 0xff)
		efuse->rf_board_option = 0;

	efuse->macaddr_valid = rtw88_native_valid_ether_addr(efuse->macaddr);
	if (efuse->macaddr_valid) {
		device_printf(sc->dev,
		    "eFuse MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
		    efuse->macaddr[0], efuse->macaddr[1], efuse->macaddr[2],
		    efuse->macaddr[3], efuse->macaddr[4], efuse->macaddr[5]);
	} else {
		device_printf(sc->dev,
		    "eFuse contains no valid unicast MAC address\n");
	}
}

int
rtw88_native_efuse_read(struct rtw88_native_softc *sc)
{
	uint8_t *physical, *logical;
	int error;

	if (sc == NULL || !sc->powered || !sc->firmware_running || sc->dying)
		return (EINVAL);
	if (sc->efuse_initialized)
		return (EALREADY);

	physical = malloc(RTW88_NATIVE_EFUSE_PHY_SIZE, M_DEVBUF,
	    M_WAITOK | M_ZERO);
	logical = malloc(RTW88_NATIVE_EFUSE_LOG_SIZE, M_DEVBUF,
	    M_WAITOK | M_ZERO);
	if (physical == NULL || logical == NULL) {
		error = ENOMEM;
		goto out;
	}

	error = rtw88_native_efuse_dump_physical(sc, physical,
	    RTW88_NATIVE_EFUSE_PHY_SIZE);
	if (error != 0)
		goto out;

	error = rtw88_native_efuse_decode_logical(physical,
	    RTW88_NATIVE_EFUSE_PHY_SIZE, RTW88_NATIVE_EFUSE_PROTECT,
	    logical, RTW88_NATIVE_EFUSE_LOG_SIZE);
	if (error != 0)
		goto out;

	rtw88_native_efuse_parse_8822bu(sc, logical);
	sc->efuse_initialized = 1;
	device_printf(sc->dev,
	    "eFuse configuration: channel-plan=0x%02x RFE=0x%02x board=0x%02x RF-antenna=0x%02x crystal=0x%02x\n",
	    sc->efuse.channel_plan, sc->efuse.rfe_option,
	    sc->efuse.rf_board_option, sc->efuse.rf_antenna_option,
	    sc->efuse.crystal_cap);

out:
	if (logical != NULL)
		free(logical, M_DEVBUF);
	if (physical != NULL)
		free(physical, M_DEVBUF);
	return (error);
}
