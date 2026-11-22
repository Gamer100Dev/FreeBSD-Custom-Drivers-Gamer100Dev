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

/*
 * RTL8822B's RTW88 chip operations use the HSSI/SIPI serial-interface
 * write port for RF register writes, while the read operation uses the
 * path-specific direct RF window. Keep the chipset-specific write behaviour
 * rather than using the mixed direct-write path used by other RTW88 chips.
 */
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
	if (addr > 0xff)
		return (EINVAL);

	sipi_reg = path == 0 ? RTW88_NATIVE_RF_SIPI_A :
	    RTW88_NATIVE_RF_SIPI_B;
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
 * RTL8822B 20-MHz channel setup for the IFEM-ext RFE option 3 board.
 * 2.4-GHz and 5-GHz use separate baseband, RF synthesizer, antenna and CCA
 * profiles. The active RF mask follows SYS_CFG1 and can fall back to RF-A.
 */
#define REG_CH_DATA_SC			0x0483
#define REG_CH_WMAC_TRXPTCL_CTL		0x0668
#define REG_CH_CCK_CHECK		0x0454
#define REG_CH_RXCCAMSK			0x0814
#define REG_CH_ACGG2TBL			0x0958
#define REG_CH_CLKTRK			0x0860
#define REG_CH_TXSF2			0x0a24
#define REG_CH_TXSF6			0x0a28
#define REG_CH_ENTXCCK			0x0a80
#define REG_CH_ADCCLK			0x08ac
#define REG_CH_ADC160			0x08c4
#define REG_CH_ACBB0			0x0948
#define REG_CH_ACBBRXFIR		0x094c
#define REG_CH_TXDFIR			0x0c20
#define REG_CH_RFESEL0			0x0cb0
#define REG_CH_RFESEL8			0x0cb4
#define REG_CH_RFEINV			0x0cbc
#define REG_CH_CCASEL			0x082c
#define REG_CH_PDMFTH			0x0830
#define REG_CH_CCA2ND			0x0838
#define REG_CH_RXIGI_A			0x0c50
#define REG_CH_RXIGI_B			0x0e50
#define REG_CH_AFE_CTRL1		0x0024
#define REG_CH_USTIME_TSF		0x055c
#define REG_CH_USTIME_EDCA		0x0638
#define REG_CH_TRSW			0x0ca0

#define CH_RF18_BAND_MASK		((1U << 16) | (1U << 9) | (1U << 8))
#define CH_RF18_CHANNEL_MASK		0x000000ffU
#define CH_RF18_RFSI_MASK		((1U << 18) | (1U << 17))
#define CH_RF18_BW_MASK		((1U << 11) | (1U << 10))
#define CH_RF18_BW_20M			CH_RF18_BW_MASK
#define CH_RF_MALSEL_MASK		((1U << 17) | (1U << 16) | (1U << 15))
#define CH_RF_XTALX2_MASK		(1U << 19)
#define CH_RF_LUTDBG_MASK		(1U << 18)
#define CH_MAC_RFMOD_MASK		((1U << 8) | (1U << 7))
#define CH_BIT_CHECK_CCK_EN		(1U << 7)
#define CH_BIT_MAC_CLK_SEL		((1U << 21) | (1U << 20))
#define CH_BIT_MAC_RXIGI		0x7f

