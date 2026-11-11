// Shift register decoding, bumper pairs, debounce, and analogue calibration.
#include <stdlib.h>
#include <string.h>

#include "ks_input.h"
#include "ks_pinmap_gen.h"
#include "test.h"

static void all_high(uint8_t raw[3]) { raw[0] = raw[1] = raw[2] = 0xFF; }

static void set_low(uint8_t raw[3], int index)
{
    raw[index >> 3] &= (uint8_t)~(1u << (index & 7));
}

void test_sr_idle(void)
{
    uint8_t raw[3];
    ks_sr_frame_t f;
    all_high(raw);
    ks_sr_decode(raw, false, NULL, &f);
    CHECK_EQ(f.pressed, 0);
    CHECK(!f.charging && !f.usb_power && !f.motor_fault);
    CHECK_EQ(f.lb_pair, KS_BUMPER_BOTH_HIGH);
    CHECK_EQ(f.rb_pair, KS_BUMPER_BOTH_HIGH);
}

void test_sr_each_input(void)
{
    static const struct {
        int sr, input;
    } map[] = {
        {KS_SR_BTN_A, KS_IN_A},         {KS_SR_BTN_B, KS_IN_B},
        {KS_SR_BTN_X, KS_IN_X},         {KS_SR_BTN_Y, KS_IN_Y},
        {KS_SR_DPAD_UP, KS_IN_DUP},     {KS_SR_DPAD_DOWN, KS_IN_DDOWN},
        {KS_SR_DPAD_LEFT, KS_IN_DLEFT}, {KS_SR_DPAD_RIGHT, KS_IN_DRIGHT},
        {KS_SR_BTN_VIEW, KS_IN_VIEW},   {KS_SR_BTN_MENU, KS_IN_MENU},
        {KS_SR_BTN_FN, KS_IN_FN},       {KS_SR_BTN_L3, KS_IN_L3},
        {KS_SR_BTN_R3, KS_IN_R3},       {KS_SR_PADDLE_1, KS_IN_P1},
        {KS_SR_PADDLE_2, KS_IN_P2},     {KS_SR_PADDLE_3, KS_IN_P3},
        {KS_SR_PADDLE_4, KS_IN_P4},     {KS_SR_BTN_LB, KS_IN_LB},
        {KS_SR_BTN_LB_ALT, KS_IN_LB},   {KS_SR_BTN_RB, KS_IN_RB},
        {KS_SR_BTN_RB_ALT, KS_IN_RB},
    };
    uint8_t raw[3];
    ks_sr_frame_t f;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        all_high(raw);
        set_low(raw, map[i].sr);
        ks_sr_decode(raw, false, NULL, &f);
        CHECK_EQ(f.pressed, KS_IN_BIT(map[i].input));
    }
    // Home has its own GPIO.
    all_high(raw);
    ks_sr_decode(raw, true, NULL, &f);
    CHECK_EQ(f.pressed, KS_IN_BIT(KS_IN_HOME));

    // The three status inputs are not buttons.
    all_high(raw);
    set_low(raw, KS_SR_CHG_N);
    ks_sr_decode(raw, false, NULL, &f);
    CHECK(f.pressed == 0 && f.charging && !f.usb_power && !f.motor_fault);
    all_high(raw);
    set_low(raw, KS_SR_PGOOD_N);
    ks_sr_decode(raw, false, NULL, &f);
    CHECK(f.pressed == 0 && !f.charging && f.usb_power && !f.motor_fault);
    all_high(raw);
    set_low(raw, KS_SR_MOTOR_FAULT_N);
    ks_sr_decode(raw, false, NULL, &f);
    CHECK(f.pressed == 0 && !f.charging && !f.usb_power && f.motor_fault);
}

// The pin map says the first byte is SR1 and that input H leaves first, so H
// is bit 7 and A is bit 0 of each byte.
void test_sr_bit_order(void)
{
    ks_sr_frame_t f;
    ks_sr_decode((const uint8_t[3]){0xFE, 0xFF, 0xFF}, false, NULL, &f);
    CHECK_EQ(f.pressed, KS_IN_BIT(KS_IN_A)); // SR1 input A
    ks_sr_decode((const uint8_t[3]){0x7F, 0xFF, 0xFF}, false, NULL, &f);
    CHECK_EQ(f.pressed, KS_IN_BIT(KS_IN_MENU)); // SR1 input H
    ks_sr_decode((const uint8_t[3]){0xFF, 0xBF, 0xFF}, false, NULL, &f);
    CHECK_EQ(f.pressed, KS_IN_BIT(KS_IN_FN)); // SR2 input G
    ks_sr_decode((const uint8_t[3]){0xFF, 0x7F, 0xFF}, false, NULL, &f);
    CHECK_EQ(f.pressed, KS_IN_BIT(KS_IN_LB)); // SR2 input H is BTN_LB_ALT
    ks_sr_decode((const uint8_t[3]){0xFF, 0xFF, 0xF7}, false, NULL, &f);
    CHECK_EQ(f.pressed, KS_IN_BIT(KS_IN_P4)); // SR3 input D
    ks_sr_decode((const uint8_t[3]){0xFF, 0xFF, 0xEF}, false, NULL, &f);
    CHECK(f.pressed == 0 && f.charging); // SR3 input E
    ks_sr_decode((const uint8_t[3]){0xFF, 0xFF, 0x7F}, false, NULL, &f);
    CHECK_EQ(f.pressed, KS_IN_BIT(KS_IN_RB)); // SR3 input H is BTN_RB_ALT
}

