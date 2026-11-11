// Mapping, profiles, storage, the Fn gesture, and host report formats.
#include <string.h>

#include "ks_profile.h"
#include "ks_reports.h"
#include "test.h"

static const int16_t centre[4] = {0, 0, 0, 0};
static const uint8_t released[2] = {0, 0};

void test_default_mapping(void)
{
    static const struct {
        int input, output;
    } same[] = {
        {KS_IN_A, KS_OUT_A},         {KS_IN_B, KS_OUT_B},           {KS_IN_X, KS_OUT_X},
        {KS_IN_Y, KS_OUT_Y},         {KS_IN_DUP, KS_OUT_DUP},       {KS_IN_DDOWN, KS_OUT_DDOWN},
        {KS_IN_DLEFT, KS_OUT_DLEFT}, {KS_IN_DRIGHT, KS_OUT_DRIGHT}, {KS_IN_VIEW, KS_OUT_VIEW},
        {KS_IN_MENU, KS_OUT_MENU},   {KS_IN_HOME, KS_OUT_HOME},     {KS_IN_LB, KS_OUT_LB},
        {KS_IN_RB, KS_OUT_RB},       {KS_IN_L3, KS_OUT_L3},         {KS_IN_R3, KS_OUT_R3},
    };
    ks_profile_t p;
    ks_gamepad_t g;
    ks_profile_default(&p, 0);
    for (size_t i = 0; i < sizeof same / sizeof same[0]; i++) {
        ks_map_apply(&p, KS_IN_BIT(same[i].input), centre, released, &g);
        CHECK_EQ(g.buttons, KS_OUT_BIT(same[i].output));
    }
    // Fn and the paddles send nothing until the user maps them.
    ks_map_apply(&p, KS_IN_BIT(KS_IN_FN) | KS_IN_BIT(KS_IN_P1) | KS_IN_BIT(KS_IN_P2) |
                         KS_IN_BIT(KS_IN_P3) | KS_IN_BIT(KS_IN_P4), centre, released, &g);
    CHECK_EQ(g.buttons, 0);
    // Analogue values pass through.
    ks_map_apply(&p, 0, (const int16_t[4]){100, -200, 300, -400}, (const uint8_t[2]){5, 250}, &g);
    CHECK(g.lx == 100 && g.ly == -200 && g.rx == 300 && g.ry == -400 && g.lt == 5 && g.rt == 250);
}

void test_any_input_to_any_output(void)
{
    ks_profile_t p;
    ks_gamepad_t g;
    ks_profile_default(&p, 0);
    for (int in = 0; in < KS_IN_COUNT; in++) {
        for (int out = 0; out < KS_OUT_COUNT; out++) {
            memset(p.map, KS_OUT_NONE, sizeof p.map);
            p.map[in] = (uint8_t)out;
            ks_map_apply(&p, KS_IN_BIT(in), centre, released, &g);
            CHECK_EQ(g.buttons, out == KS_OUT_NONE ? 0u : KS_OUT_BIT(out));
            CHECK_EQ(g.lt, out == KS_OUT_LT ? 255 : 0);
            CHECK_EQ(g.rt, out == KS_OUT_RT ? 255 : 0);
            // No other input triggers it.
            ks_map_apply(&p, 0xFFFFFu & ~KS_IN_BIT(in), centre, released, &g);
            CHECK_EQ(g.buttons, 0);
        }
    }
}

void test_paddles_remap(void)
{
    ks_profile_t p;
    ks_gamepad_t g;
    ks_profile_default(&p, 0);
    p.map[KS_IN_P1] = KS_OUT_A;
    p.map[KS_IN_P2] = KS_OUT_L3;
    p.map[KS_IN_P3] = KS_OUT_DUP;
    p.map[KS_IN_P4] = KS_OUT_EX1;
    ks_map_apply(&p, KS_IN_BIT(KS_IN_P1) | KS_IN_BIT(KS_IN_P2) | KS_IN_BIT(KS_IN_P3) |
                         KS_IN_BIT(KS_IN_P4), centre, released, &g);
    CHECK_EQ(g.buttons, KS_OUT_BIT(KS_OUT_A) | KS_OUT_BIT(KS_OUT_L3) | KS_OUT_BIT(KS_OUT_DUP) |
                            KS_OUT_BIT(KS_OUT_EX1));
    // Two inputs can share an output, and the real A still works.
    ks_map_apply(&p, KS_IN_BIT(KS_IN_A), centre, released, &g);
    CHECK_EQ(g.buttons, KS_OUT_BIT(KS_OUT_A));
    // The Shooter profile ships with the face buttons on the paddles.
    ks_profile_default(&p, 1);
    ks_map_apply(&p, KS_IN_BIT(KS_IN_P4), centre, released, &g);
    CHECK_EQ(g.buttons, KS_OUT_BIT(KS_OUT_Y));
}

