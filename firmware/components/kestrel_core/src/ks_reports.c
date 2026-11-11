#include "ks_reports.h"

#include <string.h>

#define HAS(g, o) (((g)->buttons & KS_OUT_BIT(o)) != 0)

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

// HID sticks are unsigned with +y pointing down.
static uint16_t hid_x(int16_t v) { return (uint16_t)(v + 32768); }
static uint16_t hid_y(int16_t v) { return (uint16_t)(32767 - v); }

void ks_xinput_build(const ks_gamepad_t *g, uint8_t out[KS_XINPUT_REPORT_LEN])
{
    memset(out, 0, KS_XINPUT_REPORT_LEN);
    out[1] = KS_XINPUT_REPORT_LEN;
    out[2] = (uint8_t)((HAS(g, KS_OUT_DUP) << 0) | (HAS(g, KS_OUT_DDOWN) << 1) |
                       (HAS(g, KS_OUT_DLEFT) << 2) | (HAS(g, KS_OUT_DRIGHT) << 3) |
                       (HAS(g, KS_OUT_MENU) << 4) | (HAS(g, KS_OUT_VIEW) << 5) |
                       (HAS(g, KS_OUT_L3) << 6) | (HAS(g, KS_OUT_R3) << 7));
    out[3] = (uint8_t)((HAS(g, KS_OUT_LB) << 0) | (HAS(g, KS_OUT_RB) << 1) |
                       (HAS(g, KS_OUT_HOME) << 2) | (HAS(g, KS_OUT_A) << 4) |
                       (HAS(g, KS_OUT_B) << 5) | (HAS(g, KS_OUT_X) << 6) |
                       (HAS(g, KS_OUT_Y) << 7));
    out[4] = g->lt;
    out[5] = g->rt;
    put16(out + 6, (uint16_t)g->lx);
    put16(out + 8, (uint16_t)g->ly);
    put16(out + 10, (uint16_t)g->rx);
    put16(out + 12, (uint16_t)g->ry);
}

bool ks_xinput_parse_out(const uint8_t *buf, size_t len, ks_rumble_t *rumble)
{
    if (len < 5 || buf[0] != 0x00 || buf[1] != 0x08) {
        return false;
    }
    rumble->left = buf[3];
    rumble->right = buf[4];
    return true;
}

uint8_t ks_hat_from_buttons(uint32_t buttons)
{
    // Indexed by up | right << 1 | down << 2 | left << 3.
    static const uint8_t hat[16] = {8, 0, 2, 1, 4, 8, 3, 2, 6, 7, 8, 0, 5, 6, 4, 8};
    unsigned i = ((buttons & KS_OUT_BIT(KS_OUT_DUP)) ? 1u : 0u) |
                 ((buttons & KS_OUT_BIT(KS_OUT_DRIGHT)) ? 2u : 0u) |
                 ((buttons & KS_OUT_BIT(KS_OUT_DDOWN)) ? 4u : 0u) |
                 ((buttons & KS_OUT_BIT(KS_OUT_DLEFT)) ? 8u : 0u);
    return hat[i];
}

// Button order follows the Linux gamepad codes so that Android and Linux give
// the buttons their usual meaning without a mapping file: A, B, C, X, Y, Z,
// TL, TR, TL2, TR2, Select, Start, Mode, ThumbL, ThumbR, and one spare.
const uint8_t ks_hid_desc_gamepad[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x85, KS_HID_ID_INPUT,
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x01,       //   Usage Minimum (1)
    0x29, 0x10,       //   Usage Maximum (16)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x10,       //   Report Count (16)
    0x81, 0x02,       //   Input (Data, Variable, Absolute)
    0x05, 0x01,       //   Usage Page (Generic Desktop)
    0x09, 0x39,       //   Usage (Hat switch)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x07,       //   Logical Maximum (7)
    0x35, 0x00,       //   Physical Minimum (0)
    0x46, 0x3B, 0x01, //   Physical Maximum (315)
    0x65, 0x14,       //   Unit (Degrees)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x42,       //   Input (Data, Variable, Absolute, Null state)
    0x65, 0x00,       //   Unit (None)
    0x45, 0x00,       //   Physical Maximum (0)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x03,       //   Input (Constant)
    0x09, 0x30,       //   Usage (X)
    0x09, 0x31,       //   Usage (Y)
    0x09, 0x32,       //   Usage (Z)
    0x09, 0x35,       //   Usage (Rz)
    0x15, 0x00,       //   Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00, // Logical Maximum (65535)
    0x75, 0x10,       //   Report Size (16)
    0x95, 0x04,       //   Report Count (4)
    0x81, 0x02,       //   Input (Data, Variable, Absolute)
    0x09, 0x33,       //   Usage (Rx)
    0x09, 0x34,       //   Usage (Ry)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x02,       //   Report Count (2)
    0x81, 0x02,       //   Input (Data, Variable, Absolute)
    0x85, KS_HID_ID_RUMBLE,
    0x06, 0x00, 0xFF, //   Usage Page (Vendor 0xFF00)
    0x09, 0x01,       //   Usage (1)
    0x95, KS_HID_RUMBLE_LEN,
    0x91, 0x02,       //   Output (Data, Variable, Absolute)
    0xC0,             // End Collection
    0x06, 0x00, 0xFF, // Usage Page (Vendor 0xFF00)
    0x09, 0x02,       // Usage (2)
    0xA1, 0x01,       // Collection (Application)
    0x85, KS_HID_ID_TRIGGER_FX,
    0x09, 0x03,       //   Usage (3)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, KS_HID_TRIGGER_FX_LEN,
    0x91, 0x02,       //   Output (Data, Variable, Absolute)
    0xC0,             // End Collection
};
const uint16_t ks_hid_desc_gamepad_len = sizeof ks_hid_desc_gamepad;

