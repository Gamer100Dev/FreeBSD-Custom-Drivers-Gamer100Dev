/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Gamer100Dev
 * All rights reserved.
 *
 * Native RTL8822B PHY table loader. The register data itself is kept in
 * rtw88_native_8822b_tables.c under the upstream Realtek dual licence.
 * This file implements the FreeBSD USB register-writing and conditional
 * table interpretation layer.
 */

#include <sys/param.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/systm.h>

#include "if_rtw88_native.h"

#define RTW88_NATIVE_INTF_USB		(1U << 1)
#define RTW88_NATIVE_BRANCH_ENDIF	3U
#define RTW88_NATIVE_BRANCH_ELSE	2U
#define RTW88_NATIVE_COND_POS		(1U << 31)
#define RTW88_NATIVE_COND_NEG		(1U << 30)
#define RTW88_NATIVE_COND_BRANCH	(3U << 28)
#define RTW88_NATIVE_COND_CUT		(0x0fU << 24)
#define RTW88_NATIVE_COND_PKG		(0x0fU << 12)
#define RTW88_NATIVE_COND_INTF		(0x0fU << 8)
#define RTW88_NATIVE_COND_RFE		0xffU

#define REG_SYS_FUNC_EN			0x0002
#define REG_RF_CTRL			0x001f
#define REG_WLRF1			0x00ec
#define REG_AFE_XTAL_CTRL		0x0024
#define REG_AFE_PLL_CTRL		0x0028
#define REG_RXPSEL			0x0808

#define BIT_FEN_BB_RSTB			(1U << 0)
#define BIT_FEN_BB_GLB_RST		(1U << 1)
#define BIT_RF_EN			(1U << 0)
#define BIT_RF_RSTB			(1U << 1)
#define BIT_RF_SDM_RSTB			(1U << 2)
#define BIT_WLRF1_BBRF_EN		((1U << 24) | (1U << 25) | (1U << 26))
#define BIT_RX_PSEL_RST			((1U << 28) | (1U << 29))

#define RTW88_NATIVE_RFREG_MASK		0x000fffffU

#define REG_AGCTR_A			0x0c08
#define REG_CDDTXP			0x093c
#define REG_TXPSEL			0x080c
#define REG_ADCINI			0x0a04
#define REG_TXPSEL1			0x0940
#define REG_RXDESC			0x0a2c
#define REG_AGCTR_B			0x0e08
#define REG_ANTWT			0x1904
#define REG_HTSTFWT			0x0800
#define REG_MRC			0x0850
#define REG_TRSW			0x0ca0

#define BB_PATH_A			0x01
#define BB_PATH_B			0x02
#define BB_PATH_AB			(BB_PATH_A | BB_PATH_B)

#define RF_LUTWA			0x33
#define RF_LUTWD1			0x3e
#define RF_LUTWD0			0x3f
#define RF_LUTWE			0xef
#define RF_PATH_A_BASE			0x2800
#define RF_PATH_B_BASE			0x2c00
#define RTW88_NATIVE_RF_SIPI_A		0x0c90
#define RTW88_NATIVE_RF_SIPI_B		0x0e90

enum rtw88_native_phy_table_kind {
	RTW88_NATIVE_PHY_MAC,
	RTW88_NATIVE_PHY_AGC,
	RTW88_NATIVE_PHY_BB,
	RTW88_NATIVE_PHY_RF
};

extern const uint32_t rtw88_native_rtw8822b_mac[];
extern const size_t rtw88_native_rtw8822b_mac_nwords;
extern const uint32_t rtw88_native_rtw8822b_agc[];
extern const size_t rtw88_native_rtw8822b_agc_nwords;
extern const uint32_t rtw88_native_rtw8822b_bb[];
extern const size_t rtw88_native_rtw8822b_bb_nwords;
extern const uint32_t rtw88_native_rtw8822b_rf_a[];
extern const size_t rtw88_native_rtw8822b_rf_a_nwords;
extern const uint32_t rtw88_native_rtw8822b_rf_b[];
extern const size_t rtw88_native_rtw8822b_rf_b_nwords;

