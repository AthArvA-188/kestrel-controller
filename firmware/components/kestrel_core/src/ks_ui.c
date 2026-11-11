#include "ks_ui.h"

#include <stdio.h>
#include <string.h>

#include "ks_periph.h"

#define FW_VERSION "0.1.0"

#define C_BG KS_RGB(10, 14, 20)
#define C_PANEL KS_RGB(22, 29, 39)
#define C_PANEL_HI KS_RGB(36, 47, 62)
#define C_TEXT KS_RGB(232, 238, 245)
#define C_DIM KS_RGB(132, 146, 162)
#define C_ACCENT KS_RGB(45, 212, 191)
#define C_ACCENT_DARK KS_RGB(16, 78, 72)
#define C_GOOD KS_RGB(74, 222, 128)
#define C_WARN KS_RGB(251, 191, 36)
#define C_BAD KS_RGB(248, 113, 113)

#define HEADER_H 28
#define ROW_H 23
#define ROWS_VISIBLE 6
#define MAX_ROWS KS_OUT_COUNT
#define REPEAT_DELAY_MS 400
#define REPEAT_MS 120

#define NAV_UP KS_IN_BIT(KS_IN_DUP)
#define NAV_DOWN KS_IN_BIT(KS_IN_DDOWN)
#define NAV_LEFT KS_IN_BIT(KS_IN_DLEFT)
#define NAV_RIGHT KS_IN_BIT(KS_IN_DRIGHT)
#define NAV_A KS_IN_BIT(KS_IN_A)
#define NAV_B KS_IN_BIT(KS_IN_B)
#define NAV_FN KS_IN_BIT(KS_IN_FN)
#define NAV_ALL (NAV_UP | NAV_DOWN | NAV_LEFT | NAV_RIGHT | NAV_A | NAV_B | NAV_FN)

typedef struct {
    const char *label;
    char value[24];
    bool adjustable;
} row_t;

static const struct {
    const char *name;
    uint8_t r, g, b;
} palette[] = {
    {"White", 255, 255, 255}, {"Teal", 45, 212, 191}, {"Blue", 59, 130, 246},
    {"Purple", 168, 85, 247}, {"Red", 239, 68, 68},   {"Orange", 249, 115, 22},
    {"Yellow", 250, 204, 21}, {"Green", 34, 197, 94},
};
#define PALETTE_LEN ((int)(sizeof palette / sizeof palette[0]))

static const struct {
    const char *name;
    uint8_t x10;
} curves[] = {{"Linear", 10}, {"Precise", 18}, {"Fast", 7}};
static const uint8_t sleep_options[] = {0, 2, 5, 10, 15, 30};
static const char *const zone_names[KS_ZONE_COUNT] = {
    "Face buttons", "D-pad", "System buttons", "Left ring", "Right ring",
};

static ks_profile_t *active(const ks_ui_t *ui)
{
    return &ui->profiles[ui->settings->active_profile % KS_PROFILE_COUNT];
}