void ks_hid_build(const ks_gamepad_t *g, uint8_t out[KS_HID_INPUT_LEN])
{
    uint16_t b = (uint16_t)((HAS(g, KS_OUT_A) << 0) | (HAS(g, KS_OUT_B) << 1) |
                            (HAS(g, KS_OUT_EX1) << 2) | (HAS(g, KS_OUT_X) << 3) |
                            (HAS(g, KS_OUT_Y) << 4) | (HAS(g, KS_OUT_EX2) << 5) |
                            (HAS(g, KS_OUT_LB) << 6) | (HAS(g, KS_OUT_RB) << 7) |
                            ((g->lt >= 128) << 8) | ((g->rt >= 128) << 9) |
                            (HAS(g, KS_OUT_VIEW) << 10) | (HAS(g, KS_OUT_MENU) << 11) |
                            (HAS(g, KS_OUT_HOME) << 12) | (HAS(g, KS_OUT_L3) << 13) |
                            (HAS(g, KS_OUT_R3) << 14) | (HAS(g, KS_OUT_EX3) << 15));
    put16(out, b);
    out[2] = ks_hat_from_buttons(g->buttons);
    put16(out + 3, hid_x(g->lx));
    put16(out + 5, hid_y(g->ly));
    put16(out + 7, hid_x(g->rx));
    put16(out + 9, hid_y(g->ry));
    out[11] = g->lt;
    out[12] = g->rt;
}

bool ks_hid_parse_rumble(const uint8_t *buf, size_t len, ks_rumble_t *rumble)
{
    if (len < KS_HID_RUMBLE_LEN) {
        return false;
    }
    rumble->left = buf[0];
    rumble->right = buf[1];
    return true;
}

bool ks_hid_parse_trigger_fx(const uint8_t *buf, size_t len, uint8_t *target,
                             ks_trigger_fx_cfg_t *cfg)
{
    if (len < KS_HID_TRIGGER_FX_LEN || buf[1] >= KS_FX_COUNT ||
        (buf[0] & (KS_FX_TARGET_LEFT | KS_FX_TARGET_RIGHT)) == 0) {
        return false;
    }
    *target = buf[0] & (KS_FX_TARGET_LEFT | KS_FX_TARGET_RIGHT);
    *cfg = (ks_trigger_fx_cfg_t){buf[1], buf[2], buf[3], buf[4]};
    return true;
}

