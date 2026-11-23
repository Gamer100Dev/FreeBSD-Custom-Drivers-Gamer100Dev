#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/malloc.h>
#include <sys/mutex.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include <dev/evdev/evdev.h>
#include <dev/evdev/input.h>

#define PS3_VENDOR_ID        0x054c
#define PS3_PRODUCT_ID       0x0268
#define PS3_INTERFACE        0
#define PS3_INTR_IN          0x81

#define PS3_REPORT_SIZE      64
#define PS3_REPORT_LENGTH    49
#define PS3_MIN_REPORT_LEN   22

#define PS3_OUTPUT_SIZE      36
#define PS3_OUTPUT_DATA_SIZE 35

#define PS3_XFER_IN          0
#define PS3_XFER_RUMBLE      1
#define PS3_XFER_COUNT       2

#define PS3_RPT_ID           0x01
#define PS3_HID_SET_REPORT   0x09
#define PS3_HID_OUTPUT       0x02

#define PS3_BUTTONS_1        2
#define PS3_BUTTONS_2        3
#define PS3_BUTTONS_3        4

#define PS3_LEFT_X           6
#define PS3_LEFT_Y           7
#define PS3_RIGHT_X          8
#define PS3_RIGHT_Y          9

#define PS3_L2_OFFSET        18
#define PS3_R2_OFFSET        19

#define PS3_BTN_SELECT       0x01
#define PS3_BTN_L3           0x02
#define PS3_BTN_R3           0x04
#define PS3_BTN_START        0x08
#define PS3_BTN_UP           0x10
#define PS3_BTN_RIGHT        0x20
#define PS3_BTN_DOWN         0x40
#define PS3_BTN_LEFT         0x80

#define PS3_BTN_L2           0x01
#define PS3_BTN_R2           0x02
#define PS3_BTN_L1           0x04
#define PS3_BTN_R1           0x08
#define PS3_BTN_TRIANGLE     0x10
#define PS3_BTN_CIRCLE       0x20
#define PS3_BTN_CROSS        0x40
#define PS3_BTN_SQUARE       0x80

#define PS3_BTN_PS           0x01

struct ps3_softc {
	device_t sc_dev;
	struct usb_device *sc_udev;
	struct usb_xfer *sc_xfer[PS3_XFER_COUNT];
	struct evdev_dev *sc_evdev;

	uint8_t sc_prev[PS3_REPORT_SIZE];
	int sc_have_prev;

	uint8_t sc_rumble_strong;
	uint8_t sc_rumble_weak;
	int sc_rumble_pending;

	uint8_t sc_output[PS3_OUTPUT_SIZE];
};

static int ps3_probe(device_t dev);
static int ps3_attach(device_t dev);
static int ps3_detach(device_t dev);

static void ps3_intr_callback(
    struct usb_xfer *xfer,
    usb_error_t error);

static void ps3_rumble_callback(
    struct usb_xfer *xfer,
    usb_error_t error);

static void ps3_ev_event(
    struct evdev_dev *evdev,
    uint16_t type,
    uint16_t code,
    int32_t value);

static const struct evdev_methods ps3_evdev_methods = {
	.ev_event = ps3_ev_event
};

static const STRUCT_USB_HOST_ID ps3_devs[] = {
	{
		USB_VPI(PS3_VENDOR_ID, PS3_PRODUCT_ID, 0),
	}
};

static const struct usb_config ps3_config[PS3_XFER_COUNT] = {
	[PS3_XFER_IN] = {
		.type = UE_INTERRUPT,
		.endpoint = PS3_INTR_IN,
		.direction = UE_DIR_IN,
		.bufsize = PS3_REPORT_SIZE,
		.frames = 1,
		.flags = {
			.short_xfer_ok = 1,
		},
		.callback = ps3_intr_callback,
		.timeout = 0,
	},

	[PS3_XFER_RUMBLE] = {
		.type = UE_CONTROL,
		.endpoint = 0,
		.direction = UE_DIR_OUT,
		.bufsize = sizeof(struct usb_device_request) +
		    PS3_OUTPUT_DATA_SIZE,
		.frames = 1,
		.callback = ps3_rumble_callback,
		.timeout = 1000,
	}
};

