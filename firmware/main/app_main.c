// Kestrel application: the 1 kHz input task, the UI task, and the glue
// between the board layer, the hardware-independent core, and the transports.
#include <string.h>

#include "app.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ks_periph.h"
#include "ks_reports.h"
#include "ks_trigger.h"
#include "ks_wifi_proto.h"
#include "pins_gen.h"

static const char *TAG = "kestrel";

#define UI_PERIOD_MS 20
#define PROFILE_SAVE_DELAY_MS 1500 // one flash write after a run of adjustments
#define FUEL_PERIOD_MS 5000
#define BLE_SEND_DIVIDER 8 // 125 reports per second, about one per connection interval
#define LED_MASTER_PCT 100
#define LED_MENU_PCT 30 // dim the pad's lighting while the menu is open

// The charger's power-good input tells the USB stack whether the cable is in.
// Build with -DKS_VBUS_FROM_PGOOD=0 on a dev kit that has no charger.
#ifndef KS_VBUS_FROM_PGOOD
#define KS_VBUS_FROM_PGOOD 1
#endif

static ks_settings_t settings;
static ks_profile_t profiles[KS_PROFILE_COUNT];
static ks_ui_t ui;
static TaskHandle_t input_handle;

// Values that cross tasks. Each side copies them under the lock.
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    // Written by the UI task and the transports, read by the input task.
    ks_profile_t profile;
    ks_calibration_t cal;
    ks_trigger_fx_cfg_t host_fx[2];
    uint8_t host_fx_valid;
    ks_rumble_t rumble;
    // Written by the input task.
    uint32_t pressed;
    ks_gamepad_t pad;
    ks_sr_frame_t frame;
    uint16_t analog[KS_AX_COUNT];
    bool active;
    // Written by the UI task.
    uint8_t battery_pct;
} shared;

void app_set_rumble(ks_rumble_t rumble)
{
    taskENTER_CRITICAL(&lock);
    shared.rumble = rumble;
    taskEXIT_CRITICAL(&lock);
}

void app_set_trigger_fx(uint8_t target, const ks_trigger_fx_cfg_t *cfg)
{
    taskENTER_CRITICAL(&lock);
    for (int i = 0; i < 2; i++) {
        if (target & (1u << i)) {
            shared.host_fx[i] = *cfg;
            shared.host_fx_valid |= (uint8_t)(1u << i);
        }
    }
    taskEXIT_CRITICAL(&lock);
}

void app_clear_host_state(void)
{
    taskENTER_CRITICAL(&lock);
    shared.rumble = (ks_rumble_t){0, 0};
    shared.host_fx_valid = 0;
    taskEXIT_CRITICAL(&lock);
}

void app_get_report(ks_gamepad_t *pad, uint8_t *battery_pct, uint8_t *flags)
{
    taskENTER_CRITICAL(&lock);
    *pad = shared.pad;
    *battery_pct = shared.battery_pct;
    *flags = shared.frame.charging ? KS_WP_FLAG_CHARGING : 0;
    taskEXIT_CRITICAL(&lock);
    if (ks_ui_menu_open(&ui)) {
        *flags |= KS_WP_FLAG_MENU_OPEN;
    }
}

static void publish_profile(void)
{
    taskENTER_CRITICAL(&lock);
    shared.profile = profiles[settings.active_profile];
    taskEXIT_CRITICAL(&lock);
}

static bool usb_mode(void)
{
    return settings.mode == KS_MODE_USB_XINPUT || settings.mode == KS_MODE_USB_DINPUT;
}

static bool ble_mode(void)
{
    return settings.mode == KS_MODE_BLE_XBOX || settings.mode == KS_MODE_BLE_GENERIC;
}