static int step_index(int index, int dir, int count) { return (index + dir + count) % count; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int palette_index(const ks_zone_cfg_t *z)
{
    for (int i = 0; i < PALETTE_LEN; i++) {
        if (palette[i].r == z->r && palette[i].g == z->g && palette[i].b == z->b) {
            return i;
        }
    }
    return -1;
}

static int curve_index(uint8_t x10)
{
    for (int i = 0; i < 3; i++) {
        if (curves[i].x10 == x10) {
            return i;
        }
    }
    return 0;
}

static int sleep_index(uint8_t minutes)
{
    for (int i = 0; i < (int)sizeof sleep_options; i++) {
        if (sleep_options[i] == minutes) {
            return i;
        }
    }
    return 3;
}

static int remapped_count(const ks_ui_t *ui)
{
    ks_profile_t def;
    int n = 0;
    ks_profile_default(&def, 0);
    for (int i = 0; i < KS_IN_COUNT; i++) {
        n += active(ui)->map[i] != def.map[i];
    }
    return n;
}

// Fills rows for the current list screen and returns the row count.
static int build_rows(const ks_ui_t *ui, row_t *rows, const char **title)
{
    const ks_profile_t *p = active(ui);
    int n = 0;
#define ROW(text, adj) (rows[n].label = (text), rows[n].adjustable = (adj), rows[n].value[0] = '\0', rows[n++].value)
#define FMT(text, adj, ...) snprintf(ROW(text, adj), sizeof rows[0].value, __VA_ARGS__)

    switch (ui->screen) {
    case KS_UI_MENU:
        *title = "Menu";
        FMT("Connection", false, "%s", ks_mode_name(ui->settings->mode));
        if (remapped_count(ui)) {
            FMT("Remap", false, "%d changed", remapped_count(ui));
        } else {
            ROW("Remap", false);
        }
        FMT("Triggers", false, "%s", ks_fx_name(p->trigger_fx[1].effect));
        FMT("Sticks", false, "Dead zone %d%%", p->stick_deadzone[0]);
        FMT("Lighting", false, "%d%%", p->zone[KS_ZONE_LRING].brightness);
        FMT("System", false, "%s", p->name);
        break;
    case KS_UI_CONNECTION:
        *title = "Connection";
        for (int m = 0; m < KS_MODE_COUNT; m++) {
            FMT(ks_mode_name(m), false, "%s", m == ui->settings->mode ? "Active" : "");
        }
        ROW("Forget WiFi and pairings", false);
        break;
    case KS_UI_TRIGGERS:
        *title = "Triggers";
        for (int s = 0; s < 2; s++) {
            const ks_trigger_fx_cfg_t *f = &p->trigger_fx[s];
            FMT(s ? "Right effect" : "Left effect", true, "%s", ks_fx_name(f->effect));
            FMT(s ? "Right start" : "Left start", true, "%d%%", (f->start * 100 + 127) / 255);
            FMT(s ? "Right strength" : "Left strength", true, "%d%%",
                (f->strength * 100 + 127) / 255);
        }
        break;
    case KS_UI_STICKS:
        *title = "Sticks";
        for (int s = 0; s < 2; s++) {
            FMT(s ? "Right dead zone" : "Left dead zone", true, "%d%%", p->stick_deadzone[s]);
            FMT(s ? "Right curve" : "Left curve", true, "%s",
                curves[curve_index(p->stick_curve[s])].name);
        }
        FMT("Trigger dead zone", true, "%d%%", p->trigger_deadzone[0]);
        ROW("Calibrate", false);
        break;
    case KS_UI_LIGHTING: {
        const ks_zone_cfg_t *z = &p->zone[ui->light_zone % KS_ZONE_COUNT];
        int c = palette_index(z);
        *title = "Lighting";
        FMT("Zone", true, "%s", zone_names[ui->light_zone % KS_ZONE_COUNT]);
        FMT("Colour", true, "%s", c >= 0 ? palette[c].name : "Custom");
        FMT("Brightness", true, "%d%%", z->brightness);
        ROW("Copy to all zones", false);
        break;
    }
    case KS_UI_SYSTEM:
        *title = "System";
        FMT("Profile", true, "%d %s", ui->settings->active_profile + 1, p->name);
        FMT("Screen brightness", true, "%d%%", ui->settings->backlight_pct);
        if (ui->settings->sleep_minutes) {
            FMT("Sleep after", true, "%d min", ui->settings->sleep_minutes);
        } else {
            FMT("Sleep after", true, "Never");
        }
        ROW("Reset this profile", false);
        ROW("Sleep now", false);
        FMT("Firmware", false, "%s", FW_VERSION);
        break;
    case KS_UI_REMAP_PICK:
        *title = NULL;
        for (int o = 0; o < KS_OUT_COUNT; o++) {
            FMT(ks_output_name(o), false, "%s", o == p->map[ui->remap_input] ? "Current" : "");
        }
        break;
    default:
        *title = "";
        break;
    }
#undef ROW
#undef FMT
    return n;
}

void ks_ui_init(ks_ui_t *ui, ks_settings_t *settings, ks_profile_t *profiles)
{
    memset(ui, 0, sizeof *ui);
    ui->settings = settings;
    ui->profiles = profiles;
    ui->screen = KS_UI_HOME;
}

bool ks_ui_menu_open(const ks_ui_t *ui) { return ui->screen != KS_UI_HOME; }

static void go(ks_ui_t *ui, uint8_t screen, uint8_t cursor)
{
    ui->screen = screen;
    ui->cursor = cursor;
    ui->scroll = cursor >= ROWS_VISIBLE ? (uint8_t)(cursor - ROWS_VISIBLE + 1) : 0;
    ui->timer_ms = 0;
}

void ks_ui_toggle_menu(ks_ui_t *ui)
{
    if (ui->screen == KS_UI_HOME) {
        ui->menu_cursor = 0;
        go(ui, KS_UI_MENU, 0);
        return;
    }
    if (ui->screen == KS_UI_CAL_CENTRE || ui->screen == KS_UI_CAL_RANGE) {
        ui->requests |= KS_UI_REQ_CAL_CANCEL;
    }
    go(ui, KS_UI_HOME, 0);
}

uint8_t ks_ui_take_requests(ks_ui_t *ui)
{
    uint8_t r = ui->requests;
    ui->requests = 0;
    return r;
}

void ks_ui_filter_gamepad(const ks_ui_t *ui, ks_gamepad_t *pad)
{
    if (ks_ui_menu_open(ui)) {
        memset(pad, 0, sizeof *pad);
    }
}

static void adjust(ks_ui_t *ui, int row, int dir)
{
    ks_profile_t *p = active(ui);
    ks_settings_t *s = ui->settings;

    switch (ui->screen) {
    case KS_UI_TRIGGERS: {
        ks_trigger_fx_cfg_t *f = &p->trigger_fx[row / 3];
        if (row % 3 == 0) {
            f->effect = (uint8_t)step_index(f->effect, dir, KS_FX_COUNT);
        } else if (row % 3 == 1) {
            f->start = (uint8_t)clampi(f->start + dir * 13, 0, 255);
        } else {
            f->strength = (uint8_t)clampi(f->strength + dir * 13, 0, 255);
        }
        ui->requests |= KS_UI_REQ_SAVE_PROFILE;
        break;
    }
    case KS_UI_STICKS:
        if (row == 4) {
            p->trigger_deadzone[0] = (uint8_t)clampi(p->trigger_deadzone[0] + dir * 2, 0, 30);
            p->trigger_deadzone[1] = p->trigger_deadzone[0];
        } else if (row < 4 && row % 2 == 0) {
            p->stick_deadzone[row / 2] = (uint8_t)clampi(p->stick_deadzone[row / 2] + dir * 2, 0, 40);
        } else if (row < 4) {
            p->stick_curve[row / 2] = curves[step_index(curve_index(p->stick_curve[row / 2]), dir, 3)].x10;
        } else {
            return;
        }
        ui->requests |= KS_UI_REQ_SAVE_PROFILE;
        break;
    case KS_UI_LIGHTING: {
        ks_zone_cfg_t *z = &p->zone[ui->light_zone % KS_ZONE_COUNT];
        if (row == 0) {
            ui->light_zone = (uint8_t)step_index(ui->light_zone, dir, KS_ZONE_COUNT);
            return;
        }
        if (row == 1) {
            int c = palette_index(z);
            c = c < 0 ? 0 : step_index(c, dir, PALETTE_LEN);
            z->r = palette[c].r;
            z->g = palette[c].g;
            z->b = palette[c].b;
        } else if (row == 2) {
            z->brightness = (uint8_t)clampi(z->brightness + dir * 10, 0, 100);
        } else {
            return;
        }
        ui->requests |= KS_UI_REQ_SAVE_PROFILE;
        break;
    }
    case KS_UI_SYSTEM:
        if (row == 0) {
            s->active_profile = (uint8_t)step_index(s->active_profile, dir, KS_PROFILE_COUNT);
        } else if (row == 1) {
            s->backlight_pct = (uint8_t)clampi(s->backlight_pct + dir * 10, 10, 100);
        } else if (row == 2) {
            s->sleep_minutes = sleep_options[step_index(sleep_index(s->sleep_minutes), dir,
                                                        (int)sizeof sleep_options)];
        } else {
            return;
        }
        ui->requests |= KS_UI_REQ_SAVE_SETTINGS;
        break;
    default:
        break;
    }
}

static void pick_output(ks_ui_t *ui, int output)
{
    active(ui)->map[ui->remap_input] = (uint8_t)output;
    ui->remap_output = (uint8_t)output;
    ui->requests |= KS_UI_REQ_SAVE_PROFILE;
    go(ui, KS_UI_REMAP_DONE, 0);
}

static void activate(ks_ui_t *ui, int row)
{
    static const uint8_t submenu[] = {KS_UI_CONNECTION, KS_UI_REMAP_PRESS, KS_UI_TRIGGERS,
                                      KS_UI_STICKS, KS_UI_LIGHTING, KS_UI_SYSTEM};
    ks_profile_t *p = active(ui);

    switch (ui->screen) {
    case KS_UI_MENU:
        ui->menu_cursor = (uint8_t)row;
        go(ui, submenu[row], submenu[row] == KS_UI_CONNECTION ? ui->settings->mode : 0);
        break;
    case KS_UI_CONNECTION:
        if (row < KS_MODE_COUNT) {
            if (row != ui->settings->mode) {
                ui->settings->mode = (uint8_t)row;
                ui->requests |= KS_UI_REQ_SAVE_SETTINGS | KS_UI_REQ_RESTART;
            }
        } else {
            ui->requests |= KS_UI_REQ_FORGET;
        }
        break;
    case KS_UI_STICKS:
        if (row == 5) {
            ui->requests |= KS_UI_REQ_CAL_START;
            go(ui, KS_UI_CAL_CENTRE, 0);
        }
        break;
    case KS_UI_LIGHTING:
        if (row == 3) {
            for (int z = 0; z < KS_ZONE_COUNT; z++) {
                p->zone[z] = p->zone[ui->light_zone % KS_ZONE_COUNT];
            }
            ui->requests |= KS_UI_REQ_SAVE_PROFILE;
        }
        break;
    case KS_UI_SYSTEM:
        if (row == 3) {
            ks_profile_default(p, ui->settings->active_profile);
            ui->requests |= KS_UI_REQ_SAVE_PROFILE;
        } else if (row == 4) {
            ui->requests |= KS_UI_REQ_SLEEP;
        }
        break;
    case KS_UI_REMAP_PICK:
        pick_output(ui, row);
        break;
    default:
        break;
    }
}

static bool list_input(ks_ui_t *ui, uint32_t pressed, uint32_t edges, uint16_t dt_ms)
{
    row_t rows[MAX_ROWS];
    const char *title;
    int count = build_rows(ui, rows, &title);
    uint32_t nav = edges & (NAV_UP | NAV_DOWN | NAV_LEFT | NAV_RIGHT);

    // A held direction repeats after a delay.
    if (nav) {
        ui->timer_ms = 0;
    } else if (pressed & (NAV_UP | NAV_DOWN | NAV_LEFT | NAV_RIGHT)) {
        ui->timer_ms = (uint16_t)(ui->timer_ms + dt_ms);
        if (ui->timer_ms >= REPEAT_DELAY_MS + REPEAT_MS) {
            ui->timer_ms = REPEAT_DELAY_MS;
            nav = pressed & (NAV_UP | NAV_DOWN | NAV_LEFT | NAV_RIGHT);
        }
    }

    if (edges & NAV_FN) {
        go(ui, KS_UI_HOME, 0);
        return true;
    }
    if (ui->screen == KS_UI_REMAP_PICK && (edges & ~NAV_ALL)) {
        // Pressing another control picks the output that control has by default.
        ks_profile_t def;
        ks_profile_default(&def, 0);
        for (int i = 0; i < KS_IN_COUNT; i++) {
            if ((edges & ~NAV_ALL & KS_IN_BIT(i)) && def.map[i] != KS_OUT_NONE) {
                pick_output(ui, def.map[i]);
                return true;
            }
        }
    }
    if (edges & NAV_B) {
        if (ui->screen == KS_UI_MENU) {
            go(ui, KS_UI_HOME, 0);
        } else if (ui->screen == KS_UI_REMAP_PICK) {
            go(ui, KS_UI_REMAP_PRESS, 0);
        } else {
            go(ui, KS_UI_MENU, ui->menu_cursor);
        }
        return true;
    }
    if (count == 0) {
        return edges != 0;
    }
    if (nav & NAV_UP) {
        ui->cursor = (uint8_t)step_index(ui->cursor, -1, count);
    } else if (nav & NAV_DOWN) {
        ui->cursor = (uint8_t)step_index(ui->cursor, 1, count);
    } else if ((nav & (NAV_LEFT | NAV_RIGHT)) && rows[ui->cursor].adjustable) {
        adjust(ui, ui->cursor, (nav & NAV_RIGHT) ? 1 : -1);
    } else if (edges & NAV_A) {
        activate(ui, ui->cursor);
        return true;
    }
    if (ui->cursor < ui->scroll) {
        ui->scroll = ui->cursor;
    } else if (ui->cursor >= ui->scroll + ROWS_VISIBLE) {
        ui->scroll = (uint8_t)(ui->cursor - ROWS_VISIBLE + 1);
    }
    return nav != 0 || edges != 0;
}

bool ks_ui_input(ks_ui_t *ui, uint32_t pressed, uint16_t dt_ms)
{
    uint32_t edges = pressed & ~ui->prev_pressed;
    ui->prev_pressed = pressed;

    switch (ui->screen) {
    case KS_UI_HOME:
        return false;
    case KS_UI_REMAP_PRESS:
        if (edges) {
            for (int i = 0; i < KS_IN_COUNT; i++) {
                if (edges & KS_IN_BIT(i)) {
                    ui->remap_input = (uint8_t)i;
                    go(ui, KS_UI_REMAP_PICK, active(ui)->map[i]);
                    return true;
                }
            }
        }
        ui->timer_ms = (uint16_t)(ui->timer_ms + dt_ms);
        if (ui->timer_ms >= KS_UI_REMAP_TIMEOUT_MS) {
            go(ui, KS_UI_MENU, ui->menu_cursor);
        }
        return true;
    case KS_UI_REMAP_DONE:
    case KS_UI_CAL_DONE:
        ui->timer_ms = (uint16_t)(ui->timer_ms + dt_ms);
        if (edges || ui->timer_ms >= KS_UI_DONE_MS) {
            if (ui->screen == KS_UI_CAL_DONE) {
                go(ui, KS_UI_STICKS, 5);
            } else {
                go(ui, KS_UI_MENU, ui->menu_cursor);
            }
        }
        return true;
    case KS_UI_CAL_CENTRE:
    case KS_UI_CAL_RANGE:
        if (edges & NAV_A) {
            ui->requests |= KS_UI_REQ_CAL_ADVANCE;
            go(ui, ui->screen == KS_UI_CAL_CENTRE ? KS_UI_CAL_RANGE : KS_UI_CAL_DONE, 0);
        } else if (edges & (NAV_B | NAV_FN)) {
            ui->requests |= KS_UI_REQ_CAL_CANCEL;
            go(ui, KS_UI_STICKS, 5);
        }
        return edges != 0;
    default:
        return list_input(ui, pressed, edges, dt_ms);
    }
}

// Drawing.

static int text_y(const ks_font_t *font, int top, int height)
{
    return top + (height - font->height) / 2;
}

static void draw_header(ks_fb_t *fb, const char *title, const char *right)
{
    ks_fb_fill(fb, 0, 0, fb->w, HEADER_H, C_PANEL);
    ks_fb_text(fb, &ks_font_medium, 12, text_y(&ks_font_medium, 0, HEADER_H), title, C_TEXT);
    if (right && right[0]) {
        ks_fb_text_right(fb, &ks_font_small, fb->w - 12, text_y(&ks_font_small, 0, HEADER_H),
                         right, C_DIM);
    }
}

static void draw_list(const ks_ui_t *ui, ks_fb_t *fb)
{
    row_t rows[MAX_ROWS];
    const char *title = "";
    char buf[40];
    int count = build_rows(ui, rows, &title);

    if (ui->screen == KS_UI_REMAP_PICK) {
        snprintf(buf, sizeof buf, "%s sends", ks_input_name(ui->remap_input));
        draw_header(fb, buf, "Press or pick");
    } else {
        draw_header(fb, title, active(ui)->name);
    }
    for (int i = 0; i < ROWS_VISIBLE && ui->scroll + i < count; i++) {
        int index = ui->scroll + i, y = HEADER_H + 1 + i * ROW_H;
        const row_t *r = &rows[index];
        bool sel = index == ui->cursor;
        if (sel) {
            ks_fb_round(fb, 6, y, fb->w - 12, ROW_H - 1, 6, C_PANEL_HI);
            ks_fb_round(fb, 6, y + 4, 4, ROW_H - 9, 2, C_ACCENT);
        }
        ks_fb_text(fb, &ks_font_medium, 18, text_y(&ks_font_medium, y, ROW_H - 1), r->label,
                   sel ? C_TEXT : C_DIM);
        if (r->value[0]) {
            if (sel && r->adjustable) {
                snprintf(buf, sizeof buf, "<  %s  >", r->value);
            } else {
                snprintf(buf, sizeof buf, "%s", r->value);
            }
            ks_fb_text_right(fb, &ks_font_small, fb->w - 16, text_y(&ks_font_small, y, ROW_H - 1),
                             buf, sel ? C_ACCENT : C_DIM);
            if (ui->screen == KS_UI_LIGHTING && index == 1) {
                // Show the zone's colour beside its name.
                const ks_zone_cfg_t *z = &active(ui)->zone[ui->light_zone % KS_ZONE_COUNT];
                int sx = fb->w - 16 - ks_fb_text_width(&ks_font_small, buf) - 22;
                ks_fb_round(fb, sx, y + 4, 14, 14, 4, KS_RGB(z->r, z->g, z->b));
            }
        }
    }
    if (count > ROWS_VISIBLE) {
        // Scroll bar.
        int track = ROWS_VISIBLE * ROW_H - 6;
        int bar = track * ROWS_VISIBLE / count;
        int top = HEADER_H + 4 + (track - bar) * ui->scroll / (count - ROWS_VISIBLE);
        ks_fb_round(fb, fb->w - 5, top, 3, bar, 1, C_DIM);
    }
}

static void draw_battery(ks_fb_t *fb, int right, int top, int height, const ks_ui_status_t *st)
{
    char txt[8];
    int pct = st->battery_pct > 100 ? 100 : st->battery_pct;
    uint16_t colour = pct < 20 ? C_BAD : pct < 50 ? C_WARN : C_GOOD;
    snprintf(txt, sizeof txt, "%d%%", pct);
    int x = right - ks_fb_text_width(&ks_font_medium, txt);
    ks_fb_text(fb, &ks_font_medium, x, text_y(&ks_font_medium, top, height), txt, C_TEXT);

    int bx = x - 38, by = top + (height - 14) / 2;
    ks_fb_round(fb, bx, by, 28, 14, 4, C_DIM);
    ks_fb_round(fb, bx + 2, by + 2, 24, 10, 2, C_BG);
    ks_fb_round(fb, bx + 28, by + 4, 3, 6, 1, C_DIM);
    int fill = (pct * 20 + 99) / 100;
    if (fill > 0) {
        ks_fb_fill(fb, bx + 4, by + 4, fill, 6, colour);
    }
    if (st->charge != KS_CHG_ON_BATTERY) {
        ks_fb_text_right(fb, &ks_font_small, bx - 8, text_y(&ks_font_small, top, height),
                         st->charge == KS_CHG_CHARGING ? "Charging" : "Full", C_ACCENT);
    }
}

static void draw_home(const ks_ui_t *ui, const ks_ui_status_t *st, ks_fb_t *fb)
{
    static const char *const tags[KS_MODE_COUNT] = {"USB", "USB", "BT", "BT", "WiFi"};
    static const char *const short_names[KS_MODE_COUNT] = {"XInput", "DInput", "Xbox", "HID", ""};
    static const char *const link_text[] = {"Not connected", "Searching", "Connected", "Setup"};
    const uint16_t link_colour[] = {C_DIM, C_WARN, C_GOOD, C_ACCENT};
    int mode = ui->settings->mode % KS_MODE_COUNT, link = st->link & 3;
    char buf[32];

    // Top row: mode on the left, battery on the right.
    int tw = ks_fb_text_width(&ks_font_small, tags[mode]);
    ks_fb_round(fb, 10, 8, tw + 20, 24, 12, C_ACCENT_DARK);
    ks_fb_text(fb, &ks_font_small, 20, text_y(&ks_font_small, 8, 24), tags[mode], C_ACCENT);
    ks_fb_text(fb, &ks_font_medium, tw + 38, text_y(&ks_font_medium, 8, 24), short_names[mode],
               C_TEXT);
    draw_battery(fb, fb->w - 12, 8, 24, st);

    // Host and link state.
    const char *host = st->host[0] ? st->host : "No host";
    const ks_font_t *font = ks_fb_text_width(&ks_font_large, host) <= fb->w - 24
                                ? &ks_font_large : &ks_font_medium;
    ks_fb_text(fb, font, 12, text_y(font, 40, 44), host, st->host[0] ? C_TEXT : C_DIM);
    ks_fb_round(fb, 14, 92, 10, 10, 5, link_colour[link]);
    ks_fb_text(fb, &ks_font_medium, 32, text_y(&ks_font_medium, 86, 22), link_text[link], C_TEXT);
    if (st->detail[0]) {
        ks_fb_text(fb, &ks_font_small, 14, 112, st->detail, C_DIM);
    }

    // Bottom bar: profile on the left, menu hint on the right.
    ks_fb_fill(fb, 0, 138, fb->w, 32, C_PANEL);
    snprintf(buf, sizeof buf, "Profile %d", ui->settings->active_profile + 1);
    int x = ks_fb_text(fb, &ks_font_small, 12, text_y(&ks_font_small, 138, 32), buf, C_DIM);
    ks_fb_text(fb, &ks_font_medium, x + 10, text_y(&ks_font_medium, 138, 32), active(ui)->name,
               C_TEXT);
    int mx = fb->w - 12 - ks_fb_text_width(&ks_font_small, "Menu");
    ks_fb_text(fb, &ks_font_small, mx, text_y(&ks_font_small, 138, 32), "Menu", C_DIM);
    ks_fb_round(fb, mx - 36, 145, 28, 18, 5, C_PANEL_HI);
    ks_fb_text_centre(fb, &ks_font_small, mx - 22, text_y(&ks_font_small, 145, 18), "Fn", C_TEXT);
}

static void draw_message(ks_fb_t *fb, const char *title, const char *step, const char *line1,
                         const char *line2, const char *hint)
{
    draw_header(fb, title, step);
    ks_fb_text_centre(fb, &ks_font_medium, fb->w / 2, 56, line1, C_TEXT);
    if (line2) {
        ks_fb_text_centre(fb, &ks_font_medium, fb->w / 2, 80, line2, C_TEXT);
    }
    if (hint) {
        ks_fb_text_centre(fb, &ks_font_small, fb->w / 2, 138, hint, C_DIM);
    }
}

static void draw_arrow(ks_fb_t *fb, int x, int cy, uint16_t colour)
{
    ks_fb_fill(fb, x, cy - 2, 26, 4, colour);
    for (int k = 0; k < 10; k++) {
        ks_fb_fill(fb, x + 24 + k, cy - (9 - k), 1, 2 * (9 - k) + 1, colour);
    }
}

void ks_ui_draw(const ks_ui_t *ui, const ks_ui_status_t *st, ks_fb_t *fb)
{
    char buf[48];
    ks_fb_clear(fb, C_BG);

    switch (ui->screen) {
    case KS_UI_HOME:
        draw_home(ui, st, fb);
        break;
    case KS_UI_REMAP_PRESS: {
        int left = KS_UI_REMAP_TIMEOUT_MS - ui->timer_ms;
        draw_header(fb, "Remap", "Step 2 of 3");
        ks_fb_text_centre(fb, &ks_font_large, fb->w / 2, 44, "Press a control", C_TEXT);
        snprintf(buf, sizeof buf, "It will be remapped in %s", active(ui)->name);
        ks_fb_text_centre(fb, &ks_font_small, fb->w / 2, 96, buf, C_DIM);
        ks_fb_round(fb, 40, 128, 240, 6, 3, C_PANEL_HI);
        ks_fb_round(fb, 40, 128, 240 * (left < 0 ? 0 : left) / KS_UI_REMAP_TIMEOUT_MS, 6, 3,
                    C_ACCENT);
        snprintf(buf, sizeof buf, "Goes back in %d s if nothing is pressed", (left + 999) / 1000);
        ks_fb_text_centre(fb, &ks_font_small, fb->w / 2, 142, buf, C_DIM);
        break;
    }
    case KS_UI_REMAP_DONE: {
        const char *from = ks_input_name(ui->remap_input), *to = ks_output_name(ui->remap_output);
        const ks_font_t *font = &ks_font_large;
        int wf = ks_fb_text_width(font, from), wt = ks_fb_text_width(font, to);
        if (wf + wt + 60 > fb->w - 24) {
            font = &ks_font_medium;
            wf = ks_fb_text_width(font, from);
            wt = ks_fb_text_width(font, to);
        }
        int x = (fb->w - (wf + wt + 60)) / 2, y = text_y(font, 48, 48);
        draw_header(fb, "Remap", "Saved");
        ks_fb_text(fb, font, x, y, from, C_TEXT);
        draw_arrow(fb, x + wf + 12, 72, C_ACCENT);
        ks_fb_text(fb, font, x + wf + 60, y, to, C_ACCENT);
        snprintf(buf, sizeof buf, "Saved to profile %s", active(ui)->name);
        ks_fb_text_centre(fb, &ks_font_small, fb->w / 2, 116, buf, C_DIM);
        break;
    }
    case KS_UI_CAL_CENTRE:
        draw_message(fb, "Calibrate", "Step 1 of 2", "Let go of both sticks",
                     "and both triggers", "A continue   B cancel");
        break;
    case KS_UI_CAL_RANGE:
        draw_message(fb, "Calibrate", "Step 2 of 2", "Roll both sticks around the edge",
                     "and pull both triggers fully", "A save   B cancel");
        break;
    case KS_UI_CAL_DONE:
        draw_message(fb, "Calibrate", "Done", "Calibration saved", NULL, NULL);
        break;
    default:
        draw_list(ui, fb);
        break;
    }
}