static device_method_t ps3_methods[] = {
	DEVMETHOD(device_probe, ps3_probe),
	DEVMETHOD(device_attach, ps3_attach),
	DEVMETHOD(device_detach, ps3_detach),
	DEVMETHOD_END
};

static driver_t ps3_driver = {
	"ps3joy",
	ps3_methods,
	sizeof(struct ps3_softc)
};

DRIVER_MODULE(ps3joy, uhub, ps3_driver, NULL, NULL);
MODULE_VERSION(ps3joy, 1);
MODULE_DEPEND(ps3joy, usb, 1, 1, 1);
MODULE_DEPEND(ps3joy, evdev, 1, 1, 1);
USB_PNP_HOST_INFO(ps3_devs);

static int
ps3_probe(device_t dev)
{
	struct usb_attach_arg *uaa;

	uaa = device_get_ivars(dev);

	if (uaa == NULL)
		return (ENXIO);

	if (uaa->usb_mode != USB_MODE_HOST)
		return (ENXIO);

	if (uaa->info.bIfaceIndex != PS3_INTERFACE)
		return (ENXIO);

	return (usbd_lookup_id_by_uaa(
	    ps3_devs,
	    sizeof(ps3_devs),
	    uaa));
}

static int
ps3_attach(device_t dev)
{
	struct ps3_softc *sc;
	struct usb_attach_arg *uaa;
	int error;

	sc = device_get_softc(dev);
	uaa = device_get_ivars(dev);

	sc->sc_dev = dev;
	sc->sc_udev = uaa->device;

	sc->sc_xfer[PS3_XFER_IN] = NULL;
	sc->sc_xfer[PS3_XFER_RUMBLE] = NULL;

	sc->sc_evdev = NULL;

	sc->sc_have_prev = 0;
	sc->sc_rumble_strong = 0;
	sc->sc_rumble_weak = 0;
	sc->sc_rumble_pending = 0;

	memset(
	    sc->sc_prev,
	    0,
	    sizeof(sc->sc_prev));

	memset(
	    sc->sc_output,
	    0,
	    sizeof(sc->sc_output));

	sc->sc_evdev = evdev_alloc();
	if (sc->sc_evdev == NULL) {
		device_printf(
		    dev,
		    "evdev_alloc failed\n");
		return (ENOMEM);
	}

	evdev_set_name(
	    sc->sc_evdev,
	    "PS3 Controller");

	evdev_set_phys(
	    sc->sc_evdev,
	    device_get_nameunit(dev));

	evdev_set_id(
	    sc->sc_evdev,
	    BUS_USB,
	    PS3_VENDOR_ID,
	    PS3_PRODUCT_ID,
	    0);

	evdev_set_methods(
	    sc->sc_evdev,
	    sc,
	    &ps3_evdev_methods);

	evdev_support_event(
	    sc->sc_evdev,
	    EV_KEY);

	evdev_support_event(
	    sc->sc_evdev,
	    EV_ABS);

	evdev_support_event(
	    sc->sc_evdev,
	    EV_FF);

	evdev_support_key(
	    sc->sc_evdev,
	    BTN_A);

	evdev_support_key(
	    sc->sc_evdev,
	    BTN_B);

	evdev_support_key(
	    sc->sc_evdev,
	    BTN_X);

	evdev_support_key(
	    sc->sc_evdev,
	    BTN_Y);

	evdev_support_key(
	    sc->sc_evdev,
	    BTN_TL);

	evdev_support_key(
	    sc->sc_evdev,
	    BTN_TR);

	evdev_support_key(
	    sc->sc_evdev,
	    BTN_TL2);

	evdev_support_key(
	    sc->sc_evdev,
	    BTN_TR2);

	evdev_support_key(
	    sc->sc_evdev,
	    BTN_THUMBL);

	evdev_support_key(
	    sc->sc_evdev,
	    BTN_THUMBR);

	evdev_support_key(
	    sc->sc_evdev,
	    BTN_DPAD_UP);

	evdev_support_key(
	    sc->sc_evdev,
	    BTN_DPAD_RIGHT);

	evdev_support_key(
	    sc->sc_evdev,
	    BTN_DPAD_DOWN);

	evdev_support_key(
	    sc->sc_evdev,
	    BTN_DPAD_LEFT);

	evdev_support_key(
	    sc->sc_evdev,
	    BTN_SELECT);

	evdev_support_key(
	    sc->sc_evdev,
	    BTN_START);

	evdev_support_key(
	    sc->sc_evdev,
	    BTN_MODE);

	evdev_support_abs(
	    sc->sc_evdev,
	    ABS_X,
	    0, 255,
	    0, 15, 1);

	evdev_support_abs(
	    sc->sc_evdev,
	    ABS_Y,
	    0, 255,
	    0, 15, 1);

	evdev_support_abs(
	    sc->sc_evdev,
	    ABS_RX,
	    0, 255,
	    0, 15, 1);

	evdev_support_abs(
	    sc->sc_evdev,
	    ABS_RY,
	    0, 255,
	    0, 15, 1);

	evdev_support_abs(
	    sc->sc_evdev,
	    ABS_Z,
	    0, 255,
	    0, 0, 1);

	evdev_support_abs(
	    sc->sc_evdev,
	    ABS_RZ,
	    0, 255,
	    0, 0, 1);

	error = evdev_register(
	    sc->sc_evdev);

	if (error != 0) {
		device_printf(
		    dev,
		    "evdev_register failed: %d\n",
		    error);

		evdev_free(
		    sc->sc_evdev);

		sc->sc_evdev = NULL;

		return (error);
	}

	error = usbd_transfer_setup(
	    sc->sc_udev,
	    &uaa->info.bIfaceIndex,
	    sc->sc_xfer,
	    ps3_config,
	    PS3_XFER_COUNT,
	    sc,
	    &Giant);

	if (error != 0) {
		device_printf(
		    dev,
		    "usbd_transfer_setup failed: %d\n",
		    error);

		evdev_unregister(
		    sc->sc_evdev);

		evdev_free(
		    sc->sc_evdev);

		sc->sc_evdev = NULL;

		return (error);
	}

	usbd_transfer_start(
	    sc->sc_xfer[PS3_XFER_IN]);

	device_printf(
	    dev,
	    "attached\n");

	return (0);
}