void test_socd_and_trigger_outputs(void)
{
    ks_profile_t p;
    ks_gamepad_t g;
    ks_profile_default(&p, 0);
    p.map[KS_IN_P1] = KS_OUT_DUP;
    p.map[KS_IN_P2] = KS_OUT_DLEFT;
    p.map[KS_IN_P3] = KS_OUT_RT;
    // Up from a paddle and down from the D-pad cancel.
    ks_map_apply(&p, KS_IN_BIT(KS_IN_P1) | KS_IN_BIT(KS_IN_DDOWN), centre, released, &g);
    CHECK_EQ(g.buttons, 0);
    ks_map_apply(&p, KS_IN_BIT(KS_IN_P2) | KS_IN_BIT(KS_IN_DRIGHT) | KS_IN_BIT(KS_IN_DUP),
                 centre, released, &g);
    CHECK_EQ(g.buttons, KS_OUT_BIT(KS_OUT_DUP));
    // A diagonal is fine.
    ks_map_apply(&p, KS_IN_BIT(KS_IN_P1) | KS_IN_BIT(KS_IN_P2), centre, released, &g);
    CHECK_EQ(g.buttons, KS_OUT_BIT(KS_OUT_DUP) | KS_OUT_BIT(KS_OUT_DLEFT));
    // A button mapped to RT gives a full pull and leaves LT alone.
    ks_map_apply(&p, KS_IN_BIT(KS_IN_P3), centre, (const uint8_t[2]){40, 10}, &g);
    CHECK(g.lt == 40 && g.rt == 255);
}

void test_profile_pack_roundtrip(void)
{
    uint8_t buf[KS_BLOB_MAX];
    for (int i = 0; i < KS_PROFILE_COUNT; i++) {
        ks_profile_t p, q;
        ks_profile_default(&p, i);
        p.map[KS_IN_P1 + i] = (uint8_t)(KS_OUT_A + i);
        p.stick_deadzone[1] = (uint8_t)(10 + i);
        p.trigger_fx[0] = (ks_trigger_fx_cfg_t){KS_FX_PULSE, 90, 128, 12};
        p.zone[KS_ZONE_FACE] = (ks_zone_cfg_t){1, 2, 3, 77};
        size_t len = ks_profile_pack(&p, buf, sizeof buf);
        CHECK_EQ(len, 74);
        memset(&q, 0xAA, sizeof q);
        CHECK(ks_profile_unpack(&q, buf, len));
        CHECK(memcmp(&p, &q, sizeof p) == 0);
    }
    // A buffer that is too small is refused before anything is written.
    ks_profile_t p;
    ks_profile_default(&p, 0);
    buf[0] = 0x55;
    CHECK_EQ(ks_profile_pack(&p, buf, 73), 0);
    CHECK_EQ(buf[0], 0x55);
}