static void input_task(void *arg)
{
    ks_debounce_t debounce;
    ks_trig_t trig[2];
    ks_trig_params_t trig_params;
    ks_rumble_t rumble_now = {0, 0};
    TickType_t wake = xTaskGetTickCount();
    uint32_t tick = 0;

    ks_debounce_init(&debounce, 2, 4);
    ks_trig_params_default(&trig_params);
    ks_trig_init(&trig[0]);
    ks_trig_init(&trig[1]);

    for (;;) {
        board_sample_t s;
        ks_profile_t profile;
        ks_calibration_t cal;
        ks_trigger_fx_cfg_t host_fx[2];
        uint8_t host_fx_valid;
        ks_rumble_t rumble;
        ks_sr_frame_t frame;
        ks_gamepad_t pad;
        int16_t sticks[4];
        uint8_t triggers[2];

        vTaskDelayUntil(&wake, 1);
        board_read(&s);
        taskENTER_CRITICAL(&lock);
        profile = shared.profile;
        cal = shared.cal;
        memcpy(host_fx, shared.host_fx, sizeof host_fx);
        host_fx_valid = shared.host_fx_valid;
        rumble = shared.rumble;
        taskEXIT_CRITICAL(&lock);

        ks_sr_decode(s.sr, s.home_low, &cal.bumper, &frame);
        uint32_t pressed = ks_debounce_step(&debounce, frame.pressed);
        ks_stick_apply(&cal.axis[KS_AX_LX], &cal.axis[KS_AX_LY], profile.stick_deadzone[0],
                       profile.stick_curve[0], s.analog[KS_AX_LX], s.analog[KS_AX_LY], &sticks[0],
                       &sticks[1]);
        ks_stick_apply(&cal.axis[KS_AX_RX], &cal.axis[KS_AX_RY], profile.stick_deadzone[1],
                       profile.stick_curve[1], s.analog[KS_AX_RX], s.analog[KS_AX_RY], &sticks[2],
                       &sticks[3]);

        for (int i = 0; i < 2; i++) {
            // An effect from the host replaces the profile's while the host is there.
            const ks_trigger_fx_cfg_t *fx =
                (host_fx_valid >> i) & 1 ? &host_fx[i] : &profile.trigger_fx[i];
            uint8_t pos = ks_trigger_apply(&cal.trigger[i], profile.trigger_deadzone[i],
                                           s.analog[KS_AX_LT + i]);
            ks_trig_set(&trig[i], fx);
            board_motor(i, ks_trig_step(&trig[i], &trig_params, pos, s.isen_ma[i],
                                        frame.motor_fault, 1));
            triggers[i] = ks_trig_output(fx, pos);
        }

        ks_map_apply(&profile, pressed, sticks, triggers, &pad);
        bool active = ks_input_active(pressed, sticks, triggers);
        // While the menu is open the buttons drive the menu, so the host sees
        // a neutral pad.
        ks_ui_filter_gamepad(&ui, &pad);

        if (rumble.left != rumble_now.left || rumble.right != rumble_now.right) {
            rumble_now = rumble;
            board_rumble(rumble);
        }

        taskENTER_CRITICAL(&lock);
        shared.pressed = pressed;
        shared.pad = pad;
        shared.frame = frame;
        memcpy(shared.analog, s.analog, sizeof shared.analog);
        shared.active |= active;
        taskEXIT_CRITICAL(&lock);

        if (usb_mode()) {
            usb_dev_send(&pad);
        } else if (ble_mode() && tick % BLE_SEND_DIVIDER == 0) {
            ble_hid_send(&pad);
        } // the WiFi task reads the shared report at its own rate
        tick++;
    }
}

static void save_profile_now(void)
{
    if (!ks_store_save_profile(storage_get(), settings.active_profile,
                               &profiles[settings.active_profile])) {
        ESP_LOGE(TAG, "profile %d was not saved", settings.active_profile + 1);
    }
}

static void go_to_sleep(void)
{
    uint8_t dark[KS_LED_COUNT * 3] = {0};
    // Stop the input task first. It would otherwise keep driving the rumble
    // and trigger motors while board_sleep waits for Home to be released.
    vTaskSuspend(input_handle);
    leds_show(dark);
    vTaskDelay(pdMS_TO_TICKS(5));
    board_sleep();
}

static void link_status(link_status_t *st)
{
    if (usb_mode()) {
        usb_dev_status(st);
    } else if (ble_mode()) {
        ble_hid_status(st);
    } else {
        wifi_link_status(st);
    }
}

