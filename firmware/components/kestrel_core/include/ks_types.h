// Types shared by the hardware-independent Kestrel code.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The 20 physical digital inputs, in the order the UI lists them.
typedef enum {
    KS_IN_A, KS_IN_B, KS_IN_X, KS_IN_Y,
    KS_IN_DUP, KS_IN_DDOWN, KS_IN_DLEFT, KS_IN_DRIGHT,
    KS_IN_VIEW, KS_IN_MENU, KS_IN_HOME, KS_IN_FN,
    KS_IN_LB, KS_IN_RB, KS_IN_L3, KS_IN_R3,
    KS_IN_P1, KS_IN_P2, KS_IN_P3, KS_IN_P4,
    KS_IN_COUNT
} ks_input_t;

// Gamepad outputs a physical input can map to. LT and RT here mean a full
// trigger pull. EX1 to EX3 exist only in the HID and WiFi reports.
typedef enum {
    KS_OUT_NONE,
    KS_OUT_A, KS_OUT_B, KS_OUT_X, KS_OUT_Y,
    KS_OUT_LB, KS_OUT_RB, KS_OUT_VIEW, KS_OUT_MENU, KS_OUT_HOME,
    KS_OUT_L3, KS_OUT_R3,
    KS_OUT_DUP, KS_OUT_DDOWN, KS_OUT_DLEFT, KS_OUT_DRIGHT,
    KS_OUT_LT, KS_OUT_RT,
    KS_OUT_EX1, KS_OUT_EX2, KS_OUT_EX3,
    KS_OUT_COUNT
} ks_output_t;

#define KS_IN_BIT(i) (1u << (i))
#define KS_OUT_BIT(o) (1u << ((o) - 1))

// Logical gamepad state after mapping. Sticks use the XInput convention:
// +x is right and +y is up.
typedef struct {
    uint32_t buttons; // KS_OUT_BIT() per output
    int16_t lx, ly, rx, ry;
    uint8_t lt, rt;
} ks_gamepad_t;

typedef enum {
    KS_MODE_USB_XINPUT,
    KS_MODE_USB_DINPUT,
    KS_MODE_BLE_XBOX,
    KS_MODE_BLE_GENERIC,
    KS_MODE_WIFI,
    KS_MODE_COUNT
} ks_mode_t;

typedef enum { KS_FX_OFF, KS_FX_RESISTANCE, KS_FX_HAIR, KS_FX_CLICK, KS_FX_PULSE, KS_FX_COUNT } ks_fx_t;

// One trigger effect. start is the trigger position (0 to 255) where the arm
// meets the trigger. strength is 0 to 255. param is the click span in trigger
// units for KS_FX_CLICK and the rate in hertz for KS_FX_PULSE.
typedef struct {
    uint8_t effect, start, strength, param;
} ks_trigger_fx_cfg_t;

typedef struct {
    uint8_t left, right; // 0 to 255, left is the heavy motor
} ks_rumble_t;

const char *ks_input_name(int input);
const char *ks_output_name(int output);
const char *ks_mode_name(int mode);
const char *ks_fx_name(int effect);
