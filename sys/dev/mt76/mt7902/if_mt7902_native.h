/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Gamer100Dev
 */
#ifndef _IF_MT7902_NATIVE_H_
#define _IF_MT7902_NATIVE_H_

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/condvar.h>
#include <sys/firmware.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/taskqueue.h>
#include <machine/bus.h>

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

#define MT7902_PCI_VENDOR	0x14c3
#define MT7902_PCI_DEVICE	0x7902
#define MT7902_BAR		PCIR_BAR(0)
#define MT7902_REG_BAR_MIN	0x100000
#define MT7902_REG_CHIP_ID	0x70010200
#define MT7902_REG_SW_SYNC	0x70028600
#define MT7902_CHIP_ID		0x7902
#define MT7902_REG_REMAP		0x0fe24c
#define MT7902_REMAP_BAR_BASE		0x40000
#define MT7902_REMAP_MASK		0x0000ffff

#define MT7902_FW_WM	"mediatek/WIFI_RAM_CODE_MT7902_1.bin"
#define MT7902_FW_PATCH	"mediatek/WIFI_MT7902_patch_mcu_1_1_hdr.bin"
#define MT7902_FW_BT	"mediatek/BT_RAM_CODE_MT7902_1_1_hdr.bin"
#define MT7902_FW_MAX_REGIONS	32
#define MT7902_TX_RING_SIZE	256
#define MT7902_RX_RING_SIZE	1536
#define MT7902_MCU_TX_RING_SIZE	256
#define MT7902_FWDL_TX_RING_SIZE	128
#define MT7902_RX_BUF_SIZE	4096
#define MT7902_PAGE_SIZE	2048

#define MT7902_WFDMA0_BASE		0xd4000
#define MT7902_REG_WFDMA_INT_STA	(MT7902_WFDMA0_BASE + 0x200)
#define MT7902_REG_WFDMA_INT_EN	(MT7902_WFDMA0_BASE + 0x204)
#define MT7902_REG_WFDMA_GLO_CFG	(MT7902_WFDMA0_BASE + 0x208)
#define MT7902_REG_WFDMA_RST		(MT7902_WFDMA0_BASE + 0x100)
#define MT7902_REG_WFDMA_RST_DTX_PTR	(MT7902_WFDMA0_BASE + 0x20c)
#define MT7902_REG_WFDMA_INT_DELAY	(MT7902_WFDMA0_BASE + 0x2f0)
#define MT7902_REG_WFDMA_TX_BASE	(MT7902_WFDMA0_BASE + 0x300)
#define MT7902_REG_WFDMA_RX_BASE	(MT7902_WFDMA0_BASE + 0x500)
#define MT7902_REG_WFDMA_TX_RING0_EXT	(MT7902_WFDMA0_BASE + 0x600)
#define MT7902_REG_MCU2HOST_SW_INT_ENA	(MT7902_WFDMA0_BASE + 0x1f4)
#define MT7902_REG_MCU_CMD		(MT7902_WFDMA0_BASE + 0x1f0)
#define MT7902_REG_CONN_ON_MISC		0x7c0600f0
#define MT7902_REG_SWDEF_MODE		0x0041f23c
#define MT7902_REG_DMASHDL_SW_CONTROL	0x7c026004
#define MT7902_REG_WFDMA_DUMMY_CR	0x54000120
#define MT7902_REG_PCIE_MAC_INT_ENABLE	0x00010188
#define MT7902_DMASHDL_BYPASS		(1U << 28)
#define MT7902_WFDMA_NEED_REINIT		(1U << 1)
#define MT7902_REG_TOP_MISC		0x180600f0
#define MT7902_REG_CONN_ON_LPCTL	0x7c060010
#define MT7902_REG_WFSYS_RESET		0x18000140
#define MT7902_WFSYS_RESET_B		(1U << 0)
#define MT7902_WFSYS_INIT_DONE		(1U << 4)
#define MT7902_REG_PCIE_PM		0x00010194
#define MT7902_PCIE_PM_L0S_DIS		(1U << 8)

