#include "ks_input.h"

#include <math.h>
#include <string.h>

#include "ks_pinmap_gen.h"

// The amended pin map puts the bumper _ALT inputs on the two former spares.
#if !defined(KS_SR_BTN_LB_ALT) || !defined(KS_SR_BTN_RB_ALT)
#error "shared/pinmap.json lacks BTN_LB_ALT or BTN_RB_ALT. Run tools/gen_pins.py."
#endif

static const struct {
    uint8_t sr, input;
} sr_map[] = {
    {KS_SR_BTN_A, KS_IN_A},           {KS_SR_BTN_B, KS_IN_B},
    {KS_SR_BTN_X, KS_IN_X},           {KS_SR_BTN_Y, KS_IN_Y},
    {KS_SR_DPAD_UP, KS_IN_DUP},       {KS_SR_DPAD_DOWN, KS_IN_DDOWN},
    {KS_SR_DPAD_LEFT, KS_IN_DLEFT},   {KS_SR_DPAD_RIGHT, KS_IN_DRIGHT},
    {KS_SR_BTN_VIEW, KS_IN_VIEW},     {KS_SR_BTN_MENU, KS_IN_MENU},
    {KS_SR_BTN_FN, KS_IN_FN},         {KS_SR_BTN_L3, KS_IN_L3},
    {KS_SR_BTN_R3, KS_IN_R3},         {KS_SR_PADDLE_1, KS_IN_P1},
    {KS_SR_PADDLE_2, KS_IN_P2},       {KS_SR_PADDLE_3, KS_IN_P3},
    {KS_SR_PADDLE_4, KS_IN_P4},
};

static bool sr_low(const uint8_t raw[3], int index)
{
    return ((raw[index >> 3] >> (index & 7)) & 1) == 0;
}

static uint8_t sr_pair(const uint8_t raw[3], int main_bit, int alt_bit)
{
    return (uint8_t)((sr_low(raw, main_bit) ? 0 : 1) | (sr_low(raw, alt_bit) ? 0 : 2));
}

void ks_sr_decode(const uint8_t raw[3], bool home_low, const ks_bumper_cal_t *cal,
                  ks_sr_frame_t *out)
{
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < sizeof sr_map / sizeof sr_map[0]; i++) {
        if (sr_low(raw, sr_map[i].sr)) {
            out->pressed |= KS_IN_BIT(sr_map[i].input);
        }
    }
    if (home_low) {
        out->pressed |= KS_IN_BIT(KS_IN_HOME);
    }
    out->lb_pair = sr_pair(raw, KS_SR_BTN_LB, KS_SR_BTN_LB_ALT);
    out->rb_pair = sr_pair(raw, KS_SR_BTN_RB, KS_SR_BTN_RB_ALT);
    if (out->lb_pair != (cal ? cal->lb_released : KS_BUMPER_BOTH_HIGH)) {
        out->pressed |= KS_IN_BIT(KS_IN_LB);
    }
    if (out->rb_pair != (cal ? cal->rb_released : KS_BUMPER_BOTH_HIGH)) {
        out->pressed |= KS_IN_BIT(KS_IN_RB);
    }
    out->charging = sr_low(raw, KS_SR_CHG_N);
    out->usb_power = sr_low(raw, KS_SR_PGOOD_N);
    out->motor_fault = sr_low(raw, KS_SR_MOTOR_FAULT_N);
}

void ks_debounce_init(ks_debounce_t *d, uint8_t confirm_ticks, uint8_t lockout_ticks)
{
    memset(d, 0, sizeof *d);
    d->confirm_ticks = confirm_ticks ? confirm_ticks : 1;
    d->lockout_ticks = lockout_ticks;
}

uint32_t ks_debounce_step(ks_debounce_t *d, uint32_t raw)
{
    for (int i = 0; i < KS_IN_COUNT; i++) {
        uint32_t bit = KS_IN_BIT(i);
        if (d->lockout[i]) {
            d->lockout[i]--;
            d->confirm[i] = 0;
        } else if ((raw ^ d->state) & bit) {
            if (++d->confirm[i] >= d->confirm_ticks) {
                d->state ^= bit;
                d->confirm[i] = 0;
                d->lockout[i] = d->lockout_ticks;
            }
        } else {
            d->confirm[i] = 0;
        }
    }
    return d->state;
}

// Placeholder values for sensors read at 12 dB attenuation. Real hardware
// needs the calibration step in the Sticks menu.
void ks_calibration_default(ks_calibration_t *c)
{
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 4; i++) {
        c->axis[i].min = 10500;
        c->axis[i].centre = 31700;
        c->axis[i].max = 52900;
    }
    for (int i = 0; i < 2; i++) {
        c->trigger[i].released = 48000;
        c->trigger[i].pulled = 33000;
    }
    c->bumper.lb_released = KS_BUMPER_BOTH_HIGH;
    c->bumper.rb_released = KS_BUMPER_BOTH_HIGH;
}