// Input levels for a bumper micro switch whose common, normally closed, and
// normally open terminals sit on the given pads. Pad 1 is GND, pad 2 is the
// main input, and pad 3 is the _ALT input. Both inputs have pull-ups.
static uint8_t switch_pair(int com, int nc, int no, bool pressed)
{
    int other = pressed ? no : nc;
    bool main_low = (com == 1 && other == 2) || (com == 2 && other == 1);
    bool alt_low = (com == 1 && other == 3) || (com == 3 && other == 1);
    return (uint8_t)((main_low ? 0 : 1) | (alt_low ? 0 : 2));
}

static void raw_from_pairs(uint8_t raw[3], uint8_t lb, uint8_t rb)
{
    all_high(raw);
    if (!(lb & 1)) set_low(raw, KS_SR_BTN_LB);
    if (!(lb & 2)) set_low(raw, KS_SR_BTN_LB_ALT);
    if (!(rb & 1)) set_low(raw, KS_SR_BTN_RB);
    if (!(rb & 2)) set_low(raw, KS_SR_BTN_RB_ALT);
}

// The common terminal can be on any of the three pads. Each case appears
// twice because the other two pads can be normally closed or normally open.
void test_bumper_three_wirings(void)
{
    static const int wiring[6][3] = {
        {1, 2, 3}, {1, 3, 2}, // common on pad 1 (GND)
        {2, 1, 3}, {2, 3, 1}, // common on pad 2 (BTN_LB or BTN_RB)
        {3, 1, 2}, {3, 2, 1}, // common on pad 3 (the _ALT input)
    };
    const uint32_t lb = KS_IN_BIT(KS_IN_LB), rb = KS_IN_BIT(KS_IN_RB);
    for (int w = 0; w < 6; w++) {
        uint8_t released = switch_pair(wiring[w][0], wiring[w][1], wiring[w][2], false);
        uint8_t pressed = switch_pair(wiring[w][0], wiring[w][1], wiring[w][2], true);
        ks_bumper_cal_t cal = {released, released};
        uint8_t raw[3];
        ks_sr_frame_t f;

        CHECK(released != pressed);
        raw_from_pairs(raw, released, released);
        ks_sr_decode(raw, false, &cal, &f);
        CHECK_EQ(f.pressed, 0);
        raw_from_pairs(raw, pressed, released);
        ks_sr_decode(raw, false, &cal, &f);
        CHECK_EQ(f.pressed, lb);
        raw_from_pairs(raw, released, pressed);
        ks_sr_decode(raw, false, &cal, &f);
        CHECK_EQ(f.pressed, rb);
        raw_from_pairs(raw, pressed, pressed);
        ks_sr_decode(raw, false, &cal, &f);
        CHECK_EQ(f.pressed, lb | rb);
        // The decoder reports the raw pair so that calibration can capture it.
        CHECK_EQ(f.lb_pair, pressed);
    }
}

void test_bumper_default_both_high(void)
{
    uint8_t raw[3];
    ks_sr_frame_t f;
    // Common on the main input, normally closed to _ALT: released reads both
    // high, so the default needs no calibration.
    raw_from_pairs(raw, switch_pair(2, 3, 1, false), KS_BUMPER_BOTH_HIGH);
    ks_sr_decode(raw, false, NULL, &f);
    CHECK_EQ(f.pressed, 0);
    raw_from_pairs(raw, switch_pair(2, 3, 1, true), KS_BUMPER_BOTH_HIGH);
    ks_sr_decode(raw, false, NULL, &f);
    CHECK_EQ(f.pressed, KS_IN_BIT(KS_IN_LB));

    // Common on GND: one input is low at rest, so the default reads a press
    // until calibration records the released pair.
    uint8_t released = switch_pair(1, 2, 3, false);
    raw_from_pairs(raw, released, KS_BUMPER_BOTH_HIGH);
    ks_sr_decode(raw, false, NULL, &f);
    CHECK_EQ(f.pressed, KS_IN_BIT(KS_IN_LB));
    ks_bumper_cal_t cal = {f.lb_pair, f.rb_pair};
    ks_sr_decode(raw, false, &cal, &f);
    CHECK_EQ(f.pressed, 0);
}