// Report map of the Xbox Wireless Controller (model 1914) over Bluetooth LE,
// reproduced from published descriptor dumps. Nobody has compared it with a
// capture from a real controller for this project.
const uint8_t ks_hid_desc_xbox_ble[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,             // Game Pad, report 1
    0x09, 0x01, 0xA1, 0x00, 0x09, 0x30, 0x09, 0x31,             // Pointer: X, Y
    0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x95, 0x02, 0x75, 0x10, 0x81, 0x02, 0xC0,
    0x09, 0x01, 0xA1, 0x00, 0x09, 0x32, 0x09, 0x35,             // Pointer: Z, Rz
    0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x95, 0x02, 0x75, 0x10, 0x81, 0x02, 0xC0,
    0x05, 0x02, 0x09, 0xC5, 0x15, 0x00, 0x26, 0xFF, 0x03,       // Brake, 10 bits
    0x95, 0x01, 0x75, 0x0A, 0x81, 0x02,
    0x15, 0x00, 0x25, 0x00, 0x75, 0x06, 0x95, 0x01, 0x81, 0x03, // 6 bits padding
    0x05, 0x02, 0x09, 0xC4, 0x15, 0x00, 0x26, 0xFF, 0x03,       // Accelerator, 10 bits
    0x95, 0x01, 0x75, 0x0A, 0x81, 0x02,
    0x15, 0x00, 0x25, 0x00, 0x75, 0x06, 0x95, 0x01, 0x81, 0x03, // 6 bits padding
    0x05, 0x01, 0x09, 0x39, 0x15, 0x01, 0x25, 0x08,             // Hat, 1 to 8
    0x35, 0x00, 0x46, 0x3B, 0x01, 0x66, 0x14, 0x00,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x75, 0x04, 0x95, 0x01, 0x15, 0x00, 0x25, 0x00,             // 4 bits padding
    0x35, 0x00, 0x45, 0x00, 0x65, 0x00, 0x81, 0x03,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0F, 0x15, 0x00, 0x25, 0x01, // 15 buttons
    0x75, 0x01, 0x95, 0x0F, 0x81, 0x02,
    0x15, 0x00, 0x25, 0x00, 0x75, 0x01, 0x95, 0x01, 0x81, 0x03, // 1 bit padding
    0x05, 0x0C, 0x0A, 0xB2, 0x00, 0x15, 0x00, 0x25, 0x01,       // Consumer: Record (Share)
    0x95, 0x01, 0x75, 0x01, 0x81, 0x02,
    0x15, 0x00, 0x25, 0x00, 0x75, 0x07, 0x95, 0x01, 0x81, 0x03, // 7 bits padding
    0x05, 0x0F, 0x09, 0x21, 0x85, 0x03, 0xA1, 0x02,             // PID: Set Effect, report 3
    0x09, 0x97, 0x15, 0x00, 0x25, 0x01, 0x75, 0x04, 0x95, 0x01, 0x91, 0x02, // enable bits
    0x15, 0x00, 0x25, 0x00, 0x75, 0x04, 0x95, 0x01, 0x91, 0x03,
    0x09, 0x70, 0x15, 0x00, 0x25, 0x64, 0x75, 0x08, 0x95, 0x04, 0x91, 0x02, // 4 magnitudes
    0x09, 0x50, 0x66, 0x01, 0x10, 0x55, 0x0E, 0x15, 0x00, 0x26, 0xFF, 0x00, // duration
    0x75, 0x08, 0x95, 0x01, 0x91, 0x02,
    0x09, 0xA7, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02, // delay
    0x65, 0x00, 0x55, 0x00,
    0x09, 0x7C, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02, // loops
    0xC0, 0xC0,
};
const uint16_t ks_hid_desc_xbox_ble_len = sizeof ks_hid_desc_xbox_ble;

void ks_xbox_ble_build(const ks_gamepad_t *g, uint8_t out[KS_XBOX_BLE_INPUT_LEN])
{
    uint8_t hat = ks_hat_from_buttons(g->buttons);
    uint16_t b = (uint16_t)((HAS(g, KS_OUT_A) << 0) | (HAS(g, KS_OUT_B) << 1) |
                            (HAS(g, KS_OUT_X) << 3) | (HAS(g, KS_OUT_Y) << 4) |
                            (HAS(g, KS_OUT_LB) << 6) | (HAS(g, KS_OUT_RB) << 7) |
                            (HAS(g, KS_OUT_VIEW) << 10) | (HAS(g, KS_OUT_MENU) << 11) |
                            (HAS(g, KS_OUT_HOME) << 12) | (HAS(g, KS_OUT_L3) << 13) |
                            (HAS(g, KS_OUT_R3) << 14));
    put16(out, hid_x(g->lx));
    put16(out + 2, hid_y(g->ly));
    put16(out + 4, hid_x(g->rx));
    put16(out + 6, hid_y(g->ry));
    put16(out + 8, (uint16_t)(g->lt * 1023u / 255u));
    put16(out + 10, (uint16_t)(g->rt * 1023u / 255u));
    out[12] = hat == 8 ? 0 : (uint8_t)(hat + 1);
    put16(out + 13, b);
    out[15] = 0;
}

bool ks_xbox_ble_parse_rumble(const uint8_t *buf, size_t len, ks_rumble_t *rumble)
{
    if (len < KS_XBOX_BLE_RUMBLE_LEN) {
        return false;
    }
    // buf[0] enables the motors: bit 0 weak, bit 1 strong, bits 2 and 3 the
    // trigger motors. Magnitudes run from 0 to 100.
    unsigned strong = buf[3] > 100 ? 100 : buf[3], weak = buf[4] > 100 ? 100 : buf[4];
    rumble->left = (buf[0] & 0x02) ? (uint8_t)(strong * 255u / 100u) : 0;
    rumble->right = (buf[0] & 0x01) ? (uint8_t)(weak * 255u / 100u) : 0;
    return true;
}