static int
ps3_detach(device_t dev)
{
	struct ps3_softc *sc;

	sc = device_get_softc(dev);

	if (sc->sc_xfer[PS3_XFER_IN] != NULL)
		usbd_transfer_stop(
		    sc->sc_xfer[PS3_XFER_IN]);

	if (sc->sc_xfer[PS3_XFER_RUMBLE] != NULL)
		usbd_transfer_stop(
		    sc->sc_xfer[PS3_XFER_RUMBLE]);

	usbd_transfer_unsetup(
	    sc->sc_xfer,
	    PS3_XFER_COUNT);

	sc->sc_xfer[PS3_XFER_IN] = NULL;
	sc->sc_xfer[PS3_XFER_RUMBLE] = NULL;

	if (sc->sc_evdev != NULL) {
		evdev_unregister(
		    sc->sc_evdev);

		evdev_free(
		    sc->sc_evdev);

		sc->sc_evdev = NULL;
	}

	sc->sc_udev = NULL;

	return (0);
}

static void
ps3_build_output_report(struct ps3_softc *sc)
{
	memset(
	    sc->sc_output,
	    0,
	    sizeof(sc->sc_output));

	sc->sc_output[0] = PS3_RPT_ID;
	sc->sc_output[1] = 0x00;
	sc->sc_output[2] = 0xff;
	sc->sc_output[3] =
	    sc->sc_rumble_weak != 0 ? 0x01 : 0x00;
	sc->sc_output[4] = 0xff;
	sc->sc_output[5] =
	    sc->sc_rumble_strong;
}

