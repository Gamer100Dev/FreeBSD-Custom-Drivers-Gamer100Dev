# Native FreeBSD USB driver transport for Realtek RTL8822BU.
.PATH: ${SRCTOP}/sys/dev/rtw88_native

KMOD=	if_rtw88_native
SRCS=	if_rtw88_native.c if_rtw88_native.h
SRCS+=	rtw88_native_usb.c rtw88_native_reg.c rtw88_native_fw.c
SRCS+=	rtw88_native_mac.c
SRCS+=	bus_if.h device_if.h usb_if.h

.include <bsd.kmod.mk>
