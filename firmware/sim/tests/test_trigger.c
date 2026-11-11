// Trigger effect state machine, run against a simple model of the actuator.
#include <stdlib.h>
#include <string.h>

#include "ks_trigger.h"
#include "test.h"

// The model: the arm moves in proportion to the duty, the trigger cannot pass
// the arm, and the motor draws 200 mA moving and 700 mA against an end stop.
typedef struct {
    ks_trig_t t;
    ks_trig_params_t p;
    double arm;    // true arm position in trigger units
    int finger;    // how far the finger wants to pull
    int force_ma;  // overrides the modelled current when not negative
    bool fault;
    int16_t duty;
    int reversals, last_sign;
    int pos_min, pos_max;
} rig_t;

static double fabs_diff(double a, double b) { return a > b ? a - b : b - a; }

static void rig_init(rig_t *r, double arm)
{
    memset(r, 0, sizeof *r);
    ks_trig_params_default(&r->p);
    ks_trig_init(&r->t);
    r->arm = arm;
    r->force_ma = -1;
    r->pos_min = 255;
}

static uint8_t rig_pos(const rig_t *r)
{
    double p = r->finger < r->arm ? r->finger : r->arm;
    return (uint8_t)(p < 0 ? 0 : p > 255 ? 255 : p);
}

static void rig_run(rig_t *r, int ms)
{
    for (int i = 0; i < ms; i++) {
        bool stalled = (r->duty < 0 && r->arm >= KS_ARM_PARK) || (r->duty > 0 && r->arm <= 0);
        int ma = r->duty == 0 ? 0 : stalled ? 700 : 200;
        if (r->force_ma >= 0) {
            ma = r->force_ma;
        }
        r->duty = ks_trig_step(&r->t, &r->p, rig_pos(r), (uint16_t)ma, r->fault, 1);
        r->arm -= (double)r->duty / 255.0 * KS_ARM_PARK / r->p.travel_ms;
        if (r->arm > KS_ARM_PARK) {
            r->arm = KS_ARM_PARK;
        } else if (r->arm < 0) {
            r->arm = 0;
        }
        int sign = (r->duty > 0) - (r->duty < 0);
        if (sign && r->last_sign && sign != r->last_sign) {
            r->reversals++;
        }
        if (sign) {
            r->last_sign = sign;
        }
        int pos = rig_pos(r);
        if (pos < r->pos_min) r->pos_min = pos;
        if (pos > r->pos_max) r->pos_max = pos;
    }
}

static void rig_start(rig_t *r, uint8_t effect, uint8_t start, uint8_t strength, uint8_t param)
{
    ks_trigger_fx_cfg_t cfg = {effect, start, strength, param};
    rig_init(r, 100);
    rig_run(r, 400); // home
    ks_trig_set(&r->t, &cfg);
    rig_run(r, 600); // let the arm reach its starting place
}

void test_trig_homing(void)
{
    rig_t r;
    // The arm starts somewhere unknown. Homing drives it to park and stops
    // when the current shows the end stop.
    rig_init(&r, 100);
    CHECK_EQ(r.t.phase, KS_TRIG_HOMING);
    rig_run(&r, 1);
    CHECK(r.duty < 0);
    rig_run(&r, 400);
    CHECK_EQ(r.t.phase, KS_TRIG_RUN);
    CHECK(r.arm >= KS_ARM_PARK - 1);
    CHECK_EQ(ks_trig_arm(&r.t), KS_ARM_PARK);
    CHECK_EQ(r.duty, 0);

    // Already parked: the stall shows as soon as the inrush blanking ends.
    rig_init(&r, KS_ARM_PARK);
    rig_run(&r, 40);
    CHECK_EQ(r.t.phase, KS_TRIG_RUN);

    // No current reading at all: homing still ends, on its time limit.
    rig_init(&r, 0);
    r.force_ma = 0;
    rig_run(&r, 370);
    CHECK_EQ(r.t.phase, KS_TRIG_HOMING);
    rig_run(&r, 10);
    CHECK_EQ(r.t.phase, KS_TRIG_RUN);
}

