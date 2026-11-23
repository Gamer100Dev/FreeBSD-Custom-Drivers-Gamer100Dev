# Native FreeBSD MT7902 PCI Wi-Fi driver.
KMOD=if_mt7902_native
SRCS=if_mt7902_native.c if_mt7902_native.h
SRCS+=mt7902_native_fw.c mt7902_native_dma.c
SRCS+=mt7902_native_mcu.c mt7902_native_irq.c
SRCS+=mt7902_native_net80211.c
SRCS+=device_if.h bus_if.h pci_if.h

.include <bsd.kmod.mk>
