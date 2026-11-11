// Drawing into an RGB565 framebuffer. The controller sends the buffer to the
// ST7789 and the native build writes it to a PNG.
#pragma once

#include <stdint.h>

typedef struct {
    uint32_t offset;
    uint8_t width;
} ks_glyph_t;

// Glyph cells are width by height pixels at 4 bits per pixel, two pixels per
// byte, left pixel in the high nibble, rows padded to a whole byte.
typedef struct {
    uint8_t height, ascent, first, last;
    const ks_glyph_t *glyphs;
    const uint8_t *data;
} ks_font_t;

extern const ks_font_t ks_font_small, ks_font_medium, ks_font_large;

typedef struct {
    uint16_t *px;
    int w, h;
} ks_fb_t;

#define KS_RGB(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

uint16_t ks_fb_blend(uint16_t dst, uint16_t src, int alpha); // alpha 0 to 255
void ks_fb_clear(ks_fb_t *fb, uint16_t colour);
void ks_fb_fill(ks_fb_t *fb, int x, int y, int w, int h, uint16_t colour);
// Filled rectangle with anti-aliased corners of radius r.
void ks_fb_round(ks_fb_t *fb, int x, int y, int w, int h, int r, uint16_t colour);
// Draws s with its top-left corner at x, y. Returns the x after the last glyph.
int ks_fb_text(ks_fb_t *fb, const ks_font_t *font, int x, int y, const char *s, uint16_t colour);
int ks_fb_text_width(const ks_font_t *font, const char *s);
void ks_fb_text_right(ks_fb_t *fb, const ks_font_t *font, int right, int y, const char *s,
                      uint16_t colour);
void ks_fb_text_centre(ks_fb_t *fb, const ks_font_t *font, int cx, int y, const char *s,
                       uint16_t colour);