void test_trig_off_parks(void)
{
    rig_t r;
    rig_start(&r, KS_FX_OFF, 60, 160, 0);
    CHECK(r.arm >= KS_ARM_PARK - 1);
    r.finger = 255;
    rig_run(&r, 200);
    CHECK_EQ(rig_pos(&r), 255); // nothing in the way of a full pull
    CHECK_EQ(r.duty, 0);

    // Switching from an effect back to off parks the arm again.
    ks_trigger_fx_cfg_t hair = {KS_FX_HAIR, 70, 255, 0}, off = {KS_FX_OFF, 0, 0, 0};
    r.finger = 0;
    ks_trig_set(&r.t, &hair);
    rig_run(&r, 600);
    CHECK(fabs_diff(r.arm, 70) <= 3);
    ks_trig_set(&r.t, &off);
    rig_run(&r, 600);
    CHECK(r.arm >= KS_ARM_PARK - 3);
}

void test_trig_hair(void)
{
    rig_t r;
    ks_trigger_fx_cfg_t cfg = {KS_FX_HAIR, 70, 255, 0};
    rig_start(&r, KS_FX_HAIR, 70, 255, 0);
    CHECK(fabs_diff(r.arm, 70) <= 3);
    CHECK(abs(ks_trig_arm(&r.t) - 70) <= 3);
    // A hard pull stops at the arm and the arm does not give way.
    r.finger = 255;
    rig_run(&r, 1000);
    CHECK(abs(rig_pos(&r) - 70) <= 3);
    CHECK_EQ(r.duty, 0);
    // The host sees a full pull at the stop, and a proportional value before it.
    CHECK_EQ(ks_trig_output(&cfg, rig_pos(&r)), 255);
    CHECK_EQ(ks_trig_output(&cfg, 0), 0);
    CHECK_EQ(ks_trig_output(&cfg, 32), 127);
    // Other effects report the position unchanged.
    cfg.effect = KS_FX_RESISTANCE;
    CHECK_EQ(ks_trig_output(&cfg, 32), 32);
}

static int time_to_reach(uint8_t strength, int target, int limit_ms)
{
    rig_t r;
    int ms = 0;
    rig_start(&r, KS_FX_RESISTANCE, 60, strength, 0);
    r.finger = 255;
    while (ms < limit_ms && rig_pos(&r) < target) {
        rig_run(&r, 1);
        ms++;
    }
    return ms;
}

void test_trig_resistance(void)
{
    rig_t r;
    rig_start(&r, KS_FX_RESISTANCE, 60, 128, 0);
    CHECK(fabs_diff(r.arm, 60) <= 3);
    // Below the start point the trigger moves freely.
    r.finger = 40;
    rig_run(&r, 200);
    CHECK_EQ(rig_pos(&r), 40);
    CHECK(fabs_diff(r.arm, 60) <= 3);

    // Past the start point the arm gives way, slower for a higher strength.
    int medium = time_to_reach(128, 200, 5000);
    int stiff = time_to_reach(230, 200, 5000);
    int rigid = time_to_reach(255, 200, 2000);
    CHECK(medium > 100 && medium < 600);
    CHECK(stiff > 2 * medium && stiff < 5000);
    CHECK_EQ(rigid, 2000); // full strength is a wall

    // Holding still: the arm stays close to the trigger and the motor rests.
    rig_start(&r, KS_FX_RESISTANCE, 60, 128, 0);
    r.finger = 150;
    rig_run(&r, 1500);
    CHECK_EQ(rig_pos(&r), 150);
    CHECK(r.arm >= 150 && r.arm <= 150 + 24);
    int before = r.reversals;
    rig_run(&r, 500);
    CHECK_EQ(r.reversals, before);
    CHECK_EQ(r.duty, 0);

    // Letting go: the arm follows the trigger back to the start point.
    r.finger = 0;
    rig_run(&r, 400);
    CHECK(fabs_diff(r.arm, 60) <= 4);
}

void test_trig_click(void)
{
    rig_t r;
    int ms = 0;
    rig_start(&r, KS_FX_CLICK, 60, 200, 0); // default span: click 24 units past start
    r.finger = 255;
    while (ms < 2000 && !r.t.click_fired) {
        rig_run(&r, 1);
        ms++;
    }
    CHECK(r.t.click_fired);
    CHECK(ms > 50 && ms < 1000); // the arm resisted first
    CHECK(rig_pos(&r) >= 84);
    // After the click the arm drops away and the pull completes.
    rig_run(&r, 400);
    CHECK(r.arm >= KS_ARM_PARK - 3);
    CHECK_EQ(rig_pos(&r), 255);
    // Releasing the trigger arms the click again.
    r.finger = 0;
    rig_run(&r, 600);
    CHECK(!r.t.click_fired);
    CHECK(fabs_diff(r.arm, 60) <= 4);

    // Full strength must not make the click unreachable.
    ms = 0;
    rig_start(&r, KS_FX_CLICK, 60, 255, 0);
    r.finger = 255;
    while (ms < 5000 && !r.t.click_fired) {
        rig_run(&r, 1);
        ms++;
    }
    CHECK(r.t.click_fired);
}

