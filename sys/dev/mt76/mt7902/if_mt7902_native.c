/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Gamer100Dev
 *
 * Native FreeBSD PCI attachment and hardware resource layer for MT7902.
 * The driver exposes its native net80211 radio after firmware startup.
 */
#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/priority.h>
#include <sys/malloc.h>
#include <sys/rman.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <machine/bus.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

#include "if_mt7902_native.h"

#include <net80211/ieee80211_proto.h>

static int mt7902_probe(device_t);
static int mt7902_attach(device_t);
static int mt7902_detach(device_t);
static int mt7902_sysctl_hw_start(SYSCTL_HANDLER_ARGS);
static int mt7902_suspend(device_t);
static int mt7902_resume(device_t);

int
mt7902_sysctl_init(struct mt7902_softc *sc)
{
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *tree;

	ctx = device_get_sysctl_ctx(sc->dev);
	tree = device_get_sysctl_tree(sc->dev);
	if (SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(tree), OID_AUTO, "hw_start",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0,
	    mt7902_sysctl_hw_start, "I",
	    "Load MT7902 Wi-Fi firmware and initialize WFDMA") == NULL)
		return (ENOMEM);
	if (SYSCTL_ADD_INT(ctx, SYSCTL_CHILDREN(tree), OID_AUTO, "rx_debug",
	    CTLFLAG_RW | CTLFLAG_MPSAFE, &sc->rx_debug, 0,
	    "MT7902 RX diagnostics: 0=off, 1=frames/drops, 2=RX descriptors, 3=interrupts") == NULL)
		return (ENOMEM);
	return (0);
}