static bool
rtw88_native_phy_condition_match(struct rtw88_native_softc *sc,
    uint32_t condition)
{
	uint8_t cut, package, intf, rfe;

	cut = (condition & RTW88_NATIVE_COND_CUT) >> 24;
	package = (condition & RTW88_NATIVE_COND_PKG) >> 12;
	intf = (condition & RTW88_NATIVE_COND_INTF) >> 8;
	rfe = condition & RTW88_NATIVE_COND_RFE;

	/*
	 * This is the 8822B branch of RTW88 check_positive(): package defaults
	 * to 15, interface is USB, and RFE is an exact match (not a bit mask).
	 */
	if (cut != 0 && cut != (sc->cut_version != 0 ?
	    sc->cut_version : 15))
		return (false);
	if (package != 0 && package != 15)
		return (false);
	if (intf != 0 && intf != RTW88_NATIVE_INTF_USB)
		return (false);
	if (rfe != sc->efuse.rfe_option)
		return (false);

	return (true);
}

static int
rtw88_native_phy_write_mask32(struct rtw88_native_softc *sc, uint16_t reg,
    uint32_t mask, uint32_t value)
{
	uint32_t old, shift;

	if (mask == 0)
		return (EINVAL);
	if (rtw88_native_reg_read32(sc, reg, &old) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	shift = __builtin_ctz(mask);
	old = (old & ~mask) | ((value << shift) & mask);
	return (rtw88_native_reg_write32(sc, reg, old) ==
	    USB_ERR_NORMAL_COMPLETION ? 0 : EIO);
}

static int
rtw88_native_phy_rf_write(struct rtw88_native_softc *sc, uint8_t path,
    uint32_t addr, uint32_t value)
{
	uint32_t data_and_addr;
	uint16_t sipi_reg;

	if (path >= sc->rf_path_num || path > 1)
		return (EINVAL);
	if (addr == 0x0ffe) {
		pause("rtw88rf", MAX(1, hz / 20));
		return (0);
	}
	if (addr == 0x00fe) {
		DELAY(100);
		return (0);
	}

	sipi_reg = path == 0 ? RTW88_NATIVE_RF_SIPI_A : RTW88_NATIVE_RF_SIPI_B;
	data_and_addr = (((addr & 0xffU) << 20) |
	    (value & RTW88_NATIVE_RFREG_MASK)) & 0x0fffffffU;
	if (rtw88_native_reg_write32(sc, sipi_reg, data_and_addr) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	DELAY(13);
	return (0);
}

static int
rtw88_native_phy_rf_read(struct rtw88_native_softc *sc, uint8_t path,
    uint32_t addr, uint32_t *value)
{
	uint16_t base;
	uint32_t raw;

	if (value == NULL || path >= sc->rf_path_num || path > 1)
		return (EINVAL);
	base = path == 0 ? RF_PATH_A_BASE : RF_PATH_B_BASE;
	if (rtw88_native_reg_read32(sc, base + ((addr & 0xffU) << 2),
	    &raw) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	*value = raw & RTW88_NATIVE_RFREG_MASK;
	return (0);
}

static int
rtw88_native_phy_config_trx_mode(struct rtw88_native_softc *sc,
    uint8_t tx_path, uint8_t rx_path)
{
	uint8_t txsel, rxsel;
	uint32_t rf_reg33;
	int error, counter;

	if ((tx_path & ~BB_PATH_AB) != 0 || (rx_path & ~BB_PATH_AB) != 0 ||
	    tx_path == 0 || rx_path == 0)
		return (EINVAL);
	if (((tx_path | rx_path) & BB_PATH_B) != 0 && sc->rf_path_num < 2)
		return (ENOTSUP);

	/* Select AGC chains for each enabled RF path. */
	error = rtw88_native_phy_write_mask32(sc, REG_AGCTR_A, 0x0000ffff,
	    ((tx_path | rx_path) & BB_PATH_A) ? 0x3231 : 0x1111);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_AGCTR_B,
		    0x0000ffff,
		    ((tx_path | rx_path) & BB_PATH_B) ? 0x3231 : 0x1111);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CDDTXP,
		    (1U << 19) | (1U << 18), 0x3);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_TXPSEL,
		    (1U << 29) | (1U << 28), 0x1);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_TXPSEL,
		    (1U << 30), 0x1);
	if (error != 0)
		return (error);

	if (tx_path & BB_PATH_A) {
		error = rtw88_native_phy_write_mask32(sc, REG_CDDTXP,
		    0xfff00000U, 0x001);
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_ADCINI,
			    0xf0000000U, 0x8);
	} else if (tx_path & BB_PATH_B) {
		error = rtw88_native_phy_write_mask32(sc, REG_CDDTXP,
		    0xfff00000U, 0x002);
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_ADCINI,
			    0xf0000000U, 0x4);
	}
	if (error != 0)
		return (error);

	error = rtw88_native_phy_write_mask32(sc, REG_TXPSEL1, 0x0000fff0,
	    (tx_path == BB_PATH_A || tx_path == BB_PATH_B) ? 0x01 : 0x43);
	if (error != 0)
		return (error);
	txsel = (tx_path << 4) | tx_path;
	error = rtw88_native_phy_write_mask32(sc, REG_TXPSEL, 0x000000ff,
	    txsel);
	if (error != 0)
		return (error);

	/* Clear RX descriptor path override and select the requested chain(s). */
	error = rtw88_native_phy_write_mask32(sc, REG_RXDESC,
	    (1U << 22), 0);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_RXDESC,
		    (1U << 18), 0);
	if (error != 0)
		return (error);

	if (rx_path & BB_PATH_A)
		error = rtw88_native_phy_write_mask32(sc, REG_ADCINI,
		    0x0f000000U, 0x0);
	else if (rx_path & BB_PATH_B)
		error = rtw88_native_phy_write_mask32(sc, REG_ADCINI,
		    0x0f000000U, 0x5);
	if (error != 0)
		return (error);

	rxsel = (rx_path << 4) | rx_path;
	error = rtw88_native_phy_write_mask32(sc, REG_RXPSEL, 0x000000ff,
	    rxsel);
	if (error != 0)
		return (error);

	if (rx_path == BB_PATH_A || rx_path == BB_PATH_B) {
		error = rtw88_native_phy_write_mask32(sc, REG_ANTWT,
		    (1U << 16), 0);
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_HTSTFWT,
			    (1U << 28), 0);
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_MRC,
			    (1U << 23), 0);
	} else {
		error = rtw88_native_phy_write_mask32(sc, REG_ANTWT,
		    (1U << 16), 1);
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_HTSTFWT,
			    (1U << 28), 1);
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_MRC,
			    (1U << 23), 1);
	}
	if (error != 0)
		return (error);

	/*
	 * Configure the 8822B RF-A LUT mode used by the upstream USB chipset
	 * configuration. Poll RF register 0x33 through the RF direct window.
	 */
	for (counter = 0; counter < 100; counter++) {
		error = rtw88_native_phy_rf_write(sc, 0, RF_LUTWE, 0x80000);
		if (error == 0)
			error = rtw88_native_phy_rf_write(sc, 0, RF_LUTWA, 0x00001);
		if (error != 0)
			return (error);
		DELAY(2);
		error = rtw88_native_phy_rf_read(sc, 0, 0x33, &rf_reg33);
		if (error != 0)
			return (error);
		if (rf_reg33 == 0x00001)
			break;
	}
	if (counter == 100)
		return (ETIMEDOUT);

	error = rtw88_native_phy_rf_write(sc, 0, RF_LUTWE, 0x80000);
	if (error == 0)
		error = rtw88_native_phy_rf_write(sc, 0, RF_LUTWA, 0x00001);
	if (error == 0)
		error = rtw88_native_phy_rf_write(sc, 0, RF_LUTWD1, 0x00034);
	if (error == 0)
		error = rtw88_native_phy_rf_write(sc, 0, RF_LUTWD0, 0x4080c);
	if (error == 0)
		error = rtw88_native_phy_rf_write(sc, 0, RF_LUTWE, 0x00000);
	if (error == 0)
		error = rtw88_native_phy_rf_write(sc, 0, RF_LUTWE, 0x00000);
	return (error);
}

