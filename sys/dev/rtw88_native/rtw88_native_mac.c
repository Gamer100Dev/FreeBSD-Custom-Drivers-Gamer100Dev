/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Gamer100Dev
 * All rights reserved.
 *
 * Native RTL8822B USB power sequencing and MAC system configuration.
 * The selected commands are the USB-applicable entries from the vendored
 * RTW88 8822B power-on/off flows, executed through FreeBSD USB register I/O.
 */

#include <sys/param.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>

#include "if_rtw88_native.h"

#define PWR_CMD_WRITE		0x01
#define PWR_CMD_POLLING		0x02
#define PWR_CMD_DELAY		0x03
#define PWR_CMD_END		0x04
#define PWR_DELAY_US		0
#define PWR_DELAY_MS		1

#define PWR_INTF_USB		0x02
#define PWR_INTF_ALL		0x0f
#define PWR_INTF_USB_SDIO	0x03
#define PWR_INTF_USB_PCI	0x06
#define PWR_CUT_ALL		0xff
#define PWR_CUT_C		0x08
#define PWR_POLL_COUNT		1000

#define REG_PAD_CTRL1		0x0064
#define REG_LED_CFG		0x004c
#define REG_GPIO_MUXCFG		0x0040
#define REG_RF_CTRL		0x001f
#define REG_WLRF1		0x00ec
#define REG_SYS_STATUS1		0x00f4
#define REG_CR_EXT		0x1100
#define REG_MCUFW_CTRL		0x0080

#define BIT_PAPE_WLBT_SEL	(1U << 29)
#define BIT_LNAON_WLBT_SEL	(1U << 28)
#define BIT_PAPE_SEL_EN		(1U << 25)
#define BIT_LNAON_SEL_EN	(1U << 26)
#define BIT_WLRFE_4_5_EN	(1U << 2)
#define BIT_FEN_BB_GLB_RST	(1U << 1)
#define BIT_FEN_BB_RSTB		(1U << 0)
#define BIT_RF_SDM_RSTB		(1U << 2)
#define BIT_RF_RSTB		(1U << 1)
#define BIT_RF_EN		(1U << 0)
#define BIT_WLRF1_BBRF_EN	((1U << 24) | (1U << 25) | (1U << 26))
#define BIT_BOOT_FSPI_EN	(1U << 20)
#define BIT_FSPI_EN		(1U << 19)
#define BIT_DDMA_EN		(1U << 8)
#define BIT_WL_PLATFORM_RST32	(1U << 16)

struct rtw88_native_pwr_cmd {
	uint16_t offset;
	uint8_t cut_mask;
	uint8_t intf_mask;
	uint8_t command;
	uint8_t mask;
	uint8_t value;
};

/* Transition: card-disposed -> card-emulation (USB-relevant rows only). */
static const struct rtw88_native_pwr_cmd pwr_carddis_to_emu[] = {
	{0x004a, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0x01, 0x00},
	{0x0005, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x98, 0x00},
	{0xffff, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_END, 0, 0}
};

/* Transition: card-emulation -> active (USB-relevant rows only). */
static const struct rtw88_native_pwr_cmd pwr_emu_to_active[] = {
	{0x0012, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x02, 0x00},
	{0x0012, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x01, 0x01},
	{0x0020, PWR_CUT_ALL, PWR_INTF_USB_SDIO, PWR_CMD_WRITE, 0x01, 0x01},
	{0x0001, PWR_CUT_ALL, PWR_INTF_USB_SDIO, PWR_CMD_DELAY, 0x01, PWR_DELAY_MS},
	{0x0000, PWR_CUT_ALL, PWR_INTF_USB_SDIO, PWR_CMD_WRITE, 0x20, 0x00},
	{0x0005, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x1c, 0x00},
	{0x0006, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_POLLING, 0x02, 0x02},
	{0xff1a, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0xff, 0x00},
	{0x0006, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x01, 0x01},
	{0x0005, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x80, 0x00},
	{0x0005, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x18, 0x00},
	{0x10c3, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0x01, 0x01},
	{0x0005, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x01, 0x01},
	{0x0005, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_POLLING, 0x01, 0x00},
	{0x0020, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x08, 0x08},
	{0x10a8, PWR_CUT_C, PWR_INTF_ALL, PWR_CMD_WRITE, 0xff, 0x00},
	{0x10a9, PWR_CUT_C, PWR_INTF_ALL, PWR_CMD_WRITE, 0xff, 0xef},
	{0x10aa, PWR_CUT_C, PWR_INTF_ALL, PWR_CMD_WRITE, 0xff, 0x0c},
	{0x0029, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0xff, 0xf9},
	{0x0024, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x04, 0x00},
	{0x00af, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x20, 0x20},
	{0xffff, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_END, 0, 0}
};

/* Transition: active -> card-emulation (USB-relevant rows only). */
static const struct rtw88_native_pwr_cmd pwr_active_to_emu[] = {
	{0x0093, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x08, 0x00},
	{0x001f, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0xff, 0x00},
	{0x00ef, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0xff, 0x00},
	{0xff1a, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0xff, 0x30},
	{0x0049, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x02, 0x00},
	{0x0006, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x01, 0x01},
	{0x0002, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x02, 0x00},
	{0x10c3, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0x01, 0x00},
	{0x0005, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x02, 0x02},
	{0x0005, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_POLLING, 0x02, 0x00},
	{0x0020, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x08, 0x00},
	{0x0000, PWR_CUT_ALL, PWR_INTF_USB_SDIO, PWR_CMD_WRITE, 0x20, 0x20},
	{0xffff, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_END, 0, 0}
};

/* Transition: card-emulation -> card-disposed (USB-relevant rows only). */
static const struct rtw88_native_pwr_cmd pwr_emu_to_carddis[] = {
	{0x0007, PWR_CUT_ALL, PWR_INTF_USB_SDIO, PWR_CMD_WRITE, 0xff, 0x20},
	{0x0067, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x20, 0x00},
	{0x004a, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0x01, 0x00},
	{0x0081, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0xc0, 0x00},
	{0x0005, PWR_CUT_ALL, PWR_INTF_USB_SDIO, PWR_CMD_WRITE, 0x18, 0x08},
	{0x0090, PWR_CUT_ALL, PWR_INTF_USB_PCI, PWR_CMD_WRITE, 0x02, 0x00},
	{0xffff, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_END, 0, 0}
};

static int
rtw88_native_pwr_poll(struct rtw88_native_softc *sc,
    const struct rtw88_native_pwr_cmd *cmd)
{
	uint8_t value;
	int i;

	for (i = 0; i < PWR_POLL_COUNT; i++) {
		if (rtw88_native_reg_read8(sc, cmd->offset, &value) !=
		    USB_ERR_NORMAL_COMPLETION)
			return (EIO);
		if ((value & cmd->mask) == (cmd->value & cmd->mask))
			return (0);
		pause("rtw88pwr", MAX(1, hz / 1000));
	}
	device_printf(sc->dev,
	    "power-sequence poll timed out: reg 0x%04x mask 0x%02x value 0x%02x\n",
	    cmd->offset, cmd->mask, cmd->value);
	return (ETIMEDOUT);
}

static int
rtw88_native_pwr_run(struct rtw88_native_softc *sc,
    const struct rtw88_native_pwr_cmd *sequence, uint8_t cut_mask)
{
	const struct rtw88_native_pwr_cmd *cmd;
	uint8_t value;
	int error;

	for (cmd = sequence; cmd->command != PWR_CMD_END; cmd++) {
		if ((cmd->intf_mask & PWR_INTF_USB) == 0 ||
		    (cmd->cut_mask & cut_mask) == 0)
			continue;

		switch (cmd->command) {
		case PWR_CMD_WRITE:
			if (rtw88_native_reg_read8(sc, cmd->offset, &value) !=
			    USB_ERR_NORMAL_COMPLETION)
				return (EIO);
			value &= ~cmd->mask;
			value |= cmd->value & cmd->mask;
			if (rtw88_native_reg_write8(sc, cmd->offset, value) !=
			    USB_ERR_NORMAL_COMPLETION)
				return (EIO);
			break;
		case PWR_CMD_POLLING:
			error = rtw88_native_pwr_poll(sc, cmd);
			if (error != 0)
				return (error);
			break;
		case PWR_CMD_DELAY:
			if (cmd->value == PWR_DELAY_US)
				DELAY(cmd->offset);
			else if (cmd->value == PWR_DELAY_MS)
				pause("rtw88pwr", MAX(1, hz * cmd->offset / 1000));
			else
				return (EINVAL);
			break;
		default:
			return (EINVAL);
		}
	}
	return (0);
}

