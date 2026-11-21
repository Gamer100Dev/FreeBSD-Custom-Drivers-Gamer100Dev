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

#define RTW88_NATIVE_DMA_EXTRA		0
#define RTW88_NATIVE_DMA_LOW		1
#define RTW88_NATIVE_DMA_NORMAL		2
#define RTW88_NATIVE_DMA_HIGH		3

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

	if (rtw88_native_reg_write32(sc, REG_RXFLTMAP0, 0x0fffffff) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write16(sc, REG_RXFLTMAP2, 0xffff) !=
	    USB_ERR_NORMAL_COMPLETION ||
	    rtw88_native_reg_write32(sc, REG_RCR, 0xe400220e) !=
	    USB_ERR_NORMAL_COMPLETION ||
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
 * Configure DMA routing, the 3-endpoint FIFO page table and the 8822B
 * MAC/WMAC timing/filter defaults. RX DMA is not started and there is no
 * interface/packet path until a later net80211 stage.
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
	if (error != 0)
		return (error);

	sc->mac_initialized = 1;
	return (0);
}

void
rtw88_native_mac_stop(struct rtw88_native_softc *sc)
{
	if (sc == NULL || !sc->mac_initialized)
		return;
	(void)rtw88_native_reg_write16(sc, REG_TXPAUSE, 0xffff);
	(void)rtw88_native_reg_write8(sc, RTW88_NATIVE_REG_CR, 0);
	sc->mac_initialized = 0;
}

int
rtw88_native_hw_start(struct rtw88_native_softc *sc)
{
	int error;

	if (sc == NULL || sc->dying || !sc->xfers_initialized)
		return (EINVAL);

	error = rtw88_native_chip_power_on(sc);
	if (error != 0)
		return (error);
	error = rtw88_native_firmware_get(sc);
	if (error != 0)
		goto fail;
	error = rtw88_native_firmware_download(sc);
	if (error != 0)
		goto fail;
	error = rtw88_native_efuse_read(sc);
	if (error != 0 && error != EALREADY)
		goto fail;
	error = rtw88_native_mac_init(sc);
	if (error != 0)
		goto fail;

	/* Collect raw C2H/event reports after firmware and base MAC setup. */
	error = rtw88_native_usb_intr_start(sc);
	if (error != 0 && error != EALREADY)
		goto fail;

	device_printf(sc->dev,
	    "RTL8822B power, firmware, eFuse, FIFO and base MAC setup completed; interrupt reports enabled, bulk RX/net80211 still disabled\n");
	return (0);

fail:
	rtw88_native_usb_intr_stop(sc);
	rtw88_native_mac_stop(sc);
	(void)rtw88_native_chip_power_off(sc);
	rtw88_native_firmware_put(sc);
	return (error);
}

void
rtw88_native_hw_stop(struct rtw88_native_softc *sc)
{
	if (sc == NULL)
		return;
	rtw88_native_usb_intr_stop(sc);
	rtw88_native_mac_stop(sc);
	(void)rtw88_native_chip_power_off(sc);
	sc->firmware_running = 0;
	rtw88_native_firmware_put(sc);
}
