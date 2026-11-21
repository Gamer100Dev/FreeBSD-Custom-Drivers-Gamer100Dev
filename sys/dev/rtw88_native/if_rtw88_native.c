/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Gamer100Dev
 * All rights reserved.
 *
 * This file implements the initial native FreeBSD USB transport for the
 * Realtek RTL8822BU (USB ID 0bda:b812).  It intentionally does not register
 * a wlan interface yet; chipset startup, firmware download, data transfers,
 * and net80211 integration are subsequent porting stages.
 *
 * Register request format is the one used by the RTW88 USB hardware
 * interface: vendor control request 0x05, wValue = register address,
 * wIndex = 0, and a little-endian payload.
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include "if_rtw88_native.h"

/*
 * Keep the initial port deliberately narrow: bind only to the adapter that
 * is being developed and only to its vendor-specific interface.
 */
static const STRUCT_USB_HOST_ID rtw88_native_devs[] = {
	{
		USB_VP(RTW88_NATIVE_VENDOR_REALTEK,
		    RTW88_NATIVE_PRODUCT_RTL8822BU),
		USB_IFACE_CLASS(0xff),
		USB_IFACE_SUBCLASS(0xff),
		USB_IFACE_PROTOCOL(0xff)
	},
};

static int
rtw88_native_probe(device_t dev)
{
	struct usb_attach_arg *uaa;

	uaa = device_get_ivars(dev);
	if (uaa == NULL || uaa->usb_mode != USB_MODE_HOST)
		return (ENXIO);

	return (usbd_lookup_id_by_uaa(rtw88_native_devs,
	    sizeof(rtw88_native_devs), uaa));
}

static int
rtw88_native_sysctl_hw_start(SYSCTL_HANDLER_ARGS)
{
	struct rtw88_native_softc *sc;
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
	if (sc->dying || sc->detaching) {
		mtx_unlock(&sc->mtx);
		return (ENXIO);
	}
	if (sc->hw_busy) {
		mtx_unlock(&sc->mtx);
		return (EBUSY);
	}
	if (value == sc->hw_started) {
		mtx_unlock(&sc->mtx);
		return (0);
	}
	sc->hw_busy = 1;
	mtx_unlock(&sc->mtx);

	/*
	 * FIRMWARE(9) warns that firmware_get() can access the filesystem and
	 * must not run with a driver lock held. hw_busy serializes this sleepable
	 * transition while leaving the USB/register helpers free to take mtx.
	 */
	if (value != 0)
		error = rtw88_native_hw_start(sc);
	else {
		rtw88_native_hw_stop(sc);
		error = 0;
	}

	mtx_lock(&sc->mtx);
	if (error == 0)
		sc->hw_started = value;
	sc->hw_busy = 0;
	cv_broadcast(&sc->io_drain_cv);
	mtx_unlock(&sc->mtx);

	if (error != 0)
		device_printf(sc->dev, "hardware %s failed: %d\n",
		    value ? "start" : "stop", error);
	return (error);
}

