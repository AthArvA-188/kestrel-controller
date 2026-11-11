#include "ks_trigger.h"

#include <string.h>

#define PARK_Q8 (KS_ARM_PARK * 256)
#define MARGIN 6         // trigger units between "touching" and the arm position
#define DEADBAND 2       // position error the controller leaves alone
#define HOMING_DUTY 180
#define HOMING_BLANK_MS 30 // ignore the inrush current at the start of homing
#define RETURN_Q8_PER_MS 384 // 1500 trigger units per second

void ks_trig_params_default(ks_trig_params_t *p)
{
    // Starting points for an N20 motor on a 20:1 worm stage. Tune on hardware.
    p->travel_ms = 250;
    p->stall_ma = 450;
    p->limit_ma = 600;
    p->trip_ma = 900;
    p->trip_ms = 40;
    p->cooloff_ms = 500;
    p->min_duty = 70;
}

void ks_trig_init(ks_trig_t *t)
{
    memset(t, 0, sizeof *t);
    t->phase = KS_TRIG_HOMING;
    t->target_q8 = PARK_Q8;
    t->duty_cap = 255;
}

void ks_trig_set(ks_trig_t *t, const ks_trigger_fx_cfg_t *cfg)
{
    ks_trigger_fx_cfg_t c = *cfg;
    if (c.effect >= KS_FX_COUNT) {
        c.effect = KS_FX_OFF;
    }
    if (memcmp(&c, &t->cfg, sizeof c) == 0) {
        return;
    }
    t->cfg = c;
    t->click_fired = false;
    t->pulse_on = false;
    t->pulse_ms = 0;
    t->target_q8 = c.effect == KS_FX_OFF ? PARK_Q8 : c.start * 256;
}

int ks_trig_arm(const ks_trig_t *t) { return (int)(t->arm_q8 >> 8); }

uint8_t ks_trig_output(const ks_trigger_fx_cfg_t *cfg, uint8_t trigger_pos)
{
    if (cfg->effect != KS_FX_HAIR || cfg->start <= MARGIN) {
        return trigger_pos;
    }
    int full = cfg->start - MARGIN;
    return trigger_pos >= full ? 255 : (uint8_t)(trigger_pos * 255 / full);
}

static void cool_off(ks_trig_t *t)
{
    t->phase = KS_TRIG_COOLOFF;
    t->timer_ms = 0;
    t->over_ms = 0;
    t->duty_cap = 255;
}

// The arm waits at start. While the trigger presses on it, the arm gives way
// at a rate that falls as strength rises. A strength of 255 does not give way.
// When the trigger lets go, the arm follows it back.
static void yield_target(ks_trig_t *t, uint8_t pos, int strength, uint16_t dt_ms)
{
    int arm = ks_trig_arm(t);
    int32_t start_q8 = t->cfg.start * 256;
    if (pos + MARGIN >= arm) {
        t->target_q8 += (255 - strength) * 1024 * (int32_t)dt_ms / 1000;
        if (t->target_q8 > PARK_Q8) {
            t->target_q8 = PARK_Q8;
        }
    } else if (pos + 3 * MARGIN < arm) {
        t->target_q8 -= RETURN_Q8_PER_MS * (int32_t)dt_ms;
    }
    if (t->target_q8 < start_q8) {
        t->target_q8 = start_q8;
    }
}

