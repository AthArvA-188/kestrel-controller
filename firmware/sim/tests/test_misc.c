// WiFi packets, LED and power logic, and the UI state machine.
#include <string.h>

#include "ks_periph.h"
#include "ks_reports.h"
#include "ks_ui.h"
#include "ks_wifi_proto.h"
#include "test.h"

// Packets with fixed contents. host/test_receiver.py decodes the same bytes
// from sim/out/wifi_vectors.txt and expects these values.
static void sample_packets(ks_wp_packet_t p[5])
{
    memset(p, 0, 5 * sizeof p[0]);
    p[0].type = KS_WP_DISCOVER;
    p[0].seq = 1;
    memcpy(p[0].u.discover.device_id, "\x24\x6F\x28\xAA\xBB\xCC", 6);
    p[0].u.discover.fw_version = 0x0100;
    p[0].u.discover.battery_pct = 76;
    strcpy(p[0].u.discover.name, "Kestrel");

    p[1].type = KS_WP_OFFER;
    p[1].seq = 1;
    p[1].session = 0xBEEF;
    strcpy(p[1].u.offer.name, "GAMING-PC");
    p[1].u.offer.rate_hz = 250;
    p[1].u.offer.slot = 1;

    p[2].type = KS_WP_INPUT;
    p[2].seq = 0x0102;
    p[2].session = 0x1234;
    p[2].u.input.pad = (ks_gamepad_t){KS_OUT_BIT(KS_OUT_A) | KS_OUT_BIT(KS_OUT_RB),
                                      1000, -1000, 32767, -32768, 7, 255};
    p[2].u.input.battery_pct = 88;
    p[2].u.input.flags = KS_WP_FLAG_CHARGING;
    p[2].u.input.time_ms = 0x01020304;

    p[3].type = KS_WP_FEEDBACK;
    p[3].seq = 9;
    p[3].session = 0xBEEF;
    p[3].u.feedback.rumble = (ks_rumble_t){200, 100};
    p[3].u.feedback.fx_valid = KS_FX_TARGET_LEFT | KS_FX_TARGET_RIGHT;
    p[3].u.feedback.fx[0] = (ks_trigger_fx_cfg_t){KS_FX_RESISTANCE, 60, 180, 0};
    p[3].u.feedback.fx[1] = (ks_trigger_fx_cfg_t){KS_FX_PULSE, 90, 128, 12};
    p[3].u.feedback.ack_seq = 0x0102;

    p[4].type = KS_WP_BYE;
    p[4].seq = 10;
    p[4].session = 0xBEEF;
}

void test_wifi_roundtrip(void)
{
    static const size_t lens[5] = {KS_WP_DISCOVER_LEN, KS_WP_OFFER_LEN, KS_WP_INPUT_LEN,
                                   KS_WP_FEEDBACK_LEN, KS_WP_BYE_LEN};
    ks_wp_packet_t p[5], q;
    uint8_t buf[KS_WP_MAX_LEN];
    sample_packets(p);
    for (int i = 0; i < 5; i++) {
        size_t len = ks_wp_encode(&p[i], buf, sizeof buf);
        CHECK_EQ(len, lens[i]);
        CHECK(ks_wp_decode(buf, len, &q));
        CHECK(q.type == p[i].type && q.seq == p[i].seq && q.session == p[i].session);
    }
    ks_wp_decode(buf, ks_wp_encode(&p[0], buf, sizeof buf), &q);
    CHECK(memcmp(q.u.discover.device_id, p[0].u.discover.device_id, 6) == 0);
    CHECK(q.u.discover.fw_version == 0x0100 && q.u.discover.battery_pct == 76);
    CHECK(strcmp(q.u.discover.name, "Kestrel") == 0);
    ks_wp_decode(buf, ks_wp_encode(&p[1], buf, sizeof buf), &q);
    CHECK(strcmp(q.u.offer.name, "GAMING-PC") == 0);
    CHECK(q.u.offer.rate_hz == 250 && q.u.offer.slot == 1);
    ks_wp_decode(buf, ks_wp_encode(&p[2], buf, sizeof buf), &q);
    CHECK(memcmp(&q.u.input.pad, &p[2].u.input.pad, sizeof(ks_gamepad_t)) == 0);
    CHECK(q.u.input.battery_pct == 88 && q.u.input.flags == KS_WP_FLAG_CHARGING);
    CHECK_EQ(q.u.input.time_ms, 0x01020304);
    ks_wp_decode(buf, ks_wp_encode(&p[3], buf, sizeof buf), &q);
    CHECK(q.u.feedback.rumble.left == 200 && q.u.feedback.rumble.right == 100);
    CHECK_EQ(q.u.feedback.fx_valid, 3);
    CHECK(memcmp(q.u.feedback.fx, p[3].u.feedback.fx, sizeof q.u.feedback.fx) == 0);
    CHECK_EQ(q.u.feedback.ack_seq, 0x0102);

    // A name of exactly 16 characters survives. A longer one is cut.
    strcpy(p[1].u.offer.name, "ABCDEFGHIJKLMNOP");
    ks_wp_decode(buf, ks_wp_encode(&p[1], buf, sizeof buf), &q);
    CHECK(strcmp(q.u.offer.name, "ABCDEFGHIJKLMNOP") == 0);
}

