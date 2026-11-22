/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Gamer100Dev
 * All rights reserved.
 */

#ifndef _IF_RTW88_NATIVE_H_
#define _IF_RTW88_NATIVE_H_

#include <sys/types.h>
#include <sys/bus.h>
#include <sys/condvar.h>
#include <sys/firmware.h>
#include <sys/mutex.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/taskqueue.h>

/*
 * ieee80211_var.h expects the base interface, ifmedia, routing and Ethernet
 * declarations to have been included by its consumer. Keep these prerequisites
 * here because the driver softc embeds struct ieee80211com and this header is
 * shared by all of the driver translation units.
 */
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_arp.h>
#include <net/ethernet.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>

#include <net80211/ieee80211_var.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#define RTW88_NATIVE_VENDOR_REALTEK		0x0bda
#define RTW88_NATIVE_PRODUCT_RTL8822BU		0xb812

/* Endpoint numbers reported by the RTL8822BU interface descriptor. */
#define RTW88_NATIVE_USB_EP_BULK_IN		4
#define RTW88_NATIVE_USB_EP_BULK_OUT_HIGH	5
#define RTW88_NATIVE_USB_EP_BULK_OUT_NORMAL	6
#define RTW88_NATIVE_USB_EP_INTERRUPT_IN	7
#define RTW88_NATIVE_USB_EP_BULK_OUT_LOW	8

#define RTW88_NATIVE_USB_REQUEST		0x05
#define RTW88_NATIVE_USB_REQUEST_INDEX		0x0000
#define RTW88_NATIVE_USB_TIMEOUT		1000
#define RTW88_NATIVE_USB_TX_TIMEOUT		5000
#define RTW88_NATIVE_MAX_CONTROL_DATA		254

#define RTW88_NATIVE_REG_SYS_CFG1		0x00f0
#define RTW88_NATIVE_REG_MCUFW_CTRL		0x0080
#define RTW88_NATIVE_REG_USB_SEC			0x04e0

#define RTW88_NATIVE_FW_NAME			"rtw88/rtw8822b_fw.bin"
#define RTW88_NATIVE_FW_HEADER_SIZE		64
#define RTW88_NATIVE_FW_CHECKSUM_SIZE		8
#define RTW88_NATIVE_FW_MEM_EMEM		(1U << 4)

#define RTW88_NATIVE_TX_DESC_SIZE		48
#define RTW88_NATIVE_TX_DESC_CHECKSUM_LEN	32
#define RTW88_NATIVE_TXQ_HIGH			0
#define RTW88_NATIVE_TX_QUEUES			3
#define RTW88_NATIVE_TXQSEL_TID0		0U
#define RTW88_NATIVE_TXQSEL_BEACON		16U
#define RTW88_NATIVE_TXQSEL_HIGH		17U
#define RTW88_NATIVE_TXQSEL_MGMT		18U
#define RTW88_NATIVE_TXQSEL_H2C		19U
#define RTW88_NATIVE_TXDMA_CHUNK		0x1000U
#define RTW88_NATIVE_RX_DESC_SIZE		24U
#define RTW88_NATIVE_RX_C2H_HEADER_SIZE	2U
#define RTW88_NATIVE_RX_C2H_PAYLOAD_MAX	64U
#define RTW88_NATIVE_RX_DESC_W0_PKT_LEN	0x00003fffU
#define RTW88_NATIVE_RX_DESC_W0_DRVINFO	0x000f0000U
#define RTW88_NATIVE_RX_DESC_W0_SHIFT	0x03000000U
#define RTW88_NATIVE_RX_DESC_W0_CRCERR	(1U << 14)
#define RTW88_NATIVE_RX_DESC_W0_ICVERR	(1U << 15)
#define RTW88_NATIVE_RX_DESC_W0_ENC_TYPE	0x00700000U
#define RTW88_NATIVE_RX_DESC_W0_PHYST	(1U << 26)
#define RTW88_NATIVE_RX_DESC_W0_SWDEC	(1U << 27)
#define RTW88_NATIVE_RX_DESC_W2_C2H		(1U << 28)
#define RTW88_NATIVE_RX_FCS_LEN		4U
#define RTW88_NATIVE_RX_MAX_FRAME		11454U
#define RTW88_NATIVE_RX_PHY_STATUS_SIZE	32U
#define RTW88_NATIVE_RX_RATE_MAX		0x54U
#define RTW88_NATIVE_TXPWR_IDX_SIZE		42U