void test_profile_rejects_corruption(void)
{
    uint8_t buf[KS_BLOB_MAX], bad[KS_BLOB_MAX];
    ks_profile_t p, q;
    ks_profile_default(&p, 2);
    size_t len = ks_profile_pack(&p, buf, sizeof buf);
    ks_profile_default(&q, 3);
    const ks_profile_t before = q;

    memcpy(bad, buf, len);
    bad[20] ^= 0x01; // payload bit flip
    CHECK(!ks_profile_unpack(&q, bad, len));
    memcpy(bad, buf, len);
    bad[len - 1] ^= 0x80; // checksum
    CHECK(!ks_profile_unpack(&q, bad, len));
    memcpy(bad, buf, len);
    bad[1] = 'S'; // tag of a different record
    CHECK(!ks_profile_unpack(&q, bad, len));
    memcpy(bad, buf, len);
    bad[2] = 9; // unknown version
    CHECK(!ks_profile_unpack(&q, bad, len));
    CHECK(!ks_profile_unpack(&q, buf, len - 1));
    CHECK(!ks_profile_unpack(&q, buf, 0));

    // A valid checksum over an out-of-range field is still refused.
    ks_profile_t wrong = p;
    wrong.map[KS_IN_A] = 99;
    CHECK(!ks_profile_unpack(&q, bad, ks_profile_pack(&wrong, bad, sizeof bad)));
    wrong = p;
    wrong.trigger_fx[1].effect = KS_FX_COUNT;
    CHECK(!ks_profile_unpack(&q, bad, ks_profile_pack(&wrong, bad, sizeof bad)));
    wrong = p;
    wrong.stick_deadzone[0] = 41;
    CHECK(!ks_profile_unpack(&q, bad, ks_profile_pack(&wrong, bad, sizeof bad)));

    CHECK(memcmp(&q, &before, sizeof q) == 0); // failures left q alone
    CHECK(ks_profile_unpack(&q, buf, len));
}

#define SLOTS 8
typedef struct {
    char key[SLOTS][12];
    uint8_t data[SLOTS][KS_BLOB_MAX];
    size_t len[SLOTS];
} mem_store_t;

static int mem_find(mem_store_t *m, const char *key)
{
    for (int i = 0; i < SLOTS; i++) {
        if (strcmp(m->key[i], key) == 0) {
            return i;
        }
    }
    return -1;
}

static bool mem_read(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    mem_store_t *m = ctx;
    int i = mem_find(m, key);
    if (i < 0 || m->len[i] > cap) {
        return false;
    }
    memcpy(buf, m->data[i], m->len[i]);
    *len = m->len[i];
    return true;
}

static bool mem_write(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
    mem_store_t *m = ctx;
    int i = mem_find(m, key);
    if (i < 0) {
        i = mem_find(m, "");
    }
    if (i < 0 || len > KS_BLOB_MAX || strlen(key) >= sizeof m->key[0]) {
        return false;
    }
    strcpy(m->key[i], key);
    memcpy(m->data[i], buf, len);
    m->len[i] = len;
    return true;
}

void test_storage_four_profiles(void)
{
    mem_store_t mem;
    ks_storage_t st = {mem_read, mem_write, &mem};
    ks_profile_t saved[KS_PROFILE_COUNT], p;
    memset(&mem, 0, sizeof mem);

    // Nothing stored yet: defaults, reported as such.
    CHECK(!ks_store_load_profile(&st, 1, &p));
    CHECK(strcmp(p.name, "Shooter") == 0);

    for (int i = 0; i < KS_PROFILE_COUNT; i++) {
        ks_profile_default(&saved[i], i);
        saved[i].map[KS_IN_P1] = (uint8_t)(KS_OUT_LB + i);
        saved[i].stick_deadzone[0] = (uint8_t)(4 * i);
        CHECK(ks_store_save_profile(&st, i, &saved[i]));
    }
    for (int i = 0; i < KS_PROFILE_COUNT; i++) {
        CHECK(ks_store_load_profile(&st, i, &p));
        CHECK(memcmp(&p, &saved[i], sizeof p) == 0);
    }
    // Saving one profile leaves the others alone.
    saved[2].map[KS_IN_P4] = KS_OUT_Y;
    CHECK(ks_store_save_profile(&st, 2, &saved[2]));
    CHECK(ks_store_load_profile(&st, 3, &p));
    CHECK(memcmp(&p, &saved[3], sizeof p) == 0);
    CHECK(ks_store_load_profile(&st, 2, &p));
    CHECK_EQ(p.map[KS_IN_P4], KS_OUT_Y);

    // A damaged record falls back to the default for that slot.
    mem.data[mem_find(&mem, "prof0")][10] ^= 0xFF;
    CHECK(!ks_store_load_profile(&st, 0, &p));
    CHECK(strcmp(p.name, "Default") == 0);
    CHECK_EQ(p.map[KS_IN_P1], KS_OUT_NONE);
}