static float axis_norm(const ks_axis_cal_t *c, uint16_t raw)
{
    float v = 0.0f;
    if (raw >= c->centre) {
        int span = (int)c->max - (int)c->centre;
        if (span > 0) {
            v = (float)(raw - c->centre) / (float)span;
        }
    } else {
        int span = (int)c->centre - (int)c->min;
        if (span > 0) {
            v = -(float)(c->centre - raw) / (float)span;
        }
    }
    if (v > 1.0f) {
        v = 1.0f;
    } else if (v < -1.0f) {
        v = -1.0f;
    }
    return c->invert ? -v : v;
}

void ks_stick_apply(const ks_axis_cal_t *cx, const ks_axis_cal_t *cy, uint8_t deadzone_pct,
                    uint8_t curve_x10, uint16_t raw_x, uint16_t raw_y,
                    int16_t *out_x, int16_t *out_y)
{
    // Full output arrives a little before the mechanical limit so that a worn
    // or slightly off-centre stick still reaches it.
    const float outer = 0.96f;
    float x = axis_norm(cx, raw_x), y = axis_norm(cy, raw_y);
    float r = sqrtf(x * x + y * y);
    float dz = (float)(deadzone_pct > 40 ? 40 : deadzone_pct) / 100.0f;

    *out_x = 0;
    *out_y = 0;
    if (r <= dz || r <= 0.0f) {
        return;
    }
    float t = (r - dz) / (outer - dz);
    if (t > 1.0f) {
        t = 1.0f;
    }
    if (curve_x10 != 10 && curve_x10 != 0) {
        t = powf(t, (float)curve_x10 / 10.0f);
    }
    float k = t / r * 32767.0f;
    *out_x = (int16_t)lroundf(x * k);
    *out_y = (int16_t)lroundf(y * k);
}

uint8_t ks_trigger_apply(const ks_trigger_cal_t *cal, uint8_t deadzone_pct, uint16_t raw)
{
    const float full = 0.97f;
    int span = (int)cal->pulled - (int)cal->released;
    if (span == 0) {
        return 0;
    }
    float t = (float)((int)raw - (int)cal->released) / (float)span;
    float lo = (float)(deadzone_pct > 30 ? 30 : deadzone_pct) / 100.0f;
    if (t <= lo) {
        return 0;
    }
    if (t >= full) {
        return 255;
    }
    return (uint8_t)((t - lo) / (full - lo) * 255.0f + 0.5f);
}

// Smallest movement, in 16-bit ADC units, that counts as a real range.
#define CAL_MIN_TRAVEL 2000
#define CAL_MAX_SAMPLES 4096

void ks_cal_start(ks_cal_capture_t *c)
{
    memset(c, 0, sizeof *c);
    c->phase = KS_CAL_CENTRE;
    c->lb_pair = KS_BUMPER_BOTH_HIGH;
    c->rb_pair = KS_BUMPER_BOTH_HIGH;
}

void ks_cal_feed(ks_cal_capture_t *c, const uint16_t analog[KS_AX_COUNT],
                 uint8_t lb_pair, uint8_t rb_pair)
{
    if (c->phase == KS_CAL_CENTRE) {
        if (c->count < CAL_MAX_SAMPLES) {
            for (int i = 0; i < KS_AX_COUNT; i++) {
                c->sum[i] += analog[i];
            }
            c->count++;
        }
        c->lb_pair = lb_pair;
        c->rb_pair = rb_pair;
    } else if (c->phase == KS_CAL_RANGE) {
        for (int i = 0; i < KS_AX_COUNT; i++) {
            if (analog[i] < c->lo[i]) {
                c->lo[i] = analog[i];
            }
            if (analog[i] > c->hi[i]) {
                c->hi[i] = analog[i];
            }
        }
    }
}

bool ks_cal_advance(ks_cal_capture_t *c, ks_calibration_t *out)
{
    if (c->phase == KS_CAL_CENTRE) {
        if (c->count == 0) {
            return false;
        }
        for (int i = 0; i < KS_AX_COUNT; i++) {
            c->rest[i] = (uint16_t)(c->sum[i] / c->count);
            c->lo[i] = c->rest[i];
            c->hi[i] = c->rest[i];
        }
        c->phase = KS_CAL_RANGE;
        return false;
    }
    if (c->phase != KS_CAL_RANGE) {
        return false;
    }
    c->phase = KS_CAL_IDLE;
    for (int i = 0; i < 4; i++) {
        if (c->rest[i] - c->lo[i] >= CAL_MIN_TRAVEL && c->hi[i] - c->rest[i] >= CAL_MIN_TRAVEL) {
            out->axis[i].min = c->lo[i];
            out->axis[i].centre = c->rest[i];
            out->axis[i].max = c->hi[i];
        }
    }
    for (int i = 0; i < 2; i++) {
        // A trigger moves one way from rest. The Hall sensor's polarity
        // decides which way, so take the larger excursion.
        int up = c->hi[4 + i] - c->rest[4 + i], down = c->rest[4 + i] - c->lo[4 + i];
        if (up >= CAL_MIN_TRAVEL || down >= CAL_MIN_TRAVEL) {
            out->trigger[i].released = c->rest[4 + i];
            out->trigger[i].pulled = up >= down ? c->hi[4 + i] : c->lo[4 + i];
        }
    }
    out->bumper.lb_released = c->lb_pair;
    out->bumper.rb_released = c->rb_pair;
    return true;
}
