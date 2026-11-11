#include "ks_fb.h"

#include <math.h>

uint16_t ks_fb_blend(uint16_t dst, uint16_t src, int alpha)
{
    if (alpha <= 0) {
        return dst;
    }
    if (alpha >= 255) {
        return src;
    }
    int inv = 255 - alpha;
    int r = ((dst >> 11) * inv + (src >> 11) * alpha + 127) / 255;
    int g = (((dst >> 5) & 63) * inv + ((src >> 5) & 63) * alpha + 127) / 255;
    int b = ((dst & 31) * inv + (src & 31) * alpha + 127) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void plot(ks_fb_t *fb, int x, int y, uint16_t colour, int alpha)
{
    if (x >= 0 && y >= 0 && x < fb->w && y < fb->h) {
        uint16_t *p = &fb->px[y * fb->w + x];
        *p = ks_fb_blend(*p, colour, alpha);
    }
}

void ks_fb_clear(ks_fb_t *fb, uint16_t colour)
{
    for (int i = 0; i < fb->w * fb->h; i++) {
        fb->px[i] = colour;
    }
}

void ks_fb_fill(ks_fb_t *fb, int x, int y, int w, int h, uint16_t colour)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > fb->w ? fb->w : x + w, y1 = y + h > fb->h ? fb->h : y + h;
    for (int j = y0; j < y1; j++) {
        for (int i = x0; i < x1; i++) {
            fb->px[j * fb->w + i] = colour;
        }
    }
}

void ks_fb_round(ks_fb_t *fb, int x, int y, int w, int h, int r, uint16_t colour)
{
    if (r * 2 > w) {
        r = w / 2;
    }
    if (r * 2 > h) {
        r = h / 2;
    }
    ks_fb_fill(fb, x, y + r, w, h - 2 * r, colour);
    ks_fb_fill(fb, x + r, y, w - 2 * r, r, colour);
    ks_fb_fill(fb, x + r, y + h - r, w - 2 * r, r, colour);
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            // Coverage from the distance between the pixel centre and the arc.
            float dx = (float)r - ((float)i + 0.5f), dy = (float)r - ((float)j + 0.5f);
            float cover = (float)r - sqrtf(dx * dx + dy * dy) + 0.5f;
            int alpha = cover >= 1.0f ? 255 : cover <= 0.0f ? 0 : (int)(cover * 255.0f);
            plot(fb, x + i, y + j, colour, alpha);
            plot(fb, x + w - 1 - i, y + j, colour, alpha);
            plot(fb, x + i, y + h - 1 - j, colour, alpha);
            plot(fb, x + w - 1 - i, y + h - 1 - j, colour, alpha);
        }
    }
}

static const ks_glyph_t *glyph_of(const ks_font_t *font, char c)
{
    unsigned ch = (unsigned char)c;
    if (ch < font->first || ch > font->last) {
        ch = '?';
    }
    return &font->glyphs[ch - font->first];
}

int ks_fb_text(ks_fb_t *fb, const ks_font_t *font, int x, int y, const char *s, uint16_t colour)
{
    for (; *s; s++) {
        const ks_glyph_t *g = glyph_of(font, *s);
        const uint8_t *row = font->data + g->offset;
        int stride = (g->width + 1) / 2;
        for (int j = 0; j < font->height; j++, row += stride) {
            for (int i = 0; i < g->width; i++) {
                int a = (i & 1) ? (row[i >> 1] & 15) : (row[i >> 1] >> 4);
                if (a) {
                    plot(fb, x + i, y + j, colour, a * 17);
                }
            }
        }
        x += g->width;
    }
    return x;
}

int ks_fb_text_width(const ks_font_t *font, const char *s)
{
    int w = 0;
    for (; *s; s++) {
        w += glyph_of(font, *s)->width;
    }
    return w;
}

void ks_fb_text_right(ks_fb_t *fb, const ks_font_t *font, int right, int y, const char *s,
                      uint16_t colour)
{
    ks_fb_text(fb, font, right - ks_fb_text_width(font, s), y, s, colour);
}

void ks_fb_text_centre(ks_fb_t *fb, const ks_font_t *font, int cx, int y, const char *s,
                       uint16_t colour)
{
    ks_fb_text(fb, font, cx - ks_fb_text_width(font, s) / 2, y, s, colour);
}