void test_settings_and_calibration_storage(void)
{
    mem_store_t mem;
    ks_storage_t st = {mem_read, mem_write, &mem};
    ks_settings_t s, s2;
    ks_calibration_t c, c2;
    uint8_t buf[KS_BLOB_MAX];
    memset(&mem, 0, sizeof mem);

    CHECK(!ks_store_load_settings(&st, &s));
    CHECK(s.mode == KS_MODE_USB_XINPUT && s.active_profile == 0);
    s = (ks_settings_t){KS_MODE_WIFI, 3, 60, 15};
    CHECK(ks_store_save_settings(&st, &s));
    CHECK(ks_store_load_settings(&st, &s2));
    CHECK(memcmp(&s, &s2, sizeof s) == 0);
    s.mode = KS_MODE_COUNT; // out of range
    CHECK(!ks_settings_unpack(&s2, buf, ks_settings_pack(&s, buf, sizeof buf)));
    s = (ks_settings_t){KS_MODE_WIFI, KS_PROFILE_COUNT, 60, 15};
    CHECK(!ks_settings_unpack(&s2, buf, ks_settings_pack(&s, buf, sizeof buf)));

    CHECK(!ks_store_load_calibration(&st, &c));
    CHECK_EQ(c.bumper.lb_released, KS_BUMPER_BOTH_HIGH);
    c.axis[2] = (ks_axis_cal_t){1000, 30000, 61000, 1};
    c.trigger[1] = (ks_trigger_cal_t){50000, 20000};
    c.bumper = (ks_bumper_cal_t){2, 1};
    CHECK(ks_store_save_calibration(&st, &c));
    CHECK(ks_store_load_calibration(&st, &c2));
    CHECK(c2.axis[2].min == 1000 && c2.axis[2].centre == 30000 && c2.axis[2].max == 61000);
    CHECK(c2.axis[2].invert == 1 && c2.axis[0].invert == 0);
    CHECK(c2.trigger[1].released == 50000 && c2.trigger[1].pulled == 20000);
    CHECK(c2.bumper.lb_released == 2 && c2.bumper.rb_released == 1);
    c.axis[0].min = c.axis[0].centre; // no travel below centre
    CHECK(!ks_calibration_unpack(&c2, buf, ks_calibration_pack(&c, buf, sizeof buf)));
}

void test_fn_gesture(void)
{
    ks_fn_state_t s;
    memset(&s, 0, sizeof s);
    // Nothing mapped to Fn: the press opens the menu at once.
    CHECK(ks_fn_menu_toggle(&s, true, false, false, 1));
    CHECK(!ks_fn_menu_toggle(&s, true, false, true, 1));
    CHECK(!ks_fn_menu_toggle(&s, false, false, true, 1));
    // The UI closes the menu on the next Fn press. Holding Fn after that must
    // not reopen it.
    CHECK(!ks_fn_menu_toggle(&s, true, false, true, 1));
    CHECK(!ks_fn_menu_toggle(&s, true, false, false, 1));
    CHECK(!ks_fn_menu_toggle(&s, true, false, false, 1));
    CHECK(!ks_fn_menu_toggle(&s, false, false, false, 1));
    CHECK(ks_fn_menu_toggle(&s, true, false, false, 1));

    // An output mapped to Fn: a short press does nothing here, and a one
    // second hold opens the menu once.
    memset(&s, 0, sizeof s);
    for (int ms = 0; ms < 300; ms++) {
        CHECK(!ks_fn_menu_toggle(&s, true, true, false, 1));
    }
    CHECK(!ks_fn_menu_toggle(&s, false, true, false, 1));
    int fired_at = -1, fired = 0;
    for (int ms = 0; ms <= 1500; ms++) {
        if (ks_fn_menu_toggle(&s, true, true, false, 1)) {
            fired++;
            fired_at = ms;
        }
    }
    CHECK_EQ(fired, 1);
    CHECK_EQ(fired_at, KS_FN_HOLD_MS);
}