#define RTW88_NATIVE_REG_CR			0x0100
#define RTW88_NATIVE_REG_TXDMA_PQ_MAP		0x010c
#define RTW88_NATIVE_REG_RXFF_BNDY		0x011c
#define RTW88_NATIVE_REG_FIFOPAGE_CTRL_2		0x0204
#define RTW88_NATIVE_REG_RQPN_CTRL_2		0x022c
#define RTW88_NATIVE_REG_FIFOPAGE_INFO_1		0x0230
#define RTW88_NATIVE_REG_TXDMA_STATUS		0x0210
#define RTW88_NATIVE_REG_BCN_CTRL			0x0550
#define RTW88_NATIVE_REG_SYS_FUNC_EN		0x0002
#define RTW88_NATIVE_REG_SYS_CLK_CTRL		0x0008
#define RTW88_NATIVE_REG_RSV_CTRL			0x001c
#define RTW88_NATIVE_REG_CPU_DMEM_CON		0x1080
#define RTW88_NATIVE_REG_DDMA_CH0SA		0x1200
#define RTW88_NATIVE_REG_DDMA_CH0DA		0x1204
#define RTW88_NATIVE_REG_DDMA_CH0CTRL		0x1208
#define RTW88_NATIVE_REG_H2CQ_CSR		0x1330
#define RTW88_NATIVE_REG_RXDMA_AGG_PG_TH	0x0280
#define RTW88_NATIVE_REG_RXDMA_MODE		0x0290

#define RTW88_NATIVE_BIT_ENSWBCN		(1U << 8)
#define RTW88_NATIVE_BIT_DIS_TSF_UDT		(1U << 4)
#define RTW88_NATIVE_BIT_EN_BCN_FUNCTION	(1U << 3)
#define RTW88_NATIVE_BIT_BCN_VALID_V1		(1U << 15)
#define RTW88_NATIVE_BIT_LD_RQPN		(1U << 31)
#define RTW88_NATIVE_BIT_H2CQ_FULL		(1U << 31)
#define RTW88_NATIVE_BIT_HCI_TXDMA_EN		(1U << 0)
#define RTW88_NATIVE_BIT_TXDMA_EN		(1U << 2)
#define RTW88_NATIVE_BIT_BTI_PAGE_OVF		(1U << 2)

#define RTW88_NATIVE_BIT_DDMACH0_OWN		(1U << 31)
#define RTW88_NATIVE_BIT_DDMACH0_CHKSUM_EN	(1U << 29)
#define RTW88_NATIVE_BIT_DDMACH0_CHKSUM_STS	(1U << 27)
#define RTW88_NATIVE_BIT_DDMACH0_RESET_CHKSUM_STS (1U << 25)
#define RTW88_NATIVE_BIT_DDMACH0_CHKSUM_CONT	(1U << 24)
#define RTW88_NATIVE_BIT_DDMACH0_DLEN		0x3ffff

#define RTW88_NATIVE_BIT_FEN_CPUEN		(1U << 2)
#define RTW88_NATIVE_BIT_WLMCU_IOIF		(1U << 0)
#define RTW88_NATIVE_BIT_CPU_CLK_EN		(1U << 6)
#define RTW88_NATIVE_BIT_WL_PLATFORM_RST	(1U << 0)
#define RTW88_NATIVE_BIT_MCUFWDL_EN		(1U << 0)
#define RTW88_NATIVE_BIT_FW_DW_RDY		(1U << 14)
#define RTW88_NATIVE_BIT_CHECKSUM_OK		((1U << 4) | (1U << 6))
#define RTW88_NATIVE_BIT_IMEM_DW_OK		(1U << 3)
#define RTW88_NATIVE_BIT_IMEM_CHKSUM_OK	(1U << 4)
#define RTW88_NATIVE_BIT_DMEM_DW_OK		(1U << 5)
#define RTW88_NATIVE_BIT_DMEM_CHKSUM_OK	(1U << 6)
#define RTW88_NATIVE_FW_READY_MASK		(0xffffU & ~((1U << 12) | (1U << 13)))
#define RTW88_NATIVE_FW_READY			((1U << 15) | (1U << 14) | \
						 (1U << 3) | (1U << 5) | \
						 RTW88_NATIVE_BIT_CHECKSUM_OK)