static int
rtw88_native_mac_pre_system_cfg(struct rtw88_native_softc *sc)
{
	uint32_t value32;
	uint8_t value8;

	if (rtw88_native_reg_write8(sc, 0x001c, 0) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_read32(sc, REG_PAD_CTRL1, &value32) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	value32 |= BIT_PAPE_WLBT_SEL | BIT_LNAON_WLBT_SEL;
	if (rtw88_native_reg_write32(sc, REG_PAD_CTRL1, value32) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_read32(sc, REG_LED_CFG, &value32) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	value32 &= ~(BIT_PAPE_SEL_EN | BIT_LNAON_SEL_EN);
	if (rtw88_native_reg_write32(sc, REG_LED_CFG, value32) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_read32(sc, REG_GPIO_MUXCFG, &value32) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	value32 |= BIT_WLRFE_4_5_EN;
	if (rtw88_native_reg_write32(sc, REG_GPIO_MUXCFG, value32) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_read8(sc, 0x0002, &value8) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	value8 &= ~(BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST);
	if (rtw88_native_reg_write8(sc, 0x0002, value8) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_read8(sc, REG_RF_CTRL, &value8) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	value8 &= ~(BIT_RF_SDM_RSTB | BIT_RF_RSTB | BIT_RF_EN);
	if (rtw88_native_reg_write8(sc, REG_RF_CTRL, value8) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_read32(sc, REG_WLRF1, &value32) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	value32 &= ~BIT_WLRF1_BBRF_EN;
	return (rtw88_native_reg_write32(sc, REG_WLRF1, value32) ==
	    USB_ERR_NORMAL_COMPLETION ? 0 : EIO);
}

static int
rtw88_native_mac_system_cfg(struct rtw88_native_softc *sc)
{
	uint32_t value32;
	uint8_t value8;

	if (rtw88_native_reg_read32(sc, RTW88_NATIVE_REG_CPU_DMEM_CON,
	    &value32) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	value32 |= BIT_WL_PLATFORM_RST32 | BIT_DDMA_EN;
	if (rtw88_native_reg_write32(sc, RTW88_NATIVE_REG_CPU_DMEM_CON,
	    value32) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_read8(sc, RTW88_NATIVE_REG_SYS_FUNC_EN + 1,
	    &value8) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	value8 |= 0xdc; /* RTL8822B chip-info sys_func_en */
	if (rtw88_native_reg_write8(sc, RTW88_NATIVE_REG_SYS_FUNC_EN + 1,
	    value8) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_read8(sc, REG_CR_EXT + 3, &value8) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	value8 = (value8 & 0xf0) | 0x0c;
	if (rtw88_native_reg_write8(sc, REG_CR_EXT + 3, value8) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_read32(sc, RTW88_NATIVE_REG_MCUFW_CTRL,
	    &value32) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	if (value32 & BIT_BOOT_FSPI_EN) {
		if (rtw88_native_reg_write32(sc, RTW88_NATIVE_REG_MCUFW_CTRL,
		    value32 & ~BIT_BOOT_FSPI_EN) != USB_ERR_NORMAL_COMPLETION)
			return (EIO);
		if (rtw88_native_reg_read32(sc, REG_GPIO_MUXCFG, &value32) !=
		    USB_ERR_NORMAL_COMPLETION)
			return (EIO);
		if (rtw88_native_reg_write32(sc, REG_GPIO_MUXCFG,
		    value32 & ~BIT_FSPI_EN) != USB_ERR_NORMAL_COMPLETION)
			return (EIO);
	}
	return (0);
}

static int
rtw88_native_mac_is_powered(struct rtw88_native_softc *sc, int *powered)
{
	uint8_t cr, status;

	if (rtw88_native_reg_read8(sc, RTW88_NATIVE_REG_CR, &cr) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_read8(sc, REG_SYS_STATUS1 + 1, &status) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	*powered = (cr != 0xea && (status & 0x01) == 0);
	return (0);
}

int
rtw88_native_chip_power_off(struct rtw88_native_softc *sc)
{
	uint32_t chip_version;
	uint8_t cut, cut_mask;
	int error;

	if (sc == NULL || !sc->xfers_initialized)
		return (EINVAL);
	if (!sc->powered)
		return (0);
	if (sc->firmware_running) {
		if (rtw88_native_reg_write8(sc, 0x0522, 0xff) != USB_ERR_NORMAL_COMPLETION)
			return (EIO);
		sc->firmware_running = 0;
	}
	if (rtw88_native_reg_read32(sc, RTW88_NATIVE_REG_SYS_CFG1,
	    &chip_version) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	cut = (chip_version >> 12) & 0x0f;
	if (cut > 6)
		return (ENXIO);
	cut_mask = (uint8_t)(1U << (cut + 1));

	error = rtw88_native_pwr_run(sc, pwr_active_to_emu, cut_mask);
	if (error == 0)
		error = rtw88_native_pwr_run(sc, pwr_emu_to_carddis, cut_mask);
	if (error == 0)
		sc->powered = 0;
	return (error);
}

int
rtw88_native_chip_power_on(struct rtw88_native_softc *sc)
{
	uint32_t chip_version;
	uint8_t cut, cut_mask, status;
	int powered, error;

	if (sc == NULL || !sc->xfers_initialized || sc->dying)
		return (EINVAL);

	if (rtw88_native_reg_read32(sc, RTW88_NATIVE_REG_SYS_CFG1,
	    &chip_version) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	cut = (chip_version >> 12) & 0x0f;
	if (cut > 6)
		return (ENXIO);
	cut_mask = (uint8_t)(1U << (cut + 1));
	sc->cut_version = cut;
	sc->rf_path_num = (chip_version & (1U << 27)) ? 2 : 1;
	sc->antenna_tx = sc->rf_path_num > 1 ? 0x03 : 0x01;
	sc->antenna_rx = sc->antenna_tx;

	error = rtw88_native_mac_pre_system_cfg(sc);
	if (error != 0)
		return (error);
	error = rtw88_native_mac_is_powered(sc, &powered);
	if (error != 0)
		return (error);

	if (powered) {
		error = rtw88_native_pwr_run(sc, pwr_active_to_emu, cut_mask);
		if (error == 0)
			error = rtw88_native_pwr_run(sc, pwr_emu_to_carddis, cut_mask);
		if (error != 0)
			return (error);
		/* The source driver's EALREADY recovery repeats pre-system setup. */
		error = rtw88_native_mac_pre_system_cfg(sc);
		if (error != 0)
			return (error);
	}

	error = rtw88_native_pwr_run(sc, pwr_carddis_to_emu, cut_mask);
	if (error != 0)
		return (error);
	error = rtw88_native_pwr_run(sc, pwr_emu_to_active, cut_mask);
	if (error != 0) {
		(void)rtw88_native_pwr_run(sc, pwr_active_to_emu, cut_mask);
		(void)rtw88_native_pwr_run(sc, pwr_emu_to_carddis, cut_mask);
		return (error);
	}
	/* Mark powered before system-config so a later error can roll back. */
	sc->powered = 1;

	if (rtw88_native_reg_read8(sc, REG_SYS_STATUS1 + 1, &status) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	status &= ~0x01;
	if (rtw88_native_reg_write8(sc, REG_SYS_STATUS1 + 1, status) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	error = rtw88_native_mac_system_cfg(sc);
	if (error == 0)
		sc->powered = 1;
	return (error);
}


#define RTW88_NATIVE_TX_PAGE_SIZE_SHIFT	7
#define RTW88_NATIVE_TX_PAGE_SIZE		(1U << RTW88_NATIVE_TX_PAGE_SIZE_SHIFT)
#define RTW88_NATIVE_TXFF_SIZE			262144U
#define RTW88_NATIVE_RXFF_SIZE			24576U
#define RTW88_NATIVE_C2H_PKT_BUF		256U
#define RTW88_NATIVE_RSVD_DRV_PG_NUM		8U
#define RTW88_NATIVE_RSVD_H2C_EXTRAINFO_NUM	24U
#define RTW88_NATIVE_RSVD_H2C_STATICINFO_NUM	8U
#define RTW88_NATIVE_RSVD_H2CQ_NUM		8U
#define RTW88_NATIVE_RSVD_FW_TXBUF_NUM		4U
#define RTW88_NATIVE_USB_TX_AGG_DESC_NUM		3U

#define REG_FIFOPAGE_INFO_1			0x0230
#define REG_FIFOPAGE_INFO_2			0x0234
#define REG_FIFOPAGE_INFO_3			0x0238
#define REG_FIFOPAGE_INFO_4			0x023c
#define REG_FIFOPAGE_INFO_5			0x0240
#define REG_H2C_HEAD				0x0244
#define REG_H2C_TAIL				0x0248
#define REG_H2C_READ_ADDR			0x024c
#define REG_H2C_INFO				0x0254
#define REG_AUTO_LLT_V1			0x0208
#define REG_TXDMA_OFFSET_CHK			0x020c
#define REG_FWHW_TXQ_CTRL			0x0420
#define REG_BCNQ_BDNY_V1			0x0424
#define REG_BCNQ1_BDNY_V1			0x0456
#define REG_H2C_PKT_READADDR			0x10d0
#define REG_H2C_PKT_WRITEADDR			0x10d4

#define REG_SW_AMPDU_BURST_MODE_CTRL		0x04bc
#define REG_AMPDU_MAX_TIME_V1			0x0455
#define REG_TX_HANG_CTRL			0x045e
#define REG_PROT_MODE_CTRL			0x04c8
#define REG_BAR_MODE_CTRL			0x04cc
#define REG_FAST_EDCA_VOVI_SETTING		0x1448
#define REG_FAST_EDCA_BEBK_SETTING		0x144c
#define REG_TIMER0_SRC_SEL			0x05b4
#define REG_TXPAUSE			0x0522
#define REG_SLOT				0x051b
#define REG_PIFS				0x0512
#define REG_SIFS				0x0514
#define REG_EDCA_VO_PARAM			0x0500
#define REG_EDCA_VI_PARAM			0x0504
#define REG_RD_NAV_NXT				0x0544
#define REG_RXTSF_OFFSET_CCK			0x055e
#define REG_BCN_CTRL				0x0550
#define REG_TBTT_PROHIBIT			0x0540
#define REG_DRVERLYINT				0x0558
#define REG_BCNDMATIM				0x0559
#define REG_RXFLTMAP0				0x06a0
#define REG_RXFLTMAP1				0x06a2
#define REG_RXFLTMAP2				0x06a4
#define REG_RCR				0x0608
#define REG_RX_PKT_LIMIT			0x060c
#define REG_RX_DRVINFO_SZ			0x060f
#define REG_TCR				0x0604
#define REG_WMAC_OPTION_FUNCTION		0x07d0

#define BIT_AUTO_INIT_LLT_V1			(1U << 0)
#define BIT_MASK_BLK_DESC_NUM			0xf0
#define BIT_EN_WR_FREE_TAIL			(1U << 4)
#define BIT_PRE_TX_CMD				(1U << 6)
#define BIT_EN_EOF_V1				(1U << 2)
#define BIT_TSFT_SEL_TIMER0			0x70
#define BIT_APP_PHYSTS				(1U << 28)
#define BIT_CBSSID_BCN			(1U << 7)

#define RTW88_NATIVE_DMA_EXTRA		0
#define RTW88_NATIVE_DMA_LOW		1
#define RTW88_NATIVE_DMA_NORMAL		2
#define RTW88_NATIVE_DMA_HIGH		3

#define BIT_RXDMA_AGG_EN		(1U << 2)
#define BIT_DMA_MODE			(1U << 1)
#define BIT_DMA_BURST_CNT		(3U << 2)
#define BIT_DMA_BURST_SIZE		(3U << 4)
#define BIT_DMA_BURST_SIZE_64		2U
#define BIT_DMA_BURST_SIZE_512		1U
#define BIT_DMA_BURST_SIZE_1024		0U
#define BIT_RXDMA_AGG_PG_TH_RESERVED	(1U << 7)

static int rtw88_native_write_mask8(struct rtw88_native_softc *, uint16_t,
    uint8_t, uint8_t);

/*
 * Configure the RTL8822B USB receive DMA engine before posting bulk-IN.
 * Values follow the upstream RTW88 USB interface configuration and v1 RX
 * aggregation path. This adapter enumerates at USB 2.0 High Speed (512-byte
 * max packet); the 512-byte RX burst encoding is therefore selected.
 */
static int
rtw88_native_usb_rx_dma_init(struct rtw88_native_softc *sc)
{
	enum usb_dev_speed speed;
	uint8_t rxdma, burst_size;
	uint16_t agg;

	speed = usbd_get_speed(sc->udev);
	rxdma = BIT_DMA_BURST_CNT | BIT_DMA_MODE;
	switch (speed) {
	case USB_SPEED_SUPER:
		burst_size = BIT_DMA_BURST_SIZE_1024;
		break;
	case USB_SPEED_HIGH:
		burst_size = BIT_DMA_BURST_SIZE_512;
		break;
	case USB_SPEED_FULL:
	case USB_SPEED_LOW:
		burst_size = BIT_DMA_BURST_SIZE_64;
		break;
	default:
		device_printf(sc->dev, "unsupported USB speed %d for RXDMA\n",
		    speed);
		return (ENOTSUP);
	}
	rxdma |= (uint8_t)(burst_size << 4);
	if (rtw88_native_reg_write8(sc, RTW88_NATIVE_REG_RXDMA_MODE,
	    rxdma) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_write_mask8(sc, RTW88_NATIVE_REG_TXDMA_PQ_MAP,
	    BIT_RXDMA_AGG_EN, BIT_RXDMA_AGG_EN) != 0 ||
	    rtw88_native_write_mask8(sc,
	    RTW88_NATIVE_REG_RXDMA_AGG_PG_TH + 3,
	    BIT_RXDMA_AGG_PG_TH_RESERVED, 0) != 0)
		return (EIO);

	/* RTL8822B USB RX aggregation: page threshold 5, timeout 0x20. */
	agg = 0x0005U | (0x0020U << 8);
	if (rtw88_native_reg_write16(sc,
	    RTW88_NATIVE_REG_RXDMA_AGG_PG_TH, agg) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	device_printf(sc->dev,
	    "USB RXDMA configured: speed=%d mode=0x%02x aggregation=0x%04x\n",
	    speed, rxdma, agg);
	return (0);
}

static int
rtw88_native_write_mask8(struct rtw88_native_softc *sc, uint16_t reg,
    uint8_t mask, uint8_t value)
{
	uint8_t old;

	if (rtw88_native_reg_read8(sc, reg, &old) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	old = (old & ~mask) | (value & mask);
	return (rtw88_native_reg_write8(sc, reg, old) ==
	    USB_ERR_NORMAL_COMPLETION ? 0 : EIO);
}

static int
rtw88_native_poll8(struct rtw88_native_softc *sc, uint16_t reg,
    uint8_t mask, uint8_t expected)
{
	uint8_t value;
	int i;

	for (i = 0; i < 1000; i++) {
		if (rtw88_native_reg_read8(sc, reg, &value) !=
		    USB_ERR_NORMAL_COMPLETION)
			return (EIO);
		if ((value & mask) == expected)
			return (0);
		pause("rtw88llt", MAX(1, hz / 1000));
	}
	return (ETIMEDOUT);
}

/*
 * RTL8822B's 3-OUT-endpoint configuration uses page-table row 3 and the
 * corresponding USB DMA queue map from the upstream chipset definition.
 */
static int
rtw88_native_txdma_queue_mapping(struct rtw88_native_softc *sc)
{
	uint16_t map;

	/* VO=NORMAL, VI=NORMAL, BE=LOW, BK=LOW, MG=HIGH, HI=HIGH. */
	map = (RTW88_NATIVE_DMA_NORMAL << 4) |
	    (RTW88_NATIVE_DMA_NORMAL << 6) |
	    (RTW88_NATIVE_DMA_LOW << 8) |
	    (RTW88_NATIVE_DMA_LOW << 10) |
	    (RTW88_NATIVE_DMA_HIGH << 12) |
	    (RTW88_NATIVE_DMA_HIGH << 14);

	if (rtw88_native_reg_write16(sc, RTW88_NATIVE_REG_TXDMA_PQ_MAP,
	    map) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, RTW88_NATIVE_REG_CR, 0) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, RTW88_NATIVE_REG_CR, 0xff) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	/* 3081 MCU H2C queue full indication and USB RX buffer arbiter. */
	if (rtw88_native_reg_write32(sc, RTW88_NATIVE_REG_H2CQ_CSR,
	    RTW88_NATIVE_BIT_H2CQ_FULL) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	return (rtw88_native_write_mask8(sc,
	    RTW88_NATIVE_REG_TXDMA_PQ_MAP, 0x01, 0x01));
}

static int
rtw88_native_fifo_setup(struct rtw88_native_softc *sc)
{
	const uint16_t hq = 64, nq = 64, lq = 64, exq = 0, gapq = 1;
	uint32_t txff_pg_num, rsvd_pg_num, acq_pg_num, pubq_num;
	uint32_t cur_pg_addr;

	if (sc->bulkout_num != 3)
		return (ENOTSUP);

	txff_pg_num = RTW88_NATIVE_TXFF_SIZE / RTW88_NATIVE_TX_PAGE_SIZE;
	rsvd_pg_num = RTW88_NATIVE_RSVD_DRV_PG_NUM +
	    RTW88_NATIVE_RSVD_H2C_EXTRAINFO_NUM +
	    RTW88_NATIVE_RSVD_H2C_STATICINFO_NUM +
	    RTW88_NATIVE_RSVD_H2CQ_NUM +
	    RTW88_NATIVE_RSVD_FW_TXBUF_NUM;
	if (rsvd_pg_num > txff_pg_num)
		return (ENOMEM);

	acq_pg_num = txff_pg_num - rsvd_pg_num;
	pubq_num = acq_pg_num - hq - nq - lq - exq - gapq;
	sc->txff_pg_num = (uint16_t)txff_pg_num;
	sc->rsvd_pg_num = (uint16_t)rsvd_pg_num;
	sc->rsvd_boundary = (uint16_t)acq_pg_num;

	cur_pg_addr = txff_pg_num;
	cur_pg_addr -= RTW88_NATIVE_RSVD_FW_TXBUF_NUM;
	sc->rsvd_fw_txbuf_addr = (uint16_t)cur_pg_addr;
	cur_pg_addr -= RTW88_NATIVE_RSVD_H2CQ_NUM;
	sc->rsvd_h2cq_addr = (uint16_t)cur_pg_addr;
	cur_pg_addr -= RTW88_NATIVE_RSVD_H2C_STATICINFO_NUM;
	sc->rsvd_h2c_sta_info_addr = (uint16_t)cur_pg_addr;
	cur_pg_addr -= RTW88_NATIVE_RSVD_H2C_EXTRAINFO_NUM;
	sc->rsvd_h2c_info_addr = (uint16_t)cur_pg_addr;
	cur_pg_addr -= RTW88_NATIVE_RSVD_DRV_PG_NUM;
	sc->rsvd_drv_addr = (uint16_t)cur_pg_addr;
	if (cur_pg_addr != acq_pg_num)
		return (EINVAL);

	if (rtw88_native_reg_write16(sc, REG_FIFOPAGE_INFO_1, hq) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write16(sc, REG_FIFOPAGE_INFO_2, lq) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write16(sc, REG_FIFOPAGE_INFO_3, nq) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write16(sc, REG_FIFOPAGE_INFO_4, exq) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write16(sc, REG_FIFOPAGE_INFO_5, pubq_num) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_read32(sc, RTW88_NATIVE_REG_RQPN_CTRL_2,
	    &txff_pg_num) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, RTW88_NATIVE_REG_RQPN_CTRL_2,
	    txff_pg_num | RTW88_NATIVE_BIT_LD_RQPN) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write16(sc, RTW88_NATIVE_REG_FIFOPAGE_CTRL_2,
	    sc->rsvd_boundary) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_write_mask8(sc, REG_FWHW_TXQ_CTRL + 2,
	    BIT_EN_WR_FREE_TAIL, BIT_EN_WR_FREE_TAIL) != 0 ||
	    rtw88_native_reg_write16(sc, REG_BCNQ_BDNY_V1,
	    sc->rsvd_boundary) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write16(sc, RTW88_NATIVE_REG_FIFOPAGE_CTRL_2 + 2,
	    sc->rsvd_boundary) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write16(sc, REG_BCNQ1_BDNY_V1,
	    sc->rsvd_boundary) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, RTW88_NATIVE_REG_RXFF_BNDY,
	    RTW88_NATIVE_RXFF_SIZE - RTW88_NATIVE_C2H_PKT_BUF - 1) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_write_mask8(sc, REG_AUTO_LLT_V1,
	    BIT_MASK_BLK_DESC_NUM,
	    RTW88_NATIVE_USB_TX_AGG_DESC_NUM << 4) != 0 ||
	    rtw88_native_reg_write8(sc, REG_AUTO_LLT_V1 + 3,
	    RTW88_NATIVE_USB_TX_AGG_DESC_NUM) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_write_mask8(sc, REG_TXDMA_OFFSET_CHK + 1,
	    0x02, 0x02) != 0 ||
	    rtw88_native_write_mask8(sc, REG_AUTO_LLT_V1,
	    BIT_AUTO_INIT_LLT_V1, BIT_AUTO_INIT_LLT_V1) != 0)
		return (EIO);

	if (rtw88_native_poll8(sc, REG_AUTO_LLT_V1,
	    BIT_AUTO_INIT_LLT_V1, 0) != 0)
		return (ETIMEDOUT);

	if (rtw88_native_reg_write8(sc, RTW88_NATIVE_REG_CR + 3, 0) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	/* Map all RTW88 TX queue-select classes to the three physical OUT pipes. */
	memset(sc->qsel_to_ep, 0xff, sizeof(sc->qsel_to_ep));
	sc->qsel_to_ep[0] = 2; /* BE -> LOW */
	sc->qsel_to_ep[1] = 2; /* BK -> LOW */
	sc->qsel_to_ep[2] = 2; /* BK -> LOW */
	sc->qsel_to_ep[3] = 2; /* BE -> LOW */
	sc->qsel_to_ep[4] = 1; /* VI -> NORMAL */
	sc->qsel_to_ep[5] = 1; /* VI -> NORMAL */
	sc->qsel_to_ep[6] = 1; /* VO -> NORMAL */
	sc->qsel_to_ep[7] = 1; /* VO -> NORMAL */
	sc->qsel_to_ep[16] = 0; /* BEACON -> HIGH */
	sc->qsel_to_ep[17] = 0; /* HIGH -> HIGH */
	sc->qsel_to_ep[18] = 0; /* MGMT -> HIGH */
	sc->qsel_to_ep[19] = 0; /* H2C -> HIGH */

	return (0);
}

static int
rtw88_native_h2c_init(struct rtw88_native_softc *sc)
{
	uint32_t addr, size, free_size, wp, rp, value32;
	uint8_t value8;

	addr = (uint32_t)sc->rsvd_h2cq_addr << RTW88_NATIVE_TX_PAGE_SIZE_SHIFT;
	size = RTW88_NATIVE_RSVD_H2CQ_NUM << RTW88_NATIVE_TX_PAGE_SIZE_SHIFT;

	if (rtw88_native_reg_read32(sc, REG_H2C_HEAD, &value32) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, REG_H2C_HEAD,
	    (value32 & 0xfffc0000U) | addr) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_read32(sc, REG_H2C_READ_ADDR, &value32) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, REG_H2C_READ_ADDR,
	    (value32 & 0xfffc0000U) | addr) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_read32(sc, REG_H2C_TAIL, &value32) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, REG_H2C_TAIL,
	    (value32 & 0xfffc0000U) | (addr + size)) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_read8(sc, REG_H2C_INFO, &value8) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	value8 = (value8 & 0xfc) | 0x01;
	if (rtw88_native_reg_write8(sc, REG_H2C_INFO, value8) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_read8(sc, REG_H2C_INFO, &value8) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	value8 = (value8 & 0xfb) | 0x04;
	if (rtw88_native_reg_write8(sc, REG_H2C_INFO, value8) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_write_mask8(sc, REG_TXDMA_OFFSET_CHK + 1,
	    0x80, 0x80) != 0 ||
	    rtw88_native_reg_read32(sc, REG_H2C_PKT_WRITEADDR, &wp) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_read32(sc, REG_H2C_PKT_READADDR, &rp) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	wp &= 0x3ffff;
	rp &= 0x3ffff;
	free_size = wp >= rp ? size - (wp - rp) : rp - wp;
	if (free_size != size) {
		device_printf(sc->dev,
		    "H2C queue is not empty at init (free=%u size=%u)\n",
		    free_size, size);
		return (EINVAL);
	}
	return (0);
}

static int
rtw88_native_8822b_mac_init(struct rtw88_native_softc *sc)
{
	uint32_t value32;
	uint8_t value8;

	if (rtw88_native_reg_read8(sc, REG_SW_AMPDU_BURST_MODE_CTRL,
	    &value8) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, REG_SW_AMPDU_BURST_MODE_CTRL,
	    value8 & ~BIT_PRE_TX_CMD) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, REG_AMPDU_MAX_TIME_V1, 0x70) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_read8(sc, REG_TX_HANG_CTRL, &value8) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, REG_TX_HANG_CTRL,
	    value8 | BIT_EN_EOF_V1) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, REG_PROT_MODE_CTRL, 0x202008ff) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write16(sc, REG_BAR_MODE_CTRL + 2, 0x0801) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_write8(sc, REG_FAST_EDCA_VOVI_SETTING, 0x06) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, REG_FAST_EDCA_VOVI_SETTING + 2, 0x06) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, REG_FAST_EDCA_BEBK_SETTING, 0x06) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, REG_FAST_EDCA_BEBK_SETTING + 2, 0x06) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_read8(sc, REG_TIMER0_SRC_SEL, &value8) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, REG_TIMER0_SRC_SEL,
	    value8 & ~BIT_TSFT_SEL_TIMER0) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write16(sc, REG_TXPAUSE, 0) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, REG_SLOT, 0x09) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, REG_PIFS, 0x19) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, REG_SIFS, 0x10100e0a) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write16(sc, REG_EDCA_VO_PARAM + 2, 0x0186) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write16(sc, REG_EDCA_VI_PARAM + 2, 0x03bc) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, REG_RD_NAV_NXT, 0x001b0005) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write16(sc, REG_RXTSF_OFFSET_CCK, 0x3030) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_read8(sc, REG_BCN_CTRL, &value8) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, REG_BCN_CTRL,
	    value8 | RTW88_NATIVE_BIT_EN_BCN_FUNCTION) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, REG_TBTT_PROHIBIT, 0x00006404) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, REG_DRVERLYINT, 0x04) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, REG_BCNDMATIM, 0x02) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	/*
	 * Match Linux rtw8822b_mac_init(): initialize all three RX filter
	 * maps. RXFLTMAP1 is the control-frame subtype map (0x0fff).
	 * Normal STA operation also checks the BSSID on beacons; software
	 * scanning temporarily disables this bit to hear other BSSes.
	 */
	if (rtw88_native_reg_write32(sc, REG_RXFLTMAP0, 0x0fffffff) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write16(sc, REG_RXFLTMAP1, 0x0fff) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write16(sc, REG_RXFLTMAP2, 0xffff) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, REG_RCR,
	    0xe400220e | BIT_CBSSID_BCN) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, REG_RX_PKT_LIMIT, 24) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, REG_TCR + 2, 0x30) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, REG_TCR + 1, 0x30) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, REG_WMAC_OPTION_FUNCTION + 8,
	    0xb0810041) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write8(sc, REG_WMAC_OPTION_FUNCTION + 4,
	    0x98) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	/* Attach PHY-status records to RX descriptors for the later RX parser. */
	if (rtw88_native_reg_write8(sc, REG_RX_DRVINFO_SZ, 4) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_read32(sc, REG_RCR, &value32) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, REG_RCR,
	    value32 | BIT_APP_PHYSTS) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_read32(sc, REG_WMAC_OPTION_FUNCTION + 4,
	    &value32) != USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, REG_WMAC_OPTION_FUNCTION + 4,
	    value32 & ~((1U << 8) | (1U << 9))) != USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	return (0);
}

