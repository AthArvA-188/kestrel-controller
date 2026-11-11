// Logic for the LED chain, the fuel gauge, the charger pins, and the idle timer.
#pragma once

#include "ks_pinmap_gen.h"
#include "ks_profile.h"
#include "ks_types.h"

#define KS_LED_LIMIT_MA 250

// Zone of the LED at a chain position.
uint8_t ks_led_zone_of(int chain_index);

// Fills grb with KS_LED_COUNT * 3 bytes in chain order, scaled so the
// estimated current stays at or under limit_ma. Returns that estimate.
uint16_t ks_led_render(const ks_zone_cfg_t zone[KS_ZONE_COUNT], uint8_t master_pct,
                       uint16_t limit_ma, uint8_t *grb);
uint16_t ks_led_estimate_ma(const uint8_t *grb, int led_count);

// MAX17048 register conversions.
uint8_t ks_fuel_soc_percent(uint16_t soc_reg);
uint16_t ks_fuel_vcell_mv(uint16_t vcell_reg);

typedef enum { KS_CHG_ON_BATTERY, KS_CHG_CHARGING, KS_CHG_FULL } ks_charge_state_t;
ks_charge_state_t ks_charge_state(bool charging, bool usb_power);

// True when the pad is in use: a button down, a stick off centre, or a
// trigger pulled.
bool ks_input_active(uint32_t pressed, const int16_t sticks[4], const uint8_t triggers[2]);

typedef struct {
    uint32_t idle_ms;
} ks_idle_t;

// Returns true when the pad should enter deep sleep. inhibit holds the timer
// at zero, for example while a USB host is attached.
bool ks_idle_step(ks_idle_t *t, bool active, bool inhibit, uint32_t dt_ms, uint8_t sleep_minutes);