static int
rtw88_native_phy_set_channel_2g20(struct rtw88_native_softc *sc,
    uint8_t channel)
{
	uint32_t rf18, rf_malsel, rf_xtalx2, rf_lutdbg, rf18_check;
	uint32_t igi_a, igi_b, value32, sipi_b, rf_ctrl, wlrf1;
	uint8_t value8, active_paths;
	uint16_t trx_switch;
	int error, path, rf_write_error;

	/*
	 * RFE option 3 is shared by 1T1R and 2T2R board configurations. The
	 * active antenna masks come from SYS_CFG1; do not assume that a two-path
	 * silicon part necessarily has both chains selected for this interface.
	 */
	if (sc->efuse.rfe_option != 3 || sc->rf_path_num == 0 ||
	    sc->rf_path_num > 2 ||
	    (sc->antenna_tx != BB_PATH_A && sc->antenna_tx != BB_PATH_AB) ||
	    (sc->antenna_rx != BB_PATH_A && sc->antenna_rx != BB_PATH_AB) ||
	    (((sc->antenna_tx | sc->antenna_rx) & BB_PATH_B) != 0 &&
	    sc->rf_path_num < 2))
		return (ENOTSUP);
	active_paths = sc->antenna_tx | sc->antenna_rx;

	/* Baseband 2.4-GHz channel profile. */
	error = rtw88_native_phy_write_mask32(sc, REG_RXPSEL,
	    1U << 28, 1);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_CCK_CHECK,
		    CH_BIT_CHECK_CCK_EN, 0);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_ENTXCCK,
		    1U << 18, 0);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RXCCAMSK,
		    0x0000fc00U, 15);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_ACGG2TBL,
		    0x1f, 0);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_CLKTRK,
		    0x1ffe0000U, 0x96a);
	if (error == 0)
		error = rtw88_native_reg_write32(sc, REG_CH_TXSF2,
		    0x384f6577U) == USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_TXSF6,
		    0x0000ffffU, 0x1525);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RFEINV,
		    0x00000300U, 0x2);
	if (error != 0)
		return (error);

	/* 20-MHz ADC and receive DFIR path. */
	if (rtw88_native_reg_read32(sc, REG_CH_ADCCLK, &value32) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	value32 &= 0xffcffc00U;
	if (rtw88_native_reg_write32(sc, REG_CH_ADCCLK, value32) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	error = rtw88_native_phy_write_mask32(sc, REG_CH_ADC160,
	    1U << 30, 1);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_ACBB0,
		    (3U << 28), 2);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_ACBBRXFIR,
		    (3U << 28), 2);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_TXDFIR,
		    1U << 31, 1);
	if (error != 0)
		return (error);

	/* MAC channel width and timing. Channel index zero means 20 MHz. */
	error = rtw88_native_phy_write_mask32(sc, REG_CH_WMAC_TRXPTCL_CTL,
	    CH_MAC_RFMOD_MASK, 0);
	if (error == 0)
		error = rtw88_native_reg_write8(sc, REG_CH_DATA_SC, 0) ==
		    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
	/* RTW88 MAC_CLK_HW_DEF_80M is zero: clear both select bits. */
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_AFE_CTRL1,
		    CH_BIT_MAC_CLK_SEL, 0);
	if (error == 0)
		error = rtw88_native_reg_write8(sc, REG_CH_USTIME_TSF, 80) ==
		    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
	if (error == 0)
		error = rtw88_native_reg_write8(sc, REG_CH_USTIME_EDCA, 80) ==
		    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
	if (error != 0)
		return (error);

	/* Select the 2.4-GHz RF synthesizer channel and 20-MHz bandwidth. */
	if (rtw88_native_phy_rf_read(sc, 0, 0x18, &rf18) != 0)
		return (EIO);
	rf18 &= ~(CH_RF18_BAND_MASK | CH_RF18_CHANNEL_MASK |
	    CH_RF18_RFSI_MASK | CH_RF18_BW_MASK);
	rf18 |= channel | CH_RF18_BW_20M;
	/*
	 * RF register writes are masked in the upstream driver. Preserve all
	 * unrelated RF bits rather than writing zero to the entire RF register.
	 */
	if (rtw88_native_phy_rf_read(sc, 0, 0xbe, &rf_malsel) != 0 ||
	    rtw88_native_phy_rf_read(sc, 0, 0xdf, &rf_lutdbg) != 0 ||
	    rtw88_native_phy_rf_read(sc, 0, 0xb8, &rf_xtalx2) != 0)
		return (EIO);
	rf_malsel &= ~CH_RF_MALSEL_MASK;
	rf_lutdbg &= ~CH_RF_LUTDBG_MASK; /* channels 1-13 are below channel 144 */
	if (rtw88_native_phy_rf_write(sc, 0, 0xbe, rf_malsel) != 0 ||
	    rtw88_native_phy_rf_write(sc, 0, 0xdf, rf_lutdbg) != 0)
		return (EIO);

	/*
	 * RTW88 programs RF18 identically on each active path. Verify the
	 * direct RF window after each write; this catches a command-port write
	 * that completed over USB but did not change the selected RF path.
	 */
	for (path = 0; path < sc->rf_path_num; path++) {
		if ((active_paths & (1U << path)) == 0)
			continue;
		rf18_check = 0;
		rf_write_error = rtw88_native_phy_rf_write(sc, path, 0x18,
		    rf18);
		if (rf_write_error == 0)
			rf_write_error = rtw88_native_phy_rf_read(sc, path, 0x18,
			    &rf18_check);
		if (rf_write_error == 0 && rf18_check == rf18)
			continue;

		device_printf(sc->dev,
		    "RF path %d channel register mismatch: write_error=%d wrote=0x%05x read=0x%05x\n",
		    path, rf_write_error, rf18, rf18_check);
		if (path != 1 || sc->antenna_tx != BB_PATH_AB ||
		    sc->antenna_rx != BB_PATH_AB)
			return (EIO);

		/*
		 * The RTL8822B exposes independent RF-A and RF-B SIPI ports. If
		 * this board's B path does not accept a verified channel update,
		 * continue safely in 1T1R mode rather than scanning with one chain
		 * on the old channel. Re-run the upstream TRX-path setup to disable
		 * B in both TX and RX, then keep the already verified A channel.
		 */
		error = rtw88_native_phy_config_trx_mode(sc, BB_PATH_A,
		    BB_PATH_A);
		if (error != 0)
			return (error);
		sc->rf_path_num = 1;
		sc->antenna_tx = BB_PATH_A;
		sc->antenna_rx = BB_PATH_A;
		active_paths = BB_PATH_A;

		if (rtw88_native_reg_read32(sc, RTW88_NATIVE_RF_SIPI_B,
		    &sipi_b) == USB_ERR_NORMAL_COMPLETION &&
		    rtw88_native_reg_read32(sc, REG_RF_CTRL, &rf_ctrl) ==
		    USB_ERR_NORMAL_COMPLETION &&
		    rtw88_native_reg_read32(sc, REG_WLRF1, &wlrf1) ==
		    USB_ERR_NORMAL_COMPLETION) {
			device_printf(sc->dev,
			    "RF-B fallback diagnostics: SIPI_B=0x%08x RF_CTRL=0x%08x WLRF1=0x%08x\n",
			    sipi_b, rf_ctrl, wlrf1);
		}
		device_printf(sc->dev,
		    "RF path B did not track channel %u; continuing with verified RF path A only (1T1R)\n",
		    channel);

		rf18_check = 0;
		if (rtw88_native_phy_rf_write(sc, 0, 0x18, rf18) != 0 ||
		    rtw88_native_phy_rf_read(sc, 0, 0x18,
		    &rf18_check) != 0 || rf18_check != rf18) {
			device_printf(sc->dev,
			    "RF-A channel verification failed after 1T1R fallback: wrote 0x%05x, read 0x%05x\n",
			    rf18, rf18_check);
			return (EIO);
		}
		break;
	}
	rf_xtalx2 &= ~CH_RF_XTALX2_MASK;
	if (rtw88_native_phy_rf_write(sc, 0, 0xb8, rf_xtalx2) != 0)
		return (EIO);
	rf_xtalx2 |= CH_RF_XTALX2_MASK;
	if (rtw88_native_phy_rf_write(sc, 0, 0xb8, rf_xtalx2) != 0)
		return (EIO);

	/* IFEM-extended RFE option 3: route the enabled RF path(s) to antennas. */
	error = rtw88_native_phy_write_mask32(sc, REG_CH_RFESEL0,
	    0x00ffffffU, 0x00745774U);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RFESEL0 + 0x200,
		    0x00ffffffU, 0x00745774U);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RFESEL8,
		    0x0000ff00U, 0x57);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RFESEL8 + 0x200,
		    0x0000ff00U, 0x57);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RFEINV,
		    (1U << 11) | (1U << 10) | 0x3fU, 0);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RFEINV + 0x200,
		    (1U << 11) | (1U << 10) | 0x3fU, 0);
	trx_switch = (sc->antenna_rx == BB_PATH_AB ||
	    sc->antenna_tx == BB_PATH_AB) ? 0xa501 :
	    (sc->antenna_rx == sc->antenna_tx ? 0xa500 : 0xa005);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_TRSW,
		    0x0000ffffU, trx_switch);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_TRSW + 0x200,
		    0x0000ffffU, trx_switch);
	if (error != 0)
		return (error);

	/*
	 * Use the matching IFEM-ext CCA column: RTW88's 2R values are
	 * 0x97a0eaac / 0x86666341; the 1R values are
	 * 0x79a0eaaa / 0x87765541.
	 */
	if (rtw88_native_reg_write32(sc, REG_CH_CCASEL, 0x75da8010U) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, REG_CH_PDMFTH,
	    sc->antenna_rx == BB_PATH_AB ? 0x97a0eaacU : 0x79a0eaaaU) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, REG_CH_CCA2ND,
	    sc->antenna_rx == BB_PATH_AB ? 0x86666341U : 0x87765541U) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	/*
	 * Match rtw8822b_toggle_igi(): toggle the gain index for every active
	 * receive chain, then pulse RX path selection with the selected mask.
	 */
	if (rtw88_native_reg_read32(sc, REG_CH_RXIGI_A, &igi_a) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	igi_a &= CH_BIT_MAC_RXIGI;
	if (igi_a < 2)
		return (EIO);
	error = rtw88_native_phy_write_mask32(sc, REG_CH_RXIGI_A,
	    CH_BIT_MAC_RXIGI, igi_a - 2);
	if (error == 0 && (sc->antenna_rx & BB_PATH_B) != 0) {
		if (rtw88_native_reg_read32(sc, REG_CH_RXIGI_B, &igi_b) !=
		    USB_ERR_NORMAL_COMPLETION)
			return (EIO);
		igi_b &= CH_BIT_MAC_RXIGI;
		if (igi_b < 2)
			return (EIO);
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RXIGI_B,
		    CH_BIT_MAC_RXIGI, igi_b - 2);
	}
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RXIGI_A,
		    CH_BIT_MAC_RXIGI, igi_a);
	if (error == 0 && (sc->antenna_rx & BB_PATH_B) != 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RXIGI_B,
		    CH_BIT_MAC_RXIGI, igi_b);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_RXPSEL,
		    0x000000ffU, 0);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_RXPSEL,
		    0x000000ffU,
		    sc->antenna_rx | (sc->antenna_rx << 4));
	if (error != 0)
		return (error);

	if (rtw88_native_reg_read8(sc, REG_CH_CCK_CHECK, &value8) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, REG_CH_CCK_CHECK,
	    value8 & ~CH_BIT_CHECK_CCK_EN) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	device_printf(sc->dev,
	    "RTL8822B tuned to 2.4-GHz channel %u at 20 MHz (RFE option 3, %s)\n",
	    channel, sc->antenna_rx == BB_PATH_AB ? "2T2R" : "1T1R");
	return (0);
}

