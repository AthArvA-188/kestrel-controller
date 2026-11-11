// Renders the UI to PNG files by driving the same state machine and drawing
// code the controller runs. Usage: kestrel_render [output directory]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ks_periph.h"
#include "ks_ui.h"

#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#include <sys/stat.h>
#define make_dir(path) mkdir(path, 0755)
#endif

static uint16_t pixels[KS_UI_WIDTH * KS_UI_HEIGHT];
static ks_settings_t settings;
static ks_profile_t profiles[KS_PROFILE_COUNT];
static ks_ui_t ui;
static ks_ui_status_t status;
static const char *out_dir = "out";
static int failures;

static void put32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, size_t len)
{
    uint8_t head[4], tail[4];
    uint8_t *body = malloc(len + 4);
    memcpy(body, type, 4);
    if (len) {
        memcpy(body + 4, data, len);
    }
    put32be(head, (uint32_t)len);
    put32be(tail, ks_crc32(body, len + 4));
    fwrite(head, 1, 4, f);
    fwrite(body, 1, len + 4, f);
    fwrite(tail, 1, 4, f);
    free(body);
}

// Writes an 8-bit RGB PNG. The image data uses stored deflate blocks, so no
// compression library is needed.
static bool write_png(const char *path, const uint16_t *px, int w, int h)
{
    static const uint8_t magic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    size_t raw_len = (size_t)(w * 3 + 1) * h, blocks = (raw_len + 65534) / 65535;
    uint8_t *raw = malloc(raw_len), *z = malloc(raw_len + blocks * 5 + 6), *o = z;
    uint8_t ihdr[13] = {0};
    uint32_t a = 1, b = 0;
    FILE *f = fopen(path, "wb");
    if (!f || !raw || !z) {
        return false;
    }
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (w * 3 + 1);
        *row++ = 0; // filter: none
        for (int x = 0; x < w; x++) {
            uint16_t v = px[y * w + x];
            *row++ = (uint8_t)((v >> 11) * 255 / 31);
            *row++ = (uint8_t)(((v >> 5) & 63) * 255 / 63);
            *row++ = (uint8_t)((v & 31) * 255 / 31);
        }
    }
    *o++ = 0x78;
    *o++ = 0x01;
    for (size_t pos = 0; pos < raw_len;) {
        size_t n = raw_len - pos > 65535 ? 65535 : raw_len - pos;
        *o++ = pos + n == raw_len;
        *o++ = (uint8_t)n;
        *o++ = (uint8_t)(n >> 8);
        *o++ = (uint8_t)~n;
        *o++ = (uint8_t)(~n >> 8);
        memcpy(o, raw + pos, n);
        o += n;
        pos += n;
    }
    for (size_t i = 0; i < raw_len; i++) { // Adler-32
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    put32be(o, (b << 16) | a);
    o += 4;

    put32be(ihdr, (uint32_t)w);
    put32be(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; // bit depth
    ihdr[9] = 2; // colour type: RGB
    fwrite(magic, 1, sizeof magic, f);
    png_chunk(f, "IHDR", ihdr, sizeof ihdr);
    png_chunk(f, "IDAT", z, (size_t)(o - z));
    png_chunk(f, "IEND", NULL, 0);
    free(raw);
    free(z);
    return fclose(f) == 0;
}

static void shot(const char *name)
{
    ks_fb_t fb = {pixels, KS_UI_WIDTH, KS_UI_HEIGHT};
    char path[512];
    ks_ui_draw(&ui, &status, &fb);
    snprintf(path, sizeof path, "%s/%s.png", out_dir, name);
    if (write_png(path, pixels, fb.w, fb.h)) {
        printf("wrote %s\n", path);
    } else {
        printf("FAILED %s\n", path);
        failures++;
    }
}

static void reset(void)
{
    ks_settings_default(&settings);
    for (int i = 0; i < KS_PROFILE_COUNT; i++) {
        ks_profile_default(&profiles[i], i);
    }
    ks_ui_init(&ui, &settings, profiles);
}

static void tap(int input)
{
    ks_ui_input(&ui, KS_IN_BIT(input), 16);
    ks_ui_input(&ui, 0, 16);
}

static void home(int mode, int profile, int link, int battery, int charge, const char *host,
                 const char *detail)
{
    reset();
    settings.mode = (uint8_t)mode;
    settings.active_profile = (uint8_t)profile;
    memset(&status, 0, sizeof status);
    status.link = (uint8_t)link;
    status.battery_pct = (uint8_t)battery;
    status.charge = (uint8_t)charge;
    snprintf(status.host, sizeof status.host, "%s", host);
    snprintf(status.detail, sizeof status.detail, "%s", detail);
}

static void submenu(int row)
{
    reset();
    ks_ui_toggle_menu(&ui);
    for (int i = 0; i < row; i++) {
        tap(KS_IN_DDOWN);
    }
    tap(KS_IN_A);
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        out_dir = argv[1];
    }
    make_dir(out_dir);

    // Home screen in each kind of mode, at different battery levels.
    home(KS_MODE_USB_XINPUT, 0, KS_LINK_CONNECTED, 82, KS_CHG_CHARGING, "USB host",
         "Xbox 360 protocol");
    shot("home_usb_xinput");
    home(KS_MODE_USB_DINPUT, 0, KS_LINK_IDLE, 100, KS_CHG_FULL, "", "Waiting for a USB host");
    shot("home_usb_dinput_idle");
    home(KS_MODE_BLE_XBOX, 1, KS_LINK_CONNECTED, 47, KS_CHG_ON_BATTERY, "5C:F3:70:8A:11:A1",
         "Xbox Wireless Controller identity");
    shot("home_ble_xbox");
    home(KS_MODE_BLE_GENERIC, 3, KS_LINK_SEARCHING, 64, KS_CHG_ON_BATTERY, "",
         "Pair with \"Kestrel\" on the host");
    shot("home_ble_searching");
    home(KS_MODE_WIFI, 2, KS_LINK_CONNECTED, 12, KS_CHG_ON_BATTERY, "GAMING-PC",
         "192.168.1.20   250 Hz");
    shot("home_wifi");
    home(KS_MODE_WIFI, 0, KS_LINK_SETUP, 5, KS_CHG_CHARGING, "",
         "Join Kestrel-3F2A, open 192.168.4.1");
    shot("home_wifi_setup");

    // Main menu, then the remap flow.
    reset();
    ks_ui_toggle_menu(&ui);
    shot("menu_main");
    tap(KS_IN_DDOWN);
    shot("remap_1_choose");
    tap(KS_IN_A);
    ks_ui_input(&ui, 0, 2500);
    shot("remap_2_press");
    tap(KS_IN_P1);
    shot("remap_3_pick");
    for (int i = 0; i < 9; i++) {
        tap(KS_IN_DDOWN);
    }
    shot("remap_3_pick_scrolled");
    tap(KS_IN_X);
    shot("remap_4_done");
    ks_ui_input(&ui, 0, KS_UI_DONE_MS);
    shot("menu_after_remap");

    // The longest names the done screen can show.
    submenu(1);
    tap(KS_IN_DRIGHT);
    for (int i = KS_OUT_DRIGHT; i > KS_OUT_DLEFT; i--) {
        tap(KS_IN_DUP);
    }
    tap(KS_IN_A);
    shot("remap_4_done_long");

    submenu(0);
    shot("menu_connection");
    submenu(2);
    tap(KS_IN_DRIGHT);
    tap(KS_IN_DRIGHT);
    shot("menu_triggers");
    submenu(3);
    tap(KS_IN_DDOWN);
    shot("menu_sticks");
    submenu(4);
    tap(KS_IN_DDOWN);
    shot("menu_lighting");
    submenu(5);
    shot("menu_system");
    submenu(3);
    tap(KS_IN_DUP);
    tap(KS_IN_A);
    shot("calibrate_1");
    tap(KS_IN_A);
    shot("calibrate_2");
    tap(KS_IN_A);
    shot("calibrate_3");
    return failures ? 1 : 0;
}