void test_wifi_input_bytes(void)
{
    static const uint8_t expect[KS_WP_INPUT_LEN] = {
        'K', 'S', 0x01, 0x03, 0x02, 0x01, 0x34, 0x12, // magic, version, type, seq, session
        0x21, 0x00, 0x00, 0x00,                       // buttons: A and RB
        0xE8, 0x03, 0x18, 0xFC, 0xFF, 0x7F, 0x00, 0x80, // lx, ly, rx, ry
        0x07, 0xFF, 0x58, 0x01,                       // lt, rt, battery, flags
        0x04, 0x03, 0x02, 0x01,                       // time_ms
    };
    ks_wp_packet_t p[5];
    uint8_t buf[KS_WP_MAX_LEN];
    sample_packets(p);
    CHECK_EQ(ks_wp_encode(&p[2], buf, sizeof buf), KS_WP_INPUT_LEN);
    CHECK(memcmp(buf, expect, sizeof expect) == 0);
}

void test_wifi_rejects_bad_packets(void)
{
    ks_wp_packet_t p[5], q;
    uint8_t buf[KS_WP_MAX_LEN + 8], bad[KS_WP_MAX_LEN + 8];
    sample_packets(p);
    size_t len = ks_wp_encode(&p[2], buf, sizeof buf);

    memcpy(bad, buf, len);
    bad[0] = 'X';
    CHECK(!ks_wp_decode(bad, len, &q));
    memcpy(bad, buf, len);
    bad[2] = KS_WP_VERSION + 1;
    CHECK(!ks_wp_decode(bad, len, &q));
    memcpy(bad, buf, len);
    bad[3] = 0;
    CHECK(!ks_wp_decode(bad, len, &q));
    bad[3] = 6;
    CHECK(!ks_wp_decode(bad, len, &q));
    CHECK(!ks_wp_decode(buf, len - 1, &q));
    CHECK(!ks_wp_decode(buf, KS_WP_HEADER_LEN - 1, &q));
    CHECK(!ks_wp_decode(buf, 0, &q));

    // Extra bytes on the end are ignored.
    memcpy(bad, buf, len);
    memset(bad + len, 0xEE, 8);
    CHECK(ks_wp_decode(bad, len + 8, &q));
    CHECK_EQ(q.u.input.pad.rt, 255);

    // An unknown trigger effect is refused, since it would reach the motors.
    len = ks_wp_encode(&p[3], buf, sizeof buf);
    buf[KS_WP_HEADER_LEN + 4] = KS_FX_COUNT;
    CHECK(!ks_wp_decode(buf, len, &q));

    // Encoding refuses a short buffer and an unknown type.
    CHECK_EQ(ks_wp_encode(&p[2], buf, KS_WP_INPUT_LEN - 1), 0);
    p[2].type = 0;
    CHECK_EQ(ks_wp_encode(&p[2], buf, sizeof buf), 0);
}

void test_wifi_vectors_file(void)
{
    static const char *const names[5] = {"discover", "offer", "input", "feedback", "bye"};
    ks_wp_packet_t p[5];
    uint8_t buf[KS_WP_MAX_LEN];
    char path[512];
    sample_packets(p);
    snprintf(path, sizeof path, "%s/wifi_vectors.txt", t_out_dir);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f) {
        return;
    }
    for (int i = 0; i < 5; i++) {
        size_t len = ks_wp_encode(&p[i], buf, sizeof buf);
        fprintf(f, "%s ", names[i]);
        for (size_t k = 0; k < len; k++) {
            fprintf(f, "%02x", buf[k]);
        }
        fprintf(f, "\n");
    }
    CHECK(fclose(f) == 0);
}

