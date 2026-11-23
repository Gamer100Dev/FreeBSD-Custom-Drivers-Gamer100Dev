/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Gamer100Dev
 *
 * Native PCI interrupt and MCU event-ring handling.
 */
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>
#include <machine/bus.h>
#include <dev/pci/pcivar.h>

#include "if_mt7902_native.h"

#define MT7902_RXD_SIZE	36
#define MT7902_RXD_EID_OFFSET	28
#define MT7902_RXD_SEQ_OFFSET	29
#define MT7902_RXD_EVENT	0x06
#define MT7902_RXD_LEN_OFFSET	24
#define MT7902_RX_LEN_SHIFT	16
#define MT7902_RX_LEN_MASK	0x3fffU
#define MT7902_RXD0_LEN_MASK	0xffffU
#define MT7902_RXD0_TYPE_SHIFT	27
#define MT7902_RXD1_GROUP1	(1U << 11)
#define MT7902_RXD1_GROUP2	(1U << 12)
#define MT7902_RXD1_GROUP3	(1U << 13)
#define MT7902_RXD1_GROUP4	(1U << 14)
#define MT7902_RXD1_GROUP5	(1U << 15)
#define MT7902_RXD2_PAD_MASK	(3U << 14)
#define MT7902_RXD2_PAD_SHIFT	14

static void
mt7902_rx_data_poll(struct mt7902_softc *sc)
{
	/* Data RX deliberately remains unimplemented in this feature slice. */
	(void)sc;
}