/*
 * Port of the RTL8822B 5-GHz/20-MHz channel path for the adapter's IFEM-ext
 * RFE option 3. This intentionally leaves HT/VHT bandwidth programming out.
 */
static int
rtw88_native_phy_set_channel_5g20(struct rtw88_native_softc *sc,
    uint8_t channel)
{
	static const uint8_t low_band[15] = {
		0x7, 0x6, 0x6, 0x5, 0x0, 0x0, 0x7, 0xff,
		0x6, 0x5, 0x0, 0x0, 0x7, 0x6, 0x6
	};
	static const uint8_t middle_band[23] = {
		0x6, 0x5, 0x0, 0x0, 0x7, 0x6, 0x6, 0xff,
		0x0, 0x0, 0x7, 0x6, 0x6, 0x5, 0x0, 0xff,
		0x7, 0x6, 0x6, 0x5, 0x0, 0x0, 0x7
	};
	static const uint8_t high_band[15] = {
		0x5, 0x5, 0x0, 0x7, 0x7, 0x6, 0x5, 0xff,
		0x0, 0x7, 0x7, 0x6, 0x5, 0x5, 0x0
	};
	uint32_t rf18, rf_malsel, rf_xtalx2, rf_lutdbg, rf18_check;
	uint32_t igi_a, igi_b, value32, sipi_b, rf_ctrl, wlrf1;
	uint32_t rf_be, ccamask, pdmfth, cca2nd;
	uint8_t active_paths;
	int error, path, rf_write_error;

	if (sc->efuse.rfe_option != 3 || sc->rf_path_num == 0 ||
	    sc->rf_path_num > 2 ||
	    (sc->antenna_tx != BB_PATH_A && sc->antenna_tx != BB_PATH_AB) ||
	    (sc->antenna_rx != BB_PATH_A && sc->antenna_rx != BB_PATH_AB) ||
	    (((sc->antenna_tx | sc->antenna_rx) & BB_PATH_B) != 0 &&
	    sc->rf_path_num < 2))
		return (ENOTSUP);
	active_paths = sc->antenna_tx | sc->antenna_rx;

	if (channel >= 36 && channel <= 64 && (channel & 3) == 0)
		rf_be = low_band[(channel - 36) >> 1];
	else if (channel >= 100 && channel <= 140 && (channel & 3) == 0)
		rf_be = middle_band[(channel - 100) >> 1];
	else if (channel >= 149 && channel <= 161 &&
	    ((channel - 149) & 3) == 0)
		rf_be = high_band[(channel - 149) >> 1];
	else
		return (EINVAL);

	/* 5-GHz baseband profile: disable CCK and select the proper AGC band. */
	error = rtw88_native_phy_write_mask32(sc, REG_CH_ENTXCCK,
	    1U << 18, 1);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_CCK_CHECK,
		    CH_BIT_CHECK_CCK_EN, 1);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_RXPSEL,
		    1U << 28, 0);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RXCCAMSK,
		    0x0000fc00U, 34);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_ACGG2TBL,
		    0x1f, channel <= 64 ? 1 : (channel <= 140 ? 2 : 3));
	if (channel >= 36 && channel <= 48)
		ccamask = 0x494;
	else if (channel >= 52 && channel <= 64)
		ccamask = 0x453;
	else if (channel >= 100 && channel <= 116)
		ccamask = 0x452;
	else
		ccamask = 0x412;
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_CLKTRK,
		    0x1ffe0000U, ccamask);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RFEINV,
		    0x00000300U, 1);
	if (error != 0)
		return (error);

	/* 20-MHz ADC and receive DFIR profile. */
	if (rtw88_native_reg_read32(sc, REG_CH_ADCCLK, &value32) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	value32 &= 0xffcffc00U;
	if (rtw88_native_reg_write32(sc, REG_CH_ADCCLK, value32) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	error = rtw88_native_phy_write_mask32(sc, REG_CH_ADC160,
	    1U << 30, 1);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_ACBB0,
		    (3U << 28), 2);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_ACBBRXFIR,
		    (3U << 28), 2);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_TXDFIR,
		    1U << 31, 1);
	if (error != 0)
		return (error);

	/* MAC remains at 20 MHz; channel index zero encodes 20-MHz width. */
	error = rtw88_native_phy_write_mask32(sc, REG_CH_WMAC_TRXPTCL_CTL,
	    CH_MAC_RFMOD_MASK, 0);
	if (error == 0)
		error = rtw88_native_reg_write8(sc, REG_CH_DATA_SC, 0) ==
		    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_AFE_CTRL1,
		    CH_BIT_MAC_CLK_SEL, 0);
	if (error == 0)
		error = rtw88_native_reg_write8(sc, REG_CH_USTIME_TSF, 80) ==
		    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
	if (error == 0)
		error = rtw88_native_reg_write8(sc, REG_CH_USTIME_EDCA, 80) ==
		    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
	if (error != 0)
		return (error);

	/* RF18: 5-GHz band, channel, RFSI sub-band and 20-MHz bandwidth. */
	if (rtw88_native_phy_rf_read(sc, 0, 0x18, &rf18) != 0 ||
	    rtw88_native_phy_rf_read(sc, 0, 0xbe, &rf_malsel) != 0 ||
	    rtw88_native_phy_rf_read(sc, 0, 0xdf, &rf_lutdbg) != 0 ||
	    rtw88_native_phy_rf_read(sc, 0, 0xb8, &rf_xtalx2) != 0)
		return (EIO);
	rf18 &= ~(CH_RF18_BAND_MASK | CH_RF18_CHANNEL_MASK |
	    CH_RF18_RFSI_MASK | CH_RF18_BW_MASK);
	rf18 |= (1U << 16) | (1U << 8) | channel | CH_RF18_BW_20M;
	if (channel >= 80 && channel <= 144)
		rf18 |= (1U << 17);
	else if (channel > 144)
		rf18 |= (1U << 18);
	rf_malsel &= ~CH_RF_MALSEL_MASK;
	rf_malsel |= (rf_be << 15) & CH_RF_MALSEL_MASK;
	if (channel == 144)
		rf_lutdbg |= CH_RF_LUTDBG_MASK;
	else
		rf_lutdbg &= ~CH_RF_LUTDBG_MASK;
	if (rtw88_native_phy_rf_write(sc, 0, 0xbe, rf_malsel) != 0 ||
	    rtw88_native_phy_rf_write(sc, 0, 0xdf, rf_lutdbg) != 0)
		return (EIO);

	/* Verify the per-path RF18 writes after the SIPI transaction completes. */
	for (path = 0; path < sc->rf_path_num; path++) {
		if ((active_paths & (1U << path)) == 0)
			continue;
		rf18_check = 0;
		rf_write_error = rtw88_native_phy_rf_write(sc, path, 0x18,
		    rf18);
		if (rf_write_error == 0)
			rf_write_error = rtw88_native_phy_rf_read(sc, path, 0x18,
			    &rf18_check);
		if (rf_write_error == 0 && rf18_check == rf18)
			continue;

		device_printf(sc->dev,
		    "5-GHz RF path %d channel mismatch: write_error=%d wrote=0x%05x read=0x%05x\n",
		    path, rf_write_error, rf18, rf18_check);
		if (path != 1 || sc->antenna_tx != BB_PATH_AB ||
		    sc->antenna_rx != BB_PATH_AB)
			return (EIO);
		error = rtw88_native_phy_config_trx_mode(sc, BB_PATH_A,
		    BB_PATH_A);
		if (error != 0)
			return (error);
		sc->rf_path_num = 1;
		sc->antenna_tx = BB_PATH_A;
		sc->antenna_rx = BB_PATH_A;
		active_paths = BB_PATH_A;
		if (rtw88_native_reg_read32(sc, RTW88_NATIVE_RF_SIPI_B,
		    &sipi_b) == USB_ERR_NORMAL_COMPLETION &&
		    rtw88_native_reg_read32(sc, REG_RF_CTRL, &rf_ctrl) ==
		    USB_ERR_NORMAL_COMPLETION &&
		    rtw88_native_reg_read32(sc, REG_WLRF1, &wlrf1) ==
		    USB_ERR_NORMAL_COMPLETION) {
			device_printf(sc->dev,
			    "5-GHz RF-B fallback: SIPI_B=0x%08x RF_CTRL=0x%08x WLRF1=0x%08x\n",
			    sipi_b, rf_ctrl, wlrf1);
		}
		device_printf(sc->dev,
		    "5-GHz RF path B did not track channel %u; using RF-A only (1T1R)\n",
		    channel);
		rf18_check = 0;
		if (rtw88_native_phy_rf_write(sc, 0, 0x18, rf18) != 0 ||
		    rtw88_native_phy_rf_read(sc, 0, 0x18,
		    &rf18_check) != 0 || rf18_check != rf18) {
			device_printf(sc->dev,
			    "5-GHz RF-A verification failed: wrote 0x%05x, read 0x%05x\n",
			    rf18, rf18_check);
			return (EIO);
		}
		break;
	}
	rf_xtalx2 &= ~CH_RF_XTALX2_MASK;
	if (rtw88_native_phy_rf_write(sc, 0, 0xb8, rf_xtalx2) != 0)
		return (EIO);
	rf_xtalx2 |= CH_RF_XTALX2_MASK;
	if (rtw88_native_phy_rf_write(sc, 0, 0xb8, rf_xtalx2) != 0)
		return (EIO);

	/* IFEM-ext RFE option 3: use the 5-GHz RF front-end routes. */
	error = rtw88_native_phy_write_mask32(sc, REG_CH_RFESEL0,
	    0x00ffffffU, 0x00477547U);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RFESEL0 + 0x200,
		    0x00ffffffU, 0x00477547U);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RFESEL8,
		    0x0000ff00U, 0x75);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RFESEL8 + 0x200,
		    0x0000ff00U, 0x75);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RFEINV,
		    (1U << 11) | (1U << 10) | 0x3fU, 0);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RFEINV + 0x200,
		    (1U << 11) | (1U << 10) | 0x3fU, 0);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_TRSW,
		    0x0000ffffU, 0xa5a5);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_TRSW + 0x200,
		    0x0000ffffU, 0xa5a5);
	if (error != 0)
		return (error);

	/* IFEM-ext 5-GHz 1R/2R CCA tables from the 8822B RFE configuration. */
	ccamask = 0x75da8010U;
	if (sc->antenna_rx == BB_PATH_AB) {
		pdmfth = 0x79a0eaaaU;
		cca2nd = 0x86666361U;
	} else {
		pdmfth = 0x79a0eaaaU;
		cca2nd = 0x87765561U;
	}
	if (rtw88_native_reg_write32(sc, REG_CH_CCASEL, ccamask) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, REG_CH_PDMFTH, pdmfth) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, REG_CH_CCA2ND, cca2nd) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	if ((channel >= 52 && channel <= 64) ||
	    (channel >= 100 && channel <= 140)) {
		error = rtw88_native_phy_write_mask32(sc, REG_CH_CCA2ND,
		    0x000000f0U, 4);
		if (error != 0)
			return (error);
	}

	/* Toggle receive gain and pulse the selected antenna path, as upstream. */
	if (rtw88_native_reg_read32(sc, REG_CH_RXIGI_A, &igi_a) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	igi_a &= CH_BIT_MAC_RXIGI;
	if (igi_a < 2)
		return (EIO);
	error = rtw88_native_phy_write_mask32(sc, REG_CH_RXIGI_A,
	    CH_BIT_MAC_RXIGI, igi_a - 2);
	if (error == 0 && (sc->antenna_rx & BB_PATH_B) != 0) {
		if (rtw88_native_reg_read32(sc, REG_CH_RXIGI_B, &igi_b) !=
		    USB_ERR_NORMAL_COMPLETION)
			return (EIO);
		igi_b &= CH_BIT_MAC_RXIGI;
		if (igi_b < 2)
			return (EIO);
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RXIGI_B,
		    CH_BIT_MAC_RXIGI, igi_b - 2);
	}
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RXIGI_A,
		    CH_BIT_MAC_RXIGI, igi_a);
	if (error == 0 && (sc->antenna_rx & BB_PATH_B) != 0)
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RXIGI_B,
		    CH_BIT_MAC_RXIGI, igi_b);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_RXPSEL,
		    0x000000ffU, 0);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_RXPSEL,
		    0x000000ffU,
		    sc->antenna_rx | (sc->antenna_rx << 4));
	if (error != 0)
		return (error);

	device_printf(sc->dev,
	    "RTL8822B tuned to 5-GHz channel %u at 20 MHz (RFE option 3, %s)\n",
	    channel, sc->antenna_rx == BB_PATH_AB ? "2T2R" : "1T1R");
	return (0);
}