static int
mt7902_sysctl_hw_start(SYSCTL_HANDLER_ARGS)
{
	struct mt7902_softc *sc;
	int value, error;

	sc = arg1;
	mtx_lock(&sc->mtx);
	value = sc->hw_started;
	mtx_unlock(&sc->mtx);
	error = sysctl_handle_int(oidp, &value, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (value != 0 && value != 1)
		return (EINVAL);

	mtx_lock(&sc->mtx);
	if (sc->dying || sc->hw_starting || sc->suspended) {
		mtx_unlock(&sc->mtx);
		return (EBUSY);
	}
	if (value == sc->hw_started) {
		mtx_unlock(&sc->mtx);
		return (0);
	}
	sc->hw_starting = 1;
	mtx_unlock(&sc->mtx);

	if (value) {
		/* firmware_get() may wait for the firmware filesystem; sysctl runs
		 * in process context, unlike PCI attach and interrupt context.
		 */
		error = mt7902_fw_boot(sc);
	} else {
		mt7902_irq_teardown(sc);
		mt7902_dma_stop(sc);
		error = 0;
	}

	mtx_lock(&sc->mtx);
	if (error == 0)
		sc->hw_started = value;
	sc->hw_starting = 0;
	mtx_unlock(&sc->mtx);
	return (error);
}

static const struct mt7902_reg_window mt7902_windows[] = {
	{ 0x820d0000, 0x30000, 0x10000 },
	{ 0x820ed000, 0x24800, 0x00800 },
	{ 0x820e4000, 0x21000, 0x00400 },
	{ 0x820e7000, 0x21e00, 0x00200 },
	{ 0x820eb000, 0x24200, 0x00400 },
	{ 0x820e2000, 0x20800, 0x00400 },
	{ 0x820e3000, 0x20c00, 0x00400 },
	{ 0x820e5000, 0x21400, 0x00800 },
	{ 0x00400000, 0x80000, 0x10000 },
	{ 0x00410000, 0x90000, 0x10000 },
	{ 0x40000000, 0x70000, 0x10000 },
	{ 0x54000000, 0x02000, 0x01000 },
	{ 0x55000000, 0x03000, 0x01000 },
	{ 0x58000000, 0x06000, 0x01000 },
	{ 0x59000000, 0x07000, 0x01000 },
	{ 0x7c000000, 0xf0000, 0x10000 },
	{ 0x7c020000, 0xd0000, 0x10000 },
	{ 0x7c060000, 0xe0000, 0x10000 },
	{ 0x80020000, 0xb0000, 0x10000 },
	{ 0x81020000, 0xc0000, 0x10000 },
	{ 0x820c0000, 0x08000, 0x04000 },
	{ 0x820c8000, 0x0c000, 0x02000 },
	{ 0x820cc000, 0x0e000, 0x01000 },
	{ 0x820cd000, 0x0f000, 0x01000 },
	{ 0x74030000, 0x10000, 0x10000 },
	{ 0x820ce000, 0x21c00, 0x00200 },
	{ 0x820cf000, 0x22000, 0x01000 },
	{ 0x820e0000, 0x20000, 0x00400 },
	{ 0x820e1000, 0x20400, 0x00200 },
	{ 0x820e9000, 0x23400, 0x00200 },
	{ 0x820ea000, 0x24000, 0x00200 },
	{ 0x820ec000, 0x24600, 0x00200 },
	{ 0x820f0000, 0xa0000, 0x00400 },
	{ 0x820f1000, 0xa0600, 0x00200 },
	{ 0x820f2000, 0xa0800, 0x00400 },
	{ 0x820f3000, 0xa0c00, 0x00400 },
	{ 0x820f4000, 0xa1000, 0x00400 },
	{ 0x820f5000, 0xa1400, 0x00800 },
	{ 0x820f7000, 0xa1e00, 0x00200 },
	{ 0x820f9000, 0xa3400, 0x00200 },
	{ 0x820fa000, 0xa4000, 0x00200 },
	{ 0x820fb000, 0xa4200, 0x00400 },
	{ 0x820fd000, 0xa4800, 0x00800 },
};

static int
mt7902_reg_offset(struct mt7902_softc *sc, uint32_t reg, bus_size_t *offset)
{
	unsigned int i;

	if (reg < 0x100000) {
		*offset = reg;
		return (0);
	}
	for (i = 0; i < nitems(mt7902_windows); i++) {
		const struct mt7902_reg_window *w = &mt7902_windows[i];

		if (reg >= w->phys && reg - w->phys < w->size) {
			*offset = w->bar + reg - w->phys;
			return (0);
		}
	}
	if ((reg >= 0x18000000 && reg < 0x18c00000) ||
	    (reg >= 0x70000000 && reg < 0x78000000) ||
	    (reg >= 0x7c000000 && reg < 0x7c400000)) {
		bus_write_4(sc->mem, MT7902_REG_REMAP,
		    (reg >> 16) & MT7902_REMAP_MASK);
		(void)bus_read_4(sc->mem, MT7902_REG_REMAP);
		*offset = MT7902_REMAP_BAR_BASE + (reg & 0xffff);
		return (0);
	}
	return (EFAULT);
}

static int
mt7902_probe(device_t dev)
{
	if (pci_get_vendor(dev) != MT7902_PCI_VENDOR ||
	    pci_get_device(dev) != MT7902_PCI_DEVICE)
		return (ENXIO);

	device_set_desc(dev, "MediaTek MT7902 Wi-Fi 6 PCIe (native FreeBSD port)");
	return (BUS_PROBE_DEFAULT);
}

uint32_t
mt7902_read(struct mt7902_softc *sc, bus_size_t reg)
{
	bus_size_t offset;
	uint32_t value;

	if (sc->mem == NULL)
		return (0xffffffff);
	mtx_lock(&sc->reg_mtx);
	if (mt7902_reg_offset(sc, reg, &offset) != 0 ||
	    offset + sizeof(uint32_t) > rman_get_size(sc->mem))
		value = 0xffffffff;
	else
		value = bus_read_4(sc->mem, offset);
	mtx_unlock(&sc->reg_mtx);
	return (value);
}

void
mt7902_write(struct mt7902_softc *sc, bus_size_t reg, uint32_t value)
{
	bus_size_t offset;

	if (sc->mem == NULL)
		return;
	mtx_lock(&sc->reg_mtx);
	if (mt7902_reg_offset(sc, reg, &offset) == 0 &&
	    offset + sizeof(uint32_t) <= rman_get_size(sc->mem))
		bus_write_4(sc->mem, offset, value);
	mtx_unlock(&sc->reg_mtx);
}

int
mt7902_wait(struct mt7902_softc *sc, bus_size_t reg, uint32_t mask,
	uint32_t expected, u_int timeout_us)
{
	u_int elapsed;

	for (elapsed = 0; elapsed < timeout_us; elapsed += 10) {
		if ((mt7902_read(sc, reg) & mask) == expected)
			return (0);
		DELAY(10);
	}
	return (ETIMEDOUT);
}

static int
mt7902_attach(device_t dev)
{
	struct mt7902_softc *sc;
	int error;
	uint32_t bar;

	sc = device_get_softc(dev);
	sc->dev = dev;
	/* Keep packet-level logging disabled on the normal traffic path. */
	sc->rx_debug = 0;
	mtx_init(&sc->mtx, device_get_nameunit(dev), MTX_NETWORK_LOCK,
	    MTX_DEF);
	mtx_init(&sc->reg_mtx, "mt7902 register remap", NULL, MTX_DEF);
	sx_init(&sc->mcu_sx, "mt7902 MCU command serialization");
	sc->mcu_sx_initialized = true;
	cv_init(&sc->mcu_cv, "mt7902 MCU response");

	/* Enable memory decoding and bus mastering before mapping the BAR. */
	pci_enable_busmaster(dev);
	device_printf(dev,
	    "PCI cmd/status=%04x:%04x BAR0=%08x:%08x BAR2=%08x:%08x\n",
	    pci_read_config(dev, PCIR_COMMAND, 2),
	    pci_read_config(dev, PCIR_STATUS, 2),
	    pci_read_config(dev, PCIR_BAR(1), 4),
	    pci_read_config(dev, PCIR_BAR(0), 4),
	    pci_read_config(dev, PCIR_BAR(3), 4),
	    pci_read_config(dev, PCIR_BAR(2), 4));
	bar = pci_read_config(dev, PCIR_BAR(0), 4);
	if ((bar & PCIM_BAR_MEM_TYPE) == PCIM_BAR_MEM_64)
		device_printf(dev, "64-bit BAR0 detected\n");

	sc->mem_rid = MT7902_BAR;
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &sc->mem_rid,
	    RF_ACTIVE);
	if (sc->mem == NULL) {
		device_printf(dev, "could not map MMIO BAR0\n");
		error = ENXIO;
		goto fail;
	}
	/* Keep IRQ, RX, and MCU event work on a dedicated non-global worker. */
	sc->tq = taskqueue_create_fast("mt7902_native", M_WAITOK,
	    taskqueue_thread_enqueue, &sc->tq);
	error = taskqueue_start_threads(&sc->tq, 1, PI_NET, "%s taskq",
	    device_get_nameunit(dev));
	if (error != 0) {
		device_printf(dev, "could not start private taskqueue: %d\n", error);
		taskqueue_free(sc->tq);
		sc->tq = NULL;
		goto fail;
	}
	TASK_INIT(&sc->irq_task, 0, mt7902_irq_task, sc);
	TASK_INIT(&sc->event_task, 1, mt7902_event_task, sc);
	TASK_INIT(&sc->rx_task, 2, mt7902_rx_task, sc);
	TASK_INIT(&sc->tx_task, 3, mt7902_tx_task, sc);
	sc->tx_tq = taskqueue_create_fast("mt7902_native_tx", M_WAITOK,
	    taskqueue_thread_enqueue, &sc->tx_tq);
	error = taskqueue_start_threads(&sc->tx_tq, 1, PI_NET,
	    "%s txq", device_get_nameunit(dev));
	if (error != 0) {
		device_printf(dev, "could not start TX taskqueue: %d\n", error);
		taskqueue_free(sc->tx_tq);
		sc->tx_tq = NULL;
		goto fail;
	}
	if (rman_get_size(sc->mem) < MT7902_REG_BAR_MIN) {
		device_printf(dev, "BAR0 is too small (%ju bytes)\n",
		    (uintmax_t)rman_get_size(sc->mem));
		error = ENXIO;
		goto fail;
	}

	/* Establish a DMA tag for the forthcoming native WFDMA ring port. */
	error = bus_dma_tag_create(bus_get_dma_tag(dev), 1, 0,
	    BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR, NULL, NULL,
	    BUS_SPACE_MAXSIZE, BUS_SPACE_UNRESTRICTED, BUS_SPACE_MAXSIZE,
	    0, NULL, NULL, &sc->dmat);
	if (error != 0) {
		device_printf(dev, "could not create DMA tag: %d\n", error);
		goto fail;
	}

	/* CHIP_ID lives in the indirect L1 register window. */
	sc->chip_id = mt7902_read(sc, MT7902_REG_CHIP_ID);
	if (sc->chip_id != MT7902_CHIP_ID) {
		device_printf(dev, "unexpected chip ID 0x%08x\n", sc->chip_id);
		error = ENXIO;
		goto fail;
	}
	sc->attached = true;
	error = mt7902_sysctl_init(sc);
	if (error != 0)
		goto fail;
	device_printf(dev, "MT7902 chip 0x%04x found; native PCI/MMIO/DMA initialized\n",
	    sc->chip_id);
	/*
	 * Follow the FreeBSD PCI driver's attach lifecycle: bring the device up
	 * when it is attached, instead of requiring a private sysctl write.  Keep
	 * the PCI function attached if firmware startup fails so the failure can
	 * be retried through hw_start after correcting firmware or hardware state.
	 */
	error = mt7902_fw_boot(sc);
	if (error == 0)
		sc->hw_started = 1;
	else
		device_printf(dev,
		    "automatic firmware startup failed; retry with hw_start=1\n");
	return (0);

fail:
	(void)mt7902_detach(dev);
	return (error);
}

