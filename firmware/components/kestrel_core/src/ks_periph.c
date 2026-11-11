#include "ks_periph.h"

#include <string.h>

// SK6812MINI estimates: about 1 mA per LED with all channels off and 12 mA
// per channel at full scale.
#define LED_IDLE_UA 1000
#define LED_CHANNEL_UA 12000

static const uint8_t zone_of[KS_LED_COUNT] = {
    [KS_LED_FN] = KS_ZONE_SYSTEM,      [KS_LED_VIEW] = KS_ZONE_SYSTEM,
    [KS_LED_HOME] = KS_ZONE_SYSTEM,    [KS_LED_MENU] = KS_ZONE_SYSTEM,
    [KS_LED_DPAD_SE] = KS_ZONE_DPAD,   [KS_LED_DPAD_SW] = KS_ZONE_DPAD,
    [KS_LED_DPAD_NW] = KS_ZONE_DPAD,   [KS_LED_DPAD_NE] = KS_ZONE_DPAD,
    [KS_LED_X] = KS_ZONE_FACE,         [KS_LED_Y] = KS_ZONE_FACE,
    [KS_LED_B] = KS_ZONE_FACE,         [KS_LED_A] = KS_ZONE_FACE,
    [KS_LED_LRING_270] = KS_ZONE_LRING, [KS_LED_LRING_210] = KS_ZONE_LRING,
    [KS_LED_LRING_150] = KS_ZONE_LRING, [KS_LED_LRING_90] = KS_ZONE_LRING,
    [KS_LED_LRING_30] = KS_ZONE_LRING,  [KS_LED_LRING_330] = KS_ZONE_LRING,
    [KS_LED_RRING_30] = KS_ZONE_RRING,  [KS_LED_RRING_330] = KS_ZONE_RRING,
    [KS_LED_RRING_270] = KS_ZONE_RRING, [KS_LED_RRING_210] = KS_ZONE_RRING,
    [KS_LED_RRING_150] = KS_ZONE_RRING, [KS_LED_RRING_90] = KS_ZONE_RRING,
};

uint8_t ks_led_zone_of(int chain_index)
{
    return chain_index >= 0 && chain_index < KS_LED_COUNT ? zone_of[chain_index] : KS_ZONE_SYSTEM;
}

static uint32_t estimate_ua(const uint8_t *grb, int led_count)
{
    uint32_t sum = 0;
    for (int i = 0; i < led_count * 3; i++) {
        sum += grb[i];
    }
    return (uint32_t)led_count * LED_IDLE_UA + sum * LED_CHANNEL_UA / 255;
}

uint16_t ks_led_estimate_ma(const uint8_t *grb, int led_count)
{
    return (uint16_t)((estimate_ua(grb, led_count) + 999) / 1000);
}

uint16_t ks_led_render(const ks_zone_cfg_t zone[KS_ZONE_COUNT], uint8_t master_pct,
                       uint16_t limit_ma, uint8_t *grb)
{
    uint32_t master = master_pct > 100 ? 100 : master_pct;
    for (int i = 0; i < KS_LED_COUNT; i++) {
        const ks_zone_cfg_t *z = &zone[zone_of[i]];
        uint32_t scale = (z->brightness > 100 ? 100u : z->brightness) * master; // 0 to 10000
        grb[3 * i + 0] = (uint8_t)(z->g * scale / 10000);
        grb[3 * i + 1] = (uint8_t)(z->r * scale / 10000);
        grb[3 * i + 2] = (uint8_t)(z->b * scale / 10000);
    }
    uint32_t idle = (uint32_t)KS_LED_COUNT * LED_IDLE_UA;
    uint32_t total = estimate_ua(grb, KS_LED_COUNT);
    uint32_t limit = (uint32_t)limit_ma * 1000;
    if (total > limit) {
        // Only the lit part of the current scales with the data.
        uint32_t k = limit > idle ? (limit - idle) * 256 / (total - idle) : 0;
        for (int i = 0; i < KS_LED_COUNT * 3; i++) {
            grb[i] = (uint8_t)(grb[i] * k / 256);
        }
    }
    return ks_led_estimate_ma(grb, KS_LED_COUNT);
}

uint8_t ks_fuel_soc_percent(uint16_t soc_reg)
{
    // The high byte is whole percent and the low byte is 1/256 percent.
    uint32_t pct = (soc_reg + 128u) >> 8;
    return (uint8_t)(pct > 100 ? 100 : pct);
}

uint16_t ks_fuel_vcell_mv(uint16_t vcell_reg)
{
    // 78.125 microvolts per count.
    return (uint16_t)((uint32_t)vcell_reg * 5 / 64);
}

ks_charge_state_t ks_charge_state(bool charging, bool usb_power)
{
    if (!usb_power) {
        return KS_CHG_ON_BATTERY;
    }
    return charging ? KS_CHG_CHARGING : KS_CHG_FULL;
}

bool ks_input_active(uint32_t pressed, const int16_t sticks[4], const uint8_t triggers[2])
{
    if (pressed || triggers[0] > 12 || triggers[1] > 12) {
        return true;
    }
    for (int i = 0; i < 4; i++) {
        if (sticks[i] > 3277 || sticks[i] < -3277) {
            return true;
        }
    }
    return false;
}

bool ks_idle_step(ks_idle_t *t, bool active, bool inhibit, uint32_t dt_ms, uint8_t sleep_minutes)
{
    if (active || inhibit || sleep_minutes == 0) {
        t->idle_ms = 0;
        return false;
    }
    t->idle_ms += dt_ms;
    return t->idle_ms >= (uint32_t)sleep_minutes * 60000u;
}