static bool
rtw88_native_phy_valid_5g_channel(uint8_t channel)
{
	switch (channel) {
	case 36: case 40: case 44: case 48:
	case 52: case 56: case 60: case 64:
	case 100: case 104: case 108: case 112: case 116:
	case 120: case 124: case 128: case 132: case 136: case 140:
	case 149: case 153: case 157: case 161:
		return (true);
	default:
		return (false);
	}
}

int
rtw88_native_phy_set_channel(struct rtw88_native_softc *sc, uint8_t channel)
{
	if (sc == NULL || !sc->powered || !sc->firmware_running ||
	    !sc->mac_initialized || !sc->phy_initialized || sc->dying)
		return (EINVAL);
	if (sc->efuse.rfe_option != 3)
		return (ENOTSUP);
	if (channel >= 1 && channel <= 13)
		return (rtw88_native_phy_set_channel_2g20(sc, channel));
	if (rtw88_native_phy_valid_5g_channel(channel))
		return (rtw88_native_phy_set_channel_5g20(sc, channel));
	return (EINVAL);
}

/*
 * Linux RTW88's rtw_phy_dig_set_max_coverage() writes DIG_CVRG_MIN (0x1c)
 * to both 8822B receive paths while scanning. Restore the pre-scan Path-A
 * IGI on scan completion, matching rtw_phy_dig_reset().
 */
