// Profiles, settings, their stored form, and the input-to-gamepad mapper.
#pragma once

#include "ks_input.h"
#include "ks_types.h"

#define KS_PROFILE_COUNT 4
#define KS_PROFILE_NAME_LEN 12
#define KS_BLOB_MAX 96

typedef enum {
    KS_ZONE_FACE, KS_ZONE_DPAD, KS_ZONE_SYSTEM, KS_ZONE_LRING, KS_ZONE_RRING, KS_ZONE_COUNT
} ks_led_zone_t;

typedef struct {
    uint8_t r, g, b;
    uint8_t brightness; // percent
} ks_zone_cfg_t;

typedef struct {
    char name[KS_PROFILE_NAME_LEN];
    uint8_t map[KS_IN_COUNT];    // ks_output_t for each physical input
    uint8_t stick_deadzone[2];   // percent, 0 to 40
    uint8_t stick_curve[2];      // exponent times 10, 5 to 30
    uint8_t trigger_deadzone[2]; // percent, 0 to 30
    ks_trigger_fx_cfg_t trigger_fx[2];
    ks_zone_cfg_t zone[KS_ZONE_COUNT];
} ks_profile_t;

typedef struct {
    uint8_t mode;           // ks_mode_t
    uint8_t active_profile; // 0 to KS_PROFILE_COUNT - 1
    uint8_t backlight_pct;
    uint8_t sleep_minutes;  // 0 disables the idle timeout
} ks_settings_t;

void ks_profile_default(ks_profile_t *p, int index);
void ks_settings_default(ks_settings_t *s);

// Stored form: 'K', tag, version, payload length, payload, CRC32. The unpack
// functions reject a bad tag, version, length, checksum, or field value and
// leave the destination untouched.
size_t ks_profile_pack(const ks_profile_t *p, uint8_t *buf, size_t cap);
bool ks_profile_unpack(ks_profile_t *p, const uint8_t *buf, size_t len);
size_t ks_settings_pack(const ks_settings_t *s, uint8_t *buf, size_t cap);
bool ks_settings_unpack(ks_settings_t *s, const uint8_t *buf, size_t len);
size_t ks_calibration_pack(const ks_calibration_t *c, uint8_t *buf, size_t cap);
bool ks_calibration_unpack(ks_calibration_t *c, const uint8_t *buf, size_t len);
uint32_t ks_crc32(const uint8_t *data, size_t len);

// Key-value storage supplied by the platform. NVS on the controller, an array
// in the tests.
typedef struct {
    bool (*read)(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *len);
    bool (*write)(void *ctx, const char *key, const uint8_t *buf, size_t len);
    void *ctx;
} ks_storage_t;

// The load functions fill in defaults and return false when the stored value
// is missing or invalid.
bool ks_store_load_profile(const ks_storage_t *st, int index, ks_profile_t *p);
bool ks_store_save_profile(const ks_storage_t *st, int index, const ks_profile_t *p);
bool ks_store_load_settings(const ks_storage_t *st, ks_settings_t *s);
bool ks_store_save_settings(const ks_storage_t *st, const ks_settings_t *s);
bool ks_store_load_calibration(const ks_storage_t *st, ks_calibration_t *c);
bool ks_store_save_calibration(const ks_storage_t *st, const ks_calibration_t *c);

// Applies the profile's mapping. sticks holds LX, LY, RX, RY and triggers
// holds LT, RT, both already calibrated. Opposite D-pad directions cancel.
void ks_map_apply(const ks_profile_t *p, uint32_t pressed, const int16_t sticks[4],
                  const uint8_t triggers[2], ks_gamepad_t *out);

// Decides when Fn opens the menu. With no output mapped to Fn, a press opens
// it at once. With an output mapped, Fn sends that output and opens the menu
// after a one second hold. Returns true on the tick the menu should open.
// While the menu is open the UI handles Fn itself and this returns false.
typedef struct {
    uint16_t held_ms;
    bool was_down, fired;
} ks_fn_state_t;

#define KS_FN_HOLD_MS 1000

bool ks_fn_menu_toggle(ks_fn_state_t *s, bool fn_down, bool fn_mapped, bool menu_open,
                       uint16_t dt_ms);