static int
rtw88_native_phy_write_entry(struct rtw88_native_softc *sc,
    enum rtw88_native_phy_table_kind kind, uint8_t rf_path,
    uint32_t addr, uint32_t data)
{
	usb_error_t uerr;

	switch (kind) {
	case RTW88_NATIVE_PHY_MAC:
		uerr = rtw88_native_reg_write8(sc, (uint16_t)addr,
		    (uint8_t)data);
		return (uerr == USB_ERR_NORMAL_COMPLETION ? 0 : EIO);
	case RTW88_NATIVE_PHY_AGC:
		uerr = rtw88_native_reg_write32(sc, (uint16_t)addr, data);
		return (uerr == USB_ERR_NORMAL_COMPLETION ? 0 : EIO);
	case RTW88_NATIVE_PHY_BB:
		switch (addr) {
		case 0x0fe:
			pause("rtw88bb", MAX(1, hz / 20));
			return (0);
		case 0x0fd:
			pause("rtw88bb", MAX(1, hz / 200));
			return (0);
		case 0x0fc:
			pause("rtw88bb", MAX(1, hz / 1000));
			return (0);
		case 0x0fb:
			DELAY(50);
			return (0);
		case 0x0fa:
			DELAY(5);
			return (0);
		case 0x0f9:
			DELAY(1);
			return (0);
		default:
			uerr = rtw88_native_reg_write32(sc, (uint16_t)addr, data);
			return (uerr == USB_ERR_NORMAL_COMPLETION ? 0 : EIO);
		}
	case RTW88_NATIVE_PHY_RF:
		return (rtw88_native_phy_rf_write(sc, rf_path, addr, data));
	}
	return (EINVAL);
}