void test_debounce_press_release(void)
{
    const uint32_t a = KS_IN_BIT(KS_IN_A);
    ks_debounce_t d;
    ks_debounce_init(&d, 2, 4);
    CHECK_EQ(ks_debounce_step(&d, a), 0); // one sample is not enough
    CHECK_EQ(ks_debounce_step(&d, a), a); // the second confirms
    for (int i = 0; i < 4; i++) {         // contact bounce inside the lockout
        CHECK_EQ(ks_debounce_step(&d, (i & 1) ? a : 0), a);
    }
    CHECK_EQ(ks_debounce_step(&d, 0), a);
    CHECK_EQ(ks_debounce_step(&d, 0), 0); // released after two samples
}

void test_debounce_rejects_glitch(void)
{
    const uint32_t a = KS_IN_BIT(KS_IN_A), b = KS_IN_BIT(KS_IN_B);
    ks_debounce_t d;
    ks_debounce_init(&d, 2, 4);
    // A flickers for single samples. B is held. Only B registers.
    CHECK_EQ(ks_debounce_step(&d, a | b), 0);
    CHECK_EQ(ks_debounce_step(&d, b), b);
    CHECK_EQ(ks_debounce_step(&d, a | b), b);
    CHECK_EQ(ks_debounce_step(&d, b), b);
    CHECK_EQ(ks_debounce_step(&d, a | b), b);
    CHECK_EQ(ks_debounce_step(&d, b), b);
}

void test_stick_calibration(void)
{
    ks_calibration_t c;
    int16_t x, y;
    ks_calibration_default(&c); // min 10500, centre 31700, max 52900
    const ks_axis_cal_t *cx = &c.axis[0], *cy = &c.axis[1];

    ks_stick_apply(cx, cy, 8, 10, 31700, 31700, &x, &y);
    CHECK(x == 0 && y == 0);
    ks_stick_apply(cx, cy, 8, 10, 52900, 31700, &x, &y);
    CHECK(x == 32767 && y == 0);
    ks_stick_apply(cx, cy, 8, 10, 10500, 31700, &x, &y);
    CHECK(x == -32767 && y == 0);
    ks_stick_apply(cx, cy, 8, 10, 31700, 10500, &x, &y);
    CHECK(x == 0 && y == -32767);
    // Readings past the calibrated range clamp.
    ks_stick_apply(cx, cy, 8, 10, 65535, 0, &x, &y);
    CHECK(abs(x - 23170) <= 2 && abs(y + 23170) <= 2);
    // Both axes at their limit give a full-length diagonal, not an overflow.
    ks_stick_apply(cx, cy, 8, 10, 52900, 52900, &x, &y);
    CHECK(abs(x - 23170) <= 2 && abs(y - 23170) <= 2);

    // An uneven range still maps each side to full scale.
    ks_axis_cal_t uneven = {12000, 30000, 50000, 0};
    ks_stick_apply(&uneven, cy, 0, 10, 12000, 31700, &x, &y);
    CHECK_EQ(x, -32767);
    ks_stick_apply(&uneven, cy, 0, 10, 50000, 31700, &x, &y);
    CHECK_EQ(x, 32767);

    c.axis[1].invert = 1;
    ks_stick_apply(cx, cy, 8, 10, 31700, 52900, &x, &y);
    CHECK(x == 0 && y == -32767);
}

void test_stick_deadzone_and_curve(void)
{
    ks_calibration_t c;
    int16_t x, y, linear, precise, fast;
    ks_calibration_default(&c);
    const ks_axis_cal_t *cx = &c.axis[0], *cy = &c.axis[1];
    const int span = 52900 - 31700;

    // 5 percent of travel sits inside the 8 percent dead zone.
    ks_stick_apply(cx, cy, 8, 10, (uint16_t)(31700 + span * 5 / 100), 31700, &x, &y);
    CHECK_EQ(x, 0);
    // 10 percent is outside it and starts from a small value, with no jump.
    ks_stick_apply(cx, cy, 8, 10, (uint16_t)(31700 + span * 10 / 100), 31700, &x, &y);
    CHECK(x > 0 && x < 1500);
    // The dead zone is radial: 6 percent on each axis is 8.5 percent overall.
    ks_stick_apply(cx, cy, 8, 10, (uint16_t)(31700 + span * 6 / 100),
                   (uint16_t)(31700 + span * 6 / 100), &x, &y);
    CHECK(x > 0 && y > 0);
    // Full output arrives at 96 percent of travel.
    ks_stick_apply(cx, cy, 8, 10, (uint16_t)(31700 + span * 97 / 100), 31700, &x, &y);
    CHECK_EQ(x, 32767);
    // No dead zone: small movements come straight through.
    ks_stick_apply(cx, cy, 0, 10, (uint16_t)(31700 + span * 2 / 100), 31700, &x, &y);
    CHECK(x > 400 && x < 1000);

    uint16_t half = (uint16_t)(31700 + span / 2);
    ks_stick_apply(cx, cy, 8, 10, half, 31700, &linear, &y);
    ks_stick_apply(cx, cy, 8, 18, half, 31700, &precise, &y);
    ks_stick_apply(cx, cy, 8, 7, half, 31700, &fast, &y);
    CHECK(abs(linear - 15639) < 60);
    CHECK(precise < linear && linear < fast);
    // Every curve still reaches full scale.
    ks_stick_apply(cx, cy, 8, 18, 52900, 31700, &x, &y);
    CHECK_EQ(x, 32767);
}