#define RTW88_NATIVE_OCPBASE_TXBUF		0x18780000U
#define RTW88_NATIVE_OCPBASE_DMEM		0x00200000U

#define RTW88_NATIVE_RX_XFERS		4U
#define RTW88_NATIVE_RX_BUFSIZE		(32U * 1024U)

enum {
	RTW88_NATIVE_BULK_RX0,
	RTW88_NATIVE_BULK_RX1,
	RTW88_NATIVE_BULK_RX2,
	RTW88_NATIVE_BULK_RX3,
	RTW88_NATIVE_BULK_TX0,
	RTW88_NATIVE_BULK_TX1,
	RTW88_NATIVE_BULK_TX2,
	RTW88_NATIVE_INTERRUPT_RX,
	RTW88_NATIVE_N_XFERS
};

/* Compatibility name for code that needs the first RX transfer. */
#define RTW88_NATIVE_BULK_RX		RTW88_NATIVE_BULK_RX0

struct rtw88_native_txdesc_params {
	uint16_t	packet_size;
	uint8_t		descriptor_offset;
	uint8_t		mac_id;
	uint8_t		queue_select;
	uint8_t		rate_id;
	uint8_t		security_type;
	uint8_t		packet_offset;
	uint8_t		ampdu_density;
	uint8_t		hw_ssn_select;
	uint8_t		ampdu_factor;
	uint8_t		data_rate;
	uint8_t		data_rate_fallback_limit;
	uint8_t		rts_rate;
	uint8_t		bandwidth;
	uint8_t		stbc;
	uint16_t	software_define;
	uint16_t	software_sequence;
	uint8_t		dma_aggregate_count;
	uint8_t		tim_offset;
	bool		bmc;
	bool		last_segment;
	bool		disable_qsel_sequence;
	bool		more_data;
	bool		ampdu_enable;
	bool		special_report;
	bool		bluetooth_null;
	bool		use_hardware_rate;
	bool		disable_rate_fallback;
	bool		use_rts;
	bool		nav_use_header;
	bool		data_short_gi;
	bool		data_ldpc;
	bool		rts_short_gi;
	bool		enable_hardware_sequence;
};

struct rtw88_native_fw_layout {
	const uint8_t	*dmem;
	size_t		dmem_size;
	uint32_t	dmem_addr;
	const uint8_t	*imem;
	size_t		imem_size;
	uint32_t	imem_addr;
	const uint8_t	*emem;
	size_t		emem_size;
	uint32_t	emem_addr;
};

struct rtw88_native_efuse {
	uint8_t	macaddr[6];
	uint8_t	usb_mode;
	uint8_t	channel_plan;
	uint8_t	country_code[2];
	uint8_t	rf_board_option;
	uint8_t	rfe_option;
	uint8_t	crystal_cap;
	uint8_t	thermal_meter;
	uint8_t	pa_type;
	uint8_t	lna_type_2g;
	uint8_t	lna_type_5g;
	uint8_t	bt_setting;
	uint8_t	rf_antenna_option;
	/* Packed struct rtw_txpwr_idx records for RF paths A and B. */
	uint8_t	txpwr_idx[2][RTW88_NATIVE_TXPWR_IDX_SIZE];
	bool	macaddr_valid;
};

