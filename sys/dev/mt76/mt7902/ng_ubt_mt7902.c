/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Gamer100Dev
 *
 * MT7902 Bluetooth firmware bootstrap and ng_ubt USB subclass.
 * The HCI/ACL/SCO transport and Netgraph node are inherited from ng_ubt.
 */
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/firmware.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usb_ioctl.h>
#include "usbdevs.h"

#include "mt7902_bt_fw.h"

#include <netgraph/ng_message.h>
#include <netgraph/netgraph.h>
#include <netgraph/ng_parse.h>
#include <netgraph/bluetooth/include/ng_bluetooth.h>
#include <netgraph/bluetooth/include/ng_hci.h>
#include <netgraph/bluetooth/include/ng_ubt.h>
#include <netgraph/bluetooth/drivers/ubt/ng_ubt_var.h>

#define MT7902_BT_FW "mediatek/BT_RAM_CODE_MT7902_1_1_hdr.bin"
#define MTK_WMT_OPCODE 0xfc6f
#define MTK_WMT_EVENT 0xe4
#define MTK_WMT_PATCH_DOWNLOAD 0x01
#define MTK_WMT_PATCH_PROGRESS 1
#define MTK_WMT_PATCH_DONE 2
#define MTK_WMT_START 1
#define MTK_WMT_CONTINUE 2
#define MTK_WMT_END 3
#define MTK_BT_MAX_SECTION 128
#define MTK_BT_CHUNK 250

/* MediaTek 79xx ROM patch header and section map. */
struct mt7902_bt_global_desc {
	uint32_t patch_ver;
	uint32_t sub_sys;
	uint32_t feature_opt;
	uint32_t section_num;
} __packed;

struct mt7902_bt_section_map {
	uint32_t type;
	uint32_t offset;
	uint32_t size;
	uint32_t spec[13];
} __packed;

struct mt7902_bt_wmt_hdr {
	uint8_t dir;
	uint8_t op;
	uint16_t len;
	uint8_t flag;
} __packed;

struct mt7902_bt_wmt_event {
	uint8_t event;
	uint8_t len;
	uint8_t dir;
	uint8_t op;
	uint16_t wmt_len;
	uint8_t flag;
	uint8_t data[56];
} __packed;

static device_probe_t mt7902_bt_probe;

static const STRUCT_USB_HOST_ID mt7902_bt_devs[] = {
	{ USB_VPI(USB_VENDOR_MEDIATEK, 0x7902, 0) },
	/* Common OEM IDs used by MT7902 combo modules. */
	{ USB_VPI(0x13d3, 0x3579, 0) },
	{ USB_VPI(0x13d3, 0x3580, 0) },
	{ USB_VPI(0x13d3, 0x3594, 0) },
	{ USB_VPI(0x13d3, 0x3596, 0) },
};

/* Send a vendor HCI command, then poll MediaTek's vendor control endpoint
 * until its WMT event arrives. The event is separate from HCI command-complete.
 */
