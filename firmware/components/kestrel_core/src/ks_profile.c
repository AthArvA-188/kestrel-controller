#include "ks_profile.h"

#include <stdio.h>
#include <string.h>

#define BLOB_VERSION 1
#define BLOB_OVERHEAD 8
#define PROFILE_PAYLOAD (KS_PROFILE_NAME_LEN + KS_IN_COUNT + 6 + 8 + 4 * KS_ZONE_COUNT)
#define SETTINGS_PAYLOAD 4
#define CAL_PAYLOAD (4 * 7 + 2 * 4 + 2)

static const char *const input_names[KS_IN_COUNT] = {
    "A", "B", "X", "Y", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    "View", "Menu", "Home", "Fn", "LB", "RB", "L3", "R3", "P1", "P2", "P3", "P4",
};
static const char *const output_names[KS_OUT_COUNT] = {
    "None", "A", "B", "X", "Y", "LB", "RB", "View", "Menu", "Home", "L3", "R3",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right", "LT", "RT",
    "Extra 1", "Extra 2", "Extra 3",
};
static const char *const mode_names[KS_MODE_COUNT] = {
    "USB XInput", "USB DInput", "Bluetooth Xbox", "Bluetooth HID", "WiFi",
};
static const char *const fx_names[KS_FX_COUNT] = {
    "Off", "Resistance", "Hair trigger", "Click", "Pulse",
};

const char *ks_input_name(int i) { return i >= 0 && i < KS_IN_COUNT ? input_names[i] : "?"; }
const char *ks_output_name(int o) { return o >= 0 && o < KS_OUT_COUNT ? output_names[o] : "?"; }
const char *ks_mode_name(int m) { return m >= 0 && m < KS_MODE_COUNT ? mode_names[m] : "?"; }
const char *ks_fx_name(int e) { return e >= 0 && e < KS_FX_COUNT ? fx_names[e] : "?"; }

static const uint8_t default_map[KS_IN_COUNT] = {
    KS_OUT_A, KS_OUT_B, KS_OUT_X, KS_OUT_Y,
    KS_OUT_DUP, KS_OUT_DDOWN, KS_OUT_DLEFT, KS_OUT_DRIGHT,
    KS_OUT_VIEW, KS_OUT_MENU, KS_OUT_HOME, KS_OUT_NONE,
    KS_OUT_LB, KS_OUT_RB, KS_OUT_L3, KS_OUT_R3,
    KS_OUT_NONE, KS_OUT_NONE, KS_OUT_NONE, KS_OUT_NONE,
};

void ks_profile_default(ks_profile_t *p, int index)
{
    static const char *const names[KS_PROFILE_COUNT] = {"Default", "Shooter", "Racing", "Custom"};
    // These levels keep every default profile under the 250 mA LED budget, so
    // the current cap does not dim them further.
    static const ks_zone_cfg_t ring[KS_PROFILE_COUNT] = {
        {45, 212, 191, 40}, {239, 68, 68, 40}, {59, 130, 246, 40}, {168, 85, 247, 40},
    };
    if (index < 0 || index >= KS_PROFILE_COUNT) {
        index = 0;
    }
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", names[index]);
    memcpy(p->map, default_map, sizeof p->map);
    for (int i = 0; i < 2; i++) {
        p->stick_deadzone[i] = 8;
        p->stick_curve[i] = 10;
        p->trigger_deadzone[i] = 4;
        p->trigger_fx[i] = (ks_trigger_fx_cfg_t){KS_FX_OFF, 60, 160, 0};
    }
    for (int z = 0; z < KS_ZONE_COUNT; z++) {
        p->zone[z] = (ks_zone_cfg_t){255, 255, 255, 25};
    }
    p->zone[KS_ZONE_LRING] = ring[index];
    p->zone[KS_ZONE_RRING] = ring[index];

    if (index == 1) {
        // Face buttons on the paddles so both thumbs stay on the sticks.
        p->map[KS_IN_P1] = KS_OUT_A;
        p->map[KS_IN_P2] = KS_OUT_B;
        p->map[KS_IN_P3] = KS_OUT_X;
        p->map[KS_IN_P4] = KS_OUT_Y;
        p->stick_curve[1] = 18;
        p->trigger_fx[0] = (ks_trigger_fx_cfg_t){KS_FX_HAIR, 90, 255, 0};
        p->trigger_fx[1] = (ks_trigger_fx_cfg_t){KS_FX_HAIR, 70, 255, 0};
    } else if (index == 2) {
        p->map[KS_IN_P1] = KS_OUT_X;
        p->map[KS_IN_P3] = KS_OUT_B;
        p->trigger_fx[0] = (ks_trigger_fx_cfg_t){KS_FX_RESISTANCE, 30, 200, 0};
        p->trigger_fx[1] = (ks_trigger_fx_cfg_t){KS_FX_RESISTANCE, 20, 120, 0};
    }
}