void test_trigger_calibration(void)
{
    ks_trigger_cal_t falling = {48000, 33000}; // output falls as the trigger is pulled
    ks_trigger_cal_t rising = {20000, 40000};
    CHECK_EQ(ks_trigger_apply(&falling, 4, 48000), 0);
    CHECK_EQ(ks_trigger_apply(&falling, 4, 47700), 0); // 2 percent, inside the dead zone
    CHECK_EQ(ks_trigger_apply(&falling, 4, 33000), 255);
    CHECK_EQ(ks_trigger_apply(&falling, 4, 30000), 255); // past the calibrated pull
    CHECK_EQ(ks_trigger_apply(&falling, 4, 50000), 0);   // past the calibrated rest
    int mid = ks_trigger_apply(&falling, 4, 40500);
    CHECK(mid >= 124 && mid <= 128);
    CHECK_EQ(ks_trigger_apply(&rising, 4, 20000), 0);
    CHECK_EQ(ks_trigger_apply(&rising, 4, 40000), 255);
    // The value rises steadily through the pull.
    int last = -1, steady = 1;
    for (int raw = 48000; raw >= 33000; raw -= 100) {
        int v = ks_trigger_apply(&falling, 4, (uint16_t)raw);
        steady &= v >= last;
        last = v;
    }
    CHECK(steady);
}

void test_calibration_capture(void)
{
    static const uint16_t rest[KS_AX_COUNT] = {30000, 31000, 32000, 33000, 47000, 21000};
    ks_cal_capture_t cap;
    ks_calibration_t out;
    uint16_t a[KS_AX_COUNT];
    ks_calibration_default(&out);
    ks_cal_start(&cap);

    // Centre phase with a little noise. The left bumper rests with its main
    // input low, as it does when the switch common is on GND.
    for (int k = 0; k < 100; k++) {
        for (int i = 0; i < KS_AX_COUNT; i++) {
            a[i] = (uint16_t)(rest[i] + ((k & 1) ? 8 : -8));
        }
        ks_cal_feed(&cap, a, 2, 3);
    }
    CHECK(!ks_cal_advance(&cap, &out));
    CHECK_EQ(cap.phase, KS_CAL_RANGE);

    // Range phase. RY barely moves. LT falls and RT rises.
    for (int k = 0; k <= 200; k++) {
        a[0] = (uint16_t)(rest[0] + (k - 100) * 150);
        a[1] = (uint16_t)(rest[1] + (k - 100) * 150);
        a[2] = (uint16_t)(rest[2] + (k - 100) * 150);
        a[3] = (uint16_t)(rest[3] + (k - 100) * 5);
        a[4] = (uint16_t)(rest[4] - k * 85);
        a[5] = (uint16_t)(rest[5] + k * 85);
        ks_cal_feed(&cap, a, 3, 3);
    }
    CHECK(ks_cal_advance(&cap, &out));
    CHECK_EQ(out.axis[0].min, 15000);
    CHECK_EQ(out.axis[0].centre, 30000);
    CHECK_EQ(out.axis[0].max, 45000);
    CHECK_EQ(out.axis[2].centre, 32000);
    CHECK_EQ(out.axis[3].centre, 31700); // too little travel, default kept
    CHECK_EQ(out.axis[3].max, 52900);
    CHECK_EQ(out.trigger[0].released, 47000);
    CHECK_EQ(out.trigger[0].pulled, 30000);
    CHECK_EQ(out.trigger[1].released, 21000);
    CHECK_EQ(out.trigger[1].pulled, 38000);
    // Bumper pairs come from the centre phase, when nothing was pressed.
    CHECK_EQ(out.bumper.lb_released, 2);
    CHECK_EQ(out.bumper.rb_released, 3);
    CHECK(!ks_cal_advance(&cap, &out)); // already finished
}