/*
 * RTW88 PHY tables are pairs of u32 values. Conditional records use bit 31
 * (POS), bit 30 (NEG), bits 28-29 (branch), and carry the condition in the
 * low bits.  Ordinary records are address/value pairs. The 8822B condition
 * test intentionally mirrors its upstream implementation, while USB
 * interface filtering prevents PCIe-specific table rows from being used.
 */
static int
rtw88_native_phy_load_table(struct rtw88_native_softc *sc,
    const uint32_t *table, size_t n_words,
    enum rtw88_native_phy_table_kind kind, uint8_t rf_path)
{
	uint32_t condition, addr, value;
	bool matched, skipped;
	size_t i;

	if (table == NULL || (n_words & 1) != 0)
		return (EINVAL);

	condition = 0;
	matched = true;
	skipped = false;
	for (i = 0; i < n_words; i += 2) {
		addr = table[i];
		value = table[i + 1];

		if ((addr & RTW88_NATIVE_COND_POS) != 0) {
			switch ((addr & RTW88_NATIVE_COND_BRANCH) >> 28) {
			case RTW88_NATIVE_BRANCH_ENDIF:
				matched = true;
				skipped = false;
				break;
			case RTW88_NATIVE_BRANCH_ELSE:
				matched = skipped ? false : true;
				break;
			default: /* IF and ELIF save their following condition word. */
				condition = addr;
				break;
			}
			continue;
		}
		if ((addr & RTW88_NATIVE_COND_NEG) != 0) {
			if (!skipped) {
				if (rtw88_native_phy_condition_match(sc, condition)) {
					matched = true;
					skipped = true;
				} else {
					matched = false;
					skipped = false;
				}
			} else {
				matched = false;
			}
			continue;
		}
		if (!matched)
			continue;

		if (addr > 0xffff)
			return (EINVAL);
		if (rtw88_native_phy_write_entry(sc, kind, rf_path,
		    addr, value) != 0) {
			device_printf(sc->dev,
			    "PHY table write failed: kind=%u path=%u addr=0x%04x\n",
			    kind, rf_path, (unsigned int)addr);
			return (EIO);
		}
	}
	return (0);
}

/*
 * USB-specific RTL8822B PHY bring-up in the same order as upstream
 * rtw8822b_phy_set_param(): enable BB/RF domains, release RX path selection,
 * load MAC/BB/AGC and both RF path tables, apply eFuse crystal trim, then
 * restore RX path selection and configure the board top mux.
 *
 * Dynamic DIG, IQK/DPK, thermal tracking, TX power limits and antenna/TRX path
 * policy remain separate follow-on work; no 802.11 networking is advertised.
 */