struct rtw88_native_softc {
	device_t		dev;
	struct ieee80211com	ic;
	struct mbufq		rx_queue;
	struct task		rx_task;
	struct mbufq		tx_queue;
	struct task		tx_task;
	int			net80211_attached;
	int			tx_enabled;
	int			tx_queue_depth;
	uint64_t		tx_raw_count;
	uint64_t		tx_raw_reject_count;
	uint64_t		tx_queued_count;
	uint64_t		tx_complete_count;
	uint64_t		tx_error_count;
	uint64_t		tx_mgmt_count;
	uint64_t		tx_data_count;
	uint64_t		tx_group_count;
	uint64_t		tx_eapol_count;
	uint64_t		tx_probe_req_count;
	uint64_t		tx_auth_req_count;
	uint64_t		tx_assoc_req_count;
	uint64_t		tx_deauth_req_count;
	int			tx_last_error;
	int			tx_last_qsel;
	int			tx_last_endpoint;
	int			tx_last_frame_control;
	int			tx_last_frame_len;
	uint64_t		rx_input_count;
	uint64_t		rx_input_drop_count;
	uint64_t		rx_auth_count;
	uint64_t		rx_auth_to_self_count;
	uint64_t		rx_assoc_resp_count;
	uint64_t		rx_deauth_count;
	uint64_t		rx_disassoc_count;
	uint64_t		rx_probe_resp_count;
	uint64_t		rx_beacon_count;
	uint64_t		rx_eapol_count;
	uint64_t		rx_bad_rate_count;
	uint64_t		rx_bad_drvinfo_count;
	uint64_t		rx_phy_page0_count;
	uint64_t		rx_phy_page1_count;
	uint64_t		rx_phy_unknown_count;
	uint64_t		rx_no_phy_count;
	uint64_t		rx_ht_count;
	uint64_t		rx_vht_count;
	uint64_t		rx_decrypted_count;
	uint64_t		rx_assoc_resp_to_self_count;
	uint64_t		rx_disassoc_to_self_count;
	uint64_t		rx_disassoc_from_bss_count;
	int			rx_last_assoc_status;
	int			rx_last_disassoc_reason;
	int			station_linked;
	uint16_t		station_aid;
	uint8_t		station_bssid[IEEE80211_ADDR_LEN];
	struct usb_device	*udev;
	struct usb_xfer		*xfer[RTW88_NATIVE_N_XFERS];
	struct mtx		mtx;
	struct cv		tx_cv[RTW88_NATIVE_TX_QUEUES];
	struct cv		io_drain_cv;
	uint8_t			iface_index;
	int			mtx_initialized;
	int			tx_cv_initialized;
	int			io_cv_initialized;
	int			dying;
	int			detaching;
	int			hw_busy;
	int			hw_stopping;
	int			hw_started;
	unsigned int		io_users;
	int			xfers_initialized;
	int			tx_busy[RTW88_NATIVE_TX_QUEUES];
	int			tx_done[RTW88_NATIVE_TX_QUEUES];
	usb_error_t		tx_error[RTW88_NATIVE_TX_QUEUES];
	const uint8_t		*tx_data[RTW88_NATIVE_TX_QUEUES];
	uint16_t		tx_len[RTW88_NATIVE_TX_QUEUES];
	const struct firmware	*firmware;
	struct rtw88_native_fw_layout fw_layout;
	struct rtw88_native_efuse efuse;
	uint8_t			cut_version;
	uint8_t			rf_path_num;
	uint8_t			antenna_tx;
	uint8_t			antenna_rx;
	int				hw_feature_valid;
	uint8_t			hw_feature_hci;
	uint8_t			hw_feature_bw;
	uint8_t			hw_feature_nss;
	uint8_t			hw_feature_ant_num;
	uint8_t			hw_feature_protocol;
	uint8_t			bulkout_num;
	uint8_t			qsel_to_ep[20];
	uint16_t		txff_pg_num;
	uint16_t		rsvd_pg_num;
	uint16_t		rsvd_boundary;
	uint16_t		rsvd_drv_addr;
	uint16_t		rsvd_h2c_info_addr;
	uint16_t		rsvd_h2c_sta_info_addr;
	uint16_t		rsvd_h2cq_addr;
	uint16_t		rsvd_fw_txbuf_addr;
	uint16_t		fw_h2c_seq;
	uint64_t		fw_h2c_general_count;
	uint64_t		fw_h2c_phydm_count;
	uint64_t		fw_h2c_iqk_count;
	uint64_t		fw_h2c_error_count;
	int			fw_h2c_error;
	int			wifi_only_coex_initialized;
	int			rfk_pending;
	int			rfk_done;
	uint64_t		rfk_attempt_count;
	uint32_t		rfk_poll_count;
	uint32_t		rfk_fail_mask;
	int			rfk_error;
	int			rfk_reload;
	uint8_t		scan_igi_saved;
	int			scan_igi_valid;
	int			powered;
	int			firmware_running;
	int			mac_initialized;
	int			phy_initialized;
	int			efuse_initialized;
	int			current_channel;
	int			intr_started;
	uint8_t			intr_buf[64];
	uint8_t			last_intr[64];
	uint16_t		last_intr_len;
	uint64_t		intr_count;
	usb_error_t		intr_error;
	int			rx_started;
	uint8_t			rx_buf[RTW88_NATIVE_RX_XFERS][RTW88_NATIVE_RX_BUFSIZE];
	uint8_t			rx_last[32 * 1024];
	uint32_t		rx_last_len;
	uint64_t		rx_count;
	uint64_t		rx_desc_count;
	uint64_t		rx_data_count;
	uint64_t		rx_c2h_count;
	uint64_t		rx_malformed_count;
	uint64_t		rx_submit_count;
	uint64_t		rx_error_count;
	int			rx_error_code;
	int			rx_last_c2h_id;
	int			rx_last_c2h_seq;
	int			rx_last_c2h_len;
	uint8_t		rx_last_c2h_payload[RTW88_NATIVE_RX_C2H_PAYLOAD_MAX];
	usb_error_t		rx_error;
};

