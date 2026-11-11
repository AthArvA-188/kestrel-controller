// USB device: XInput (Xbox 360 wired protocol on a vendor interface) or
// DInput (HID gamepad), chosen when the stack starts.
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "device/usbd_pvt.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ks_reports.h"
#include "tusb.h"

static const char *TAG = "usb";

#define EP_HID_IN 0x81
#define EP_HID_OUT 0x01
#define EP_XINPUT_IN 0x81
#define EP_XINPUT_OUT 0x02
#define XINPUT_EP_SIZE 32
// Request code Windows uses to fetch the Microsoft OS 1.0 feature descriptors.
#define MS_OS_VENDOR_CODE 0x90

#define XINPUT_CFG_LEN (9 + 9 + 17 + 7 + 7)
#define HID_CFG_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)
// Offset of wDescriptorLength in the HID configuration: configuration (9),
// interface (9), then seven bytes into the HID descriptor.
#define HID_REPORT_LEN_OFFSET 25

static ks_mode_t usb_mode;
static char serial[20];
static usb_phy_handle_t phy;
static volatile bool mounted, suspended, resend;
static uint8_t hid_cfg[HID_CFG_LEN];

// Both modes use the placeholder ID. They differ in bcdDevice and in the
// serial number so that Windows keeps separate driver bindings for them.
static const tusb_desc_device_t dev_xinput = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0xFF,
    .bDeviceSubClass = 0xFF,
    .bDeviceProtocol = 0xFF,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = KS_USB_VID,
    .idProduct = KS_USB_PID,
    .bcdDevice = 0x0114,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const tusb_desc_device_t dev_dinput = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = KS_USB_VID,
    .idProduct = KS_USB_PID,
    .bcdDevice = 0x0200,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

// One interface laid out like interface 0 of the Xbox 360 wired controller:
// class FF, subclass 5D, protocol 01, its 17-byte class descriptor, and two
// interrupt endpoints. The IN endpoint polls every millisecond.
static const uint8_t xinput_cfg[XINPUT_CFG_LEN] = {
    9, TUSB_DESC_CONFIGURATION, U16_TO_U8S_LE(XINPUT_CFG_LEN), 1, 1, 0, 0xA0, 250,
    9, TUSB_DESC_INTERFACE, 0, 0, 2, 0xFF, 0x5D, 0x01, 0,
    17, 0x21, 0x10, 0x01, 0x01, 0x25, EP_XINPUT_IN, 0x14, 0x03, 0x03, 0x03, 0x04, 0x13,
    EP_XINPUT_OUT, 0x08, 0x03, 0x03,
    7, TUSB_DESC_ENDPOINT, EP_XINPUT_IN, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(XINPUT_EP_SIZE), 1,
    7, TUSB_DESC_ENDPOINT, EP_XINPUT_OUT, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(XINPUT_EP_SIZE), 8,
};

// Microsoft OS 1.0 extended compat ID descriptor. It names XUSB10 for
// interface 0, which makes Windows load its XInput driver for this device.
static const uint8_t ms_compat_id[40] = {
    0x28, 0x00, 0x00, 0x00, // dwLength
    0x00, 0x01,             // bcdVersion 1.0
    0x04, 0x00,             // wIndex: extended compat ID
    0x01,                   // one function section
    0, 0, 0, 0, 0, 0, 0,
    0x00,                   // bFirstInterfaceNumber
    0x01,
    'X', 'U', 'S', 'B', '1', '0', 0, 0, // compatibleID
    0, 0, 0, 0, 0, 0, 0, 0,             // subCompatibleID
    0, 0, 0, 0, 0, 0,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)(usb_mode == KS_MODE_USB_XINPUT ? &dev_xinput : &dev_dinput);
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    return usb_mode == KS_MODE_USB_XINPUT ? xinput_cfg : hid_cfg;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static uint16_t desc[32];
    const char *text = NULL;
    size_t n = 0;

    if (index == 0) {
        desc[1] = 0x0409; // English (United States)
        n = 1;
    } else if (index == 0xEE && usb_mode == KS_MODE_USB_XINPUT) {
        // Microsoft OS string descriptor: the signature, then the request
        // code for the feature descriptors.
        static const char signature[] = "MSFT100";
        for (n = 0; n < 7; n++) {
            desc[1 + n] = (uint16_t)signature[n];
        }
        desc[1 + n++] = MS_OS_VENDOR_CODE;
    } else {
        switch (index) {
        case 1: text = "Kestrel"; break;
        case 2: text = usb_mode == KS_MODE_USB_XINPUT ? "Kestrel Controller (XInput)"
                                                      : "Kestrel Controller"; break;
        case 3: text = serial; break;
        default: return NULL;
        }
        for (n = 0; text[n] && n < 31; n++) {
            desc[1 + n] = (uint16_t)text[n];
        }
    }
    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return desc;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t *req)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (usb_mode == KS_MODE_USB_XINPUT && req->bRequest == MS_OS_VENDOR_CODE &&
        req->wIndex == 0x0004 && req->bmRequestType_bit.direction == TUSB_DIR_IN) {
        return tud_control_xfer(rhport, req, (void *)(uintptr_t)ms_compat_id, sizeof ms_compat_id);
    }
    return false; // stall anything else
}