void test_led_zones_and_order(void)
{
    int per_zone[KS_ZONE_COUNT] = {0};
    for (int i = 0; i < KS_LED_COUNT; i++) {
        per_zone[ks_led_zone_of(i)]++;
    }
    CHECK_EQ(per_zone[KS_ZONE_FACE], 4);
    CHECK_EQ(per_zone[KS_ZONE_DPAD], 4);
    CHECK_EQ(per_zone[KS_ZONE_SYSTEM], 4);
    CHECK_EQ(per_zone[KS_ZONE_LRING], 6);
    CHECK_EQ(per_zone[KS_ZONE_RRING], 6);
    // Chain positions from the pin map: FN first, the right ring last.
    CHECK_EQ(ks_led_zone_of(0), KS_ZONE_SYSTEM);
    CHECK_EQ(ks_led_zone_of(1), KS_ZONE_DPAD);
    CHECK_EQ(ks_led_zone_of(5), KS_ZONE_LRING);
    CHECK_EQ(ks_led_zone_of(12), KS_ZONE_SYSTEM); // HOME
    CHECK_EQ(ks_led_zone_of(17), KS_ZONE_FACE);   // A
    CHECK_EQ(ks_led_zone_of(23), KS_ZONE_RRING);

    // Each zone gets its own colour, sent green first.
    ks_zone_cfg_t zone[KS_ZONE_COUNT] = {
        {10, 20, 30, 100}, {40, 50, 60, 100}, {70, 80, 90, 50}, {100, 110, 120, 100}, {0, 0, 0, 100},
    };
    uint8_t grb[KS_LED_COUNT * 3];
    ks_led_render(zone, 100, KS_LED_LIMIT_MA, grb);
    CHECK(grb[17 * 3] == 20 && grb[17 * 3 + 1] == 10 && grb[17 * 3 + 2] == 30); // A: face
    CHECK(grb[3] == 50 && grb[4] == 40 && grb[5] == 60);                        // D-pad
    CHECK(grb[0] == 40 && grb[1] == 35 && grb[2] == 45);       // FN: system at half
    CHECK(grb[5 * 3] == 110 && grb[5 * 3 + 1] == 100);         // left ring
    CHECK(grb[23 * 3] == 0 && grb[23 * 3 + 1] == 0 && grb[23 * 3 + 2] == 0);
    // The master level scales everything.
    ks_led_render(zone, 50, KS_LED_LIMIT_MA, grb);
    CHECK(grb[17 * 3] == 10 && grb[17 * 3 + 1] == 5 && grb[17 * 3 + 2] == 15);
    ks_led_render(zone, 0, KS_LED_LIMIT_MA, grb);
    CHECK_EQ(ks_led_estimate_ma(grb, KS_LED_COUNT), 24); // idle current only
}

void test_led_current_cap(void)
{
    ks_zone_cfg_t zone[KS_ZONE_COUNT];
    uint8_t grb[KS_LED_COUNT * 3], dim[KS_LED_COUNT * 3];
    for (int z = 0; z < KS_ZONE_COUNT; z++) {
        zone[z] = (ks_zone_cfg_t){255, 255, 255, 100};
    }
    // Everything white at full level would draw about 888 mA.
    memset(grb, 255, sizeof grb);
    CHECK_EQ(ks_led_estimate_ma(grb, KS_LED_COUNT), 888);
    uint16_t ma = ks_led_render(zone, 100, KS_LED_LIMIT_MA, grb);
    CHECK(ma <= KS_LED_LIMIT_MA && ma >= KS_LED_LIMIT_MA - 15);
    CHECK_EQ(ma, ks_led_estimate_ma(grb, KS_LED_COUNT));
    CHECK(grb[0] == grb[1] && grb[1] == grb[2] && grb[0] > 40); // still white, dimmer

    // Colour balance survives the scaling.
    for (int z = 0; z < KS_ZONE_COUNT; z++) {
        zone[z] = (ks_zone_cfg_t){255, 128, 0, 100};
    }
    ma = ks_led_render(zone, 100, 100, grb);
    CHECK(ma <= 100);
    CHECK(grb[2] == 0 && grb[1] > grb[0] && grb[0] > 0); // red above green, no blue

    // A setting under the limit is left as it is.
    for (int z = 0; z < KS_ZONE_COUNT; z++) {
        zone[z] = (ks_zone_cfg_t){45, 212, 191, 40};
    }
    ma = ks_led_render(zone, 100, KS_LED_LIMIT_MA, grb);
    ks_led_render(zone, 100, 10000, dim);
    CHECK(ma < KS_LED_LIMIT_MA);
    CHECK(memcmp(grb, dim, sizeof grb) == 0);

    // The default profiles stay inside the budget at full master level.
    for (int i = 0; i < KS_PROFILE_COUNT; i++) {
        ks_profile_t p;
        ks_profile_default(&p, i);
        CHECK(ks_led_render(p.zone, 100, 10000, grb) <= KS_LED_LIMIT_MA);
    }
}