static void
mt7902_rx_event_poll(struct mt7902_softc *sc)
{
	struct mt7902_ring *ring;
	struct mt7902_dma_desc *desc;
	struct mt7902_dma_buffer *buf, tmp;
	uint8_t *data;
	uint16_t mcu_len;
	uint32_t control;
	size_t event_len;
	int idx, refill_idx, count;
	uint8_t seq, eid;

	ring = &sc->rx_event;
	if (ring->desc == NULL)
		return;
	for (count = 0; count < ring->ndesc; count++) {
		idx = ring->tail;
		desc = &ring->desc[idx];
		bus_dmamap_sync(ring->desc_tag, ring->desc_map,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		control = le32toh(desc->control);
		if ((control & MT7902_DMA_DESC_DONE) == 0)
			break;
		buf = &ring->buf[idx];
		bus_dmamap_sync(ring->buf_tag, buf->map, BUS_DMASYNC_POSTREAD);
		data = buf->vaddr;
		mcu_len = 0;
		if (((control >> MT7902_RX_LEN_SHIFT) & MT7902_RX_LEN_MASK) >=
		    MT7902_RXD_SIZE) {
			event_len = ((control >> MT7902_RX_LEN_SHIFT) &
			    MT7902_RX_LEN_MASK) - MT7902_RXD_SIZE;
			memcpy(&mcu_len, data + MT7902_RXD_LEN_OFFSET,
			    sizeof(mcu_len));
			eid = data[MT7902_RXD_EID_OFFSET];
			seq = data[MT7902_RXD_SEQ_OFFSET];
			mtx_lock(&sc->mtx);
			sc->rx_event_count++;
			/* mt76 matches MCU replies by sequence; firmware replies can
			 * use EIDs other than the generic 0x06 event identifier.
			 */
			if (sc->mcu_waiting && seq == sc->mcu_wait_seq) {
				device_printf(sc->dev,
				    "MCU RX response cmd=%02x seq=%u eid=%02x len=%u status=%02x\n",
				    sc->mcu_wait_cmd, seq, eid,
				    le16toh(mcu_len), data[MT7902_RXD_SIZE - 4]);
				/* PATCH_SEM_CONTROL and PATCH_FINISH_REQ return
				 * their status in the final word of the RXD header,
				 * matching mt7921_mcu_parse_response().
				 */
				if (sc->mcu_wait_cmd == MT7902_MCU_CMD_PATCH_SEM ||
				    sc->mcu_wait_cmd == MT7902_MCU_CMD_PATCH_FINISH)
					sc->mcu_error = data[MT7902_RXD_SIZE - 4];
				else
					sc->mcu_error = 0;
				sc->mcu_response_len = MIN(event_len,
				    sizeof(sc->mcu_response));
				if (sc->mcu_response_len != 0)
					memcpy(sc->mcu_response,
					    data + MT7902_RXD_SIZE,
					    sc->mcu_response_len);
				sc->mcu_waiting = 0;
				cv_broadcast(&sc->mcu_cv);
			}
			mtx_unlock(&sc->mtx);
		} else {
			device_printf(sc->dev, "short MCU event descriptor (%u bytes)\n",
			    (control >> MT7902_RX_LEN_SHIFT) & MT7902_RX_LEN_MASK);
		}
		/* Refill the reserved producer slot, as mt76_dma_rx_fill() does.
		 * The received buffer is returned there; the old reserved buffer
		 * takes its place behind the consumer cursor.
		 */
		refill_idx = ring->head;
		if (refill_idx == idx) {
			device_printf(sc->dev,
			    "RX event ring cursor collision at descriptor %d\n", idx);
			break;
		}
		bus_dmamap_sync(ring->buf_tag, ring->buf[refill_idx].map,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		tmp = ring->buf[idx];
		ring->buf[idx] = ring->buf[refill_idx];
		ring->buf[refill_idx] = tmp;
		buf = &ring->buf[refill_idx];
		bus_dmamap_sync(ring->buf_tag, buf->map,
		    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
		ring->desc[refill_idx].buf0 = htole32((uint32_t)buf->paddr);
		ring->desc[refill_idx].buf1 = 0;
		ring->desc[refill_idx].info = 0;
		ring->desc[refill_idx].control = htole32(((uint32_t)buf->len &
		    MT7902_DMA_LEN_MASK) << MT7902_DMA_LEN_SHIFT);
		bus_dmamap_sync(ring->desc_tag, ring->desc_map,
		    BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);
		ring->tail = (idx + 1) % ring->ndesc;
		ring->head = (refill_idx + 1) % ring->ndesc;
		mt7902_write(sc, ring->reg_base + 0x8, ring->head);
	}
}

static void
mt7902_intr(void *arg)
{
	struct mt7902_softc *sc;
	uint32_t status;

	sc = arg;
	status = mt7902_read(sc, MT7902_REG_WFDMA_INT_STA);
	if (status == 0 || status == 0xffffffff)
		return;
	mt7902_write(sc, MT7902_REG_WFDMA_INT_STA, status);
	mt7902_write(sc, MT7902_REG_WFDMA_INT_EN, 0);
	mtx_lock(&sc->mtx);
	sc->irq_status |= status;
	sc->irq_last_status = status;
	sc->irq_count++;
	mtx_unlock(&sc->mtx);
	taskqueue_enqueue(taskqueue_thread, &sc->irq_task);
}

int
mt7902_irq_setup(struct mt7902_softc *sc)
{
	int error, count;

	count = 1;
	error = pci_alloc_msi(sc->dev, &count);
	if (error == 0)
		sc->msi_count = count;
	else
		sc->msi_count = 0;
	sc->irq_rid = sc->msi_count != 0 ? 1 : 0;
	sc->irq = bus_alloc_resource_any(sc->dev, SYS_RES_IRQ, &sc->irq_rid,
	    RF_ACTIVE | (sc->msi_count == 0 ? RF_SHAREABLE : 0));
	if (sc->irq == NULL) {
		if (sc->msi_count != 0)
			pci_release_msi(sc->dev);
		sc->msi_count = 0;
		return (ENXIO);
	}
	error = bus_setup_intr(sc->dev, sc->irq,
	    INTR_TYPE_NET | INTR_MPSAFE, NULL, mt7902_intr, sc,
	    &sc->irq_cookie);
	if (error != 0) {
		bus_release_resource(sc->dev, SYS_RES_IRQ, sc->irq_rid, sc->irq);
		sc->irq = NULL;
		if (sc->msi_count != 0)
			pci_release_msi(sc->dev);
		sc->msi_count = 0;
		return (error);
	}
	mt7902_write(sc, MT7902_REG_WFDMA_INT_STA, 0xffffffff);
	mt7902_write(sc, MT7902_REG_WFDMA_INT_EN,
	    MT7902_INT_MASK | MT7902_INT_MCU_CMD);
	return (0);
}

void
mt7902_irq_teardown(struct mt7902_softc *sc)
{
	if (sc->mem != NULL)
		mt7902_write(sc, MT7902_REG_WFDMA_INT_EN, 0);
	if (sc->irq_cookie != NULL) {
		bus_teardown_intr(sc->dev, sc->irq, sc->irq_cookie);
		sc->irq_cookie = NULL;
	}
	/* A queued worker may still be consuming an RX ring. Drain it only
	 * after the interrupt source is detached, and before the caller frees
	 * ring memory.
	 */
	taskqueue_drain(taskqueue_thread, &sc->irq_task);
	taskqueue_drain(taskqueue_thread, &sc->rx_task);
	taskqueue_drain(taskqueue_thread, &sc->tx_task);
	if (sc->irq != NULL) {
		bus_release_resource(sc->dev, SYS_RES_IRQ, sc->irq_rid, sc->irq);
		sc->irq = NULL;
	}
	if (sc->msi_count != 0) {
		pci_release_msi(sc->dev);
		sc->msi_count = 0;
	}
}

void
mt7902_irq_task(void *arg, int pending __unused)
{
	struct mt7902_softc *sc;
	uint32_t status;

	sc = arg;
	mtx_lock(&sc->mtx);
	status = sc->irq_status;
	sc->irq_status = 0;
	mtx_unlock(&sc->mtx);
	if (status & (MT7902_INT_RX_DONE_WM | MT7902_INT_RX_DONE_WM2))
		mt7902_rx_event_poll(sc);
	if (status & MT7902_INT_RX_DONE_DATA)
		taskqueue_enqueue(taskqueue_thread, &sc->rx_task);
	mt7902_write(sc, MT7902_REG_WFDMA_INT_EN,
	    MT7902_INT_MASK | MT7902_INT_MCU_CMD);
}

/*
 * The data RXD-to-mbuf/net80211 decoder is intentionally not installed yet.
 * Keep the task entry point defined so an RX interrupt can never call an
 * unresolved symbol; data RX remains disabled at the protocol level until
 * that decoder and its ownership transitions are implemented.
 */
void
mt7902_rx_task(void *arg __unused, int pending __unused)
{
	struct mt7902_softc *sc = arg;

	mt7902_rx_data_poll(sc);
}