static void
ps3_rumble_callback(
    struct usb_xfer *xfer,
    usb_error_t error)
{
	struct ps3_softc *sc;
	struct usb_device_request req;
	struct usb_page_cache *pc;

	sc = usbd_xfer_softc(xfer);

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_SETUP:
		ps3_build_output_report(sc);

		req.bmRequestType =
		    UT_WRITE_CLASS_INTERFACE;

		req.bRequest =
		    PS3_HID_SET_REPORT;

		USETW(
		    req.wValue,
		    (PS3_HID_OUTPUT << 8) |
		    PS3_RPT_ID);

		USETW(
		    req.wIndex,
		    PS3_INTERFACE);

		USETW(
		    req.wLength,
		    PS3_OUTPUT_DATA_SIZE);

		pc = usbd_xfer_get_frame(
		    xfer,
		    0);

		usbd_copy_in(
		    pc,
		    0,
		    &req,
		    sizeof(req));

		usbd_copy_in(
		    pc,
		    sizeof(req),
		    &sc->sc_output[1],
		    PS3_OUTPUT_DATA_SIZE);

		usbd_xfer_set_frame_len(
		    xfer,
		    0,
		    sizeof(req) +
		    PS3_OUTPUT_DATA_SIZE);

		usbd_transfer_submit(
		    xfer);

		return;

	case USB_ST_TRANSFERRED:
		sc->sc_rumble_pending = 0;
		return;

	default:
		sc->sc_rumble_pending = 0;
		return;
	}
}

static void
ps3_set_rumble(
    struct ps3_softc *sc,
    uint8_t strong,
    uint8_t weak)
{
	if (sc->sc_xfer[PS3_XFER_RUMBLE] == NULL)
		return;

	sc->sc_rumble_strong = strong;
	sc->sc_rumble_weak = weak;

	if (sc->sc_rumble_pending)
		return;

	sc->sc_rumble_pending = 1;

	usbd_transfer_start(
	    sc->sc_xfer[PS3_XFER_RUMBLE]);
}

static void
ps3_ev_event(
    struct evdev_dev *evdev,
    uint16_t type,
    uint16_t code,
    int32_t value)
{
	struct ps3_softc *sc;

	sc = evdev_get_softc(evdev);

	if (sc == NULL ||
	    type != EV_FF)
		return;

	if (code != FF_RUMBLE)
		return;

	{
		uint8_t strong;
		uint8_t weak;

		strong = (uint32_t)value >> 8;
		weak = (uint32_t)value & 0xff;

		ps3_set_rumble(
		    sc,
		    strong,
		    weak);
	}
}

