/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Gamer100Dev
 *
 * Native busdma-backed WFDMA rings. Queue numbering and the 16-byte DMA
 * descriptor layout follow the MT7921/MT7902 PCI implementation in mt76.
 */
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/rman.h>
#include <sys/systm.h>
#include <machine/bus.h>
#include <dev/pci/pcivar.h>

#include "if_mt7902_native.h"

#define MT7902_RING_STRIDE	0x10
#define MT7902_TX_RING_REG(q)	(0x54000300 + (q) * MT7902_RING_STRIDE)
#define MT7902_RX_RING_REG(q)	(0x54000500 + (q) * MT7902_RING_STRIDE)
#define MT7902_WFDMA_RX_EN	(1U << 2)
#define MT7902_WFDMA_TX_BUSY	(1U << 1)
#define MT7902_WFDMA_RX_BUSY	(1U << 3)
#define MT7902_WFDMA_TX_EN	(1U << 0)
#define MT7902_WFDMA_TX_WB	(1U << 6)
#define MT7902_WFDMA_RX_WB	(1U << 13)
#define MT7902_WFDMA_DMA_SIZE	(3U << 4)
#define MT7902_WFDMA_FIFO_LE	(1U << 12)
#define MT7902_WFDMA_CLK_GAT_DIS	(1U << 30)
#define MT7902_WFDMA_OMIT_TX_INFO	(1U << 28)
#define MT7902_WFDMA_FIFO_DIS_CHECK	(1U << 11)
#define MT7902_WFDMA_BASE_PTR_CHAIN	(1U << 15)
#define MT7902_WFDMA_OMIT_RX_INFO_PFET2	(1U << 21)
#define MT7902_WFDMA_RST_LOGIC	(1U << 4)
#define MT7902_WFDMA_RST_DMASHDL	(1U << 5)
#define MT7902_PREFETCH(_base, _depth)	(((_base) << 16) | (_depth))

struct mt7902_dma_load_ctx {
	bus_addr_t address;
	int error;
};

static void
mt7902_dma_map_cb(void *arg, bus_dma_segment_t *segs, int nsegs, int error)
{
	struct mt7902_dma_load_ctx *ctx;

	ctx = arg;
	ctx->error = error;
	if (error == 0 && nsegs == 1)
		ctx->address = segs[0].ds_addr;
	else if (error == 0)
		ctx->error = EFBIG;
}