void test_fuel_and_charger(void)
{
    CHECK_EQ(ks_fuel_soc_percent(0x0000), 0);
    CHECK_EQ(ks_fuel_soc_percent(0x3200), 50);
    CHECK_EQ(ks_fuel_soc_percent(0x327F), 50);
    CHECK_EQ(ks_fuel_soc_percent(0x3280), 51); // rounds to nearest
    CHECK_EQ(ks_fuel_soc_percent(0x6400), 100);
    CHECK_EQ(ks_fuel_soc_percent(0xFFFF), 100); // the gauge can read above 100
    CHECK_EQ(ks_fuel_vcell_mv(53760), 4200);
    CHECK_EQ(ks_fuel_vcell_mv(42240), 3300);
    CHECK_EQ(ks_fuel_vcell_mv(0), 0);

    CHECK_EQ(ks_charge_state(false, false), KS_CHG_ON_BATTERY);
    CHECK_EQ(ks_charge_state(true, true), KS_CHG_CHARGING);
    CHECK_EQ(ks_charge_state(false, true), KS_CHG_FULL);
    CHECK_EQ(ks_charge_state(true, false), KS_CHG_ON_BATTERY);
}

void test_idle_timer(void)
{
    static const int16_t still[4] = {0, 0, 0, 0}, drift[4] = {900, -900, 0, 0};
    static const int16_t moved[4] = {0, 0, -5000, 0};
    static const uint8_t none[2] = {0, 0}, pulled[2] = {0, 40};
    ks_idle_t t = {0};

    CHECK(!ks_input_active(0, still, none));
    CHECK(!ks_input_active(0, drift, none)); // a slightly off-centre stick is not use
    CHECK(ks_input_active(KS_IN_BIT(KS_IN_P2), still, none));
    CHECK(ks_input_active(0, moved, none));
    CHECK(ks_input_active(0, still, pulled));

    // Ten idle minutes, then sleep.
    CHECK(!ks_idle_step(&t, false, false, 599999, 10));
    CHECK(ks_idle_step(&t, false, false, 1, 10));
    // Activity restarts the count.
    CHECK(!ks_idle_step(&t, true, false, 1, 10));
    CHECK(!ks_idle_step(&t, false, false, 599999, 10));
    // So does an attached USB host.
    CHECK(!ks_idle_step(&t, false, true, 1, 10));
    CHECK(!ks_idle_step(&t, false, false, 599999, 10));
    // Zero minutes turns the timeout off.
    CHECK(!ks_idle_step(&t, false, false, 10000000, 0));
    CHECK(!ks_idle_step(&t, false, false, 119999, 2));
    CHECK(ks_idle_step(&t, false, false, 1, 2));
}

static ks_settings_t settings;
static ks_profile_t profiles[KS_PROFILE_COUNT];
static ks_ui_t ui;

static void ui_setup(void)
{
    ks_settings_default(&settings);
    for (int i = 0; i < KS_PROFILE_COUNT; i++) {
        ks_profile_default(&profiles[i], i);
    }
    ks_ui_init(&ui, &settings, profiles);
}

static void tap(int input)
{
    ks_ui_input(&ui, KS_IN_BIT(input), 10);
    ks_ui_input(&ui, 0, 10);
}

static void open_submenu(int row)
{
    ui_setup();
    ks_ui_toggle_menu(&ui);
    for (int i = 0; i < row; i++) {
        tap(KS_IN_DDOWN);
    }
    tap(KS_IN_A);
}