static void
ps3_intr_callback(
    struct usb_xfer *xfer,
    usb_error_t error)
{
	struct ps3_softc *sc;
	struct usb_page_cache *pc;
	uint8_t buf[PS3_REPORT_SIZE];
	int actlen;
	int i;

	static const struct {
		uint8_t mask;
		uint16_t key;
	} dpad_map[] = {
		{PS3_BTN_SELECT, BTN_SELECT},
		{PS3_BTN_L3, BTN_THUMBL},
		{PS3_BTN_R3, BTN_THUMBR},
		{PS3_BTN_START, BTN_START},
		{PS3_BTN_UP, BTN_DPAD_UP},
		{PS3_BTN_RIGHT, BTN_DPAD_RIGHT},
		{PS3_BTN_DOWN, BTN_DPAD_DOWN},
		{PS3_BTN_LEFT, BTN_DPAD_LEFT}
	};

	static const struct {
		uint8_t mask;
		uint16_t key;
	} button_map[] = {
		{PS3_BTN_L2, BTN_TL2},
		{PS3_BTN_R2, BTN_TR2},
		{PS3_BTN_L1, BTN_TL},
		{PS3_BTN_R1, BTN_TR},
		{PS3_BTN_TRIANGLE, BTN_X},
		{PS3_BTN_CIRCLE, BTN_B},
		{PS3_BTN_CROSS, BTN_A},
		{PS3_BTN_SQUARE, BTN_Y}
	};

	sc = usbd_xfer_softc(xfer);

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED:
		actlen = 0;

		usbd_xfer_status(
		    xfer,
		    &actlen,
		    NULL,
		    NULL,
		    NULL);

		if (actlen < PS3_MIN_REPORT_LEN)
			goto submit;

		pc = usbd_xfer_get_frame(
		    xfer,
		    0);

		memset(
		    buf,
		    0,
		    sizeof(buf));

		usbd_copy_out(
		    pc,
		    0,
		    buf,
		    MIN(
		        actlen,
		        PS3_REPORT_SIZE));

		if (buf[0] != PS3_RPT_ID)
			goto submit;

		for (i = 0; i < nitems(dpad_map); i++) {
			int cur;
			int prev;

			cur =
			    (buf[PS3_BUTTONS_1] &
			    dpad_map[i].mask) != 0;

			if (sc->sc_have_prev)
				prev =
				    (sc->sc_prev[PS3_BUTTONS_1] &
				    dpad_map[i].mask) != 0;
			else
				prev = -1;

			if (cur != prev)
				evdev_push_key(
				    sc->sc_evdev,
				    dpad_map[i].key,
				    cur);
		}

		for (i = 0; i < nitems(button_map); i++) {
			int cur;
			int prev;

			cur =
			    (buf[PS3_BUTTONS_2] &
			    button_map[i].mask) != 0;

			if (sc->sc_have_prev)
				prev =
				    (sc->sc_prev[PS3_BUTTONS_2] &
				    button_map[i].mask) != 0;
			else
				prev = -1;

			if (cur != prev)
				evdev_push_key(
				    sc->sc_evdev,
				    button_map[i].key,
				    cur);
		}

		{
			int cur;
			int prev;

			cur =
			    (buf[PS3_BUTTONS_3] &
			    PS3_BTN_PS) != 0;

			if (sc->sc_have_prev)
				prev =
				    (sc->sc_prev[PS3_BUTTONS_3] &
				    PS3_BTN_PS) != 0;
			else
				prev = -1;

			if (cur != prev)
				evdev_push_key(
				    sc->sc_evdev,
				    BTN_MODE,
				    cur);
		}

		if (!sc->sc_have_prev ||
		    buf[PS3_LEFT_X] !=
		    sc->sc_prev[PS3_LEFT_X])
			evdev_push_abs(
			    sc->sc_evdev,
			    ABS_X,
			    buf[PS3_LEFT_X]);

		if (!sc->sc_have_prev ||
		    buf[PS3_LEFT_Y] !=
		    sc->sc_prev[PS3_LEFT_Y])
			evdev_push_abs(
			    sc->sc_evdev,
			    ABS_Y,
			    buf[PS3_LEFT_Y]);

		if (!sc->sc_have_prev ||
		    buf[PS3_RIGHT_X] !=
		    sc->sc_prev[PS3_RIGHT_X])
			evdev_push_abs(
			    sc->sc_evdev,
			    ABS_RX,
			    buf[PS3_RIGHT_X]);

		if (!sc->sc_have_prev ||
		    buf[PS3_RIGHT_Y] !=
		    sc->sc_prev[PS3_RIGHT_Y])
			evdev_push_abs(
			    sc->sc_evdev,
			    ABS_RY,
			    buf[PS3_RIGHT_Y]);

		if (!sc->sc_have_prev ||
		    buf[PS3_L2_OFFSET] !=
		    sc->sc_prev[PS3_L2_OFFSET])
			evdev_push_abs(
			    sc->sc_evdev,
			    ABS_Z,
			    buf[PS3_L2_OFFSET]);

		if (!sc->sc_have_prev ||
		    buf[PS3_R2_OFFSET] !=
		    sc->sc_prev[PS3_R2_OFFSET])
			evdev_push_abs(
			    sc->sc_evdev,
			    ABS_RZ,
			    buf[PS3_R2_OFFSET]);

		evdev_sync(
		    sc->sc_evdev);

		memcpy(
		    sc->sc_prev,
		    buf,
		    PS3_REPORT_SIZE);

		sc->sc_have_prev = 1;

		goto submit;

	case USB_ST_SETUP:
		goto submit;

	default:
		if (error != USB_ERR_CANCELLED)
			goto submit;
		return;
	}

submit:
	usbd_xfer_set_frame_len(
	    xfer,
	    0,
	    PS3_REPORT_SIZE);

	usbd_transfer_submit(
	    xfer);
}