/*
 * Configure DMA routing, the 3-endpoint FIFO page table, USB RXDMA burst/
 * aggregation mode and the 8822B MAC/WMAC timing/filter defaults. net80211
 * attaches only after this hardware-start sequence has fully succeeded.
 */
int
rtw88_native_mac_init(struct rtw88_native_softc *sc)
{
	int error;

	if (sc == NULL || !sc->powered || !sc->firmware_running ||
	    !sc->xfers_initialized || sc->dying)
		return (EINVAL);
	if (sc->mac_initialized)
		return (0);

	error = rtw88_native_txdma_queue_mapping(sc);
	if (error == 0)
		error = rtw88_native_fifo_setup(sc);
	if (error == 0)
		error = rtw88_native_h2c_init(sc);
	if (error == 0)
		error = rtw88_native_8822b_mac_init(sc);
	if (error == 0)
		error = rtw88_native_usb_rx_dma_init(sc);
	if (error != 0)
		return (error);

	sc->mac_initialized = 1;
	return (0);
}

/*
 * Match RTW88's FIF_BCN_PRBRESP_PROMISC filter transition:
 * filter beacons by the programmed BSSID during normal operation, but
 * accept all BSSIDs while the net80211 software scanner is collecting
 * scan results. USB register access sleeps, so serialize it with hw_busy
 * without holding the driver mutex across the transfer.
 */