void test_ui_menu_navigation(void)
{
    ui_setup();
    CHECK(!ks_ui_menu_open(&ui));
    // Buttons do nothing on the home screen. They belong to the game.
    CHECK(!ks_ui_input(&ui, KS_IN_BIT(KS_IN_A) | KS_IN_BIT(KS_IN_DDOWN), 10));
    ks_ui_input(&ui, 0, 10);
    CHECK_EQ(ui.screen, KS_UI_HOME);

    ks_ui_toggle_menu(&ui); // the application does this on Fn
    CHECK(ks_ui_menu_open(&ui));
    CHECK(ui.screen == KS_UI_MENU && ui.cursor == 0);
    tap(KS_IN_DDOWN);
    CHECK_EQ(ui.cursor, 1);
    tap(KS_IN_DUP);
    tap(KS_IN_DUP);
    CHECK_EQ(ui.cursor, 5); // wraps
    tap(KS_IN_A);
    CHECK_EQ(ui.screen, KS_UI_SYSTEM);
    tap(KS_IN_B);
    CHECK(ui.screen == KS_UI_MENU && ui.cursor == 5); // back where it was

    // A selects each of the six entries.
    static const uint8_t expect[6] = {KS_UI_CONNECTION, KS_UI_REMAP_PRESS, KS_UI_TRIGGERS,
                                      KS_UI_STICKS, KS_UI_LIGHTING, KS_UI_SYSTEM};
    for (int row = 0; row < 6; row++) {
        open_submenu(row);
        CHECK_EQ(ui.screen, expect[row]);
    }
    // B on the main menu and Fn anywhere return to the home screen.
    ui_setup();
    ks_ui_toggle_menu(&ui);
    tap(KS_IN_B);
    CHECK_EQ(ui.screen, KS_UI_HOME);
    open_submenu(2);
    tap(KS_IN_FN);
    CHECK_EQ(ui.screen, KS_UI_HOME);
}

void test_ui_remap_flow_press(void)
{
    open_submenu(1); // step 1: choose Remap
    CHECK_EQ(ui.screen, KS_UI_REMAP_PRESS);
    tap(KS_IN_P1); // step 2: press the physical control
    CHECK_EQ(ui.screen, KS_UI_REMAP_PICK);
    CHECK_EQ(ui.remap_input, KS_IN_P1);
    CHECK_EQ(ui.cursor, KS_OUT_NONE); // the list opens on the current mapping
    CHECK_EQ(ks_ui_take_requests(&ui), 0);
    tap(KS_IN_X); // step 3: press the output
    CHECK_EQ(ui.screen, KS_UI_REMAP_DONE);
    CHECK_EQ(profiles[0].map[KS_IN_P1], KS_OUT_X);
    CHECK_EQ(ks_ui_take_requests(&ui), KS_UI_REQ_SAVE_PROFILE);
    CHECK_EQ(ks_ui_take_requests(&ui), 0);
    // Only the active profile changed.
    CHECK_EQ(profiles[1].map[KS_IN_P1], KS_OUT_A);
    CHECK_EQ(profiles[3].map[KS_IN_P1], KS_OUT_NONE);
    ks_ui_input(&ui, 0, KS_UI_DONE_MS);
    CHECK(ui.screen == KS_UI_MENU && ui.cursor == 1);

    // Fn can be the control being remapped.
    open_submenu(1);
    tap(KS_IN_FN);
    CHECK(ui.screen == KS_UI_REMAP_PICK && ui.remap_input == KS_IN_FN);
    tap(KS_IN_RB);
    CHECK_EQ(profiles[0].map[KS_IN_FN], KS_OUT_RB);

    // A paddle has no output of its own, so pressing one in step 3 picks nothing.
    open_submenu(1);
    tap(KS_IN_P2);
    tap(KS_IN_P3);
    CHECK_EQ(ui.screen, KS_UI_REMAP_PICK);
    CHECK_EQ(profiles[0].map[KS_IN_P2], KS_OUT_NONE);
}

