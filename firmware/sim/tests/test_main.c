#include "test.h"

#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#include <sys/stat.h>
#define make_dir(path) mkdir(path, 0755)
#endif

int t_checks, t_failed_checks;
const char *t_out_dir = "out";

#define TESTS(X)                              \
    X(test_sr_idle)                           \
    X(test_sr_each_input)                     \
    X(test_sr_bit_order)                      \
    X(test_bumper_three_wirings)              \
    X(test_bumper_default_both_high)          \
    X(test_debounce_press_release)            \
    X(test_debounce_rejects_glitch)           \
    X(test_stick_calibration)                 \
    X(test_stick_deadzone_and_curve)          \
    X(test_trigger_calibration)               \
    X(test_calibration_capture)               \
    X(test_default_mapping)                   \
    X(test_any_input_to_any_output)           \
    X(test_paddles_remap)                     \
    X(test_socd_and_trigger_outputs)          \
    X(test_profile_pack_roundtrip)            \
    X(test_profile_rejects_corruption)        \
    X(test_storage_four_profiles)             \
    X(test_settings_and_calibration_storage)  \
    X(test_fn_gesture)                        \
    X(test_xinput_report)                     \
    X(test_hid_report)                        \
    X(test_hat)                               \
    X(test_xbox_ble_report)                   \
    X(test_hid_descriptor_sizes)              \
    X(test_output_report_parsers)             \
    X(test_trig_homing)                       \
    X(test_trig_off_parks)                    \
    X(test_trig_hair)                         \
    X(test_trig_resistance)                   \
    X(test_trig_click)                        \
    X(test_trig_pulse)                        \
    X(test_trig_current_limit)                \
    X(test_trig_driver_fault)                 \
    X(test_wifi_roundtrip)                    \
    X(test_wifi_input_bytes)                  \
    X(test_wifi_rejects_bad_packets)          \
    X(test_wifi_vectors_file)                 \
    X(test_led_zones_and_order)               \
    X(test_led_current_cap)                   \
    X(test_fuel_and_charger)                  \
    X(test_idle_timer)                        \
    X(test_ui_menu_navigation)                \
    X(test_ui_remap_flow_press)               \
    X(test_ui_remap_flow_pick)                \
    X(test_ui_remap_timeout)                  \
    X(test_ui_neutral_report_in_menu)         \
    X(test_ui_mode_change_requests_restart)   \
    X(test_ui_adjust_settings)

#define DECLARE(fn) void fn(void);
TESTS(DECLARE)

#define ENTRY(fn) {#fn, fn},
static const struct {
    const char *name;
    void (*fn)(void);
} tests[] = {TESTS(ENTRY)};

int main(int argc, char **argv)
{
    int passed = 0, failed = 0;
    if (argc > 1) {
        t_out_dir = argv[1];
    }
    make_dir(t_out_dir);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = t_failed_checks;
        tests[i].fn();
        if (t_failed_checks == before) {
            passed++;
            printf("ok    %s\n", tests[i].name);
        } else {
            failed++;
            printf("FAIL  %s\n", tests[i].name);
        }
    }
    printf("\n%d tests: %d passed, %d failed (%d checks, %d failed)\n", passed + failed, passed,
           failed, t_checks, t_failed_checks);
    return failed ? 1 : 0;
}