void test_xinput_report(void)
{
    static const struct {
        int output, byte, mask;
    } bits[] = {
        {KS_OUT_DUP, 2, 0x01},  {KS_OUT_DDOWN, 2, 0x02}, {KS_OUT_DLEFT, 2, 0x04},
        {KS_OUT_DRIGHT, 2, 0x08}, {KS_OUT_MENU, 2, 0x10}, {KS_OUT_VIEW, 2, 0x20},
        {KS_OUT_L3, 2, 0x40},   {KS_OUT_R3, 2, 0x80},    {KS_OUT_LB, 3, 0x01},
        {KS_OUT_RB, 3, 0x02},   {KS_OUT_HOME, 3, 0x04},  {KS_OUT_A, 3, 0x10},
        {KS_OUT_B, 3, 0x20},    {KS_OUT_X, 3, 0x40},     {KS_OUT_Y, 3, 0x80},
    };
    static const uint8_t expect[KS_XINPUT_REPORT_LEN] = {
        0x00, 0x14, 0x01, 0x15, 10, 200, 0xE8, 0x03, 0x30, 0xF8, 0xFF, 0x7F, 0x00, 0x80,
    };
    uint8_t r[KS_XINPUT_REPORT_LEN];
    ks_gamepad_t g = {0};
    g.buttons = KS_OUT_BIT(KS_OUT_A) | KS_OUT_BIT(KS_OUT_DUP) | KS_OUT_BIT(KS_OUT_LB) |
                KS_OUT_BIT(KS_OUT_HOME);
    g.lt = 10;
    g.rt = 200;
    g.lx = 1000;
    g.ly = -2000;
    g.rx = 32767;
    g.ry = -32768;
    ks_xinput_build(&g, r);
    CHECK(memcmp(r, expect, sizeof r) == 0);

    for (size_t i = 0; i < sizeof bits / sizeof bits[0]; i++) {
        memset(&g, 0, sizeof g);
        g.buttons = KS_OUT_BIT(bits[i].output);
        ks_xinput_build(&g, r);
        CHECK_EQ(r[bits[i].byte], bits[i].mask);
        CHECK_EQ(r[bits[i].byte == 2 ? 3 : 2], 0);
    }
    // Outputs with no XInput equivalent are dropped.
    g.buttons = KS_OUT_BIT(KS_OUT_EX1) | KS_OUT_BIT(KS_OUT_EX2) | KS_OUT_BIT(KS_OUT_EX3);
    ks_xinput_build(&g, r);
    CHECK(r[2] == 0 && r[3] == 0);
}

void test_hid_report(void)
{
    uint8_t r[KS_HID_INPUT_LEN];
    ks_gamepad_t g = {0};
    g.buttons = KS_OUT_BIT(KS_OUT_A) | KS_OUT_BIT(KS_OUT_X) | KS_OUT_BIT(KS_OUT_RB) |
                KS_OUT_BIT(KS_OUT_MENU) | KS_OUT_BIT(KS_OUT_DUP) | KS_OUT_BIT(KS_OUT_DRIGHT);
    g.lt = 200;
    g.rt = 100;
    ks_hid_build(&g, r);
    CHECK(r[0] == 0x89 && r[1] == 0x09); // A, X, RB, LT past half, Menu
    CHECK_EQ(r[2], 1);                   // hat up-right
    CHECK(r[3] == 0x00 && r[4] == 0x80); // X centred at 32768
    CHECK(r[5] == 0xFF && r[6] == 0x7F); // Y centred at 32767
    CHECK(r[11] == 200 && r[12] == 100);

    // +y is up in the gamepad state and down in HID.
    memset(&g, 0, sizeof g);
    g.ly = 32767;
    g.lx = -32768;
    g.ry = -32768;
    g.rx = 32767;
    ks_hid_build(&g, r);
    CHECK(r[3] == 0x00 && r[4] == 0x00); // X full left
    CHECK(r[5] == 0x00 && r[6] == 0x00); // Y full up
    CHECK(r[7] == 0xFF && r[8] == 0xFF); // Z full right
    CHECK(r[9] == 0xFF && r[10] == 0xFF); // Rz full down
    CHECK_EQ(r[2], 8);                   // hat centred

    g.buttons = KS_OUT_BIT(KS_OUT_EX1) | KS_OUT_BIT(KS_OUT_EX2) | KS_OUT_BIT(KS_OUT_EX3) |
                KS_OUT_BIT(KS_OUT_HOME);
    ks_hid_build(&g, r);
    CHECK(r[0] == 0x24 && r[1] == 0x90);
}