int
rtw88_native_mac_set_scan_filter(struct rtw88_native_softc *sc, int scanning)
{
	int error;

	if (sc == NULL || (scanning != 0 && scanning != 1))
		return (EINVAL);

	mtx_lock(&sc->mtx);
	if (sc->dying || sc->detaching || !sc->hw_started ||
	    !sc->mac_initialized) {
		mtx_unlock(&sc->mtx);
		return (ENETDOWN);
	}
	if (sc->hw_busy) {
		mtx_unlock(&sc->mtx);
		return (EBUSY);
	}
	sc->hw_busy = 1;
	mtx_unlock(&sc->mtx);

	error = rtw88_native_write_mask8(sc, REG_RCR, BIT_CBSSID_BCN,
	    scanning ? 0 : BIT_CBSSID_BCN);

	mtx_lock(&sc->mtx);
	sc->hw_busy = 0;
	cv_broadcast(&sc->io_drain_cv);
	mtx_unlock(&sc->mtx);

	if (error != 0)
		device_printf(sc->dev,
		    "could not %s beacon BSSID RX filtering: %d\n",
		    scanning ? "disable" : "enable", error);
	return (error);
}

#define REG_PORT0_MACID		0x0610

/*
 * Program the station address into RTL8822B port 0 before entering the
 * station state machine. Linux rtw_ops_add_interface() calls
 * rtw_vif_port_config(PORT_SET_MAC_ADDR), whose port-0 MAC register is
 * 0x0610. BSSID/AID programming alone is not enough: RCR.APM performs
 * physical-address filtering in hardware, so leaving MACID0 unset can
 * admit group traffic while rejecting unicast frames for the station.
 */