static int
rtw88_native_attach(device_t dev)
{
	struct rtw88_native_softc *sc;
	struct usb_attach_arg *uaa;
	uint32_t sys_cfg1;
	usb_error_t uerr;
	int i;

	sc = device_get_softc(dev);
	bzero(sc, sizeof(*sc));
	uaa = device_get_ivars(dev);
	sc->dev = dev;
	if (uaa == NULL || uaa->device == NULL)
		return (ENXIO);

	sc->udev = uaa->device;
	sc->iface_index = uaa->info.bIfaceIndex;

	mtx_init(&sc->mtx, device_get_nameunit(dev), MTX_NETWORK_LOCK, MTX_DEF);
	sc->mtx_initialized = 1;
	for (i = 0; i < RTW88_NATIVE_TX_QUEUES; i++)
		cv_init(&sc->tx_cv[i], "rtw88 native tx");
	sc->tx_cv_initialized = 1;
	cv_init(&sc->io_drain_cv, "rtw88 native io");
	sc->io_cv_initialized = 1;

	device_printf(dev,
	    "native FreeBSD USB transport attached (RTL8822BU, 0bda:b812)\n");

	/*
	 * Allocate endpoint-backed resources through the native FreeBSD USB
	 * layer. Endpoint addresses are resolved from interface descriptors.
	 */
	uerr = rtw88_native_usb_setup(sc);
	if (uerr != USB_ERR_NORMAL_COMPLETION) {
		device_printf(dev, "could not allocate native USB transfers: %s\n",
		    usbd_errstr(uerr));
		for (i = 0; i < RTW88_NATIVE_TX_QUEUES; i++)
			cv_destroy(&sc->tx_cv[i]);
		sc->tx_cv_initialized = 0;
		cv_destroy(&sc->io_drain_cv);
		sc->io_cv_initialized = 0;
		mtx_destroy(&sc->mtx);
		sc->mtx_initialized = 0;
		return (ENXIO);
	}
	device_printf(dev,
	    "allocated native USB pipes: 1 bulk IN, 3 bulk OUT, 1 interrupt IN (idle)\n");

	/*
	 * REG_SYS_CFG1 (0x00f0) is the read-only system configuration register
	 * used by the upstream RTW88 chip-identification path.  Probe it once
	 * without changing device state; this tests the native control pipe.
	 */
	uerr = rtw88_native_raw_reg_read32(sc, RTW88_NATIVE_REG_SYS_CFG1, &sys_cfg1);
	if (uerr == USB_ERR_NORMAL_COMPLETION) {
		device_printf(dev,
		    "native vendor-control read succeeded: SYS_CFG1=0x%08x\n",
		    sys_cfg1);
	} else {
		device_printf(dev,
		    "native vendor-control read of SYS_CFG1 failed: %s\n",
		    usbd_errstr(uerr));
	}

	/*
	 * Hardware startup is intentionally deferred until the root filesystem
	 * is mounted. The sysctl handler runs in sleepable process context.
	 */
	if (SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "hw_start", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE,
	    sc, 0, rtw88_native_sysctl_hw_start, "I",
	    "Start (1) or stop (0) RTL8822BU hardware and interrupt capture") == NULL) {
		rtw88_native_usb_unsetup(sc);
		for (i = 0; i < RTW88_NATIVE_TX_QUEUES; i++)
			cv_destroy(&sc->tx_cv[i]);
		sc->tx_cv_initialized = 0;
		cv_destroy(&sc->io_drain_cv);
		sc->io_cv_initialized = 0;
		mtx_destroy(&sc->mtx);
		sc->mtx_initialized = 0;
		return (ENOMEM);
	}
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "intr_count", CTLFLAG_RD, &sc->intr_count, 0,
	    "Number of USB interrupt reports captured");
	SYSCTL_ADD_U16(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "intr_last_len", CTLFLAG_RD, &sc->last_intr_len, 0,
	    "Length of the most recent USB interrupt report");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_count", CTLFLAG_RD, &sc->rx_count, 0,
	    "Number of USB bulk-IN aggregates received");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_desc_count", CTLFLAG_RD, &sc->rx_desc_count, 0,
	    "Number of RX descriptors parsed");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_data_count", CTLFLAG_RD, &sc->rx_data_count, 0,
	    "Number of non-C2H RX frames identified");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_c2h_count", CTLFLAG_RD, &sc->rx_c2h_count, 0,
	    "Number of firmware C2H RX records identified");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_malformed_count", CTLFLAG_RD, &sc->rx_malformed_count, 0,
	    "Number of malformed/truncated RX records");
	SYSCTL_ADD_U32(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_last_len", CTLFLAG_RD, &sc->rx_last_len, 0,
	    "Length of the latest raw USB bulk-IN aggregate");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_last_c2h_id", CTLFLAG_RD, &sc->rx_last_c2h_id, 0,
	    "C2H command ID from the latest firmware event");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_last_c2h_seq", CTLFLAG_RD, &sc->rx_last_c2h_seq, 0,
	    "Sequence number from the latest firmware C2H event");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_last_c2h_len", CTLFLAG_RD, &sc->rx_last_c2h_len, 0,
	    "Payload length from the latest firmware C2H event");

	device_printf(dev,
	    "deferred hardware start available at dev.rtw88_native.%d.hw_start\n",
	    device_get_unit(dev));

	return (0);
}

static int
rtw88_native_detach(device_t dev)
{
	struct rtw88_native_softc *sc;
	int i;

	sc = device_get_softc(dev);

	/*
	 * Prevent new sysctl-triggered hardware transitions and wait for any
	 * current start/stop operation before draining interrupt I/O and power.
	 */
	if (sc->mtx_initialized) {
		mtx_lock(&sc->mtx);
		sc->detaching = 1;
		while (sc->hw_busy)
			cv_wait(&sc->io_drain_cv, &sc->mtx);
		mtx_unlock(&sc->mtx);
	}
	rtw88_native_hw_stop(sc);
	if (sc->mtx_initialized) {
		mtx_lock(&sc->mtx);
		sc->hw_started = 0;
		sc->dying = 1;
		mtx_unlock(&sc->mtx);
	}

	/* Stop DMA callbacks first, then wait for synchronous I/O callers. */
	rtw88_native_usb_unsetup(sc);
	if (sc->mtx_initialized && sc->io_cv_initialized) {
		mtx_lock(&sc->mtx);
		while (sc->io_users != 0)
			cv_wait(&sc->io_drain_cv, &sc->mtx);
		mtx_unlock(&sc->mtx);
	}

	rtw88_native_firmware_put(sc);
	if (sc->tx_cv_initialized) {
		for (i = 0; i < RTW88_NATIVE_TX_QUEUES; i++)
			cv_destroy(&sc->tx_cv[i]);
		sc->tx_cv_initialized = 0;
	}
	if (sc->io_cv_initialized) {
		cv_destroy(&sc->io_drain_cv);
		sc->io_cv_initialized = 0;
	}
	if (sc->mtx_initialized) {
		mtx_destroy(&sc->mtx);
		sc->mtx_initialized = 0;
	}
	return (0);
}

static device_method_t rtw88_native_methods[] = {
	DEVMETHOD(device_probe, 	rtw88_native_probe),
	DEVMETHOD(device_attach,	rtw88_native_attach),
	DEVMETHOD(device_detach,	rtw88_native_detach),
	DEVMETHOD_END
};

static driver_t rtw88_native_driver = {
	"rtw88_native",
	rtw88_native_methods,
	sizeof(struct rtw88_native_softc)
};

DRIVER_MODULE(rtw88_native, uhub, rtw88_native_driver, NULL, NULL);
MODULE_DEPEND(rtw88_native, usb, 1, 1, 1);
MODULE_VERSION(rtw88_native, 1);
USB_PNP_HOST_INFO(rtw88_native_devs);