void test_ui_remap_flow_pick(void)
{
    open_submenu(1);
    tap(KS_IN_LB);
    CHECK_EQ(ui.screen, KS_UI_REMAP_PICK);
    CHECK_EQ(ui.cursor, KS_OUT_LB);
    // D-pad down to RT, with the list scrolling to keep the cursor in view.
    for (int i = KS_OUT_LB; i < KS_OUT_RT; i++) {
        tap(KS_IN_DDOWN);
        CHECK(ui.cursor >= ui.scroll && ui.cursor < ui.scroll + 6);
    }
    CHECK_EQ(ui.cursor, KS_OUT_RT);
    tap(KS_IN_A);
    CHECK_EQ(ui.screen, KS_UI_REMAP_DONE);
    CHECK_EQ(profiles[0].map[KS_IN_LB], KS_OUT_RT);
    CHECK_EQ(ks_ui_take_requests(&ui), KS_UI_REQ_SAVE_PROFILE);

    // The list is the way to pick A, B, and the D-pad, which navigate here.
    open_submenu(1);
    tap(KS_IN_P4);
    tap(KS_IN_DDOWN); // None to A
    tap(KS_IN_A);
    CHECK_EQ(profiles[0].map[KS_IN_P4], KS_OUT_A);

    // B in step 3 returns to step 2 and changes nothing.
    open_submenu(1);
    tap(KS_IN_Y);
    tap(KS_IN_B);
    CHECK_EQ(ui.screen, KS_UI_REMAP_PRESS);
    CHECK_EQ(profiles[0].map[KS_IN_Y], KS_OUT_Y);
    CHECK_EQ(ks_ui_take_requests(&ui), 0);
}

void test_ui_remap_timeout(void)
{
    open_submenu(1); // the release half of the last tap already spent 10 ms here
    ks_ui_input(&ui, 0, 4000);
    CHECK_EQ(ui.screen, KS_UI_REMAP_PRESS);
    ks_ui_input(&ui, 0, KS_UI_REMAP_TIMEOUT_MS - 4000 - 10 - 1);
    CHECK_EQ(ui.screen, KS_UI_REMAP_PRESS);
    ks_ui_input(&ui, 0, 1);
    CHECK(ui.screen == KS_UI_MENU && ui.cursor == 1);
    CHECK_EQ(ks_ui_take_requests(&ui), 0);
}

void test_ui_neutral_report_in_menu(void)
{
    const ks_gamepad_t held = {KS_OUT_BIT(KS_OUT_A) | KS_OUT_BIT(KS_OUT_DUP), 12000, -8000,
                               300, 32767, 200, 255};
    const ks_gamepad_t neutral = {0};
    ks_gamepad_t pad = held;
    ui_setup();
    ks_ui_filter_gamepad(&ui, &pad);
    CHECK(memcmp(&pad, &held, sizeof pad) == 0); // home screen: untouched

    ks_ui_toggle_menu(&ui);
    ks_ui_filter_gamepad(&ui, &pad);
    CHECK(memcmp(&pad, &neutral, sizeof pad) == 0);
    // Every menu screen is covered, including the remap steps.
    for (int row = 0; row < 6; row++) {
        open_submenu(row);
        pad = held;
        ks_ui_filter_gamepad(&ui, &pad);
        CHECK(memcmp(&pad, &neutral, sizeof pad) == 0);
    }
    open_submenu(1);
    tap(KS_IN_P1);
    pad = held;
    ks_ui_filter_gamepad(&ui, &pad);
    CHECK(memcmp(&pad, &neutral, sizeof pad) == 0);

    tap(KS_IN_FN);
    pad = held;
    ks_ui_filter_gamepad(&ui, &pad);
    CHECK(memcmp(&pad, &held, sizeof pad) == 0);
}

void test_ui_mode_change_requests_restart(void)
{
    open_submenu(0);
    CHECK(ui.screen == KS_UI_CONNECTION && ui.cursor == KS_MODE_USB_XINPUT);
    tap(KS_IN_A); // the active mode again: nothing to do
    CHECK_EQ(ks_ui_take_requests(&ui), 0);
    for (int i = 0; i < KS_MODE_WIFI; i++) {
        tap(KS_IN_DDOWN);
    }
    tap(KS_IN_A);
    CHECK_EQ(settings.mode, KS_MODE_WIFI);
    CHECK_EQ(ks_ui_take_requests(&ui), KS_UI_REQ_SAVE_SETTINGS | KS_UI_REQ_RESTART);
    tap(KS_IN_DDOWN);
    tap(KS_IN_A);
    CHECK_EQ(ks_ui_take_requests(&ui), KS_UI_REQ_FORGET);
}