static void update_target(ks_trig_t *t, uint8_t pos, uint16_t dt_ms)
{
    const ks_trigger_fx_cfg_t *c = &t->cfg;
    switch (c->effect) {
    case KS_FX_HAIR:
        t->target_q8 = c->start * 256;
        break;
    case KS_FX_RESISTANCE:
        yield_target(t, pos, c->strength, dt_ms);
        break;
    case KS_FX_CLICK: {
        int span = c->param ? c->param : 24;
        if (!t->click_fired) {
            // Cap the stiffness so the trigger can always reach the click point.
            yield_target(t, pos, c->strength > 230 ? 230 : c->strength, dt_ms);
            if (pos >= c->start + span) {
                t->click_fired = true;
            }
        }
        if (t->click_fired) {
            t->target_q8 = PARK_Q8;
            if (pos + 2 * MARGIN < c->start) {
                t->click_fired = false;
                t->target_q8 = c->start * 256;
            }
        }
        break;
    }
    case KS_FX_PULSE: {
        // Each cycle has a kick half, where the arm pushes the trigger back by
        // amp, and a retreat half, where it backs off by twice that. Both
        // targets come from a trigger position latched when the half begins,
        // so the arm never chases a trigger that it is moving itself. The
        // finger can still pull through at amp per cycle.
        int half_ms = 500 / (c->param ? c->param : 12);
        int amp = 4 + c->strength / 16;
        if (half_ms < 4) {
            half_ms = 4;
        }
        // Once pulsing, stay on until the trigger falls clear of the kick.
        if (pos > 0 && pos + MARGIN + (t->pulse_on ? amp + MARGIN : 0) >= c->start) {
            int before = t->pulse_ms / half_ms;
            t->pulse_ms = (uint16_t)((t->pulse_ms + dt_ms) % (2 * half_ms));
            int half = t->pulse_ms / half_ms;
            if (!t->pulse_on || half != before) {
                t->pulse_base = pos;
                t->pulse_on = true;
            }
            int target = half == 0 ? t->pulse_base - amp : t->pulse_base + 2 * amp;
            if (target < 0) {
                target = 0;
            }
            t->target_q8 = target * 256;
        } else {
            t->pulse_on = false;
            t->pulse_ms = 0;
            t->target_q8 = c->start * 256;
        }
        break;
    }
    default:
        t->target_q8 = PARK_Q8;
        break;
    }
}

int16_t ks_trig_step(ks_trig_t *t, const ks_trig_params_t *p, uint8_t trigger_pos,
                     uint16_t current_ma, bool driver_fault, uint16_t dt_ms)
{
    if (driver_fault) {
        cool_off(t);
        return 0;
    }
    if (t->phase == KS_TRIG_COOLOFF) {
        t->timer_ms += dt_ms;
        if (t->timer_ms >= p->cooloff_ms) {
            // The arm may have been pushed while the motor rested, so find
            // the end stop again.
            t->phase = KS_TRIG_HOMING;
            t->timer_ms = 0;
        }
        return 0;
    }
    if (t->phase == KS_TRIG_HOMING) {
        t->timer_ms += dt_ms;
        bool stalled = t->timer_ms > HOMING_BLANK_MS && current_ma >= p->stall_ma;
        if (stalled || t->timer_ms >= (uint32_t)p->travel_ms * 3 / 2) {
            t->arm_q8 = PARK_Q8;
            t->phase = KS_TRIG_RUN;
            t->timer_ms = 0;
            return 0;
        }
        return -HOMING_DUTY;
    }

    if (current_ma > p->trip_ma) {
        t->over_ms = (uint16_t)(t->over_ms + dt_ms);
        if (t->over_ms >= p->trip_ms) {
            t->trips++;
            cool_off(t);
            return 0;
        }
    } else {
        t->over_ms = 0;
    }
    if (current_ma > p->limit_ma) {
        t->duty_cap = t->duty_cap > p->min_duty + 16 ? (uint8_t)(t->duty_cap - 16) : p->min_duty;
    } else if (t->duty_cap < 255) {
        t->duty_cap = t->duty_cap > 251 ? 255 : (uint8_t)(t->duty_cap + 4);
    }

    update_target(t, trigger_pos, dt_ms);

    int32_t err = t->target_q8 - t->arm_q8;
    int32_t mag = (err < 0 ? -err : err) >> 8;
    if (mag <= DEADBAND) {
        return 0;
    }
    int32_t duty = mag * 16;
    if (duty > 255) {
        duty = 255;
    }
    duty = duty * t->duty_cap / 255;
    if (duty < p->min_duty) {
        duty = p->min_duty;
    }
    int32_t step = (int32_t)((int64_t)PARK_Q8 * duty * dt_ms / (255 * (int32_t)p->travel_ms));
    t->arm_q8 += err > 0 ? step : -step;
    if (t->arm_q8 > PARK_Q8) {
        t->arm_q8 = PARK_Q8;
    } else if (t->arm_q8 < 0) {
        t->arm_q8 = 0;
    }
    // A larger arm position is nearer park, which is the negative direction.
    return (int16_t)(err > 0 ? -duty : duty);
}
