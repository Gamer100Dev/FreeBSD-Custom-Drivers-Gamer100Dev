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


/* RTL8822B channel and RF front-end configuration. */
#define REG_DATA_SC			0x0483
#define REG_WMAC_TRXPTCL_CTL		0x0668
#define REG_AFE_CTRL1			0x0024
#define REG_USTIME_TSF			0x055c
#define REG_USTIME_EDCA			0x0638
#define REG_CCK_CHECK			0x0454
#define REG_RXCCAMSK			0x0814
#define REG_L1WT			0x083c
#define REG_L1PKWT			0x0840
#define REG_CCASEL			0x082c
#define REG_PDMFTH			0x0830
#define REG_CCA2ND			0x0838
#define REG_CLKTRK			0x0860
#define REG_ADCCLK			0x08ac
#define REG_ADC160			0x08c4
#define REG_RXSB			0x0a00
#define REG_ACBB0			0x0948
#define REG_ACBBRXFIR			0x094c
#define REG_ACGG2TBL			0x0958
#define REG_ADC40			0x08c8
#define REG_TXSF2			0x0a24
#define REG_TXSF6			0x0a28
#define REG_ENTXCCK			0x0a80
#define REG_TXDFIR			0x0c20
#define REG_RXIGI_A			0x0c50
#define REG_RXIGI_B			0x0e50
#define REG_RFESEL0			0x0cb0
#define REG_RFESEL8			0x0cb4
#define REG_RFECTL			0x0cb8
#define REG_RFEINV			0x0cbc

#define BIT_RFMOD			((1U << 7) | (1U << 8))
#define BIT_RFMOD_80M			(1U << 8)
#define BIT_RFMOD_40M			(1U << 7)
#define BIT_MAC_CLK_SEL			((1U << 20) | (1U << 21))
#define BIT_CHECK_CCK_EN		(1U << 7)
#define BIT_RXSB_20_UPPER		(1U << 4)

#define RF18_BAND_MASK			((1U << 16) | (1U << 9) | (1U << 8))
#define RF18_BAND_2G			0U
#define RF18_BAND_5G			((1U << 16) | (1U << 8))
#define RF18_CHANNEL_MASK		0xffU
#define RF18_RFSI_MASK			((1U << 18) | (1U << 17))
#define RF18_RFSI_GE_CH80		(1U << 17)
#define RF18_RFSI_GT_CH144		(1U << 18)
#define RF18_BW_MASK			((1U << 11) | (1U << 10))
#define RF18_BW_20M			((1U << 11) | (1U << 10))
#define RF18_BW_40M			(1U << 11)
#define RF18_BW_80M			(1U << 10)
#define RFBE_MASK			((1U << 17) | (1U << 16) | (1U << 15))

#define RTW88_NATIVE_BW_20		0
#define RTW88_NATIVE_BW_40		1
#define RTW88_NATIVE_BW_80		2
#define RTW88_NATIVE_BW_5		5
#define RTW88_NATIVE_BW_10		6
#define RTW88_NATIVE_SC_20_UPPER	1
#define RTW88_NATIVE_SC_20_UPMOST	3

struct rtw88_native_cca_table {
	uint32_t reg82c[4];
	uint32_t reg830[4];
	uint32_t reg838[4];
};

static const struct rtw88_native_cca_table rtw88_native_cca_ifem = {
	{ 0x75c97010, 0x75c97010, 0x75c97010, 0x75c97010 },
	{ 0x79a0eaaa, 0x79a0eaac, 0x79a0eaaa, 0x79a0eaaa },
	{ 0x87765541, 0x87746341, 0x87765541, 0x87746341 }
};

static const struct rtw88_native_cca_table rtw88_native_cca_efem = {
	{ 0x75b86010, 0x75b76010, 0x75b86010, 0x75b76010 },
	{ 0x79a0eaa8, 0x79a0eaac, 0x79a0eaa8, 0x79a0eaaa },
	{ 0x87766451, 0x87766431, 0x87766451, 0x87766431 }
};

