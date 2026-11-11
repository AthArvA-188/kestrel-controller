// Screen UI: the home screen, the menu, and the remap flow.
//
// The UI edits the settings and profiles it is given and raises request bits.
// The application saves, restarts, or sleeps in response.
#pragma once

#include "ks_fb.h"
#include "ks_profile.h"
#include "ks_types.h"

#define KS_UI_WIDTH 320
#define KS_UI_HEIGHT 170

typedef enum { KS_LINK_IDLE, KS_LINK_SEARCHING, KS_LINK_CONNECTED, KS_LINK_SETUP } ks_link_t;

// What the home screen shows. The application fills this in.
typedef struct {
    uint8_t link;        // ks_link_t
    uint8_t battery_pct;
    uint8_t charge;      // ks_charge_state_t
    char host[24];       // connected host, empty when there is none
    char detail[40];     // second line, such as an address or a setup hint
} ks_ui_status_t;

typedef enum {
    KS_UI_HOME,
    KS_UI_MENU,
    KS_UI_CONNECTION,
    KS_UI_TRIGGERS,
    KS_UI_STICKS,
    KS_UI_LIGHTING,
    KS_UI_SYSTEM,
    KS_UI_REMAP_PRESS, // step 2: press the physical control
    KS_UI_REMAP_PICK,  // step 3: press or pick the output
    KS_UI_REMAP_DONE,
    KS_UI_CAL_CENTRE,
    KS_UI_CAL_RANGE,
    KS_UI_CAL_DONE
} ks_ui_screen_t;

#define KS_UI_REQ_SAVE_PROFILE 0x01
#define KS_UI_REQ_SAVE_SETTINGS 0x02
#define KS_UI_REQ_RESTART 0x04     // the mode changed; save and restart
#define KS_UI_REQ_FORGET 0x08      // erase WiFi credentials and Bluetooth bonds
#define KS_UI_REQ_SLEEP 0x10
#define KS_UI_REQ_CAL_START 0x20   // begin the centre phase
#define KS_UI_REQ_CAL_ADVANCE 0x40 // end the current calibration phase
#define KS_UI_REQ_CAL_CANCEL 0x80

#define KS_UI_REMAP_TIMEOUT_MS 8000
#define KS_UI_DONE_MS 1500

typedef struct {
    ks_settings_t *settings;
    ks_profile_t *profiles; // KS_PROFILE_COUNT entries
    uint8_t screen;
    uint8_t cursor;         // row on the current list screen
    uint8_t menu_cursor;    // row to return to on the main menu
    uint8_t scroll;
    uint8_t remap_input;    // ks_input_t chosen in step 2
    uint8_t remap_output;
    uint8_t light_zone;
    uint16_t timer_ms;
    uint32_t prev_pressed;
    uint8_t requests;
} ks_ui_t;

void ks_ui_init(ks_ui_t *ui, ks_settings_t *settings, ks_profile_t *profiles);
bool ks_ui_menu_open(const ks_ui_t *ui);
void ks_ui_toggle_menu(ks_ui_t *ui);
// Feeds the debounced inputs. Call it on every UI tick, also with nothing
// pressed. Returns true when the screen changed.
bool ks_ui_input(ks_ui_t *ui, uint32_t pressed, uint16_t dt_ms);
// Returns the pending request bits and clears them.
uint8_t ks_ui_take_requests(ks_ui_t *ui);
// Makes the host report neutral while the menu is open: no buttons, sticks
// centred, triggers released.
void ks_ui_filter_gamepad(const ks_ui_t *ui, ks_gamepad_t *pad);
void ks_ui_draw(const ks_ui_t *ui, const ks_ui_status_t *st, ks_fb_t *fb);
