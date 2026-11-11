// Interfaces between the firmware's ESP-IDF modules.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ks_input.h"
#include "ks_profile.h"
#include "ks_types.h"
#include "ks_ui.h"

#define KS_FW_VERSION 0x0100

// pid.codes test ID. A placeholder: a product needs its own vendor ID.
#define KS_USB_VID 0x1209
#define KS_USB_PID 0x0001

// board.c: power sequencing, buttons, analogue inputs, motors.
typedef struct {
    uint8_t sr[3];                // SR1, SR2, SR3 as clocked out of the 74HC165 chain
    bool home_low;                // BTN_HOME on its own GPIO
    uint16_t analog[KS_AX_COUNT]; // sticks and triggers, 12-bit average times 16
    uint16_t isen_ma[2];          // trigger motor current
} board_sample_t;

void board_init(void);
void board_read(board_sample_t *s);       // call once per millisecond
void board_motor(int index, int16_t duty); // trigger motor, -255 to 255
void board_rumble(ks_rumble_t rumble);
void board_backlight(uint8_t pct);
void board_sleep(void); // powers the peripherals down and does not return

// board_periph.c: display, LEDs, fuel gauge, NVS storage.
void periph_init(void);
uint16_t *display_buffer(void); // LCD_WIDTH by LCD_HEIGHT pixels, RGB565
void display_flush(void);
void leds_show(const uint8_t *grb);
bool fuel_read(uint8_t *pct, uint16_t *mv);
const ks_storage_t *storage_get(void);
void storage_erase_namespace(const char *name);

// What a transport reports for the home screen.
typedef struct {
    uint8_t link; // ks_link_t
    char host[24];
    char detail[40];
} link_status_t;

// usb_dev.c
void usb_dev_start(ks_mode_t mode);
void usb_dev_send(const ks_gamepad_t *pad);
void usb_dev_set_vbus(bool present);
void usb_dev_status(link_status_t *st);

// ble_hid.c
void ble_hid_start(ks_mode_t mode);
void ble_hid_send(const ks_gamepad_t *pad);
void ble_hid_set_battery(uint8_t pct);
void ble_hid_status(link_status_t *st);

// wifi_link.c
void wifi_link_start(void);
void wifi_link_status(link_status_t *st);
void wifi_link_forget(void);

// app_main.c: called by the transports.
void app_set_rumble(ks_rumble_t rumble);
// target holds KS_FX_TARGET_* bits. The effect replaces the profile's until
// app_clear_host_state.
void app_set_trigger_fx(uint8_t target, const ks_trigger_fx_cfg_t *cfg);
void app_clear_host_state(void); // the host went away: stop rumble, drop its effects
void app_get_report(ks_gamepad_t *pad, uint8_t *battery_pct, uint8_t *flags);