int
rtw88_native_mac_set_address(struct rtw88_native_softc *sc,
    const uint8_t addr[IEEE80211_ADDR_LEN])
{
	uint32_t mac_lo, check32;
	uint16_t mac_hi, check16;
	int error;

	if (sc == NULL || addr == NULL || (addr[0] & 0x01) != 0)
		return (EINVAL);

	mtx_lock(&sc->mtx);
	if (sc->dying || sc->detaching || !sc->hw_started ||
	    !sc->mac_initialized || !sc->firmware_running) {
		mtx_unlock(&sc->mtx);
		return (ENETDOWN);
	}
	if (sc->station_mac_programmed &&
	    IEEE80211_ADDR_EQ(sc->station_macaddr, addr)) {
		mtx_unlock(&sc->mtx);
		return (0);
	}
	if (sc->hw_busy) {
		mtx_unlock(&sc->mtx);
		return (EBUSY);
	}
	sc->hw_busy = 1;
	mtx_unlock(&sc->mtx);

	/* Port-0 MAC address is stored in little-endian register order. */
	mac_lo = (uint32_t)addr[0] | ((uint32_t)addr[1] << 8) |
	    ((uint32_t)addr[2] << 16) | ((uint32_t)addr[3] << 24);
	mac_hi = (uint16_t)addr[4] | ((uint16_t)addr[5] << 8);

	error = 0;
	if (rtw88_native_reg_write32(sc, REG_PORT0_MACID, mac_lo) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write16(sc, REG_PORT0_MACID + 4, mac_hi) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_read32(sc, REG_PORT0_MACID, &check32) !=
	    USB_ERR_NORMAL_COMPLETION || check32 != mac_lo ||
	    rtw88_native_reg_read16(sc, REG_PORT0_MACID + 4, &check16) !=
	    USB_ERR_NORMAL_COMPLETION || check16 != mac_hi)
		error = EIO;

	mtx_lock(&sc->mtx);
	if (error == 0) {
		IEEE80211_ADDR_COPY(sc->station_macaddr, addr);
		sc->station_mac_programmed = 1;
	} else {
		sc->station_mac_programmed = 0;
		bzero(sc->station_macaddr, sizeof(sc->station_macaddr));
	}
	sc->hw_busy = 0;
	cv_broadcast(&sc->io_drain_cv);
	mtx_unlock(&sc->mtx);

	if (error != 0)
		device_printf(sc->dev,
		    "failed to program/verify port-0 station MAC %6D: %d\n",
		    addr, ":", error);
	else
		device_printf(sc->dev,
		    "programmed port-0 station MAC %6D\n", addr, ":");
	return (error);
}