void tud_mount_cb(void)
{
    mounted = true;
    suspended = false;
    resend = true;
}

void tud_umount_cb(void)
{
    mounted = false;
    app_clear_host_state();
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    suspended = true;
    app_clear_host_state();
}

void tud_resume_cb(void)
{
    suspended = false;
    resend = true;
}

// HID class callbacks (DInput mode).

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) { return ks_hid_desc_gamepad; }

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                               uint8_t *buffer, uint16_t reqlen)
{
    ks_gamepad_t pad;
    uint8_t battery, flags;
    if (type != HID_REPORT_TYPE_INPUT || report_id != KS_HID_ID_INPUT ||
        reqlen < KS_HID_INPUT_LEN) {
        return 0;
    }
    app_get_report(&pad, &battery, &flags);
    ks_hid_build(&pad, buffer);
    return KS_HID_INPUT_LEN;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                           const uint8_t *buffer, uint16_t bufsize)
{
    ks_rumble_t rumble;
    ks_trigger_fx_cfg_t fx;
    uint8_t target;
    // A report from the OUT endpoint arrives with its ID as the first byte.
    if (report_id == 0 && bufsize > 0) {
        report_id = buffer[0];
        buffer++;
        bufsize--;
    }
    if (report_id == KS_HID_ID_RUMBLE && ks_hid_parse_rumble(buffer, bufsize, &rumble)) {
        app_set_rumble(rumble);
    } else if (report_id == KS_HID_ID_TRIGGER_FX &&
               ks_hid_parse_trigger_fx(buffer, bufsize, &target, &fx)) {
        app_set_trigger_fx(target, &fx);
    }
}

// Class driver for the XInput interface.

static struct {
    uint8_t ep_in, ep_out;
    CFG_TUSB_MEM_ALIGN uint8_t in_buf[XINPUT_EP_SIZE];
    CFG_TUSB_MEM_ALIGN uint8_t out_buf[XINPUT_EP_SIZE];
} xin;

static void xinput_init(void) { memset(&xin, 0, sizeof xin); }

static bool xinput_deinit(void) { return true; }

static void xinput_reset(uint8_t rhport) { memset(&xin, 0, sizeof xin); }

static uint16_t xinput_open(uint8_t rhport, const tusb_desc_interface_t *itf, uint16_t max_len)
{
    if (itf->bInterfaceClass != 0xFF || itf->bInterfaceSubClass != 0x5D ||
        itf->bInterfaceProtocol != 0x01) {
        return 0;
    }
    uint16_t len = itf->bLength;
    const uint8_t *desc = tu_desc_next(itf);
    while (len < max_len && tu_desc_type(desc) != TUSB_DESC_INTERFACE) {
        if (tu_desc_type(desc) == TUSB_DESC_ENDPOINT) {
            const tusb_desc_endpoint_t *ep = (const tusb_desc_endpoint_t *)desc;
            if (!usbd_edpt_open(rhport, ep)) {
                return 0;
            }
            if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) {
                xin.ep_in = ep->bEndpointAddress;
            } else {
                xin.ep_out = ep->bEndpointAddress;
            }
        }
        len = (uint16_t)(len + tu_desc_len(desc));
        desc = tu_desc_next(desc);
    }
    if (xin.ep_out) {
        usbd_edpt_xfer(rhport, xin.ep_out, xin.out_buf, sizeof xin.out_buf, false);
    }
    return len;
}

static bool xinput_control(uint8_t rhport, uint8_t stage, const tusb_control_request_t *req)
{
    return false; // the protocol needs no class requests
}

static bool xinput_xfer(uint8_t rhport, uint8_t ep, xfer_result_t result, uint32_t bytes)
{
    if (ep == xin.ep_out) {
        ks_rumble_t rumble;
        if (result == XFER_RESULT_SUCCESS && ks_xinput_parse_out(xin.out_buf, bytes, &rumble)) {
            app_set_rumble(rumble);
        }
        usbd_edpt_xfer(rhport, xin.ep_out, xin.out_buf, sizeof xin.out_buf, false);
    }
    return true;
}

