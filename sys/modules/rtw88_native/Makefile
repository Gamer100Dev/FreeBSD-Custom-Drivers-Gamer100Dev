# Native FreeBSD USB transport for Realtek RTL8822BU.
.PATH: ${SRCTOP}/sys/dev/rtw88_native

KMOD=	if_rtw88_native
SRCS=	if_rtw88_native.c if_rtw88_native.h
SRCS+=	bus_if.h device_if.h usb_if.h

.include <bsd.kmod.mk>