#define MT7902_INT_RX_DONE_DATA	(1U << 2)
#define MT7902_INT_RX_DONE_WM	(1U << 0)
/* MT7902 has no second (MCU-WA) RX event ring. */
#define MT7902_INT_TX_DONE_MCU	(1U << 27)
#define MT7902_INT_TX_DONE_FWDL	(1U << 26)
#define MT7902_INT_TX_DONE_DATA	(1U << 4)
#define MT7902_INT_MCU_CMD	(1U << 29)
#define MT7902_INT_MASK	(MT7902_INT_RX_DONE_DATA | MT7902_INT_RX_DONE_WM | \
	MT7902_INT_TX_DONE_MCU | MT7902_INT_TX_DONE_FWDL | \
	MT7902_INT_TX_DONE_DATA)

#define MT7902_DMA_DESC_DONE	(1U << 31)
#define MT7902_DMA_DESC_LAST	(1U << 30)
#define MT7902_DMA_LEN_MASK	0x3fffU
#define MT7902_DMA_LEN_SHIFT	16

#define MT7902_MCU_PKT_ID	0xa0
#define MT7902_MCU_CMD_FW_SCATTER	0xee
#define MT7902_MCU_CMD_PATCH_START	0x05
#define MT7902_MCU_CMD_TARGET_ADDR	0x01
#define MT7902_MCU_CMD_PATCH_FINISH	0x07
#define MT7902_MCU_CMD_FW_START	0x02
#define MT7902_MCU_CMD_NIC_POWER	0x04
#define MT7902_MCU_CMD_PATCH_SEM	0x10
#define MT7902_MCU_CE_SET_RX_FILTER	0x0a
#define MT7902_MCU_CMD_WAKE_RX_PCIE	(1U << 0)

#define MT7902_RX_FILTER_ENABLE		(1U << 31)
#define MT7902_RX_FILTER_OTHER_BSS	(1U << 6)
#define MT7902_MCU_CMD_QUERY	0x01
#define MT7902_MCU_SEQ_MAX	15
#define MT7902_MCU_EXT_CID		0xed
#define MT7902_MCU_CE_BIT		(1U << 18)
#define MT7902_MCU_UNI_BIT		(1U << 17)
#define MT7902_MCU_QUERY_BIT		(1U << 16)
#define MT7902_MCU_ID_MASK		0xffU
#define MT7902_MCU_EXT_MASK		0xff00U
#define MT7902_MCU_CE_CMD(_id)	(MT7902_MCU_CE_BIT | (_id))
#define MT7902_MCU_EXT_CMD(_id)	(MT7902_MCU_EXT_CID | ((_id) << 8))
#define MT7902_MCU_EXT_QUERY(_id)	(MT7902_MCU_EXT_CMD(_id) | MT7902_MCU_QUERY_BIT)
#define MT7902_MCU_UNI_CMD(_id)	(MT7902_MCU_UNI_BIT | (_id))
#define MT7902_MCU_UNI_QUERY(_id)	(MT7902_MCU_UNI_CMD(_id) | MT7902_MCU_QUERY_BIT)
#define MT7902_MCU_CE_START_HW_SCAN	0x03
#define MT7902_MCU_CE_CANCEL_HW_SCAN	0x04
#define MT7902_MCU_EXT_EFUSE_ACCESS	0x01
#define MT7902_MCU_UNI_STA_REC_UPDATE	0x03
#define MT7902_MCU_UNI_SCAN_REQ	0x16

struct mt7902_reg_window {
	uint32_t	phys;
	uint32_t	bar;
	uint32_t	size;
};

struct mt7902_dma_desc {
	uint32_t buf0;
	uint32_t control;
	uint32_t buf1;
	uint32_t info;
} __packed __aligned(4);

struct mt7902_dma_buffer {
	void *vaddr;
	bus_addr_t paddr;
	bus_dmamap_t map;
	size_t len;
};

struct mt7902_ring {
	struct mt7902_dma_desc *desc;
	struct mt7902_dma_buffer *buf;
	bus_addr_t desc_paddr;
	bus_dmamap_t desc_map;
	bus_dma_tag_t desc_tag;
	bus_dma_tag_t buf_tag;
	int ndesc;
	int head;
	int tail;
	int queued;
	int hw_qid;
	bus_size_t reg_base;
	bool is_rx;
};

struct mt7902_fw_region {
	const uint8_t *data;
	size_t len;
	uint32_t addr;
	uint32_t feature;
	uint32_t sec;
};