void ks_settings_default(ks_settings_t *s)
{
    s->mode = KS_MODE_USB_XINPUT;
    s->active_profile = 0;
    s->backlight_pct = 80;
    s->sleep_minutes = 10;
}

uint32_t ks_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// The payload of n bytes is already at buf + 4.
static size_t seal(uint8_t *buf, uint8_t tag, size_t n)
{
    buf[0] = 'K';
    buf[1] = tag;
    buf[2] = BLOB_VERSION;
    buf[3] = (uint8_t)n;
    uint32_t crc = ks_crc32(buf, n + 4);
    put16(buf + 4 + n, (uint16_t)crc);
    put16(buf + 6 + n, (uint16_t)(crc >> 16));
    return n + BLOB_OVERHEAD;
}

static const uint8_t *unseal(const uint8_t *buf, size_t len, uint8_t tag, size_t n)
{
    if (len != n + BLOB_OVERHEAD || buf[0] != 'K' || buf[1] != tag || buf[2] != BLOB_VERSION ||
        buf[3] != n) {
        return NULL;
    }
    uint32_t crc = get16(buf + 4 + n) | ((uint32_t)get16(buf + 6 + n) << 16);
    return crc == ks_crc32(buf, n + 4) ? buf + 4 : NULL;
}

size_t ks_profile_pack(const ks_profile_t *p, uint8_t *buf, size_t cap)
{
    if (cap < PROFILE_PAYLOAD + BLOB_OVERHEAD) {
        return 0;
    }
    uint8_t *w = buf + 4;
    memcpy(w, p->name, KS_PROFILE_NAME_LEN);
    w += KS_PROFILE_NAME_LEN;
    memcpy(w, p->map, KS_IN_COUNT);
    w += KS_IN_COUNT;
    for (int i = 0; i < 2; i++) {
        *w++ = p->stick_deadzone[i];
        *w++ = p->stick_curve[i];
        *w++ = p->trigger_deadzone[i];
    }
    for (int i = 0; i < 2; i++) {
        const ks_trigger_fx_cfg_t *f = &p->trigger_fx[i];
        *w++ = f->effect;
        *w++ = f->start;
        *w++ = f->strength;
        *w++ = f->param;
    }
    for (int z = 0; z < KS_ZONE_COUNT; z++) {
        *w++ = p->zone[z].r;
        *w++ = p->zone[z].g;
        *w++ = p->zone[z].b;
        *w++ = p->zone[z].brightness;
    }
    return seal(buf, 'P', PROFILE_PAYLOAD);
}

bool ks_profile_unpack(ks_profile_t *p, const uint8_t *buf, size_t len)
{
    const uint8_t *r = unseal(buf, len, 'P', PROFILE_PAYLOAD);
    if (!r) {
        return false;
    }
    ks_profile_t t;
    memcpy(t.name, r, KS_PROFILE_NAME_LEN);
    t.name[KS_PROFILE_NAME_LEN - 1] = '\0';
    r += KS_PROFILE_NAME_LEN;
    memcpy(t.map, r, KS_IN_COUNT);
    r += KS_IN_COUNT;
    for (int i = 0; i < KS_IN_COUNT; i++) {
        if (t.map[i] >= KS_OUT_COUNT) {
            return false;
        }
    }
    for (int i = 0; i < 2; i++) {
        t.stick_deadzone[i] = *r++;
        t.stick_curve[i] = *r++;
        t.trigger_deadzone[i] = *r++;
        if (t.stick_deadzone[i] > 40 || t.stick_curve[i] < 5 || t.stick_curve[i] > 30 ||
            t.trigger_deadzone[i] > 30) {
            return false;
        }
    }
    for (int i = 0; i < 2; i++) {
        t.trigger_fx[i] = (ks_trigger_fx_cfg_t){r[0], r[1], r[2], r[3]};
        r += 4;
        if (t.trigger_fx[i].effect >= KS_FX_COUNT) {
            return false;
        }
    }
    for (int z = 0; z < KS_ZONE_COUNT; z++) {
        t.zone[z] = (ks_zone_cfg_t){r[0], r[1], r[2], r[3]};
        r += 4;
        if (t.zone[z].brightness > 100) {
            return false;
        }
    }
    *p = t;
    return true;
}