static const usbd_class_driver_t xinput_driver = {
    .name = "XINPUT",
    .init = xinput_init,
    .deinit = xinput_deinit,
    .reset = xinput_reset,
    .open = xinput_open,
    .control_xfer_cb = xinput_control,
    .xfer_cb = xinput_xfer,
};

const usbd_class_driver_t *usbd_app_driver_get_cb(uint8_t *count)
{
    *count = 1;
    return &xinput_driver;
}

static bool xinput_send(const uint8_t *report)
{
    if (!xin.ep_in || !usbd_edpt_claim(0, xin.ep_in)) {
        return false;
    }
    memcpy(xin.in_buf, report, KS_XINPUT_REPORT_LEN);
    return usbd_edpt_xfer(0, xin.ep_in, xin.in_buf, KS_XINPUT_REPORT_LEN, false);
}

// Sends a report when the state changed. Called once per millisecond.
void usb_dev_send(const ks_gamepad_t *pad)
{
    static uint8_t last[KS_XINPUT_REPORT_LEN];
    uint8_t report[KS_XINPUT_REPORT_LEN] = {0};
    bool xinput = usb_mode == KS_MODE_USB_XINPUT;
    size_t len = xinput ? KS_XINPUT_REPORT_LEN : KS_HID_INPUT_LEN;

    if (!mounted) {
        return;
    }
    if (xinput) {
        ks_xinput_build(pad, report);
    } else {
        ks_hid_build(pad, report);
    }
    if (!resend && memcmp(report, last, len) == 0) {
        return;
    }
    if (suspended) {
        tud_remote_wakeup(); // an input wakes a sleeping host that allows it
        return;
    }
    bool sent = xinput ? xinput_send(report)
                       : tud_hid_ready() && tud_hid_report(KS_HID_ID_INPUT, report, (uint16_t)len);
    if (sent) {
        memcpy(last, report, len);
        resend = false;
    }
}

// The pad powers itself, so it must not pull D+ up without bus power. The
// charger's power-good input stands in for VBUS detection.
void usb_dev_set_vbus(bool present)
{
    static int8_t state = -1;
    if (state == (int8_t)present) {
        return;
    }
    state = (int8_t)present;
    if (present) {
        tud_connect();
    } else {
        tud_disconnect();
        mounted = false;
        app_clear_host_state();
    }
}

void usb_dev_status(link_status_t *st)
{
    bool up = mounted && !suspended;
    st->link = up ? KS_LINK_CONNECTED : KS_LINK_IDLE;
    snprintf(st->host, sizeof st->host, "%s", up ? "USB host" : "");
    snprintf(st->detail, sizeof st->detail, "%s",
             !up ? "Waiting for a USB host"
                 : usb_mode == KS_MODE_USB_XINPUT ? "Xbox 360 protocol" : "HID gamepad");
}

static void usb_task(void *arg)
{
    for (;;) {
        tud_task();
    }
}

void usb_dev_start(ks_mode_t mode)
{
    static const uint8_t hid_template[HID_CFG_LEN] = {
        TUD_CONFIG_DESCRIPTOR(1, 1, 0, HID_CFG_LEN, 0xA0, 500),
        TUD_HID_INOUT_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, 0, EP_HID_OUT, EP_HID_IN, 64, 1),
    };
    uint8_t mac[6] = {0};
    usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
    };
    tusb_rhport_init_t init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO};

    usb_mode = mode;
    memcpy(hid_cfg, hid_template, sizeof hid_cfg);
    hid_cfg[HID_REPORT_LEN_OFFSET] = (uint8_t)ks_hid_desc_gamepad_len;
    hid_cfg[HID_REPORT_LEN_OFFSET + 1] = (uint8_t)(ks_hid_desc_gamepad_len >> 8);
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(serial, sizeof serial, "KS%c-%02X%02X%02X%02X%02X%02X",
             mode == KS_MODE_USB_XINPUT ? 'X' : 'D', mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_ERROR_CHECK(usb_new_phy(&phy_cfg, &phy));
    if (!tusb_init(0, &init)) {
        ESP_LOGE(TAG, "TinyUSB did not start");
        return;
    }
    xTaskCreatePinnedToCore(usb_task, "usb", 4096, NULL, 8, NULL, 1);
    ESP_LOGI(TAG, "started as %s, serial %s", ks_mode_name(mode), serial);
}