struct mt7902_fw_image {
	const struct firmware *fw;
	struct mt7902_fw_region region[MT7902_FW_MAX_REGIONS];
	unsigned int nregion;
	uint32_t override_addr;
};

struct mt7902_softc {
	device_t		dev;
	struct mtx		mtx;
	struct mtx		reg_mtx;
	struct resource		*mem;
	int			mem_rid;
	struct resource		*irq;
	int			irq_rid;
	int			msi_count;
	void			*irq_cookie;
	bus_dma_tag_t		parent_dmat;
	bus_dma_tag_t		dmat;
	bus_dma_tag_t		ring_dmat;
	uint32_t		chip_id;
	struct ieee80211com	ic;
	struct task		irq_task;
	struct task		rx_task;
	struct task		tx_task;
	struct mtx		irq_mtx;
	struct cv		mcu_cv;
	struct mt7902_ring	tx_data;
	struct mt7902_ring	tx_mcu;
	struct mt7902_ring	tx_fwdl;
	struct mt7902_ring	rx_data;
	struct mt7902_ring	rx_event;
	struct mt7902_fw_image	fw_wm;
	struct mt7902_fw_image	fw_patch;
	struct mbufq		txq;
	struct mbufq		rxq;
	uint32_t		irq_status;
	uint32_t		irq_last_status;
	uint64_t		irq_count;
	uint64_t		rx_event_count;
	uint64_t		rx_data_count;
	uint64_t		rx_drop_count;
	uint64_t		rx_mgmt_count;
	uint64_t		rx_beacon_count;
	uint32_t		rx_last_len;
	uint32_t		rx_last_rxd[4];
	uint8_t		mcu_seq;
	uint8_t		mcu_wait_seq;
	uint8_t		mcu_wait_cmd;
	uint8_t		mcu_response[256];
	size_t		mcu_response_len;
	int			mcu_error;
	int			mcu_waiting;
	int			net80211_attached;
	int			hw_started;
	int			dying;
	int			hw_starting;
	int			rx_running;
	int			station_linked;
	uint16_t		station_aid;
	uint8_t		macaddr[IEEE80211_ADDR_LEN];
	uint8_t		current_channel;
	uint8_t		current_band;
	uint16_t		pci_vendor;
	uint16_t		pci_device;
	bool			scan_active;
	bool			attached;
};

uint32_t mt7902_read(struct mt7902_softc *, bus_size_t);
void mt7902_write(struct mt7902_softc *, bus_size_t, uint32_t);
int mt7902_wait(struct mt7902_softc *, bus_size_t, uint32_t, uint32_t,
	    u_int);
int mt7902_fw_get(struct mt7902_softc *, const char *, struct mt7902_fw_image *);
void mt7902_fw_put(struct mt7902_fw_image *);
int mt7902_fw_validate_wm(struct mt7902_fw_image *);
int mt7902_fw_validate_patch(struct mt7902_fw_image *);
int mt7902_fw_boot(struct mt7902_softc *);
int mt7902_dma_init(struct mt7902_softc *);
void mt7902_dma_stop(struct mt7902_softc *);
int mt7902_irq_setup(struct mt7902_softc *);
void mt7902_irq_teardown(struct mt7902_softc *);
int mt7902_fw_boot(struct mt7902_softc *);
int mt7902_mcu_send(struct mt7902_softc *, uint8_t, uint8_t,
	    const void *, size_t, bool);
int mt7902_mcu_send_cmd(struct mt7902_softc *, uint32_t,
	    const void *, size_t, bool);
int mt7902_mcu_send_response(struct mt7902_softc *, uint32_t,
	    const void *, size_t, void *, size_t *);
int mt7902_mcu_init(struct mt7902_softc *);
int mt7902_mcu_read_mac(struct mt7902_softc *);
int mt7902_mcu_set_rxfilter(struct mt7902_softc *, uint32_t);
void mt7902_irq_task(void *, int);
void mt7902_rx_task(void *, int);
int mt7902_net80211_attach(struct mt7902_softc *);
void mt7902_net80211_detach(struct mt7902_softc *);
int mt7902_sysctl_init(struct mt7902_softc *);
int mt7902_set_channel(struct mt7902_softc *, struct ieee80211_channel *);
void mt7902_tx_task(void *, int);

#endif