void test_ui_adjust_settings(void)
{
    open_submenu(2); // Triggers
    tap(KS_IN_DRIGHT);
    CHECK_EQ(profiles[0].trigger_fx[0].effect, KS_FX_RESISTANCE);
    CHECK_EQ(ks_ui_take_requests(&ui), KS_UI_REQ_SAVE_PROFILE);
    tap(KS_IN_DLEFT);
    tap(KS_IN_DLEFT);
    CHECK_EQ(profiles[0].trigger_fx[0].effect, KS_FX_PULSE); // wraps
    for (int i = 0; i < 3; i++) {
        tap(KS_IN_DDOWN);
    }
    tap(KS_IN_DRIGHT);
    tap(KS_IN_DRIGHT);
    CHECK_EQ(profiles[0].trigger_fx[1].effect, KS_FX_HAIR);
    CHECK_EQ(profiles[0].trigger_fx[0].effect, KS_FX_PULSE);

    open_submenu(3); // Sticks
    tap(KS_IN_DRIGHT);
    CHECK_EQ(profiles[0].stick_deadzone[0], 10);
    tap(KS_IN_DDOWN);
    tap(KS_IN_DRIGHT);
    CHECK_EQ(profiles[0].stick_curve[0], 18);
    for (int i = 0; i < 30; i++) {
        tap(KS_IN_DLEFT);
    }
    tap(KS_IN_DUP);
    for (int i = 0; i < 30; i++) {
        tap(KS_IN_DLEFT);
    }
    CHECK_EQ(profiles[0].stick_deadzone[0], 0); // stops at the limit
    // Calibrate asks the application to start, advance, and finish.
    ks_ui_take_requests(&ui);
    for (int i = 0; i < 5; i++) {
        tap(KS_IN_DDOWN);
    }
    tap(KS_IN_A);
    CHECK_EQ(ui.screen, KS_UI_CAL_CENTRE);
    CHECK_EQ(ks_ui_take_requests(&ui), KS_UI_REQ_CAL_START);
    tap(KS_IN_A);
    CHECK_EQ(ui.screen, KS_UI_CAL_RANGE);
    CHECK_EQ(ks_ui_take_requests(&ui), KS_UI_REQ_CAL_ADVANCE);
    tap(KS_IN_B);
    CHECK_EQ(ui.screen, KS_UI_STICKS);
    CHECK_EQ(ks_ui_take_requests(&ui), KS_UI_REQ_CAL_CANCEL);

    open_submenu(4); // Lighting
    tap(KS_IN_DRIGHT); // zone: D-pad
    tap(KS_IN_DDOWN);
    tap(KS_IN_DRIGHT); // colour: white to teal
    CHECK(profiles[0].zone[KS_ZONE_DPAD].r == 45 && profiles[0].zone[KS_ZONE_DPAD].g == 212);
    CHECK_EQ(profiles[0].zone[KS_ZONE_FACE].r, 255);
    tap(KS_IN_DDOWN);
    tap(KS_IN_DLEFT);
    CHECK_EQ(profiles[0].zone[KS_ZONE_DPAD].brightness, 15);

    open_submenu(5); // System
    tap(KS_IN_DRIGHT);
    CHECK_EQ(settings.active_profile, 1);
    CHECK_EQ(ks_ui_take_requests(&ui), KS_UI_REQ_SAVE_SETTINGS);
    tap(KS_IN_DLEFT);
    tap(KS_IN_DLEFT);
    CHECK_EQ(settings.active_profile, 3);
    // Holding a direction repeats: screen brightness falls from 80 to its floor.
    tap(KS_IN_DDOWN);
    for (int ms = 0; ms < 3000; ms += 10) {
        ks_ui_input(&ui, KS_IN_BIT(KS_IN_DLEFT), 10);
    }
    ks_ui_input(&ui, 0, 10);
    CHECK_EQ(settings.backlight_pct, 10);
    // Reset restores the active profile only.
    profiles[3].map[KS_IN_P1] = KS_OUT_B;
    profiles[0].map[KS_IN_P1] = KS_OUT_B;
    tap(KS_IN_DDOWN);
    tap(KS_IN_DDOWN);
    tap(KS_IN_A);
    CHECK_EQ(profiles[3].map[KS_IN_P1], KS_OUT_NONE);
    CHECK_EQ(profiles[0].map[KS_IN_P1], KS_OUT_B);
    ks_ui_take_requests(&ui);
    tap(KS_IN_DDOWN);
    tap(KS_IN_A);
    CHECK_EQ(ks_ui_take_requests(&ui), KS_UI_REQ_SLEEP);
}