static usb_error_t
mt7902_bt_wmt(struct usb_device *udev, uint8_t op, uint8_t flag,
	const void *payload, size_t payload_len, uint8_t *result)
{
	struct usb_device_request req;
	struct mt7902_bt_wmt_hdr *wmt;
	struct mt7902_bt_wmt_event evt;
	uint8_t *cmd;
	uint16_t actual;
	size_t cmd_len;
	unsigned int i;
	usb_error_t error;

	if (payload_len > 250 || payload_len + sizeof(*wmt) > UINT8_MAX)
		return (USB_ERR_INVAL);
	cmd_len = sizeof(*wmt) + payload_len;
	cmd = malloc(cmd_len + 4, M_TEMP, M_WAITOK | M_ZERO);
	*(uint16_t *)cmd = htole16(MTK_WMT_OPCODE);
	cmd[2] = cmd_len;
	wmt = (struct mt7902_bt_wmt_hdr *)(cmd + 3);
	wmt->dir = 1;
	wmt->op = op;
	wmt->len = htole16(payload_len + 1);
	wmt->flag = flag;
	if (payload_len != 0)
		memcpy(cmd + 3 + sizeof(*wmt), payload, payload_len);

	bzero(&req, sizeof(req));
	req.bmRequestType = 0x20; /* Host-to-device, class, device. */
	req.bRequest = 0;
	req.wIndex[0] = 0;
	USETW(req.wLength, cmd_len + 3);
	error = usbd_do_request(udev, NULL, &req, cmd);
	free(cmd, M_TEMP);
	if (error != USB_ERR_NORMAL_COMPLETION)
		return (error);

	for (i = 0; i < 200; i++) {
		DELAY(500);
		bzero(&req, sizeof(req));
		req.bmRequestType = 0xc0; /* Device-to-host, vendor, device. */
		req.bRequest = 1;
		USETW(req.wValue, 48);
		USETW(req.wIndex, 0);
		USETW(req.wLength, sizeof(evt));
		actual = 0;
		bzero(&evt, sizeof(evt));
		error = usbd_do_request_flags(udev, NULL, &req, &evt,
		    USB_SHORT_XFER_OK, &actual, 1000);
		if (error != USB_ERR_NORMAL_COMPLETION)
			return (error);
		if (actual == 0)
			continue;
		if (!mt7902_bt_wmt_event_valid((const uint8_t *)&evt,
		    actual, op) || evt.dir != 2)
			continue;
		if (result != NULL)
			*result = evt.flag;
		return (USB_ERR_NORMAL_COMPLETION);
	}
	return (USB_ERR_TIMEOUT);
}

static int
mt7902_bt_fw_download(struct usb_device *udev)
{
	const struct mt7902_bt_global_desc *gd;
	const struct mt7902_bt_section_map *map;
	const struct firmware *fw;
	const uint8_t *bytes, *secmap, *data;
	size_t maps_end;
	uint32_t nsections, offset, size;
	uint8_t status, init[53];
	size_t left, chunk;
	unsigned int i;
	int error;

	fw = firmware_get(MT7902_BT_FW);
	if (fw == NULL)
		return (ENOENT);
	bytes = fw->data;
	error = EINVAL;
	if (fw->datasize < 96)
		goto out;
	gd = (const struct mt7902_bt_global_desc *)(bytes + 32);
	nsections = le32toh(gd->section_num);
	if (nsections == 0 || nsections > MTK_BT_MAX_SECTION)
		goto out;
	maps_end = 96 + (size_t)nsections * 64;
	if (maps_end > fw->datasize)
		goto out;
	secmap = bytes + 96;

	for (i = 0; i < nsections; i++) {
		map = (const struct mt7902_bt_section_map *)(secmap + i * 64);
		offset = le32toh(map->offset);
		size = le32toh(map->spec[1]); /* bin_info_spec.dlsize */
		if (size == 0)
			continue;
		if (!mt7902_bt_fw_section_valid(fw->datasize, maps_end,
		    offset, size))
			goto out;
		/* Ask ROM to accept the section map entry before sending bytes. */
		init[0] = 0;
		memcpy(init + 1, secmap + i * 64 + 12, 52);
		error = EIO;
		{
			unsigned int retry;
			for (retry = 0; retry < 20; retry++) {
				usb_error_t ue = mt7902_bt_wmt(udev,
				    MTK_WMT_PATCH_DOWNLOAD, 0, init, sizeof(init),
				    &status);
				if (ue != USB_ERR_NORMAL_COMPLETION) {
					error = EIO;
					goto out;
				}
				if (status == 0)
					break;
				if (status == MTK_WMT_PATCH_DONE)
					goto next_section;
				if (status != MTK_WMT_PATCH_PROGRESS)
					goto out;
				pause("mt79bt", hz / 10);
			}
			if (retry == 20)
				goto out;
		}
		data = bytes + offset;
		left = size;
		while (left != 0) {
			chunk = MIN(left, MTK_BT_CHUNK);
			status = mt7902_bt_download_flag(left == size, left, chunk);
			if (mt7902_bt_wmt(udev, MTK_WMT_PATCH_DOWNLOAD,
			    status, data, chunk, &status) != USB_ERR_NORMAL_COMPLETION ||
			    status == MTK_WMT_PATCH_PROGRESS) {
				error = EIO;
				goto out;
			}
			data += chunk;
			left -= chunk;
		}
next_section:
		continue;
	}
	pause("mt79bt", hz / 10);
	error = 0;
out:
	firmware_put(fw, FIRMWARE_UNLOAD);
	return (error);
}

