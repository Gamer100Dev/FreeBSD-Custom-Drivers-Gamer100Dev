# Native MT7902 USB Bluetooth firmware bootstrap / ng_ubt subclass.
.PATH: ${SRCTOP}/sys/dev/mt76/mt7902
.PATH: ${SRCTOP}/sys/netgraph/bluetooth/drivers/ubt
CFLAGS+=-I${SRCTOP}/sys/netgraph/bluetooth/include \
	-I${SRCTOP}/sys/netgraph/bluetooth/drivers/ubt
KMOD=ng_ubt_mt7902
SRCS=ng_ubt_mt7902.c opt_bus.h opt_usb.h device_if.h bus_if.h usb_if.h usbdevs.h

.include <bsd.kmod.mk>