static const struct rtw88_native_cca_table rtw88_native_cca_ifem_ext = {
	{ 0x75da8010, 0x75da8010, 0x75da8010, 0x75da8010 },
	{ 0x79a0eaaa, 0x97a0eaac, 0x79a0eaaa, 0x79a0eaaa },
	{ 0x87765541, 0x86666341, 0x87765561, 0x86666361 }
};

static const uint8_t rtw88_native_rf_low_band[15] = {
	0x7, 0x6, 0x6, 0x5, 0x0, 0x0, 0x7, 0xff,
	0x6, 0x5, 0x0, 0x0, 0x7, 0x6, 0x6
};

static const uint8_t rtw88_native_rf_middle_band[23] = {
	0x6, 0x5, 0x0, 0x0, 0x7, 0x6, 0x6, 0xff,
	0x0, 0x0, 0x7, 0x6, 0x6, 0x5, 0x0, 0xff,
	0x7, 0x6, 0x6, 0x5, 0x0, 0x0, 0x7
};

static const uint8_t rtw88_native_rf_high_band[15] = {
	0x5, 0x5, 0x0, 0x7, 0x7, 0x6, 0x5, 0xff,
	0x0, 0x7, 0x7, 0x6, 0x5, 0x5, 0x0
};

static bool
rtw88_native_is_2g(uint8_t channel)
{
	return (channel >= 1 && channel <= 14);
}

static bool
rtw88_native_is_5g_band1(uint8_t channel)
{
	return (channel >= 36 && channel <= 48);
}

static bool
rtw88_native_is_5g_band2(uint8_t channel)
{
	return (channel >= 52 && channel <= 64);
}

static bool
rtw88_native_is_5g_band3(uint8_t channel)
{
	return (channel >= 100 && channel <= 144);
}

static bool
rtw88_native_is_5g_band4(uint8_t channel)
{
	return (channel >= 149 && channel <= 177);
}

static bool
rtw88_native_is_5g(uint8_t channel)
{
	return (rtw88_native_is_5g_band1(channel) ||
	    rtw88_native_is_5g_band2(channel) ||
	    rtw88_native_is_5g_band3(channel) ||
	    rtw88_native_is_5g_band4(channel));
}

static int
rtw88_native_phy_write_mask32s(struct rtw88_native_softc *sc,
    uint16_t reg, uint32_t mask, uint32_t value)
{
	int error;

	/* RTL8822B C/D and E/F PHY register banks are mirrored by 0x200. */
	error = rtw88_native_phy_write_mask32(sc, reg, mask, value);
	if (error != 0)
		return (error);
	return (rtw88_native_phy_write_mask32(sc, reg + 0x200, mask, value));
}

static int
rtw88_native_phy_rf_write_mask(struct rtw88_native_softc *sc, uint8_t path,
    uint32_t addr, uint32_t mask, uint32_t value)
{
	uint32_t old, shift;

	if (mask == 0)
		return (EINVAL);
	if (rtw88_native_phy_rf_read(sc, path, addr, &old) != 0)
		return (EIO);
	shift = __builtin_ctz(mask);
	old = (old & ~mask) | ((value << shift) & mask);
	return (rtw88_native_phy_rf_write(sc, path, addr, old));
}

