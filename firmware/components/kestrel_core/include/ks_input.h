// Input pipeline: shift register decoding, debounce, and analogue calibration.
#pragma once

#include "ks_types.h"

// Each bumper micro switch drives two inputs because the PCB cannot know which
// pad is the switch common. A pair value holds the main input level in bit 0
// and the _ALT input level in bit 1. The bumper counts as pressed when the
// pair differs from its released value.
typedef struct {
    uint8_t lb_released, rb_released;
} ks_bumper_cal_t;

#define KS_BUMPER_BOTH_HIGH 3

typedef struct {
    uint32_t pressed;        // KS_IN_BIT() per input, not debounced
    uint8_t lb_pair, rb_pair; // raw pair levels, for calibration
    bool charging;           // CHG_N low
    bool usb_power;          // PGOOD_N low
    bool motor_fault;        // MOTOR_FAULT_N low
} ks_sr_frame_t;

// raw holds the three bytes in the order SR1, SR2, SR3. home_low is the level
// of BTN_HOME on its own GPIO.
void ks_sr_decode(const uint8_t raw[3], bool home_low, const ks_bumper_cal_t *cal,
                  ks_sr_frame_t *out);

// A change is accepted after confirm_ticks equal samples, then that input
// ignores further changes for lockout_ticks.
typedef struct {
    uint32_t state;
    uint8_t confirm[KS_IN_COUNT];
    uint8_t lockout[KS_IN_COUNT];
    uint8_t confirm_ticks, lockout_ticks;
} ks_debounce_t;

void ks_debounce_init(ks_debounce_t *d, uint8_t confirm_ticks, uint8_t lockout_ticks);
uint32_t ks_debounce_step(ks_debounce_t *d, uint32_t raw);

// Analogue values are 16-bit: the oversampled 12-bit ADC reading times 16.
enum { KS_AX_LX, KS_AX_LY, KS_AX_RX, KS_AX_RY, KS_AX_LT, KS_AX_RT, KS_AX_COUNT };

typedef struct {
    uint16_t min, centre, max;
    uint8_t invert;
} ks_axis_cal_t;

typedef struct {
    uint16_t released, pulled; // pulled may be below released
} ks_trigger_cal_t;

typedef struct {
    ks_axis_cal_t axis[4];
    ks_trigger_cal_t trigger[2];
    ks_bumper_cal_t bumper;
} ks_calibration_t;

void ks_calibration_default(ks_calibration_t *c);

// Radial dead zone, then a power curve on the remaining travel. deadzone_pct
// is 0 to 40. curve_x10 is the exponent times 10, so 10 is linear.
void ks_stick_apply(const ks_axis_cal_t *cx, const ks_axis_cal_t *cy, uint8_t deadzone_pct,
                    uint8_t curve_x10, uint16_t raw_x, uint16_t raw_y,
                    int16_t *out_x, int16_t *out_y);

uint8_t ks_trigger_apply(const ks_trigger_cal_t *cal, uint8_t deadzone_pct, uint16_t raw);

// Two-step calibration. During KS_CAL_CENTRE the user leaves everything
// released. During KS_CAL_RANGE the user rotates both sticks and pulls both
// triggers.
typedef enum { KS_CAL_IDLE, KS_CAL_CENTRE, KS_CAL_RANGE } ks_cal_phase_t;

typedef struct {
    uint8_t phase;
    uint32_t sum[KS_AX_COUNT];
    uint32_t count;
    uint16_t rest[KS_AX_COUNT], lo[KS_AX_COUNT], hi[KS_AX_COUNT];
    uint8_t lb_pair, rb_pair;
} ks_cal_capture_t;

void ks_cal_start(ks_cal_capture_t *c);
void ks_cal_feed(ks_cal_capture_t *c, const uint16_t analog[KS_AX_COUNT],
                 uint8_t lb_pair, uint8_t rb_pair);
// Ends the current phase. Returns true when the range phase ends and *out
// holds the new calibration. An axis that moved too little keeps its value
// from *out.
bool ks_cal_advance(ks_cal_capture_t *c, ks_calibration_t *out);