static int
mt7902_bt_probe(device_t dev)
{
	struct usb_attach_arg *uaa;
	int error;

	uaa = device_get_ivars(dev);
	if (uaa == NULL || uaa->usb_mode != USB_MODE_HOST ||
	    uaa->info.bIfaceIndex != 0)
		return (ENXIO);
	error = usbd_lookup_id_by_uaa(mt7902_bt_devs,
	    sizeof(mt7902_bt_devs), uaa);
	if (error != 0)
		return (error);
	error = mt7902_bt_fw_download(uaa->device);
	if (error != 0) {
		device_printf(dev, "Bluetooth firmware setup failed: %d\n", error);
		return (ENXIO);
	}
	device_set_desc(dev, "MediaTek MT7902 Bluetooth (ng_ubt)");
	/* Prefer this exact hardware match over ng_ubt's generic class match. */
	return (BUS_PROBE_VENDOR);
}

static device_method_t mt7902_bt_methods[] = {
	DEVMETHOD(device_probe, mt7902_bt_probe),
	DEVMETHOD_END
};

/* ng_ubt intentionally does not export ubt_driver. Resolve its registered
 * class through the bus devclass at module load, then use it as our base
 * class. MODULE_DEPEND keeps that class alive while this subclass is loaded.
 */
static kobj_class_t mt7902_bt_baseclasses[] = { NULL, NULL };
static driver_t mt7902_bt_driver = {
	.name = "ubt",
	.methods = mt7902_bt_methods,
	.size = sizeof(struct ubt_softc),
	.baseclasses = mt7902_bt_baseclasses,
};
static struct driver_module_data mt7902_bt_driver_mod = {
	.dmd_busname = "uhub",
	.dmd_driver = &mt7902_bt_driver,
	.dmd_pass = BUS_PASS_DEFAULT,
};

static int
mt7902_bt_module_event(module_t mod, int event, void *arg)
{
	driver_t **drivers, *base;
	devclass_t dc;
	int count, error, i;

	if (event == MOD_LOAD) {
		dc = devclass_find("uhub");
		if (dc == NULL)
			return (ENXIO);
		error = devclass_get_drivers(dc, &drivers, &count);
		if (error != 0)
			return (error);
		base = NULL;
		for (i = 0; i < count; i++) {
			if (strcmp(drivers[i]->name, "ubt") == 0) {
				base = drivers[i];
				break;
			}
		}
		free(drivers, M_TEMP);
		if (base == NULL)
			return (ENXIO);
		mt7902_bt_baseclasses[0] = base;
	}
	error = driver_module_handler(mod, event, &mt7902_bt_driver_mod);
	if (event == MOD_UNLOAD && error == 0)
		mt7902_bt_baseclasses[0] = NULL;
	(void)arg;
	return (error);
}

static moduledata_t mt7902_bt_mod = {
	.name = "uhub/ng_ubt_mt7902",
	.evhand = mt7902_bt_module_event,
	.priv = &mt7902_bt_driver_mod,
};
DECLARE_MODULE(ng_ubt_mt7902, mt7902_bt_mod, SI_SUB_DRIVERS,
	SI_ORDER_MIDDLE);
MODULE_VERSION(ng_ubt_mt7902, NG_BLUETOOTH_VERSION);
MODULE_DEPEND(ng_ubt_mt7902, netgraph, NG_ABI_VERSION, NG_ABI_VERSION,
	 NG_ABI_VERSION);
MODULE_DEPEND(ng_ubt_mt7902, ng_hci, NG_BLUETOOTH_VERSION,
	 NG_BLUETOOTH_VERSION, NG_BLUETOOTH_VERSION);
MODULE_DEPEND(ng_ubt_mt7902, ng_ubt, NG_BLUETOOTH_VERSION,
	 NG_BLUETOOTH_VERSION, NG_BLUETOOTH_VERSION);
MODULE_DEPEND(ng_ubt_mt7902, usb, 1, 1, 1);