static int
mt7902_detach(device_t dev)
{
	struct mt7902_softc *sc;

	sc = device_get_softc(dev);
	if (mtx_initialized(&sc->mtx)) {
		mtx_lock(&sc->mtx);
		sc->dying = 1;
		mtx_unlock(&sc->mtx);
	}
	if (sc->irq_cookie != NULL || sc->irq != NULL || sc->hw_started)
		mt7902_irq_teardown(sc);
	if (sc->hw_started) {
		mt7902_dma_stop(sc);
		sc->hw_started = 0;
	}
	if (sc->tq != NULL) {
		taskqueue_drain_all(sc->tq);
		taskqueue_free(sc->tq);
		sc->tq = NULL;
	}
	if (sc->tx_tq != NULL) {
		taskqueue_drain_all(sc->tx_tq);
		taskqueue_free(sc->tx_tq);
		sc->tx_tq = NULL;
	}
	if (sc->net80211_attached)
		mt7902_net80211_detach(sc);
	if (sc->irq_cookie != NULL)
		bus_teardown_intr(dev, sc->irq, sc->irq_cookie);
	if (sc->irq != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, sc->irq_rid, sc->irq);
	if (sc->dmat != NULL)
		bus_dma_tag_destroy(sc->dmat);
	if (sc->mem != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, sc->mem_rid, sc->mem);
	if (mtx_initialized(&sc->mtx))
		cv_destroy(&sc->mcu_cv);
	if (mtx_initialized(&sc->mtx))
		mtx_destroy(&sc->mtx);
	if (sc->mcu_sx_initialized) {
		sx_destroy(&sc->mcu_sx);
		sc->mcu_sx_initialized = false;
	}
	if (mtx_initialized(&sc->reg_mtx))
		mtx_destroy(&sc->reg_mtx);
	sc->attached = false;
	return (0);
}

