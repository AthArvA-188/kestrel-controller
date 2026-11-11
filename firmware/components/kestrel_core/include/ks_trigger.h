// Adaptive trigger control. One instance per trigger.
//
// A motor turns a self-locking worm that moves a stop arm. Nothing senses the
// arm, so the controller estimates its position from the commanded duty and
// finds the park end stop by watching the motor current. Positions are in
// trigger units: 0 is released and 255 is a full pull. The arm position is
// the trigger position at which the trigger meets the arm.
#pragma once

#include "ks_types.h"

#define KS_ARM_PARK 280 // arm clear of the trigger's whole travel

typedef struct {
    uint16_t travel_ms;  // time for the arm to cross KS_ARM_PARK units at full duty
    uint16_t stall_ma;   // current that marks the park end stop while homing
    uint16_t limit_ma;   // above this the duty folds back
    uint16_t trip_ma;    // above this for trip_ms the motor stops
    uint16_t trip_ms;
    uint16_t cooloff_ms; // rest after a trip or a driver fault
    uint8_t min_duty;    // smallest duty that moves the arm
} ks_trig_params_t;

typedef enum { KS_TRIG_HOMING, KS_TRIG_RUN, KS_TRIG_COOLOFF } ks_trig_phase_t;

typedef struct {
    ks_trigger_fx_cfg_t cfg;
    uint8_t phase;
    bool click_fired;
    bool pulse_on;
    uint8_t pulse_base; // trigger position latched at the last half period
    int32_t arm_q8;    // estimated arm position times 256
    int32_t target_q8; // wanted arm position times 256
    uint32_t timer_ms;
    uint16_t over_ms;
    uint16_t pulse_ms;
    uint8_t duty_cap;
    uint16_t trips;
} ks_trig_t;

void ks_trig_params_default(ks_trig_params_t *p);
void ks_trig_init(ks_trig_t *t); // starts by homing to park
void ks_trig_set(ks_trig_t *t, const ks_trigger_fx_cfg_t *cfg);

// Runs one control step. Returns the motor duty from -255 to 255. Positive
// moves the arm toward the released end of the trigger travel, negative moves
// it toward park, and 0 leaves the worm to hold the arm.
int16_t ks_trig_step(ks_trig_t *t, const ks_trig_params_t *p, uint8_t trigger_pos,
                     uint16_t current_ma, bool driver_fault, uint16_t dt_ms);

int ks_trig_arm(const ks_trig_t *t);

// Value to report to the host. A hair trigger reads as a full pull once the
// trigger reaches the arm.
uint8_t ks_trig_output(const ks_trigger_fx_cfg_t *cfg, uint8_t trigger_pos);