static void ui_task(void *arg)
{
    ks_fb_t fb = {display_buffer(), LCD_WIDTH, LCD_HEIGHT};
    ks_fn_state_t fn = {0};
    ks_idle_t idle = {0};
    ks_cal_capture_t capture = {0};
    ks_ui_status_t shown = {0}, st = {0};
    uint8_t grb[KS_LED_COUNT * 3];
    uint32_t save_in_ms = 0, fuel_in_ms = 0;
    bool dirty = true, profile_dirty = false;

    st.battery_pct = 100;
    for (;;) {
        uint32_t pressed;
        ks_sr_frame_t frame;
        uint16_t analog[KS_AX_COUNT];
        bool active;
        link_status_t link;

        vTaskDelay(pdMS_TO_TICKS(UI_PERIOD_MS));
        taskENTER_CRITICAL(&lock);
        pressed = shared.pressed;
        frame = shared.frame;
        memcpy(analog, shared.analog, sizeof analog);
        active = shared.active;
        shared.active = false;
        taskEXIT_CRITICAL(&lock);

        // The UI sees the inputs first, so it tracks buttons that are already
        // down when the menu opens.
        dirty |= ks_ui_input(&ui, pressed, UI_PERIOD_MS);
        bool fn_mapped = profiles[settings.active_profile].map[KS_IN_FN] != KS_OUT_NONE;
        if (ks_fn_menu_toggle(&fn, (pressed & KS_IN_BIT(KS_IN_FN)) != 0, fn_mapped,
                              ks_ui_menu_open(&ui), UI_PERIOD_MS)) {
            ks_ui_toggle_menu(&ui);
            dirty = true;
        }

        uint8_t req = ks_ui_take_requests(&ui);
        if (req & KS_UI_REQ_SAVE_PROFILE) {
            profile_dirty = true;
            save_in_ms = PROFILE_SAVE_DELAY_MS;
            publish_profile();
        }
        if (req & KS_UI_REQ_SAVE_SETTINGS) {
            if (profile_dirty) {
                save_profile_now(); // before the active profile can change
                profile_dirty = false;
            }
            ks_store_save_settings(storage_get(), &settings);
            board_backlight(settings.backlight_pct);
            publish_profile();
        }
        if (req & KS_UI_REQ_CAL_START) {
            ks_cal_start(&capture);
        }
        if (req & KS_UI_REQ_CAL_CANCEL) {
            capture.phase = KS_CAL_IDLE;
        }
        if (req & KS_UI_REQ_CAL_ADVANCE) {
            ks_calibration_t cal;
            taskENTER_CRITICAL(&lock);
            cal = shared.cal;
            taskEXIT_CRITICAL(&lock);
            if (ks_cal_advance(&capture, &cal)) {
                taskENTER_CRITICAL(&lock);
                shared.cal = cal;
                taskEXIT_CRITICAL(&lock);
                ks_store_save_calibration(storage_get(), &cal);
            }
        }
        if (capture.phase != KS_CAL_IDLE) {
            ks_cal_feed(&capture, analog, frame.lb_pair, frame.rb_pair);
        }
        if (profile_dirty) {
            if (save_in_ms <= UI_PERIOD_MS) {
                save_profile_now();
                profile_dirty = false;
            } else {
                save_in_ms -= UI_PERIOD_MS;
            }
        }
        if (req & (KS_UI_REQ_RESTART | KS_UI_REQ_FORGET | KS_UI_REQ_SLEEP)) {
            if (profile_dirty) {
                save_profile_now();
            }
            if (req & KS_UI_REQ_FORGET) {
                wifi_link_forget();
                storage_erase_namespace("nimble_bond");
            }
            if (req & KS_UI_REQ_SLEEP) {
                go_to_sleep();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart(); // the mode is applied at start-up
        }

        if (fuel_in_ms <= UI_PERIOD_MS) {
            uint8_t pct;
            uint16_t mv;
            fuel_in_ms = FUEL_PERIOD_MS;
            if (fuel_read(&pct, &mv)) {
                st.battery_pct = pct;
                taskENTER_CRITICAL(&lock);
                shared.battery_pct = pct;
                taskEXIT_CRITICAL(&lock);
                if (ble_mode()) {
                    ble_hid_set_battery(pct);
                }
            } else {
                static bool warned;
                if (!warned) {
                    warned = true;
                    ESP_LOGW(TAG, "fuel gauge did not answer; the battery level shown is not real");
                }
            }
        } else {
            fuel_in_ms -= UI_PERIOD_MS;
        }
        if (usb_mode()) {
            usb_dev_set_vbus(KS_VBUS_FROM_PGOOD ? frame.usb_power : true);
        }
        link_status(&link);
        st.link = link.link;
        st.charge = (uint8_t)ks_charge_state(frame.charging, frame.usb_power);
        memcpy(st.host, link.host, sizeof st.host);
        memcpy(st.detail, link.detail, sizeof st.detail);
        if (memcmp(&st, &shown, sizeof st) != 0) {
            dirty = true;
        }
        if (dirty) {
            ks_ui_draw(&ui, &st, &fb);
            display_flush();
            shown = st;
            dirty = false;
        }

        ks_led_render(profiles[settings.active_profile].zone,
                      ks_ui_menu_open(&ui) ? LED_MENU_PCT : LED_MASTER_PCT, KS_LED_LIMIT_MA, grb);
        leds_show(grb);

        // A wired pad with a live host stays awake.
        bool inhibit = usb_mode() && link.link == KS_LINK_CONNECTED;
        if (ks_idle_step(&idle, active || ks_ui_menu_open(&ui), inhibit, UI_PERIOD_MS,
                         settings.sleep_minutes)) {
            if (profile_dirty) {
                save_profile_now();
            }
            go_to_sleep();
        }
    }
}

// Holding Home with one of these at power-on picks the mode.
static void boot_mode_select(const ks_sr_frame_t *frame)
{
    static const struct {
        uint8_t input, mode;
    } combo[] = {
        {KS_IN_A, KS_MODE_USB_XINPUT}, {KS_IN_B, KS_MODE_USB_DINPUT},
        {KS_IN_X, KS_MODE_BLE_XBOX},   {KS_IN_Y, KS_MODE_BLE_GENERIC},
        {KS_IN_MENU, KS_MODE_WIFI},
    };
    if (!(frame->pressed & KS_IN_BIT(KS_IN_HOME))) {
        return;
    }
    for (size_t i = 0; i < sizeof combo / sizeof combo[0]; i++) {
        if ((frame->pressed & KS_IN_BIT(combo[i].input)) && settings.mode != combo[i].mode) {
            settings.mode = combo[i].mode;
            ks_store_save_settings(storage_get(), &settings);
            ESP_LOGI(TAG, "mode set at power-on: %s", ks_mode_name(settings.mode));
            return;
        }
    }
}

void app_main(void)
{
    board_sample_t s;
    ks_sr_frame_t frame;
    ks_calibration_t cal;

    board_init();
    periph_init();

    ks_store_load_settings(storage_get(), &settings);
    for (int i = 0; i < KS_PROFILE_COUNT; i++) {
        ks_store_load_profile(storage_get(), i, &profiles[i]);
    }
    bool calibrated = ks_store_load_calibration(storage_get(), &cal);

    vTaskDelay(pdMS_TO_TICKS(10));
    board_read(&s);
    ks_sr_decode(s.sr, s.home_low, NULL, &frame);
    if (!calibrated) {
        // No calibration yet. Take the bumper levels at power-on as their
        // released state, whichever way the switches turn out to be wired.
        cal.bumper.lb_released = frame.lb_pair;
        cal.bumper.rb_released = frame.rb_pair;
        ESP_LOGW(TAG, "not calibrated: run Sticks, Calibrate from the menu");
    }
    boot_mode_select(&frame);

    ks_ui_init(&ui, &settings, profiles);
    shared.cal = cal;
    shared.battery_pct = 100;
    publish_profile();
    board_backlight(settings.backlight_pct);
    ESP_LOGI(TAG, "mode %s, profile %d", ks_mode_name(settings.mode), settings.active_profile + 1);

    switch (settings.mode) {
    case KS_MODE_USB_XINPUT:
    case KS_MODE_USB_DINPUT:
        usb_dev_start((ks_mode_t)settings.mode);
        break;
    case KS_MODE_BLE_XBOX:
    case KS_MODE_BLE_GENERIC:
        ble_hid_start((ks_mode_t)settings.mode);
        break;
    default:
        wifi_link_start();
        break;
    }

    xTaskCreatePinnedToCore(input_task, "input", 4096, NULL, 10, &input_handle, 1);
    xTaskCreatePinnedToCore(ui_task, "ui", 6144, NULL, 4, NULL, 1);
}