void test_hat(void)
{
    const uint32_t u = KS_OUT_BIT(KS_OUT_DUP), d = KS_OUT_BIT(KS_OUT_DDOWN);
    const uint32_t l = KS_OUT_BIT(KS_OUT_DLEFT), r = KS_OUT_BIT(KS_OUT_DRIGHT);
    CHECK_EQ(ks_hat_from_buttons(0), 8);
    CHECK_EQ(ks_hat_from_buttons(u), 0);
    CHECK_EQ(ks_hat_from_buttons(u | r), 1);
    CHECK_EQ(ks_hat_from_buttons(r), 2);
    CHECK_EQ(ks_hat_from_buttons(r | d), 3);
    CHECK_EQ(ks_hat_from_buttons(d), 4);
    CHECK_EQ(ks_hat_from_buttons(d | l), 5);
    CHECK_EQ(ks_hat_from_buttons(l), 6);
    CHECK_EQ(ks_hat_from_buttons(l | u), 7);
    CHECK_EQ(ks_hat_from_buttons(u | d), 8);
    CHECK_EQ(ks_hat_from_buttons(KS_OUT_BIT(KS_OUT_A)), 8);
}

void test_xbox_ble_report(void)
{
    uint8_t r[KS_XBOX_BLE_INPUT_LEN];
    ks_gamepad_t g = {0};
    ks_xbox_ble_build(&g, r);
    static const uint8_t idle[KS_XBOX_BLE_INPUT_LEN] = {
        0x00, 0x80, 0xFF, 0x7F, 0x00, 0x80, 0xFF, 0x7F,
    };
    CHECK(memcmp(r, idle, sizeof r) == 0);

    g.buttons = KS_OUT_BIT(KS_OUT_A) | KS_OUT_BIT(KS_OUT_Y) | KS_OUT_BIT(KS_OUT_HOME) |
                KS_OUT_BIT(KS_OUT_R3) | KS_OUT_BIT(KS_OUT_DLEFT) | KS_OUT_BIT(KS_OUT_DUP);
    g.lt = 255;
    g.rt = 128;
    ks_xbox_ble_build(&g, r);
    CHECK(r[8] == 0xFF && r[9] == 0x03);   // 1023
    CHECK(r[10] == 0x01 && r[11] == 0x02); // 128 * 1023 / 255 = 513
    CHECK_EQ(r[12], 8);                    // up-left, on the 1 to 8 scale
    CHECK(r[13] == 0x11 && r[14] == 0x50); // A, Y, Xbox, right stick
    CHECK_EQ(r[15], 0);
    g.buttons = KS_OUT_BIT(KS_OUT_DUP);
    ks_xbox_ble_build(&g, r);
    CHECK_EQ(r[12], 1);
    g.buttons = KS_OUT_BIT(KS_OUT_B) | KS_OUT_BIT(KS_OUT_X) | KS_OUT_BIT(KS_OUT_LB) |
                KS_OUT_BIT(KS_OUT_RB) | KS_OUT_BIT(KS_OUT_VIEW) | KS_OUT_BIT(KS_OUT_MENU) |
                KS_OUT_BIT(KS_OUT_L3);
    ks_xbox_ble_build(&g, r);
    CHECK(r[13] == 0xCA && r[14] == 0x2C);
}

typedef struct {
    int in_bits[8], out_bits[8], depth;
    bool ok;
} hid_sizes_t;

// Adds up the report sizes a HID report descriptor declares.
static void hid_measure(const uint8_t *d, size_t len, hid_sizes_t *s)
{
    uint32_t size = 0, count = 0, id = 0;
    memset(s, 0, sizeof *s);
    s->ok = true;
    for (size_t i = 0; i < len;) {
        size_t n = d[i] & 3;
        uint32_t v = 0;
        if (n == 3) {
            n = 4;
        }
        if (i + 1 + n > len) {
            s->ok = false;
            return;
        }
        for (size_t k = 0; k < n; k++) {
            v |= (uint32_t)d[i + 1 + k] << (8 * k);
        }
        switch (d[i] & 0xFC) {
        case 0x74: size = v; break;                               // Report Size
        case 0x94: count = v; break;                              // Report Count
        case 0x84: id = v & 7; break;                             // Report ID
        case 0x80: s->in_bits[id] += (int)(size * count); break;  // Input
        case 0x90: s->out_bits[id] += (int)(size * count); break; // Output
        case 0xA0: s->depth++; break;                             // Collection
        case 0xC0: s->depth--; break;                             // End Collection
        default: break;
        }
        i += 1 + n;
    }
}