void
rtw88_native_mac_stop(struct rtw88_native_softc *sc)
{
	if (sc == NULL || !sc->mac_initialized)
		return;
	mtx_lock(&sc->mtx);
	sc->station_mac_programmed = 0;
	bzero(sc->station_macaddr, sizeof(sc->station_macaddr));
	mtx_unlock(&sc->mtx);
	(void)rtw88_native_reg_write16(sc, REG_TXPAUSE, 0xffff);
	(void)rtw88_native_reg_write8(sc, RTW88_NATIVE_REG_CR, 0);
	sc->phy_initialized = 0;
	sc->mac_initialized = 0;
}

#define REG_C2HEVT			0x01a0
#define C2H_HW_FEATURE_REPORT		0x19
#define C2H_HW_FEATURE_DUMP		0xfd
#define RTW88_NATIVE_HW_FEATURE_LEN	13U
#define RTW88_NATIVE_HW_FEATURE_POLLS	1000

/*
 * Ask the RTL8822B firmware for its hardware capability report before
 * downloading firmware, matching RTW88's efuse-info setup. The report
 * describes the actual board antenna count; SYS_CFG1 only describes the
 * silicon RF type and must not be used as a substitute for board capability.
 */
static int
rtw88_native_hw_feature_request(struct rtw88_native_softc *sc)
{
	return (rtw88_native_reg_write8(sc, REG_C2HEVT,
	    C2H_HW_FEATURE_DUMP) == USB_ERR_NORMAL_COMPLETION ? 0 : EIO);
}

static void
rtw88_native_hw_feature_rf_a_fallback(struct rtw88_native_softc *sc,
    uint8_t event_id, const char *reason)
{
	sc->hw_feature_valid = 0;
	sc->hw_feature_hci = 0;
	sc->hw_feature_bw = 0;
	sc->hw_feature_nss = 1;
	sc->hw_feature_ant_num = 1;
	sc->hw_feature_protocol = 0;
	if (sc->rf_path_num > 1) {
		sc->rf_path_num = 1;
		sc->antenna_tx = 0x01;
		sc->antenna_rx = 0x01;
	}
	(void)rtw88_native_reg_write8(sc, REG_C2HEVT, 0);
	device_printf(sc->dev,
	    "hardware feature report %s (C2H id=0x%02x); conservatively selecting RF-A only (1T1R)\n",
	    reason, event_id);
}

/*
 * Decode the 13-byte RTW88 HW_FEATURE_REPORT. The capability word occupies
 * bytes 4..7; antenna count is bits 21..23 and NSS is bits 19..20. Upstream
 * RTW88 switches to RF-A/1T1R when ant_num is one.
 */
static int
rtw88_native_hw_feature_read(struct rtw88_native_softc *sc)
{
	uint8_t report[RTW88_NATIVE_HW_FEATURE_LEN];
	uint8_t event_id;
	uint32_t caps;
	int i, tries;

	sc->hw_feature_valid = 0;
	event_id = 0;
	for (tries = 0; tries < RTW88_NATIVE_HW_FEATURE_POLLS; tries++) {
		if (rtw88_native_reg_read8(sc, REG_C2HEVT, &event_id) !=
		    USB_ERR_NORMAL_COMPLETION) {
			rtw88_native_hw_feature_rf_a_fallback(sc, event_id,
			    "register read failed");
			return (0);
		}
		if (event_id == C2H_HW_FEATURE_REPORT)
			break;
		pause("rtw88hf", MAX(1, hz / 1000));
	}
	if (tries == RTW88_NATIVE_HW_FEATURE_POLLS) {
		rtw88_native_hw_feature_rf_a_fallback(sc, event_id,
		    "not received");
		return (0);
	}

	for (i = 0; i < RTW88_NATIVE_HW_FEATURE_LEN; i++) {
		if (rtw88_native_reg_read8(sc,
		    REG_C2HEVT + 2 + i, &report[i]) !=
		    USB_ERR_NORMAL_COMPLETION) {
			rtw88_native_hw_feature_rf_a_fallback(sc, event_id,
			    "payload read failed");
			return (0);
		}
	}
	if (rtw88_native_reg_write8(sc, REG_C2HEVT, 0) !=
	    USB_ERR_NORMAL_COMPLETION) {
		device_printf(sc->dev,
		    "could not clear the consumed hardware feature report\n");
	}

	caps = (uint32_t)report[4] |
	    ((uint32_t)report[5] << 8) |
	    ((uint32_t)report[6] << 16) |
	    ((uint32_t)report[7] << 24);
	sc->hw_feature_hci = caps & 0x0fU;
	sc->hw_feature_bw = (caps >> 16) & 0x07U;
	sc->hw_feature_nss = (caps >> 19) & 0x03U;
	sc->hw_feature_ant_num = (caps >> 21) & 0x07U;
	sc->hw_feature_protocol = (caps >> 26) & 0x03U;
	sc->hw_feature_valid = 1;

	if (sc->hw_feature_ant_num == 1 && sc->rf_path_num > 1) {
		sc->rf_path_num = 1;
		sc->antenna_tx = 0x01;
		sc->antenna_rx = 0x01;
	}
	if (sc->hw_feature_nss == 0 ||
	    sc->hw_feature_nss > sc->rf_path_num)
		sc->hw_feature_nss = sc->rf_path_num;
	device_printf(sc->dev,
	    "RTL8822B HW feature: HCI=%u BW=%u NSS=%u antennas=%u protocol=%u; RF paths=%u TX=0x%x RX=0x%x\n",
	    sc->hw_feature_hci, sc->hw_feature_bw, sc->hw_feature_nss,
	    sc->hw_feature_ant_num, sc->hw_feature_protocol, sc->rf_path_num,
	    sc->antenna_tx, sc->antenna_rx);
	return (0);
}

int
rtw88_native_hw_start(struct rtw88_native_softc *sc)
{
	int error, hw_feature_requested;

	if (sc == NULL || sc->dying || !sc->xfers_initialized)
		return (EINVAL);

	error = rtw88_native_chip_power_on(sc);
	if (error != 0)
		return (error);
	error = rtw88_native_firmware_get(sc);
	if (error != 0)
		goto fail;
	hw_feature_requested = (rtw88_native_hw_feature_request(sc) == 0);
	if (!hw_feature_requested)
		device_printf(sc->dev,
		    "could not request RTL8822B HW feature report; will use RF-A only\n");
	error = rtw88_native_firmware_download(sc);
	if (error != 0)
		goto fail;
	error = rtw88_native_efuse_read(sc);
	if (error != 0 && error != EALREADY)
		goto fail;
	if (hw_feature_requested)
		(void)rtw88_native_hw_feature_read(sc);
	else
		rtw88_native_hw_feature_rf_a_fallback(sc, 0,
		    "request write failed");
	error = rtw88_native_mac_init(sc);
	if (error != 0)
		goto fail;
	error = rtw88_native_phy_init(sc);
	if (error != 0)
		goto fail;

	mtx_lock(&sc->mtx);
	sc->rfk_pending = 1;
	sc->rfk_done = 0;
	sc->rfk_error = 0;
	sc->rfk_poll_count = 0;
	sc->rfk_fail_mask = 0;
	sc->rfk_reload = 0;
	mtx_unlock(&sc->mtx);

	/* Firmware requires the General Info and PHYDM Info H2C packets. */
	sc->fw_h2c_seq = 0;
	sc->fw_h2c_mailbox = 0;
	sc->fw_h2c_error = 0;
	error = rtw88_native_firmware_send_initial_h2c(sc);
	if (error != 0)
		goto fail;

	/* Route supported Wi-Fi-only RFE3 boards to the WLAN antenna/grants. */
	error = rtw88_native_coex_wifi_only_init(sc);
	if (error == ENOTSUP) {
		device_printf(sc->dev,
		    "Wi-Fi-only RFE route not applicable to this eFuse/RF topology; leaving board route unchanged\n");
		error = 0;
	} else if (error != 0) {
		/*
		 * Linux RTW88 exposes this coexistence initialisation as a void,
		 * best-effort callback. Keep a partially unavailable LTE/BT
		 * coexistence block from taking down an otherwise initialised WLAN
		 * MAC; the WLAN antenna route is programmed before these optional
		 * coexistence accesses.
		 */
		device_printf(sc->dev,
		    "warning: Wi-Fi-only coexistence setup incomplete (%d); continuing with WLAN radio startup\n",
		    error);
		error = 0;
	}

	/*
	 * Register the net80211 radio before posting receive requests, matching
	 * Linux RTW88's order (rtw_register_hw() before rtw_usb_setup_rx()).
	 * This prevents a first RX completion from being consumed while the
	 * native net80211 input path is still detached.
	 */
	error = rtw88_native_net80211_attach(sc);
	if (error != 0)
		goto fail;
	error = rtw88_native_usb_intr_start(sc);
	if (error != 0 && error != EALREADY)
		goto fail;
	error = rtw88_native_usb_rx_start(sc);
	if (error != 0 && error != EALREADY)
		goto fail;
	mtx_lock(&sc->mtx);
	sc->tx_enabled = 1;
	mtx_unlock(&sc->mtx);

	device_printf(sc->dev,
	    "RTL8822B startup complete; net80211 station/monitor VAPs, probe TX and authentication/association management TX enabled\n");
	return (0);

fail:
	sc->wifi_only_coex_initialized = 0;
	mtx_lock(&sc->mtx);
	sc->rfk_pending = 0;
	sc->scan_igi_valid = 0;
	sc->tx_enabled = 0;
	mtx_unlock(&sc->mtx);
	rtw88_native_usb_rx_stop(sc);
	rtw88_native_usb_intr_stop(sc);
	rtw88_native_net80211_detach(sc);
	rtw88_native_mac_stop(sc);
	(void)rtw88_native_chip_power_off(sc);
	rtw88_native_firmware_put(sc);
	return (error);
}