size_t ks_settings_pack(const ks_settings_t *s, uint8_t *buf, size_t cap)
{
    if (cap < SETTINGS_PAYLOAD + BLOB_OVERHEAD) {
        return 0;
    }
    buf[4] = s->mode;
    buf[5] = s->active_profile;
    buf[6] = s->backlight_pct;
    buf[7] = s->sleep_minutes;
    return seal(buf, 'S', SETTINGS_PAYLOAD);
}

bool ks_settings_unpack(ks_settings_t *s, const uint8_t *buf, size_t len)
{
    const uint8_t *r = unseal(buf, len, 'S', SETTINGS_PAYLOAD);
    if (!r || r[0] >= KS_MODE_COUNT || r[1] >= KS_PROFILE_COUNT || r[2] < 5 || r[2] > 100 ||
        r[3] > 60) {
        return false;
    }
    *s = (ks_settings_t){r[0], r[1], r[2], r[3]};
    return true;
}

size_t ks_calibration_pack(const ks_calibration_t *c, uint8_t *buf, size_t cap)
{
    if (cap < CAL_PAYLOAD + BLOB_OVERHEAD) {
        return 0;
    }
    uint8_t *w = buf + 4;
    for (int i = 0; i < 4; i++) {
        put16(w, c->axis[i].min);
        put16(w + 2, c->axis[i].centre);
        put16(w + 4, c->axis[i].max);
        w[6] = c->axis[i].invert;
        w += 7;
    }
    for (int i = 0; i < 2; i++) {
        put16(w, c->trigger[i].released);
        put16(w + 2, c->trigger[i].pulled);
        w += 4;
    }
    *w++ = c->bumper.lb_released;
    *w++ = c->bumper.rb_released;
    return seal(buf, 'C', CAL_PAYLOAD);
}

bool ks_calibration_unpack(ks_calibration_t *c, const uint8_t *buf, size_t len)
{
    const uint8_t *r = unseal(buf, len, 'C', CAL_PAYLOAD);
    if (!r) {
        return false;
    }
    ks_calibration_t t;
    for (int i = 0; i < 4; i++) {
        t.axis[i] = (ks_axis_cal_t){get16(r), get16(r + 2), get16(r + 4), r[6] ? 1 : 0};
        r += 7;
        if (!(t.axis[i].min < t.axis[i].centre && t.axis[i].centre < t.axis[i].max)) {
            return false;
        }
    }
    for (int i = 0; i < 2; i++) {
        t.trigger[i] = (ks_trigger_cal_t){get16(r), get16(r + 2)};
        r += 4;
        if (t.trigger[i].released == t.trigger[i].pulled) {
            return false;
        }
    }
    t.bumper.lb_released = r[0];
    t.bumper.rb_released = r[1];
    if (r[0] > 3 || r[1] > 3) {
        return false;
    }
    *c = t;
    return true;
}

static void profile_key(int index, char key[8]) { snprintf(key, 8, "prof%d", index); }

bool ks_store_load_profile(const ks_storage_t *st, int index, ks_profile_t *p)
{
    uint8_t buf[KS_BLOB_MAX];
    size_t len = 0;
    char key[8];
    profile_key(index, key);
    if (st && st->read(st->ctx, key, buf, sizeof buf, &len) && ks_profile_unpack(p, buf, len)) {
        return true;
    }
    ks_profile_default(p, index);
    return false;
}

