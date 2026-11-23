KMOD=	ps3joy

SRCS=	ps3controller.c

# Newbus interfaces
SRCS+=	device_if.h
SRCS+=	bus_if.h

# USB subsystem
SRCS+=	usb_if.h
SRCS+=	opt_usb.h

# Keyboard definitions pulled in by evdev
SRCS+=	opt_kbd.h

.include <bsd.kmod.mk>