int
rtw88_native_phy_scan_gain(struct rtw88_native_softc *sc, int enable)
{
	uint32_t igi_a;
	uint8_t saved;
	int error;

	if (sc == NULL || (enable != 0 && enable != 1))
		return (EINVAL);

	mtx_lock(&sc->mtx);
	if (sc->dying || sc->detaching || !sc->powered ||
	    !sc->phy_initialized) {
		mtx_unlock(&sc->mtx);
		return (EAGAIN);
	}
	if (sc->hw_busy) {
		mtx_unlock(&sc->mtx);
		return (EBUSY);
	}
	if ((enable && sc->scan_igi_valid) ||
	    (!enable && !sc->scan_igi_valid)) {
		mtx_unlock(&sc->mtx);
		return (0);
	}
	sc->hw_busy = 1;
	saved = sc->scan_igi_saved;
	mtx_unlock(&sc->mtx);

	error = 0;
	if (enable) {
		if (rtw88_native_reg_read32(sc, REG_CH_RXIGI_A, &igi_a) !=
		    USB_ERR_NORMAL_COMPLETION) {
			error = EIO;
		} else {
			saved = (uint8_t)(igi_a & CH_BIT_MAC_RXIGI);
			/*
			 * DIG_CVRG_MIN=0x1c is the upstream RTW88 coverage
			 * setting. Lower IGI means higher receive gain.
			 */
			error = rtw88_native_phy_write_mask32(sc,
			    REG_CH_RXIGI_A, CH_BIT_MAC_RXIGI, 0x1c);
			if (error == 0)
				error = rtw88_native_phy_write_mask32(sc,
				    REG_CH_RXIGI_B, CH_BIT_MAC_RXIGI, 0x1c);
			if (error != 0) {
				(void)rtw88_native_phy_write_mask32(sc,
				    REG_CH_RXIGI_A, CH_BIT_MAC_RXIGI, saved);
				(void)rtw88_native_phy_write_mask32(sc,
				    REG_CH_RXIGI_B, CH_BIT_MAC_RXIGI, saved);
			}
		}
	} else {
		error = rtw88_native_phy_write_mask32(sc, REG_CH_RXIGI_A,
		    CH_BIT_MAC_RXIGI, saved);
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_CH_RXIGI_B,
			    CH_BIT_MAC_RXIGI, saved);
	}

	mtx_lock(&sc->mtx);
	if (error == 0) {
		if (enable) {
			sc->scan_igi_saved = saved;
			sc->scan_igi_valid = 1;
		} else {
			sc->scan_igi_valid = 0;
		}
	}
	sc->hw_busy = 0;
	cv_broadcast(&sc->io_drain_cv);
	mtx_unlock(&sc->mtx);
	return (error);
}