static int
mt7902_dma_mem_alloc(bus_dma_tag_t tag, size_t size, void **vaddr,
	bus_dmamap_t *map, bus_addr_t *paddr)
{
	struct mt7902_dma_load_ctx ctx;
	int error;

	error = bus_dmamem_alloc(tag, vaddr,
	    BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_COHERENT, map);
	if (error != 0)
		return (error);
	ctx.address = 0;
	ctx.error = 0;
	error = bus_dmamap_load(tag, *map, *vaddr, size, mt7902_dma_map_cb,
	    &ctx, BUS_DMA_WAITOK);
	if (error == 0)
		error = ctx.error;
	if (error != 0)
		goto fail_mem;
	if (ctx.address > BUS_SPACE_MAXADDR_32BIT - size) {
		error = EFBIG;
		goto fail_unload;
	}
	*paddr = ctx.address;
	bus_dmamap_sync(tag, *map, BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	return (0);

fail_unload:
	bus_dmamap_unload(tag, *map);
fail_mem:
	bus_dmamem_free(tag, *vaddr, *map);
	*vaddr = NULL;
	*map = NULL;
	return (error);
}

static int
mt7902_ring_alloc(struct mt7902_softc *sc, struct mt7902_ring *ring,
	int ndesc, int qid, bool is_rx, size_t buf_size)
{
	struct mt7902_dma_buffer *buf;
	size_t desc_size;
	uint32_t reg, ctl;
	int i, error;

	bzero(ring, sizeof(*ring));
	ring->ndesc = ndesc;
	ring->hw_qid = qid;
	ring->is_rx = is_rx;
	reg = is_rx ? MT7902_REG_WFDMA_RX_BASE +
	    qid * MT7902_RING_STRIDE : MT7902_REG_WFDMA_TX_BASE +
	    qid * MT7902_RING_STRIDE;
	ring->reg_base = reg;

	error = bus_dma_tag_create(sc->dmat, 16, 0, BUS_SPACE_MAXADDR_32BIT,
	    BUS_SPACE_MAXADDR, NULL, NULL, sizeof(*ring->desc) * ndesc, 1,
	    sizeof(*ring->desc) * ndesc, BUS_DMA_ALLOCNOW, NULL, NULL,
	    &ring->desc_tag);
	if (error != 0)
		goto fail;
	desc_size = sizeof(*ring->desc) * ndesc;
	error = mt7902_dma_mem_alloc(ring->desc_tag, desc_size,
	    (void **)&ring->desc, &ring->desc_map, &ring->desc_paddr);
	if (error != 0)
		goto fail;

	error = bus_dma_tag_create(sc->dmat, 64, 0, BUS_SPACE_MAXADDR_32BIT,
	    BUS_SPACE_MAXADDR, NULL, NULL, buf_size, 1, buf_size,
	    BUS_DMA_ALLOCNOW, NULL, NULL, &ring->buf_tag);
	if (error != 0)
		goto fail;
	ring->buf = malloc(sizeof(*ring->buf) * ndesc, M_DEVBUF,
	    M_WAITOK | M_ZERO);
	for (i = 0; i < ndesc; i++) {
		buf = &ring->buf[i];
		buf->len = buf_size;
		error = mt7902_dma_mem_alloc(ring->buf_tag, buf_size,
		    &buf->vaddr, &buf->map, &buf->paddr);
		if (error != 0)
			goto fail;
		if (is_rx) {
			ring->desc[i].buf0 = htole32((uint32_t)buf->paddr);
			/* RX uses one empty slot to distinguish a full ring. */
			ctl = i == ndesc - 1 ? 0 :
			    (((uint32_t)buf_size & MT7902_DMA_LEN_MASK) <<
			    MT7902_DMA_LEN_SHIFT);
			ring->desc[i].control = htole32(ctl);
		}
	}
	for (i = 0; i < ndesc; i++)
		if (!is_rx)
			ring->desc[i].control = htole32(MT7902_DMA_DESC_DONE);

	bus_dmamap_sync(ring->desc_tag, ring->desc_map,
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	if (is_rx) {
		ring->head = ndesc - 1;
		ring->tail = 0;
		ring->queued = ndesc - 1;
	}
	mt7902_write(sc, reg + 0x0, (uint32_t)ring->desc_paddr);
	mt7902_write(sc, reg + 0x4, ndesc);
	mt7902_write(sc, reg + 0x8, is_rx ? ring->head : 0);
	mt7902_write(sc, reg + 0xc, 0);
	return (0);

fail:
	/* Cleanup is shared with detach and tolerates partially built rings. */
	return (error);
}

static void
mt7902_ring_free(struct mt7902_ring *ring)
{
	int i;

	if (ring->buf != NULL) {
		for (i = 0; i < ring->ndesc; i++) {
			if (ring->buf[i].map == NULL)
				continue;
			bus_dmamap_sync(ring->buf_tag, ring->buf[i].map,
			    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(ring->buf_tag, ring->buf[i].map);
			bus_dmamem_free(ring->buf_tag, ring->buf[i].vaddr,
			    ring->buf[i].map);
		}
		free(ring->buf, M_DEVBUF);
	}
	if (ring->buf_tag != NULL)
		bus_dma_tag_destroy(ring->buf_tag);
	if (ring->desc_map != NULL) {
		bus_dmamap_sync(ring->desc_tag, ring->desc_map,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(ring->desc_tag, ring->desc_map);
		bus_dmamem_free(ring->desc_tag, ring->desc, ring->desc_map);
	}
	if (ring->desc_tag != NULL)
		bus_dma_tag_destroy(ring->desc_tag);
	bzero(ring, sizeof(*ring));
}

int
mt7902_dma_init(struct mt7902_softc *sc)
{
	uint32_t cfg, value;
	int error, i;

	mt7902_write(sc, MT7902_REG_WFDMA_GLO_CFG, 0);
	value = mt7902_read(sc, MT7902_REG_WFDMA_RST);
	value &= ~(MT7902_WFDMA_RST_LOGIC | MT7902_WFDMA_RST_DMASHDL);
	mt7902_write(sc, MT7902_REG_WFDMA_RST, value);
	mt7902_write(sc, MT7902_REG_WFDMA_RST,
	    value | MT7902_WFDMA_RST_LOGIC | MT7902_WFDMA_RST_DMASHDL);
	for (i = 0; i < MT7902_TX_DATA_QUEUES; i++) {
		error = mt7902_ring_alloc(sc, &sc->tx_data[i],
		    MT7902_TX_RING_SIZE, i, false, 4096);
		if (error != 0)
			goto fail;
	}
	if ((error = mt7902_ring_alloc(sc, &sc->tx_mcu, MT7902_MCU_TX_RING_SIZE,
	    15, false, 4096)) != 0 ||
	    (error = mt7902_ring_alloc(sc, &sc->tx_fwdl, MT7902_FWDL_TX_RING_SIZE,
	    16, false, 4096)) != 0 ||
	    (error = mt7902_ring_alloc(sc, &sc->rx_event, 512, 0, true,
	    MT7902_RX_BUF_SIZE)) != 0 ||
	    (error = mt7902_ring_alloc(sc, &sc->rx_data, MT7902_RX_RING_SIZE,
	    2, true, MT7902_RX_BUF_SIZE)) != 0)
		goto fail;
	/* MT7902 uses distinct prefetch windows for RX0-3 and TX0-6,15,16. */
	mt7902_write(sc, MT7902_REG_WFDMA_RX_BASE + 0x180,
	    MT7902_PREFETCH(0x0000, 0x4));
	mt7902_write(sc, MT7902_REG_WFDMA_RX_BASE + 0x184,
	    MT7902_PREFETCH(0x0040, 0x4));
	mt7902_write(sc, MT7902_REG_WFDMA_RX_BASE + 0x188,
	    MT7902_PREFETCH(0x0080, 0x4));
	mt7902_write(sc, MT7902_REG_WFDMA_RX_BASE + 0x18c,
	    MT7902_PREFETCH(0x00c0, 0x4));
	mt7902_write(sc, MT7902_REG_WFDMA_TX_RING0_EXT,
	    MT7902_PREFETCH(0x0100, 0x4));
	mt7902_write(sc, MT7902_REG_WFDMA_TX_RING0_EXT + 0x4,
	    MT7902_PREFETCH(0x0140, 0x4));
	mt7902_write(sc, MT7902_REG_WFDMA_TX_RING0_EXT + 0x8,
	    MT7902_PREFETCH(0x0180, 0x4));
	mt7902_write(sc, MT7902_REG_WFDMA_TX_RING0_EXT + 0xc,
	    MT7902_PREFETCH(0x01c0, 0x4));
	mt7902_write(sc, MT7902_REG_WFDMA_TX_RING0_EXT + 0x10,
	    MT7902_PREFETCH(0x0200, 0x4));
	mt7902_write(sc, MT7902_REG_WFDMA_TX_RING0_EXT + 0x14,
	    MT7902_PREFETCH(0x0240, 0x4));
	mt7902_write(sc, MT7902_REG_WFDMA_TX_RING0_EXT + 0x18,
	    MT7902_PREFETCH(0x0280, 0x4));
	mt7902_write(sc, MT7902_REG_WFDMA_TX_RING0_EXT + 0x3c,
	    MT7902_PREFETCH(0x02c0, 0x4));
	mt7902_write(sc, MT7902_REG_WFDMA_TX_RING0_EXT + 0x40,
	    MT7902_PREFETCH(0x0300, 0x4));
	mt7902_write(sc, MT7902_REG_WFDMA_RST_DTX_PTR, 0xffffffff);
	mt7902_write(sc, MT7902_REG_WFDMA_INT_DELAY, 0);
	cfg = MT7902_WFDMA_DMA_SIZE | MT7902_WFDMA_TX_WB |
	    MT7902_WFDMA_RX_WB | MT7902_WFDMA_FIFO_LE |
	    MT7902_WFDMA_CLK_GAT_DIS | MT7902_WFDMA_OMIT_TX_INFO |
	    MT7902_WFDMA_FIFO_DIS_CHECK | MT7902_WFDMA_BASE_PTR_CHAIN |
	    MT7902_WFDMA_OMIT_RX_INFO_PFET2;
	mt7902_write(sc, MT7902_REG_WFDMA_GLO_CFG,
	    cfg | MT7902_WFDMA_TX_EN | MT7902_WFDMA_RX_EN);
	error = mt7902_wait(sc, MT7902_REG_WFDMA_GLO_CFG,
	    MT7902_WFDMA_TX_EN | MT7902_WFDMA_RX_EN,
	    MT7902_WFDMA_TX_EN | MT7902_WFDMA_RX_EN, 100000);
	if (error == 0) {
		/* mt792x_dma_disable() leaves DMASHDL bypassed for host queues. */
		mt7902_write(sc, MT7902_REG_DMASHDL_SW_CONTROL,
		    mt7902_read(sc, MT7902_REG_DMASHDL_SW_CONTROL) |
		    MT7902_DMASHDL_BYPASS);
		mt7902_write(sc, MT7902_REG_WFDMA_DUMMY_CR,
		    mt7902_read(sc, MT7902_REG_WFDMA_DUMMY_CR) |
		    MT7902_WFDMA_NEED_REINIT);
		/* MT76 enables the MCU command interrupt and PCIe RX wake source. */
		mt7902_write(sc, MT7902_REG_MCU2HOST_SW_INT_ENA, 1U);
		mt7902_write(sc, MT7902_REG_WFDMA_INT_EN,
		    MT7902_INT_MASK | MT7902_INT_MCU_CMD);
		device_printf(sc->dev,
		    "WFDMA ready GLO=%08x INT_EN=%08x RX0=%08x/%08x/%08x/%08x TX15=%08x/%08x/%08x/%08x RX0desc=%08x RX0dma=%08x TX15dma=%08x\n",
		    mt7902_read(sc, MT7902_REG_WFDMA_GLO_CFG),
		    mt7902_read(sc, MT7902_REG_WFDMA_INT_EN),
		    mt7902_read(sc, sc->rx_event.reg_base),
		    mt7902_read(sc, sc->rx_event.reg_base + 4),
		    mt7902_read(sc, sc->rx_event.reg_base + 8),
		    mt7902_read(sc, sc->rx_event.reg_base + 12),
		    mt7902_read(sc, sc->tx_mcu.reg_base),
		    mt7902_read(sc, sc->tx_mcu.reg_base + 4),
		    mt7902_read(sc, sc->tx_mcu.reg_base + 8),
		    mt7902_read(sc, sc->tx_mcu.reg_base + 12),
		    le32toh(sc->rx_event.desc[0].control),
		    (uint32_t)sc->rx_event.buf[0].paddr,
		    (uint32_t)sc->tx_mcu.buf[0].paddr);
	}
	return (error);

fail:
	mt7902_dma_stop(sc);
	return (error);
}

void
mt7902_dma_stop(struct mt7902_softc *sc)
{
	int i, error;

	if (sc->mem != NULL) {
		mt7902_write(sc, MT7902_REG_WFDMA_INT_EN, 0);
		mt7902_write(sc, MT7902_REG_WFDMA_GLO_CFG, 0);
		error = mt7902_wait(sc, MT7902_REG_WFDMA_GLO_CFG,
		    MT7902_WFDMA_TX_BUSY | MT7902_WFDMA_RX_BUSY, 0, 100000);
		if (error != 0) {
			device_printf(sc->dev,
			    "WFDMA did not become idle before ring teardown (GLO=%08x)\n",
			    mt7902_read(sc, MT7902_REG_WFDMA_GLO_CFG));
			/* Stop further device DMA before releasing host ring memory. */
			pci_disable_busmaster(sc->dev);
			DELAY(10000);
		}
	}
	for (i = 0; i < MT7902_TX_DATA_QUEUES; i++)
		mt7902_ring_free(&sc->tx_data[i]);
	mt7902_ring_free(&sc->tx_mcu);
	mt7902_ring_free(&sc->tx_fwdl);
	mt7902_ring_free(&sc->rx_data);
	mt7902_ring_free(&sc->rx_event);
}
