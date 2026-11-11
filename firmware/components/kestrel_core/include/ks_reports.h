// Host report formats: Xbox 360 wired (XInput), the HID gamepad used by USB
// DInput and Bluetooth generic mode, and the Xbox Wireless Controller layout
// used by Bluetooth Xbox-identity mode.
#pragma once

#include "ks_types.h"

// Xbox 360 wired protocol.
#define KS_XINPUT_REPORT_LEN 20
void ks_xinput_build(const ks_gamepad_t *g, uint8_t out[KS_XINPUT_REPORT_LEN]);
// Accepts the rumble message 00 08 00 LL RR 00 00 00.
bool ks_xinput_parse_out(const uint8_t *buf, size_t len, ks_rumble_t *rumble);

// HID gamepad: 16 buttons, hat, X Y Z Rz at 16 bits, Rx Ry at 8 bits.
#define KS_HID_ID_INPUT 1
#define KS_HID_ID_RUMBLE 2     // output: left, right
#define KS_HID_ID_TRIGGER_FX 3 // output: target, effect, start, strength, param
#define KS_HID_INPUT_LEN 13
#define KS_HID_RUMBLE_LEN 2
#define KS_HID_TRIGGER_FX_LEN 5
#define KS_FX_TARGET_LEFT 1
#define KS_FX_TARGET_RIGHT 2

extern const uint8_t ks_hid_desc_gamepad[];
extern const uint16_t ks_hid_desc_gamepad_len;

// Report bodies exclude the report ID byte.
void ks_hid_build(const ks_gamepad_t *g, uint8_t out[KS_HID_INPUT_LEN]);
bool ks_hid_parse_rumble(const uint8_t *buf, size_t len, ks_rumble_t *rumble);
bool ks_hid_parse_trigger_fx(const uint8_t *buf, size_t len, uint8_t *target,
                             ks_trigger_fx_cfg_t *cfg);

// Hat value: 0 is up, then clockwise in 45 degree steps. 8 means centred.
uint8_t ks_hat_from_buttons(uint32_t buttons);

// Xbox Wireless Controller over Bluetooth LE.
#define KS_XBOX_BLE_ID_INPUT 1
#define KS_XBOX_BLE_ID_RUMBLE 3
#define KS_XBOX_BLE_INPUT_LEN 16
#define KS_XBOX_BLE_RUMBLE_LEN 8
#define KS_XBOX_BLE_VID 0x045E
#define KS_XBOX_BLE_PID 0x0B13

extern const uint8_t ks_hid_desc_xbox_ble[];
extern const uint16_t ks_hid_desc_xbox_ble_len;

void ks_xbox_ble_build(const ks_gamepad_t *g, uint8_t out[KS_XBOX_BLE_INPUT_LEN]);
bool ks_xbox_ble_parse_rumble(const uint8_t *buf, size_t len, ks_rumble_t *rumble);