/*
 * RTL8822B adaptivity/EDCCA initialization from upstream
 * rtw8822b_adaptivity_init(). Leaving these registers at reset defaults
 * makes receive CCA behaviour board/firmware dependent.
 */
static int
rtw88_native_phy_adaptivity_init(struct rtw88_native_softc *sc)
{
	int error;

	/* EDCCA L2H/H2L thresholds are the low/high bytes of REG 0x08a4. */
	error = rtw88_native_phy_write_mask32(sc, 0x08a4, 0x000000ffU,
	    0x7f);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, 0x08a4,
		    0x0000ff00U, 0x7f);
	/* Enable MAC EDCCA gating and its countdown source. */
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, 0x0520,
		    1U << 15, 0);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, 0x0524,
		    1U << 11, 1);
	/* RTL8822B default EDCCA source = 1. */
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, 0x0944,
		    3U << 28, 1);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, 0x08a0,
		    3U, 0);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, 0x08dc,
		    1U << 5, 1);

	return (error);
}


/*
 * RTL8822B LTE coexistence indirect-register interface, as used by Linux
 * RTW88 util.c. The Wi-Fi-only RFE path still needs its grant state and
 * external-antenna switch routed to WLAN even when Bluetooth coexistence is
 * disabled in eFuse.
 */
#define REG_LTECOEX_ACCESS_CTRL		0x1700
#define REG_LTECOEX_WRITE_DATA		0x1704
#define REG_LTECOEX_READ_DATA		0x1708
#define REG_SYS_SDIO_CTRL			0x0070
#define REG_LED_CFG			0x004c
#define REG_RFE_CTRL_E			0x0974
#define REG_RFE_CTRL8			0x0cb4
#define REG_RFE_INV8			0x0cbd
#define REG_RFE_INV16			0x0cbe
#define REG_RFESEL_CTRL			0x1990
#define REG_WIFI_BT_INFO			0x00aa
#define REG_BT_COEX_TABLE_H			0x06cc
#define REG_BT_COEX_TABLE_H3			0x06cf
#define REG_BT_TDMA_TIME			0x0790
#define REG_BT_STAT_CTRL			0x0778
#define REG_GPIO_MUXCFG			0x0040
#define REG_QUEUE_CTRL			0x04c6
#define REG_BT_COEX_V2			0x0762

#define LTECOEX_READY			(1U << 29)
#define BIT_LTE_MUX_CTRL_PATH		(1U << 26)
#define BIT_LTE_COEX_EN			(1U << 7)
#define BIT_RFE_BUF_EN			(1U << 3)
#define BIT_BT_INT_EN			(1U << 15)
#define BIT_BT_PTA_EN			(1U << 5)
#define BIT_PO_BT_PTA_PINS		(1U << 9)
#define BIT_PTA_WL_TX_EN		(1U << 4)
#define BIT_PTA_EDCCA_EN		(1U << 5)
#define BIT_GNT_BT_POLARITY		(1U << 12)