int		rtw88_native_txdesc_build(uint8_t *, size_t,
		    const struct rtw88_native_txdesc_params *);

usb_error_t	rtw88_native_usb_setup(struct rtw88_native_softc *);
void		rtw88_native_usb_unsetup(struct rtw88_native_softc *);
int		rtw88_native_usb_intr_start(struct rtw88_native_softc *);
void		rtw88_native_usb_intr_stop(struct rtw88_native_softc *);
int		rtw88_native_usb_rx_start(struct rtw88_native_softc *);
void		rtw88_native_usb_rx_stop(struct rtw88_native_softc *);
usb_error_t	rtw88_native_usb_control(struct rtw88_native_softc *,
		    uint8_t, uint16_t, void *, uint16_t, uint16_t *);
usb_error_t	rtw88_native_usb_tx_sync(struct rtw88_native_softc *, uint8_t,
		    const void *, uint16_t);

usb_error_t	rtw88_native_raw_reg_read32(struct rtw88_native_softc *,
		    uint16_t, uint32_t *);
usb_error_t	rtw88_native_raw_reg_write(struct rtw88_native_softc *,
		    uint16_t, const void *, uint16_t);

usb_error_t	rtw88_native_reg_read8(struct rtw88_native_softc *,
		    uint16_t, uint8_t *);
usb_error_t	rtw88_native_reg_read16(struct rtw88_native_softc *,
		    uint16_t, uint16_t *);
usb_error_t	rtw88_native_reg_read32(struct rtw88_native_softc *,
		    uint16_t, uint32_t *);
usb_error_t	rtw88_native_reg_write8(struct rtw88_native_softc *,
		    uint16_t, uint8_t);
usb_error_t	rtw88_native_reg_write16(struct rtw88_native_softc *,
		    uint16_t, uint16_t);
usb_error_t	rtw88_native_reg_write32(struct rtw88_native_softc *,
		    uint16_t, uint32_t);

int		rtw88_native_firmware_get(struct rtw88_native_softc *);
void		rtw88_native_firmware_put(struct rtw88_native_softc *);
int		rtw88_native_firmware_download(struct rtw88_native_softc *);
int		rtw88_native_firmware_send_initial_h2c(struct rtw88_native_softc *);
int		rtw88_native_firmware_send_iqk(struct rtw88_native_softc *);
int		rtw88_native_efuse_read(struct rtw88_native_softc *);
int		rtw88_native_chip_power_on(struct rtw88_native_softc *);
int		rtw88_native_chip_power_off(struct rtw88_native_softc *);
int		rtw88_native_mac_init(struct rtw88_native_softc *);
int		rtw88_native_mac_set_link(struct rtw88_native_softc *,
		    const uint8_t *, uint16_t, int);
int		rtw88_native_phy_init(struct rtw88_native_softc *);
int		rtw88_native_coex_wifi_only_init(struct rtw88_native_softc *);
int		rtw88_native_phy_scan_gain(struct rtw88_native_softc *, int);
int		rtw88_native_phy_iqk(struct rtw88_native_softc *);
int		rtw88_native_phy_set_channel(struct rtw88_native_softc *,
		    uint8_t);
void		rtw88_native_mac_stop(struct rtw88_native_softc *);
int		rtw88_native_hw_start(struct rtw88_native_softc *);
void		rtw88_native_hw_stop(struct rtw88_native_softc *);
int		rtw88_native_net80211_attach(struct rtw88_native_softc *);
void		rtw88_native_net80211_detach(struct rtw88_native_softc *);
void		rtw88_native_rx_task(void *, int);
void		rtw88_native_tx_task(void *, int);

#endif /* _IF_RTW88_NATIVE_H_ */