static int
rtw88_native_phy_set_channel_mac(struct rtw88_native_softc *sc,
    uint8_t channel, uint8_t bandwidth, uint8_t primary)
{
	uint32_t value32;
	uint8_t txsc40, cck;

	txsc40 = 0;
	if (bandwidth == RTW88_NATIVE_BW_80) {
		if (primary == RTW88_NATIVE_SC_20_UPPER ||
		    primary == RTW88_NATIVE_SC_20_UPMOST)
			txsc40 = 9; /* RTW_SC_40_UPPER */
		else
			txsc40 = 10; /* RTW_SC_40_LOWER */
	}
	if (rtw88_native_reg_write8(sc, REG_DATA_SC,
	    ((txsc40 & 0xf) << 4) | (primary & 0xf)) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_read32(sc, REG_WMAC_TRXPTCL_CTL,
	    &value32) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	value32 &= ~BIT_RFMOD;
	if (bandwidth == RTW88_NATIVE_BW_80)
		value32 |= BIT_RFMOD_80M;
	else if (bandwidth == RTW88_NATIVE_BW_40)
		value32 |= BIT_RFMOD_40M;
	if (rtw88_native_reg_write32(sc, REG_WMAC_TRXPTCL_CTL, value32) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	/* RTL8822B uses the 80 MHz MAC clock even when RF bandwidth is narrower. */
	if (rtw88_native_reg_read32(sc, REG_AFE_CTRL1,
	    &value32) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	value32 &= ~BIT_MAC_CLK_SEL;
	if (rtw88_native_reg_write32(sc, REG_AFE_CTRL1, value32) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	if (rtw88_native_reg_write8(sc, REG_USTIME_TSF, 80) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	if (rtw88_native_reg_write8(sc, REG_USTIME_EDCA, 80) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_read8(sc, REG_CCK_CHECK, &cck) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	cck &= ~BIT_CHECK_CCK_EN;
	if (rtw88_native_is_5g(channel))
		cck |= BIT_CHECK_CCK_EN;
	if (rtw88_native_reg_write8(sc, REG_CCK_CHECK, cck) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	return (0);
}

static int
rtw88_native_phy_set_channel_rf(struct rtw88_native_softc *sc,
    uint8_t channel, uint8_t bandwidth)
{
	uint32_t rf18, rfbe;
	int error;

	if (rtw88_native_phy_rf_read(sc, 0, 0x18, &rf18) != 0)
		return (EIO);
	rf18 &= ~(RF18_BAND_MASK | RF18_CHANNEL_MASK | RF18_RFSI_MASK |
	    RF18_BW_MASK);
	rf18 |= rtw88_native_is_2g(channel) ? RF18_BAND_2G : RF18_BAND_5G;
	rf18 |= channel;
	if (channel > 144)
		rf18 |= RF18_RFSI_GT_CH144;
	else if (channel >= 80)
		rf18 |= RF18_RFSI_GE_CH80;

	switch (bandwidth) {
	case RTW88_NATIVE_BW_40:
		rf18 |= RF18_BW_40M;
		break;
	case RTW88_NATIVE_BW_80:
		rf18 |= RF18_BW_80M;
		break;
	case RTW88_NATIVE_BW_20:
	case RTW88_NATIVE_BW_5:
	case RTW88_NATIVE_BW_10:
	default:
		rf18 |= RF18_BW_20M;
		break;
	}

	if (rtw88_native_is_2g(channel))
		rfbe = 0;
	else if (rtw88_native_is_5g_band1(channel) ||
	    rtw88_native_is_5g_band2(channel))
		rfbe = rtw88_native_rf_low_band[(channel - 36) >> 1];
	else if (rtw88_native_is_5g_band3(channel))
		rfbe = rtw88_native_rf_middle_band[(channel - 100) >> 1];
	else if (rtw88_native_is_5g_band4(channel))
		rfbe = rtw88_native_rf_high_band[(channel - 149) >> 1];
	else
		return (EINVAL);

	error = rtw88_native_phy_rf_write_mask(sc, 0, 0xbe, RFBE_MASK, rfbe);
	if (error != 0)
		return (error);
	error = rtw88_native_phy_rf_write_mask(sc, 0, 0xdf, 1U << 18,
	    channel == 144 ? 1 : 0);
	if (error != 0)
		return (error);
	error = rtw88_native_phy_rf_write(sc, 0, 0x18, rf18);
	if (error != 0)
		return (error);
	if (sc->rf_path_num > 1) {
		error = rtw88_native_phy_rf_write(sc, 1, 0x18, rf18);
		if (error != 0)
			return (error);
	}

	error = rtw88_native_phy_rf_write_mask(sc, 0, 0xb8, 1U << 19, 0);
	if (error == 0)
		error = rtw88_native_phy_rf_write_mask(sc, 0, 0xb8,
		    1U << 19, 1);
	return (error);
}

static int
rtw88_native_phy_set_channel_bb(struct rtw88_native_softc *sc,
    uint8_t channel, uint8_t bandwidth, uint8_t primary)
{
	uint32_t value32;
	int error;

	if (rtw88_native_is_2g(channel)) {
		error = rtw88_native_phy_write_mask32(sc, REG_RXPSEL,
		    1U << 28, 1);
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_CCK_CHECK,
			    BIT_CHECK_CCK_EN, 0);
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_ENTXCCK,
			    1U << 18, 0);
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_RXCCAMSK,
			    0x0000fc00, 15);
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_ACGG2TBL,
			    0x1f, 0);
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_CLKTRK,
			    0x1ffe0000, 0x96a);
		if (error == 0) {
			error = rtw88_native_reg_write32(sc,
			    channel == 14 ? REG_TXSF2 : REG_TXSF2,
			    channel == 14 ? 0x00006577 : 0x384f6577) ==
			    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
		}
		if (error == 0)
			error = rtw88_native_reg_write16(sc, REG_TXSF6,
			    channel == 14 ? 0 : 0x1525) ==
			    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_RFEINV,
			    0x300, 0x2);
		if (error != 0)
			return (error);
	} else if (rtw88_native_is_5g(channel)) {
		error = rtw88_native_phy_write_mask32(sc, REG_ENTXCCK,
		    1U << 18, 1);
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_CCK_CHECK,
			    BIT_CHECK_CCK_EN, 1);
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_RXPSEL,
			    1U << 28, 0);
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_RXCCAMSK,
			    0x0000fc00, 34);
		if (error == 0) {
			uint32_t agc_band = 0;
			if (rtw88_native_is_5g_band1(channel) ||
			    rtw88_native_is_5g_band2(channel))
				agc_band = 1;
			else if (rtw88_native_is_5g_band3(channel))
				agc_band = 2;
			else if (rtw88_native_is_5g_band4(channel))
				agc_band = 3;
			error = rtw88_native_phy_write_mask32(sc, REG_ACGG2TBL,
			    0x1f, agc_band);
		}
		if (error == 0) {
			uint32_t clk = 0;
			if (rtw88_native_is_5g_band1(channel))
				clk = 0x494;
			else if (rtw88_native_is_5g_band2(channel))
				clk = 0x453;
			else if (channel >= 100 && channel <= 116)
				clk = 0x452;
			else if (channel >= 118 && channel <= 177)
				clk = 0x412;
			if (clk != 0)
				error = rtw88_native_phy_write_mask32(sc, REG_CLKTRK,
				    0x1ffe0000, clk);
		}
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_RFEINV,
			    0x300, 0x1);
		if (error != 0)
			return (error);
	}

	switch (bandwidth) {
	case RTW88_NATIVE_BW_20:
		if (rtw88_native_reg_read32(sc, REG_ADCCLK,
		    &value32) != USB_ERR_NORMAL_COMPLETION)
			return (EIO);
		value32 = (value32 & 0xffcffc00U) | RTW88_NATIVE_BW_20;
		error = rtw88_native_reg_write32(sc, REG_ADCCLK, value32) ==
		    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_ADC160,
			    1U << 30, 1);
		break;
	case RTW88_NATIVE_BW_40:
		if (primary == RTW88_NATIVE_SC_20_UPPER)
			error = rtw88_native_phy_write_mask32(sc, REG_RXSB,
			    BIT_RXSB_20_UPPER, 1);
		else
			error = rtw88_native_phy_write_mask32(sc, REG_RXSB,
			    BIT_RXSB_20_UPPER, 0);
		if (error != 0)
			return (error);
		if (rtw88_native_reg_read32(sc, REG_ADCCLK,
		    &value32) != USB_ERR_NORMAL_COMPLETION)
			return (EIO);
		value32 = (value32 & 0xff3ff300U) |
		    (((primary & 0x0f) << 2) | RTW88_NATIVE_BW_40);
		error = rtw88_native_reg_write32(sc, REG_ADCCLK, value32) ==
		    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_ADC160,
			    1U << 30, 1);
		break;
	case RTW88_NATIVE_BW_80:
		if (rtw88_native_reg_read32(sc, REG_ADCCLK,
		    &value32) != USB_ERR_NORMAL_COMPLETION)
			return (EIO);
		value32 = (value32 & 0xfcefcf00U) |
		    (((primary & 0x0f) << 2) | RTW88_NATIVE_BW_80);
		error = rtw88_native_reg_write32(sc, REG_ADCCLK, value32) ==
		    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_ADC160,
			    1U << 30, 1);
		if (error == 0 &&
		    (sc->efuse.rfe_option == 2 || sc->efuse.rfe_option == 3)) {
			error = rtw88_native_phy_write_mask32(sc, REG_L1PKWT,
			    0x0000f000, 6);
			if (error == 0)
				error = rtw88_native_phy_write_mask32(sc, REG_ADC40,
				    1U << 10, 1);
		}
		break;
	case RTW88_NATIVE_BW_5:
		if (rtw88_native_reg_read32(sc, REG_ADCCLK,
		    &value32) != USB_ERR_NORMAL_COMPLETION)
			return (EIO);
		value32 = (value32 & 0xefeeff00U) |
		    ((1U << 6) | RTW88_NATIVE_BW_20);
		error = rtw88_native_reg_write32(sc, REG_ADCCLK, value32) ==
		    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_ADC160,
			    1U << 30, 0);
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_ADC40,
			    1U << 31, 1);
		break;
	case RTW88_NATIVE_BW_10:
		if (rtw88_native_reg_read32(sc, REG_ADCCLK,
		    &value32) != USB_ERR_NORMAL_COMPLETION)
			return (EIO);
		value32 = (value32 & 0xeffeff00U) |
		    ((1U << 7) | RTW88_NATIVE_BW_20);
		error = rtw88_native_reg_write32(sc, REG_ADCCLK, value32) ==
		    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_ADC160,
			    1U << 30, 0);
		if (error == 0)
			error = rtw88_native_phy_write_mask32(sc, REG_ADC40,
			    1U << 31, 1);
		break;
	default:
		return (EINVAL);
	}
	return (error);
}

