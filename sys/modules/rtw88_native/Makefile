# Native FreeBSD USB driver for Realtek RTL8822BU.
.PATH: ${SRCTOP}/sys/dev/rtw88_native

SYSDIR?=${SRCTOP}/sys
.include "${SYSDIR}/conf/kern.opts.mk"

KMOD=	if_rtw88_native
SRCS=	if_rtw88_native.c if_rtw88_native.h
SRCS+=	rtw88_native_usb.c rtw88_native_reg.c rtw88_native_fw.c
SRCS+=	rtw88_native_mac.c rtw88_native_efuse.c rtw88_native_txdesc.c
SRCS+=	bus_if.h device_if.h opt_bus.h opt_usb.h usb_if.h usbdevs.h

.include <bsd.kmod.mk>