void
rtw88_native_hw_stop(struct rtw88_native_softc *sc)
{
	struct ieee80211vap *vap;
	struct ieee80211_node *ni;
	struct mbuf *m;
	struct mbufq pending;

	if (sc == NULL)
		return;
	mtx_lock(&sc->mtx);
	sc->tx_enabled = 0;
	mtx_unlock(&sc->mtx);
	/* Stop new USB completions before draining deferred packet work. */
	rtw88_native_usb_rx_stop(sc);
	rtw88_native_usb_intr_stop(sc);
	if (sc->net80211_attached) {
		TAILQ_FOREACH(vap, &sc->ic.ic_vaps, iv_next)
			ieee80211_stop(vap);
		ieee80211_drain(&sc->ic);
	}
	taskqueue_drain(taskqueue_thread, &sc->tx_task);
	taskqueue_drain(taskqueue_thread, &sc->rx_task);
	mbufq_init(&pending, 64);
	mtx_lock(&sc->mtx);
	while ((m = mbufq_dequeue(&sc->tx_queue)) != NULL) {
		if (sc->tx_queue_depth > 0)
			sc->tx_queue_depth--;
		(void)mbufq_enqueue(&pending, m);
	}
	while ((m = mbufq_dequeue(&sc->rx_queue)) != NULL) {
		sc->rx_input_drop_count++;
		m_freem(m);
	}
	mtx_unlock(&sc->mtx);
	while ((m = mbufq_dequeue(&pending)) != NULL) {
		ni = (struct ieee80211_node *)m->m_pkthdr.rcvif;
		m->m_pkthdr.rcvif = NULL;
		mtx_lock(&sc->mtx);
		sc->tx_error_count++;
		sc->tx_last_error = ENETDOWN;
		mtx_unlock(&sc->mtx);
		ieee80211_tx_complete(ni, m, 1);
	}
	rtw88_native_mac_stop(sc);
	(void)rtw88_native_chip_power_off(sc);
	sc->firmware_running = 0;
	sc->current_channel = 0;
	sc->wifi_only_coex_initialized = 0;
	mtx_lock(&sc->mtx);
	sc->rfk_pending = 0;
	sc->scan_igi_valid = 0;
	sc->scan_igi_saved = 0;
	mtx_unlock(&sc->mtx);
	rtw88_native_firmware_put(sc);
}

#define REG_HMETFR			0x01cc
#define REG_HMEBOX0			0x01d0
#define REG_HMEBOX0_EX			0x01f0
#define H2C_CMD_MEDIA_STATUS_RPT	0x01U
#define H2C_CMD_DEFAULT_PORT		0x2cU
#define H2C_MAILBOX_COUNT		4
#define H2C_MAILBOX_POLL_COUNT		30
#define RTW88_NATIVE_STA_PORT		0U
#define RTW88_NATIVE_STA_MACID		0U

#define REG_PORT0_BSSID		0x0618
#define REG_PORT0_AID		0x06a8
#define PORT0_NET_TYPE_MASK	0x00030000U
#define PORT0_NET_TYPE_MGD_LINKED	2U
#define PORT0_AID_MASK		0x000007ffU

/*
 * Notify the RTL8822B firmware that station MACID 0 has joined or left the
 * managed BSS. Upstream RTW88 sends H2C_CMD_MEDIA_STATUS_RPT through the
 * HMEBOX mailbox (fw.c:rtw_fw_media_status_report); programming the MAC
 * BSSID/AID registers alone does not perform this firmware state transition.
 */
static int
rtw88_native_mac_h2c_send(struct rtw88_native_softc *sc,
    uint32_t message, uint32_t extended, const char *command)
{
	uint8_t box, box_state;
	int error, retry;

	box = sc->fw_h2c_mailbox % H2C_MAILBOX_COUNT;
	for (retry = 0; retry < H2C_MAILBOX_POLL_COUNT; retry++) {
		if (rtw88_native_reg_read8(sc, REG_HMETFR, &box_state) !=
		    USB_ERR_NORMAL_COMPLETION) {
			error = EIO;
			goto fail;
		}
		if ((box_state & (1U << box)) == 0)
			break;
		DELAY(100);
	}
	if (retry == H2C_MAILBOX_POLL_COUNT) {
		error = ETIMEDOUT;
		goto fail;
	}

	/* Upstream writes the extended dword first, then the command dword. */
	if (rtw88_native_reg_write32(sc,
	    REG_HMEBOX0_EX + (box * sizeof(uint32_t)), extended) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc,
	    REG_HMEBOX0 + (box * sizeof(uint32_t)), message) !=
	    USB_ERR_NORMAL_COMPLETION) {
		error = EIO;
		goto fail;
	}

	sc->fw_h2c_mailbox = (box + 1) % H2C_MAILBOX_COUNT;
	sc->fw_h2c_mailbox_count++;
	sc->fw_h2c_mailbox_error = 0;
	return (0);

fail:
	sc->fw_h2c_mailbox_error_count++;
	sc->fw_h2c_mailbox_error = error;
	device_printf(sc->dev,
	    "firmware H2C mailbox %s failed (MACID %u mailbox %u): %d\n",
	    command, RTW88_NATIVE_STA_MACID, box, error);
	return (error);
}

/*
 * Keep firmware station state in step with the port-0 BSSID/AID registers.
 * These command fields match fw.h: MEDIA_STATUS_RPT command ID 0x01, operation
 * mode in bit 8 and station MACID in bits 23:16.
 */
static int
rtw88_native_mac_media_status_report(struct rtw88_native_softc *sc,
    int connected)
{
	uint32_t message;

	message = H2C_CMD_MEDIA_STATUS_RPT |
	    (connected ? (1U << 8) : 0) |
	    (RTW88_NATIVE_STA_MACID << 16);
	return (rtw88_native_mac_h2c_send(sc, message, 0,
	    connected ? "MEDIA_STATUS_RPT connect" :
	    "MEDIA_STATUS_RPT disconnect"));
}

/*
 * Match upstream rtw_fw_default_port() for this single-interface driver:
 * port 0 and MACID 0 are the only supported station port.
 */
static int
rtw88_native_mac_default_port_report(struct rtw88_native_softc *sc)
{
	uint32_t message;

	message = H2C_CMD_DEFAULT_PORT |
	    (RTW88_NATIVE_STA_PORT << 8) |
	    (RTW88_NATIVE_STA_MACID << 16);
	return (rtw88_native_mac_h2c_send(sc, message, 0, "DEFAULT_PORT"));
}

/*
 * Construct the legacy-rate portion of the RTW88 RA mask from the
 * negotiated net80211 Supported Rates set. This driver currently advertises
 * 11b/g on 2.4 GHz and 11a on 5 GHz; HT/VHT rate bits are intentionally not
 * enabled until those PHY modes are implemented and advertised.
 *
 * Linux RTW88's RA_INFO H2C uses CCK bits 0..3 and OFDM bits 4..11 in the
 * low 32-bit mask. FreeBSD stores rates in 500-kbit/s units, with BASIC in
 * the high bit, so map by rate value rather than the AP's IE ordering.
 */