static int
rtw88_native_phy_set_channel_rxdfir(struct rtw88_native_softc *sc,
    uint8_t bandwidth)
{
	int error;
	uint32_t acbb0, fir, txdfir;

	if (bandwidth == RTW88_NATIVE_BW_40) {
		acbb0 = 1;
		fir = 0;
		txdfir = 0;
	} else if (bandwidth == RTW88_NATIVE_BW_80) {
		acbb0 = 2;
		fir = 1;
		txdfir = 0;
	} else {
		acbb0 = 2;
		fir = 2;
		txdfir = 1;
	}
	error = rtw88_native_phy_write_mask32(sc, REG_ACBB0,
	    (3U << 28), acbb0);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_ACBBRXFIR,
		    (3U << 28), fir);
	if (error == 0)
		error = rtw88_native_phy_write_mask32(sc, REG_TXDFIR,
		    1U << 31, txdfir);
	return (error);
}

static int
rtw88_native_phy_toggle_igi(struct rtw88_native_softc *sc)
{
	uint8_t igi;
	int error;

	if (rtw88_native_reg_read8(sc, REG_RXIGI_A, &igi) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	error = rtw88_native_reg_write8(sc, REG_RXIGI_A,
	    (uint8_t)((igi - 2) & 0x7f));
	if (error != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	error = rtw88_native_reg_write8(sc, REG_RXIGI_A, igi & 0x7f);
	if (error != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	error = rtw88_native_reg_write8(sc, REG_RXIGI_B,
	    (uint8_t)((igi - 2) & 0x7f));
	if (error != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	error = rtw88_native_reg_write8(sc, REG_RXIGI_B, igi & 0x7f);
	if (error != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	error = rtw88_native_reg_write8(sc, REG_RXPSEL, 0);
	if (error != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	error = rtw88_native_reg_write8(sc, REG_RXPSEL,
	    sc->antenna_rx | (sc->antenna_rx << 4));
	return (error == USB_ERR_NORMAL_COMPLETION ? 0 : EIO);
}

static int
rtw88_native_phy_set_channel_cca(struct rtw88_native_softc *sc,
    uint8_t channel, uint8_t bandwidth)
{
	const struct rtw88_native_cca_table *table;
	uint32_t reg82c, reg830, reg838;
	uint8_t col;
	bool is_ifem, is_efem, ifem_ext;
	int error;

	if (sc->efuse.rfe_option == 2) {
		if (rtw88_native_is_2g(channel)) {
			table = &rtw88_native_cca_ifem;
			is_ifem = true;
			is_efem = false;
		} else {
			table = &rtw88_native_cca_efem;
			is_ifem = false;
			is_efem = true;
		}
		ifem_ext = false;
	} else if (sc->efuse.rfe_option == 3 ||
	    sc->efuse.rfe_option == 5) {
		table = &rtw88_native_cca_ifem_ext;
		is_ifem = true;
		is_efem = false;
		ifem_ext = true;
	} else {
		return (ENOTSUP);
	}

	if (rtw88_native_is_2g(channel))
		col = (sc->antenna_rx == BB_PATH_A ||
		    sc->antenna_rx == BB_PATH_B) ? 0 : 1;
	else
		col = (sc->antenna_rx == BB_PATH_A ||
		    sc->antenna_rx == BB_PATH_B) ? 2 : 3;

	reg82c = table->reg82c[col];
	reg830 = table->reg830[col];
	reg838 = table->reg838[col];
	if (is_ifem &&
	    ((sc->cut_version == 1 && (col == 1 || col == 3) &&
	    bandwidth == RTW88_NATIVE_BW_40) ||
	    (!ifem_ext && col == 3 && bandwidth == RTW88_NATIVE_BW_40) ||
	    (sc->efuse.rfe_option == 5 && col == 3)))
		reg830 = 0x79a0ea28;

	error = rtw88_native_reg_write32(sc, REG_CCASEL, reg82c) ==
	    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
	if (error == 0)
		error = rtw88_native_reg_write32(sc, REG_PDMFTH, reg830) ==
		    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
	if (error == 0)
		error = rtw88_native_reg_write32(sc, REG_CCA2ND, reg838) ==
		    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
	if (error == 0 && is_efem && sc->cut_version != 1)
		error = rtw88_native_reg_write32(sc, REG_L1WT, 0x9194b2b9) ==
		    USB_ERR_NORMAL_COMPLETION ? 0 : EIO;
	if (error == 0 && bandwidth == RTW88_NATIVE_BW_20 &&
	    (rtw88_native_is_5g_band2(channel) ||
	    rtw88_native_is_5g_band3(channel)))
		error = rtw88_native_phy_write_mask32(sc, REG_CCA2ND,
		    0xf0, 0x4);
	return (error);
}

static int
rtw88_native_phy_set_channel_rfe(struct rtw88_native_softc *sc,
    uint8_t channel)
{
	uint32_t trsw;
	int error;

	if (sc->efuse.rfe_option != 2 &&
	    sc->efuse.rfe_option != 3 && sc->efuse.rfe_option != 5)
		return (ENOTSUP);

	if (sc->efuse.rfe_option == 2) {
		if (rtw88_native_is_2g(channel)) {
			error = rtw88_native_phy_write_mask32s(sc, REG_RFESEL0,
			    0x00ffffff, 0x00705770);
			if (error == 0)
				error = rtw88_native_phy_write_mask32s(sc,
				    REG_RFESEL8, 0x0000ff00, 0x57);
			if (error == 0)
				error = rtw88_native_phy_write_mask32s(sc,
				    REG_RFECTL, 1U << 4, 0);
		} else {
			error = rtw88_native_phy_write_mask32s(sc, REG_RFESEL0,
			    0x00ffffff, 0x00177517);
			if (error == 0)
				error = rtw88_native_phy_write_mask32s(sc,
				    REG_RFESEL8, 0x0000ff00, 0x75);
			if (error == 0)
				error = rtw88_native_phy_write_mask32s(sc,
				    REG_RFECTL, 1U << 5, 0);
		}
	} else {
		if (rtw88_native_is_2g(channel)) {
			error = rtw88_native_phy_write_mask32s(sc, REG_RFESEL0,
			    0x00ffffff, 0x00745774);
			if (error == 0)
				error = rtw88_native_phy_write_mask32s(sc,
				    REG_RFESEL8, 0x0000ff00, 0x57);
		} else {
			error = rtw88_native_phy_write_mask32s(sc, REG_RFESEL0,
			    0x00ffffff, 0x00477547);
			if (error == 0)
				error = rtw88_native_phy_write_mask32s(sc,
				    REG_RFESEL8, 0x0000ff00, 0x75);
		}
	}
	if (error != 0)
		return (error);
	error = rtw88_native_phy_write_mask32s(sc, REG_RFEINV,
	    (1U << 11) | (1U << 10) | 0x3f, 0);
	if (error != 0)
		return (error);

	if (rtw88_native_reg_read32(sc, REG_TRSW, &trsw) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	if (sc->antenna_rx == BB_PATH_AB || sc->antenna_tx == BB_PATH_AB)
		trsw = 0xa501;
	else if (sc->antenna_rx == sc->antenna_tx)
		trsw = 0xa500;
	else
		trsw = 0xa005;
	return (rtw88_native_phy_write_mask32s(sc, REG_TRSW, 0x0000ffff, trsw));
}

int
rtw88_native_phy_set_channel(struct rtw88_native_softc *sc,
    uint8_t channel, uint8_t bandwidth, uint8_t primary)
{
	int error;

	if (sc == NULL || !sc->phy_initialized || !sc->powered ||
	    !sc->firmware_running || sc->dying)
		return (EINVAL);
	if (!rtw88_native_is_2g(channel) && !rtw88_native_is_5g(channel))
		return (EINVAL);
	if (bandwidth != RTW88_NATIVE_BW_20 &&
	    bandwidth != RTW88_NATIVE_BW_40 &&
	    bandwidth != RTW88_NATIVE_BW_80 &&
	    bandwidth != RTW88_NATIVE_BW_5 &&
	    bandwidth != RTW88_NATIVE_BW_10)
		return (EINVAL);
	if ((bandwidth == RTW88_NATIVE_BW_40 &&
	    primary != 1 && primary != 2) ||
	    (bandwidth == RTW88_NATIVE_BW_80 &&
	    primary != 1 && primary != 2 && primary != 3 && primary != 4))
		return (EINVAL);
	if ((bandwidth == RTW88_NATIVE_BW_40 ||
	    bandwidth == RTW88_NATIVE_BW_80) && rtw88_native_is_2g(channel) &&
	    channel > 13)
		return (EINVAL);

	/* This hardware has no 160 MHz support in the upstream RTW88 driver. */
	error = rtw88_native_phy_set_channel_bb(sc, channel, bandwidth, primary);
	if (error == 0)
		error = rtw88_native_phy_set_channel_mac(sc, channel, bandwidth,
		    primary);
	if (error == 0)
		error = rtw88_native_phy_set_channel_rf(sc, channel, bandwidth);
	if (error == 0)
		error = rtw88_native_phy_set_channel_rxdfir(sc, bandwidth);
	if (error == 0)
		error = rtw88_native_phy_toggle_igi(sc);
	if (error == 0)
		error = rtw88_native_phy_set_channel_cca(sc, channel, bandwidth);
	if (error == 0)
		error = rtw88_native_phy_set_channel_rfe(sc, channel);
	if (error != 0)
		return (error);

	sc->current_channel = channel;
	sc->current_bandwidth = bandwidth;
	sc->current_primary = primary;
	return (0);
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