/*
 * Runtime power management is deliberately not implemented. Only the bus
 * suspend/resume callbacks below are supported; do not add idle runtime-PM
 * transitions until firmware, DMA ownership, and wake signalling are proven.
 *
 * Suspend all net80211 VAPs before stopping firmware and WFDMA.  The
 * net80211 core remembers which VAPs were running and resume_all() restores
 * their states only after the firmware and DMA rings are ready again.
 */
static int
mt7902_suspend(device_t dev)
{
	struct mt7902_softc *sc;
	bool was_started;

	sc = device_get_softc(dev);
	mtx_lock(&sc->mtx);
	if (sc->dying || sc->suspended) {
		mtx_unlock(&sc->mtx);
		return (sc->dying ? ENXIO : 0);
	}
	was_started = sc->hw_started != 0;
	sc->suspended = true;
	sc->suspend_was_started = was_started;
	mtx_unlock(&sc->mtx);

	if (was_started && sc->net80211_attached)
		ieee80211_suspend_all(&sc->ic);
	if (was_started) {
		mt7902_irq_teardown(sc);
		mt7902_dma_stop(sc);
		mtx_lock(&sc->mtx);
		sc->hw_started = 0;
		mtx_unlock(&sc->mtx);
	}
	return (0);
}

static int
mt7902_resume(device_t dev)
{
	struct mt7902_softc *sc;
	bool was_started;
	int error;

	sc = device_get_softc(dev);
	mtx_lock(&sc->mtx);
	if (!sc->suspended) {
		mtx_unlock(&sc->mtx);
		return (0);
	}
	if (sc->dying || sc->hw_starting) {
		mtx_unlock(&sc->mtx);
		return (EBUSY);
	}
	was_started = sc->suspend_was_started;
	sc->hw_starting = was_started;
	mtx_unlock(&sc->mtx);

	if (was_started) {
		pci_enable_busmaster(dev);
		error = mt7902_fw_boot(sc);
		if (error != 0) {
			mtx_lock(&sc->mtx);
			sc->hw_starting = 0;
			mtx_unlock(&sc->mtx);
			device_printf(dev, "firmware restart after suspend failed: %d\n",
			    error);
			return (error);
		}
	}

	mtx_lock(&sc->mtx);
	if (was_started)
		sc->hw_started = 1;
	sc->suspend_was_started = false;
	sc->suspended = false;
	sc->hw_starting = 0;
	mtx_unlock(&sc->mtx);

	if (was_started && sc->net80211_attached)
		ieee80211_resume_all(&sc->ic);
	return (0);
}

static device_method_t mt7902_methods[] = {
	DEVMETHOD(device_probe, mt7902_probe),
	DEVMETHOD(device_attach, mt7902_attach),
	DEVMETHOD(device_detach, mt7902_detach),
	DEVMETHOD(device_suspend, mt7902_suspend),
	DEVMETHOD(device_resume, mt7902_resume),
	DEVMETHOD_END
};

static driver_t mt7902_driver = {
	"mt7902_native", mt7902_methods, sizeof(struct mt7902_softc)
};
DRIVER_MODULE(mt7902_native, pci, mt7902_driver, 0, 0);
MODULE_VERSION(mt7902_native, 1);