static int
rtw88_native_mac_build_legacy_ra(const struct ieee80211_rateset *rates,
    int channel, uint32_t *ra_mask, uint8_t *rate_id)
{
	uint32_t mask;
	uint8_t rate;
	int i, bit;

	if (rates == NULL || ra_mask == NULL || rate_id == NULL ||
	    rates->rs_nrates == 0 || rates->rs_nrates > IEEE80211_RATE_MAXSIZE)
		return (EINVAL);

	mask = 0;
	for (i = 0; i < rates->rs_nrates; i++) {
		rate = rates->rs_rates[i] & IEEE80211_RATE_VAL;
		switch (rate) {
		case 2:   bit = 0; break;  /* 1 Mbps */
		case 4:   bit = 1; break;  /* 2 Mbps */
		case 11:  bit = 2; break;  /* 5.5 Mbps */
		case 22:  bit = 3; break;  /* 11 Mbps */
		case 12:  bit = 4; break;  /* 6 Mbps */
		case 18:  bit = 5; break;  /* 9 Mbps */
		case 24:  bit = 6; break;  /* 12 Mbps */
		case 36:  bit = 7; break;  /* 18 Mbps */
		case 48:  bit = 8; break;  /* 24 Mbps */
		case 72:  bit = 9; break;  /* 36 Mbps */
		case 96:  bit = 10; break; /* 48 Mbps */
		case 108: bit = 11; break; /* 54 Mbps */
		default:
			continue;
		}
		if (channel >= 36 && bit < 4)
			continue;
		mask |= (1U << bit);
	}

	if (mask == 0)
		return (EINVAL);

	if (channel >= 36)
		*rate_id = 7; /* RTW_RATEID_G: 5-GHz OFDM */
	else if ((mask & 0x00000fU) == 0)
		*rate_id = 7; /* OFDM only */
	else if ((mask & 0x000ff0U) == 0)
		*rate_id = 8; /* CCK only */
	else
		*rate_id = 6; /* BG mixed mode */

	*ra_mask = mask;
	return (0);
}

/*
 * Port of Linux rtw_fw_send_ra_info() for the advertised legacy modes.
 * HMEBOX0_EX carries the RA mask (H2C dword 1); HMEBOX0 carries the packed
 * command/rate-id fields (H2C dword 0).
 */
static int
rtw88_native_mac_send_legacy_ra(struct rtw88_native_softc *sc,
    const struct ieee80211_rateset *rates)
{
	uint32_t message, ra_mask;
	uint8_t rate_id;
	int error, channel;

	channel = sc->current_channel;
	if (!((channel >= 1 && channel <= 13) ||
	    (channel >= 36 && channel <= 161)))
		return (ENXIO);
	error = rtw88_native_mac_build_legacy_ra(rates, channel, &ra_mask,
	    &rate_id);
	if (error != 0)
		return (error);

	message = 0x40U | (RTW88_NATIVE_STA_MACID << 8) |
	    ((uint32_t)rate_id << 16) | (1U << 30);
	error = rtw88_native_mac_h2c_send(sc, message, ra_mask, "RA_INFO");
	if (error != 0)
		return (error);

	sc->station_ra_mask = ra_mask;
	sc->station_rate_id = rate_id;
	device_printf(sc->dev,
	    "firmware legacy RA configured: MACID %u rate-id %u mask 0x%08x channel %d\n",
	    RTW88_NATIVE_STA_MACID, rate_id, ra_mask, channel);
	return (0);
}

int
rtw88_native_mac_set_link(struct rtw88_native_softc *sc,
    const uint8_t *bssid, uint16_t aid, int linked,
    const struct ieee80211_rateset *rates)
{
	uint32_t cr, aid_reg, bssid_lo, check32;
	uint16_t bssid_hi, check16;
	int media_error;

	if (sc == NULL || !sc->mac_initialized || !sc->firmware_running)
		return (ENETDOWN);
	if (linked && (bssid == NULL || (bssid[0] & 0x01) != 0 ||
	    aid == 0 || aid > PORT0_AID_MASK))
		return (EINVAL);
	if (rtw88_native_reg_read32(sc, RTW88_NATIVE_REG_CR, &cr) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_read32(sc, REG_PORT0_AID, &aid_reg) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (!linked) {
		/* Disable the managed link, then tell firmware the station left. */
		cr &= ~PORT0_NET_TYPE_MASK;
		if (rtw88_native_reg_write32(sc, RTW88_NATIVE_REG_CR, cr) !=
		    USB_ERR_NORMAL_COMPLETION)
			return (EIO);
		media_error = rtw88_native_mac_media_status_report(sc, 0);
		sc->station_ra_mask = 0;
		sc->station_rate_id = 0;
		aid_reg &= ~PORT0_AID_MASK;
		if (rtw88_native_reg_write32(sc, REG_PORT0_AID, aid_reg) !=
		    USB_ERR_NORMAL_COMPLETION ||
		    rtw88_native_reg_write32(sc, REG_PORT0_BSSID, 0) !=
		    USB_ERR_NORMAL_COMPLETION ||
		    rtw88_native_reg_write16(sc, REG_PORT0_BSSID + 4, 0) !=
		    USB_ERR_NORMAL_COMPLETION)
			return (EIO);
		if (rtw88_native_reg_read32(sc, RTW88_NATIVE_REG_CR, &check32) !=
		    USB_ERR_NORMAL_COMPLETION ||
		    (check32 & PORT0_NET_TYPE_MASK) != 0 ||
		    rtw88_native_reg_read32(sc, REG_PORT0_AID, &check32) !=
		    USB_ERR_NORMAL_COMPLETION || (check32 & PORT0_AID_MASK) != 0 ||
		    rtw88_native_reg_read32(sc, REG_PORT0_BSSID, &check32) !=
		    USB_ERR_NORMAL_COMPLETION || check32 != 0 ||
		    rtw88_native_reg_read16(sc, REG_PORT0_BSSID + 4, &check16) !=
		    USB_ERR_NORMAL_COMPLETION || check16 != 0)
			return (EIO);
		return (media_error);
	}

	/* RTL8822B register payloads are little-endian. */
	bssid_lo = (uint32_t)bssid[0] | ((uint32_t)bssid[1] << 8) |
	    ((uint32_t)bssid[2] << 16) | ((uint32_t)bssid[3] << 24);
	bssid_hi = (uint16_t)bssid[4] | ((uint16_t)bssid[5] << 8);
	if (rtw88_native_reg_write32(sc, REG_PORT0_BSSID, bssid_lo) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write16(sc, REG_PORT0_BSSID + 4, bssid_hi) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	aid_reg = (aid_reg & ~PORT0_AID_MASK) | aid;
	if (rtw88_native_reg_write32(sc, REG_PORT0_AID, aid_reg) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	/* Commit managed-link mode last, after BSSID and AID are valid. */
	cr = (cr & ~PORT0_NET_TYPE_MASK) |
	    (PORT0_NET_TYPE_MGD_LINKED << 16);
	if (rtw88_native_reg_write32(sc, RTW88_NATIVE_REG_CR, cr) !=
	    USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	if (rtw88_native_reg_read32(sc, REG_PORT0_BSSID, &check32) !=
	    USB_ERR_NORMAL_COMPLETION || check32 != bssid_lo ||
	    rtw88_native_reg_read16(sc, REG_PORT0_BSSID + 4, &check16) !=
	    USB_ERR_NORMAL_COMPLETION || check16 != bssid_hi ||
	    rtw88_native_reg_read32(sc, REG_PORT0_AID, &check32) !=
	    USB_ERR_NORMAL_COMPLETION || (check32 & PORT0_AID_MASK) != aid ||
	    rtw88_native_reg_read32(sc, RTW88_NATIVE_REG_CR, &check32) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    (check32 & PORT0_NET_TYPE_MASK) !=
	    (PORT0_NET_TYPE_MGD_LINKED << 16))
		return (EIO);

	/*
	 * Match upstream association handling: report the connected STA first,
	 * then select its firmware default port for MACID 0 / port 0.
	 */
	media_error = rtw88_native_mac_media_status_report(sc, 1);
	if (media_error != 0)
		return (media_error);
	media_error = rtw88_native_mac_default_port_report(sc);
	if (media_error != 0)
		return (media_error);
	return (rtw88_native_mac_send_legacy_ra(sc, rates));
}