bool ks_store_save_profile(const ks_storage_t *st, int index, const ks_profile_t *p)
{
    uint8_t buf[KS_BLOB_MAX];
    char key[8];
    profile_key(index, key);
    size_t len = ks_profile_pack(p, buf, sizeof buf);
    return st && len && st->write(st->ctx, key, buf, len);
}

bool ks_store_load_settings(const ks_storage_t *st, ks_settings_t *s)
{
    uint8_t buf[KS_BLOB_MAX];
    size_t len = 0;
    if (st && st->read(st->ctx, "settings", buf, sizeof buf, &len) &&
        ks_settings_unpack(s, buf, len)) {
        return true;
    }
    ks_settings_default(s);
    return false;
}

bool ks_store_save_settings(const ks_storage_t *st, const ks_settings_t *s)
{
    uint8_t buf[KS_BLOB_MAX];
    size_t len = ks_settings_pack(s, buf, sizeof buf);
    return st && len && st->write(st->ctx, "settings", buf, len);
}

bool ks_store_load_calibration(const ks_storage_t *st, ks_calibration_t *c)
{
    uint8_t buf[KS_BLOB_MAX];
    size_t len = 0;
    if (st && st->read(st->ctx, "cal", buf, sizeof buf, &len) &&
        ks_calibration_unpack(c, buf, len)) {
        return true;
    }
    ks_calibration_default(c);
    return false;
}

bool ks_store_save_calibration(const ks_storage_t *st, const ks_calibration_t *c)
{
    uint8_t buf[KS_BLOB_MAX];
    size_t len = ks_calibration_pack(c, buf, sizeof buf);
    return st && len && st->write(st->ctx, "cal", buf, len);
}

void ks_map_apply(const ks_profile_t *p, uint32_t pressed, const int16_t sticks[4],
                  const uint8_t triggers[2], ks_gamepad_t *out)
{
    const uint32_t ud = KS_OUT_BIT(KS_OUT_DUP) | KS_OUT_BIT(KS_OUT_DDOWN);
    const uint32_t lr = KS_OUT_BIT(KS_OUT_DLEFT) | KS_OUT_BIT(KS_OUT_DRIGHT);

    memset(out, 0, sizeof *out);
    out->lx = sticks[0];
    out->ly = sticks[1];
    out->rx = sticks[2];
    out->ry = sticks[3];
    out->lt = triggers[0];
    out->rt = triggers[1];
    for (int i = 0; i < KS_IN_COUNT; i++) {
        uint8_t o = p->map[i];
        if ((pressed & KS_IN_BIT(i)) && o != KS_OUT_NONE && o < KS_OUT_COUNT) {
            out->buttons |= KS_OUT_BIT(o);
        }
    }
    if (out->buttons & KS_OUT_BIT(KS_OUT_LT)) {
        out->lt = 255;
    }
    if (out->buttons & KS_OUT_BIT(KS_OUT_RT)) {
        out->rt = 255;
    }
    if ((out->buttons & ud) == ud) {
        out->buttons &= ~ud;
    }
    if ((out->buttons & lr) == lr) {
        out->buttons &= ~lr;
    }
}

bool ks_fn_menu_toggle(ks_fn_state_t *s, bool fn_down, bool fn_mapped, bool menu_open,
                       uint16_t dt_ms)
{
    bool toggle = false;
    if (menu_open) {
        // The UI owns Fn while the menu is open. Mark the press as used so
        // that holding Fn after the menu closes does not reopen it.
        s->fired = true;
    } else if (fn_down && !s->was_down) {
        s->held_ms = 0;
        s->fired = !fn_mapped;
        toggle = !fn_mapped;
    } else if (fn_down && !s->fired) {
        s->held_ms = (uint16_t)(s->held_ms + dt_ms < KS_FN_HOLD_MS ? s->held_ms + dt_ms
                                                                  : KS_FN_HOLD_MS);
        if (s->held_ms >= KS_FN_HOLD_MS) {
            s->fired = true;
            toggle = true;
        }
    }
    s->was_down = fn_down;
    return toggle;
}