// The descriptors must declare exactly the bytes the report builders write.
void test_hid_descriptor_sizes(void)
{
    hid_sizes_t s;
    hid_measure(ks_hid_desc_gamepad, ks_hid_desc_gamepad_len, &s);
    CHECK(s.ok && s.depth == 0);
    CHECK_EQ(s.in_bits[KS_HID_ID_INPUT], KS_HID_INPUT_LEN * 8);
    CHECK_EQ(s.out_bits[KS_HID_ID_RUMBLE], KS_HID_RUMBLE_LEN * 8);
    CHECK_EQ(s.out_bits[KS_HID_ID_TRIGGER_FX], KS_HID_TRIGGER_FX_LEN * 8);

    hid_measure(ks_hid_desc_xbox_ble, ks_hid_desc_xbox_ble_len, &s);
    CHECK(s.ok && s.depth == 0);
    CHECK_EQ(s.in_bits[KS_XBOX_BLE_ID_INPUT], KS_XBOX_BLE_INPUT_LEN * 8);
    CHECK_EQ(s.out_bits[KS_XBOX_BLE_ID_RUMBLE], KS_XBOX_BLE_RUMBLE_LEN * 8);
}

void test_output_report_parsers(void)
{
    ks_rumble_t r = {0, 0};
    ks_trigger_fx_cfg_t fx;
    uint8_t target = 0;

    CHECK(ks_xinput_parse_out((const uint8_t[]){0x00, 0x08, 0x00, 0x80, 0x40, 0, 0, 0}, 8, &r));
    CHECK(r.left == 0x80 && r.right == 0x40);
    CHECK(!ks_xinput_parse_out((const uint8_t[]){0x01, 0x03, 0x06}, 3, &r)); // LED message
    CHECK(!ks_xinput_parse_out((const uint8_t[]){0x00, 0x08, 0x00}, 3, &r));

    CHECK(ks_hid_parse_rumble((const uint8_t[]){255, 7}, 2, &r));
    CHECK(r.left == 255 && r.right == 7);
    CHECK(!ks_hid_parse_rumble((const uint8_t[]){255}, 1, &r));

    CHECK(ks_hid_parse_trigger_fx((const uint8_t[]){3, KS_FX_CLICK, 60, 200, 30}, 5, &target, &fx));
    CHECK_EQ(target, KS_FX_TARGET_LEFT | KS_FX_TARGET_RIGHT);
    CHECK(fx.effect == KS_FX_CLICK && fx.start == 60 && fx.strength == 200 && fx.param == 30);
    CHECK(!ks_hid_parse_trigger_fx((const uint8_t[]){0, KS_FX_CLICK, 60, 200, 30}, 5, &target, &fx));
    CHECK(!ks_hid_parse_trigger_fx((const uint8_t[]){1, KS_FX_COUNT, 60, 200, 30}, 5, &target, &fx));
    CHECK(!ks_hid_parse_trigger_fx((const uint8_t[]){1, KS_FX_OFF, 60, 200}, 4, &target, &fx));

    // Xbox rumble: enable bits, two trigger magnitudes, strong, weak, timing.
    CHECK(ks_xbox_ble_parse_rumble((const uint8_t[]){0x03, 0, 0, 100, 50, 0xFF, 0, 0}, 8, &r));
    CHECK(r.left == 255 && r.right == 127);
    CHECK(ks_xbox_ble_parse_rumble((const uint8_t[]){0x01, 0, 0, 100, 50, 0xFF, 0, 0}, 8, &r));
    CHECK(r.left == 0 && r.right == 127);
    CHECK(!ks_xbox_ble_parse_rumble((const uint8_t[]){0x03, 0, 0, 100}, 4, &r));
}