int
rtw88_native_phy_init(struct rtw88_native_softc *sc)
{
	static const uint16_t rf_sipi_addr[2] = {
		RTW88_NATIVE_RF_SIPI_A, RTW88_NATIVE_RF_SIPI_B
	};
	uint32_t xtal;
	int error;

	if (sc == NULL || !sc->powered || !sc->firmware_running ||
	    !sc->mac_initialized || sc->dying)
		return (EINVAL);
	if (sc->phy_initialized)
		return (0);
	if (sc->rf_path_num == 0 || sc->rf_path_num > nitems(rf_sipi_addr))
		return (ENOTSUP);

	/* Enable baseband global reset release and RF front-end control. */
	error = rtw88_native_phy_write_mask32(sc, REG_SYS_FUNC_EN,
	    BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST,
	    BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST);
	if (error != 0)
		return (error);
	error = rtw88_native_phy_write_mask32(sc, REG_RF_CTRL,
	    BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB,
	    BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB);
	if (error != 0)
		return (error);
	error = rtw88_native_phy_write_mask32(sc, REG_WLRF1,
	    BIT_WLRF1_BBRF_EN, BIT_WLRF1_BBRF_EN);
	if (error != 0)
		return (error);

	/* Disable RX path select while the baseband tables are being written. */
	error = rtw88_native_phy_write_mask32(sc, REG_RXPSEL,
	    BIT_RX_PSEL_RST, 0);
	if (error != 0)
		return (error);

	error = rtw88_native_phy_load_table(sc, rtw88_native_rtw8822b_mac,
	    rtw88_native_rtw8822b_mac_nwords, RTW88_NATIVE_PHY_MAC, 0);
	if (error == 0)
		error = rtw88_native_phy_load_table(sc, rtw88_native_rtw8822b_bb,
		    rtw88_native_rtw8822b_bb_nwords, RTW88_NATIVE_PHY_BB, 0);
	if (error == 0)
		error = rtw88_native_phy_load_table(sc, rtw88_native_rtw8822b_agc,
		    rtw88_native_rtw8822b_agc_nwords, RTW88_NATIVE_PHY_AGC, 0);
	if (error == 0)
		error = rtw88_native_phy_load_table(sc, rtw88_native_rtw8822b_rf_a,
		    rtw88_native_rtw8822b_rf_a_nwords, RTW88_NATIVE_PHY_RF, 0);
	if (error == 0 && sc->rf_path_num > 1)
		error = rtw88_native_phy_load_table(sc, rtw88_native_rtw8822b_rf_b,
		    rtw88_native_rtw8822b_rf_b_nwords, RTW88_NATIVE_PHY_RF, 1);
	if (error != 0)
		return (error);

	/* Program the board's crystal capacitor calibration from eFuse. */
	xtal = sc->efuse.crystal_cap & 0x3f;
	error = rtw88_native_phy_write_mask32(sc, REG_AFE_XTAL_CTRL,
	    0x7e000000U, xtal);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_AFE_PLL_CTRL,
		    0x0000007eU, xtal);
	if (error != 0)
		return (error);

	error = rtw88_native_phy_write_mask32(sc, REG_RXPSEL,
	    BIT_RX_PSEL_RST, BIT_RX_PSEL_RST);
	if (error != 0)
		return (error);

	/* Common 8822B USB board mux setup from rtw8822b_phy_rfe_init(). */
	error = rtw88_native_phy_write_mask32(sc, 0x0064,
	    (1U << 29) | (1U << 28), 0x3);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, 0x004c,
		    (1U << 26) | (1U << 25), 0x0);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, 0x0040,
		    (1U << 2), 0x1);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, 0x1990, 0x3f, 0x30);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, 0x1990,
		    (1U << 11) | (1U << 10), 0x3);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, 0x0974, 0x3f, 0x3f);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, 0x0974,
		    (1U << 11) | (1U << 10), 0x3);
	if (error != 0)
		return (error);

	error = rtw88_native_phy_config_trx_mode(sc,
	    sc->antenna_tx, sc->antenna_rx);
	if (error != 0)
		return (error);

	sc->phy_initialized = 1;
	device_printf(sc->dev,
	    "RTL8822B USB PHY tables loaded: MAC, BB, AGC and %u RF path(s)\n",
	    sc->rf_path_num);
	return (0);
}