static int
rtw88_native_ltecoex_wait_ready(struct rtw88_native_softc *sc)
{
	uint32_t value;
	int i;

	for (i = 0; i < 1000; i++) {
		if (rtw88_native_reg_read32(sc, REG_LTECOEX_ACCESS_CTRL,
		    &value) != USB_ERR_NORMAL_COMPLETION)
			return (EIO);
		if ((value & LTECOEX_READY) != 0)
			return (0);
		DELAY(10);
	}
	return (ETIMEDOUT);
}

static int
rtw88_native_ltecoex_read(struct rtw88_native_softc *sc, uint16_t offset,
    uint32_t *value)
{
	int error;

	if (value == NULL)
		return (EINVAL);
	error = rtw88_native_ltecoex_wait_ready(sc);
	if (error != 0)
		return (error);
	if (rtw88_native_reg_write32(sc, REG_LTECOEX_ACCESS_CTRL,
	    0x800f0000U | offset) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_read32(sc, REG_LTECOEX_READ_DATA, value) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	return (0);
}

static int
rtw88_native_ltecoex_write(struct rtw88_native_softc *sc, uint16_t offset,
    uint32_t value)
{
	int error;

	error = rtw88_native_ltecoex_wait_ready(sc);
	if (error != 0)
		return (error);
	if (rtw88_native_reg_write32(sc, REG_LTECOEX_WRITE_DATA, value) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, REG_LTECOEX_ACCESS_CTRL,
	    0xc00f0000U | offset) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	return (0);
}

static int
rtw88_native_ltecoex_write_mask(struct rtw88_native_softc *sc,
    uint16_t offset, uint32_t mask, uint32_t value)
{
	uint32_t old, shift;
	int error;

	if (mask == 0)
		return (EINVAL);
	error = rtw88_native_ltecoex_read(sc, offset, &old);
	if (error != 0)
		return (error);
	shift = __builtin_ctz(mask);
	old = (old & ~mask) | ((value << shift) & mask);
	return (rtw88_native_ltecoex_write(sc, offset, old));
}

static int
rtw88_native_phy_write_mask8(struct rtw88_native_softc *sc, uint16_t reg,
    uint8_t mask, uint8_t value)
{
	uint8_t old;

	if (rtw88_native_reg_read8(sc, reg, &old) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	old = (old & ~mask) | (value & mask);
	return (rtw88_native_reg_write8(sc, reg, old) ==
	    USB_ERR_NORMAL_COMPLETION ? 0 : EIO);
}

/*
 * Linux calls rtw_coex_init_hw_config(wifi_only=true) for this board.
 * Port only the required 8822B RFE routing and grant programming: no
 * Bluetooth profile scheduler, PTA TDMA or BT firmware commands are enabled.
 *
 * For RFE option 3, the upstream driver selects the WLAN DPDT path by BBSW,
 * drives GNT_BT low / GNT_WL high, selects the WLG route (0x77) and sets its
 * inversion value (0x2). The existing RFESEL0/8 and RFEINV values remain
 * preserved by masked writes.
 */
int
rtw88_native_coex_wifi_only_init(struct rtw88_native_softc *sc)
{
	uint32_t value;
	int error;

	if (sc == NULL || !sc->powered || !sc->firmware_running ||
	    !sc->phy_initialized || sc->dying)
		return (EINVAL);
	if (sc->efuse.rfe_option != 3 || sc->rf_path_num != 2 ||
	    sc->antenna_tx != BB_PATH_AB || sc->antenna_rx != BB_PATH_AB)
		return (ENOTSUP);
	if ((sc->efuse.rf_board_option & 0xe0U) == 0x20U)
		return (ENOTSUP); /* Full BT coexistence is not implemented. */
	if (sc->wifi_only_coex_initialized)
		return (0);

	/* rtw8822b_coex_cfg_rfe_type(): select the 8822B RFE signal path. */
	error = rtw88_native_reg_write8(sc, REG_RFE_CTRL_E, 0xff) ==
	    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
	if (error == 0)
		error = rtw88_native_phy_write_mask8(sc,
		    REG_RFESEL_CTRL + 1, 0x03, 0x00);
	if (error == 0)
		error = rtw88_native_phy_write_mask8(sc, REG_RFE_INV16,
		    BIT_RFE_BUF_EN, 0x00);
	if (error != 0)
		return (error);

	/*
	 * Linux disables LTE coexistence, then sets both WLAN/BT-vs-LTE
	 * direction masks to all ones. Keep these indirect writes checked.
	 */
	error = rtw88_native_ltecoex_write_mask(sc, 0x0038,
	    BIT_LTE_COEX_EN, 0);
	if (error == 0)
		error = rtw88_native_ltecoex_write_mask(sc, 0x00a0,
		    0x0000ffffU, 0xffff);
	if (error == 0)
		error = rtw88_native_ltecoex_write_mask(sc, 0x00a4,
		    0x0000ffffU, 0xffff);

	/* COEX_SET_ANT_WONLY: grants are fixed to WLAN for this Wi-Fi-only board. */
	if (error == 0)
		error = rtw88_native_ltecoex_write_mask(sc, 0x0038,
		    0x0000c000U, 0x1);
	if (error == 0)
		error = rtw88_native_ltecoex_write_mask(sc, 0x0038,
		    0x00000c00U, 0x1);
	if (error == 0)
		error = rtw88_native_ltecoex_write_mask(sc, 0x0038,
		    0x00003000U, 0x3);
	if (error == 0)
		error = rtw88_native_ltecoex_write_mask(sc, 0x0038,
		    0x00000300U, 0x3);
	if (error != 0) {
		device_printf(sc->dev,
		    "Wi-Fi-only LTE grant configuration failed: %d\n", error);
		return (error);
	}

	/*
	 * rtw8822b_coex_cfg_init(): initialise the shared grant/antenna GPIO,
	 * WLAN-to-PTA indication and BT statistics sampling registers. These
	 * are common 8822B setup writes and are also performed in wifi_only mode.
	 */
	error = rtw88_native_phy_write_mask8(sc, REG_BT_TDMA_TIME,
	    0x3f, 0x05);
	if (error == 0)
		error = rtw88_native_reg_write8(sc, REG_BT_STAT_CTRL, 0x01) ==
		    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_GPIO_MUXCFG,
		    BIT_BT_PTA_EN | BIT_PO_BT_PTA_PINS,
		    BIT_BT_PTA_EN | BIT_PO_BT_PTA_PINS);
	if (error == 0)
		error = rtw88_native_phy_write_mask8(sc, REG_QUEUE_CTRL,
		    BIT_PTA_WL_TX_EN | BIT_PTA_EDCCA_EN,
		    BIT_PTA_WL_TX_EN);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_BT_COEX_V2,
		    BIT_GNT_BT_POLARITY, BIT_GNT_BT_POLARITY);
	/* Linux gives ACK/BA/CTS, beacon and beacon-queue traffic high priority. */
	if (error == 0)
		error = rtw88_native_phy_write_mask8(sc,
		    REG_BT_COEX_TABLE_H, (1U << 3) | (1U << 4),
		    (1U << 3) | (1U << 4));
	if (error == 0)
		error = rtw88_native_phy_write_mask8(sc,
		    REG_BT_COEX_TABLE_H3, (1U << 3), (1U << 3));
	if (error != 0)
		return (error);

	/* COEX_SET_ANT_WONLY: grant path control to WLAN. 8822B has no btg_reg. */
	if (rtw88_native_phy_write_mask8(sc, REG_SYS_SDIO_CTRL + 3,
	    (uint8_t)(BIT_LTE_MUX_CTRL_PATH >> 24),
	    (uint8_t)(BIT_LTE_MUX_CTRL_PATH >> 24)) != 0)
		return (EIO);

	/*
	 * rtw8822b_coex_cfg_ant_switch(BBSW, WLG): LED_CFG[23]=0,
	 * LED_CFG[24]=1, RFE_CTRL8[7:0]=0x77, RFE_INV8[1:0]=2.
	 */
	error = rtw88_native_phy_write_mask8(sc, REG_LED_CFG + 2,
	    0x80, 0x00);
	if (error == 0)
		error = rtw88_native_phy_write_mask8(sc, REG_LED_CFG + 3,
		    0x01, 0x01);
	if (error == 0)
		error = rtw88_native_phy_write_mask8(sc, REG_RFE_CTRL8,
		    0xff, 0x77);
	if (error == 0)
		error = rtw88_native_phy_write_mask8(sc, REG_RFE_INV8,
		    0x03, 0x02);
	if (error != 0)
		return (error);

	/*
	 * COEX_SCBD_ACTIVE | COEX_SCBD_ONOFF: tell firmware that WLAN is
	 * active and its radio is enabled. This is the Wi-Fi-only scoreboard
	 * state written by Linux after the WONLY antenna path is selected.
	 */
	if (rtw88_native_reg_write16(sc, REG_WIFI_BT_INFO,
	    BIT_BT_INT_EN | 0x0003) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	/* Read the LTE control register once more to ensure the path is ready. */
	error = rtw88_native_ltecoex_read(sc, 0x0038, &value);
	if (error != 0)
		return (error);
	sc->wifi_only_coex_initialized = 1;
	device_printf(sc->dev,
	    "RTL8822B Wi-Fi-only RFE route active: WLAN grants, WLG antenna path, LTE control=0x%08x\n",
	    value);
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
	uint32_t xtal, value32;
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
	/*
	 * rtw88_native_phy_write_mask32() takes an unshifted field value.
	 * WLRF1's enable field is bits 24..26, so pass 0x7, not the
	 * already-positioned bit mask (which would be shifted a second time
	 * and silently write zero).
	 */
	error = rtw88_native_phy_write_mask32(sc, REG_WLRF1,
	    BIT_WLRF1_BBRF_EN, 0x7);
	if (error != 0)
		return (error);
	value32 = 0;
	if (rtw88_native_reg_read32(sc, REG_WLRF1, &value32) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    (value32 & BIT_WLRF1_BBRF_EN) != BIT_WLRF1_BBRF_EN) {
		device_printf(sc->dev,
		    "BB/RF domain enable did not latch: WLRF1=0x%08x\n",
		    value32);
		return (EIO);
	}
	device_printf(sc->dev, "BB/RF domain enabled: WLRF1=0x%08x\n",
	    value32);

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

	/*
	 * Release both RX path-select reset bits. The mask covers bits 28..29,
	 * so its unshifted field value is 0x3 (not BIT_RX_PSEL_RST).
	 */
	error = rtw88_native_phy_write_mask32(sc, REG_RXPSEL,
	    BIT_RX_PSEL_RST, 0x3);
	if (error != 0)
		return (error);
	value32 = 0;
	if (rtw88_native_reg_read32(sc, REG_RXPSEL, &value32) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    (value32 & BIT_RX_PSEL_RST) != BIT_RX_PSEL_RST) {
		device_printf(sc->dev,
		    "RX path-select reset release did not latch: RXPSEL=0x%08x\n",
		    value32);
		return (EIO);
	}

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
	if (error == 0)
		error = rtw88_native_phy_adaptivity_init(sc);
	if (error != 0)
		return (error);

	sc->phy_initialized = 1;
	device_printf(sc->dev,
	    "RTL8822B USB PHY tables loaded: MAC, BB, AGC and %u RF path(s)\n",
	    sc->rf_path_num);
	return (0);
}