void test_trig_pulse(void)
{
    rig_t r;
    rig_start(&r, KS_FX_PULSE, 80, 128, 10); // 10 Hz, amplitude 12 units
    // Short of the start point nothing pulses.
    r.finger = 40;
    rig_run(&r, 300);
    CHECK(!r.t.pulse_on);
    CHECK_EQ(r.duty, 0);

    // Held past the start point: the finger can pull through to where it
    // wants to be, and the arm keeps kicking the trigger back.
    r.finger = 150;
    rig_run(&r, 2000);
    CHECK(r.t.pulse_on);
    r.reversals = 0;
    r.pos_min = 255;
    r.pos_max = 0;
    rig_run(&r, 1000);
    CHECK(r.reversals >= 16 && r.reversals <= 24); // two direction changes per cycle
    CHECK_EQ(r.pos_max, 150);
    CHECK(r.pos_min <= 142 && r.pos_min >= 130);

    // Letting go stops the pulse and returns the arm to the start point.
    r.finger = 0;
    rig_run(&r, 600);
    CHECK(!r.t.pulse_on);
    CHECK(fabs_diff(r.arm, 80) <= 4);
    CHECK_EQ(r.duty, 0);
}

void test_trig_current_limit(void)
{
    rig_t r;
    ks_trigger_fx_cfg_t far_move = {KS_FX_HAIR, 0, 255, 0};
    rig_start(&r, KS_FX_OFF, 0, 0, 0);
    ks_trig_set(&r.t, &far_move);
    rig_run(&r, 5);
    CHECK_EQ(r.duty, 255); // a long move starts at full duty

    // Above the soft limit the duty folds back to the minimum that still moves.
    r.force_ma = 700;
    rig_run(&r, 30);
    CHECK_EQ(r.duty, r.p.min_duty);
    CHECK_EQ(r.t.phase, KS_TRIG_RUN);
    // It recovers when the current falls.
    r.force_ma = 100;
    rig_run(&r, 60);
    CHECK_EQ(r.duty, 255);

    // Above the trip level for trip_ms the motor stops and rests.
    r.force_ma = 1000;
    rig_run(&r, r.p.trip_ms - 1);
    CHECK_EQ(r.t.phase, KS_TRIG_RUN);
    rig_run(&r, 1);
    CHECK_EQ(r.t.phase, KS_TRIG_COOLOFF);
    CHECK_EQ(r.duty, 0);
    CHECK_EQ(r.t.trips, 1);
    r.force_ma = -1;
    rig_run(&r, r.p.cooloff_ms - 1);
    CHECK_EQ(r.t.phase, KS_TRIG_COOLOFF);
    CHECK_EQ(r.duty, 0);
    // Then it homes again, because the estimate can no longer be trusted.
    rig_run(&r, 1);
    CHECK_EQ(r.t.phase, KS_TRIG_HOMING);
    rig_run(&r, 400);
    CHECK_EQ(r.t.phase, KS_TRIG_RUN);

    // A brief spike shorter than trip_ms does not trip.
    r.force_ma = 1000;
    rig_run(&r, r.p.trip_ms / 2);
    r.force_ma = 100;
    rig_run(&r, 5);
    r.force_ma = 1000;
    rig_run(&r, r.p.trip_ms / 2);
    CHECK_EQ(r.t.phase, KS_TRIG_RUN);
    CHECK_EQ(r.t.trips, 1);
}

void test_trig_driver_fault(void)
{
    rig_t r;
    ks_trigger_fx_cfg_t far_move = {KS_FX_HAIR, 0, 255, 0};
    rig_start(&r, KS_FX_OFF, 0, 0, 0);
    ks_trig_set(&r.t, &far_move);
    rig_run(&r, 5);
    CHECK(r.duty != 0);
    // The DRV8833 fault line stops the motor on the same step.
    r.fault = true;
    rig_run(&r, 1);
    CHECK_EQ(r.duty, 0);
    CHECK_EQ(r.t.phase, KS_TRIG_COOLOFF);
    rig_run(&r, 2000);
    CHECK_EQ(r.duty, 0); // stays off while the fault lasts
    r.fault = false;
    rig_run(&r, r.p.cooloff_ms + 400);
    CHECK_EQ(r.t.phase, KS_TRIG_RUN);
}
