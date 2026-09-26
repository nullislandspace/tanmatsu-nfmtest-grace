// =====================================================================
//  pattern  --  test pictures (see pattern.h)
// =====================================================================

#include "pattern.h"
#include <stdio.h>
#include <string.h>
#include "pax_fonts.h"
#include "pax_text.h"

static char const* const NAMES[PAT_COUNT] = {"static", "bars", "text", "motion", "noise"};

bool pattern_parse(char const* name, pattern_t* out) {
    for (int i = 0; i < PAT_COUNT; i++) {
        if (strcmp(name, NAMES[i]) == 0) {
            *out = (pattern_t)i;
            return true;
        }
    }
    return false;
}

char const* pattern_name(pattern_t p) {
    return (unsigned)p < PAT_COUNT ? NAMES[p] : "?";
}

// A cheap integer hash: noise that is the same for the same (frame, i).
static inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static void bars(pax_buf_t* fb, int w, int h) {
    static pax_col_t const COLS[8] = {0xFFFFFFFF, 0xFFFFFF00, 0xFF00FFFF, 0xFF00FF00,
                                      0xFFFF00FF, 0xFFFF0000, 0xFF0000FF, 0xFF000000};
    int const bw = w / 8;
    for (int i = 0; i < 8; i++) pax_simple_rect(fb, COLS[i], (float)(i * bw), 0, (float)bw, (float)(h * 2 / 3));
    // A grey ramp below: banding and chroma subsampling show here.
    for (int i = 0; i < 32; i++) {
        uint8_t const v = (uint8_t)(i * 255 / 31);
        pax_simple_rect(fb, pax_col_rgb(v, v, v), (float)(i * w / 32), (float)(h * 2 / 3), (float)(w / 32 + 1),
                        (float)(h / 6));
    }
    // Fine grid, one-pixel lines: the hardest thing for 4:2:0.
    for (int x = 0; x < w; x += 16) pax_simple_line(fb, 0xFF808080, (float)x, (float)(h * 5 / 6), (float)x, (float)h);
    for (int y = h * 5 / 6; y < h; y += 8) pax_simple_line(fb, 0xFF808080, 0, (float)y, (float)w, (float)y);
}

static void text(pax_buf_t* fb, int w, int h, uint32_t frame) {
    pax_background(fb, 0xFF101820);
    int const line_h = 12;
    int const off    = (int)(frame % (uint32_t)line_h);
    char      buf[96];
    for (int row = -1; row < h / line_h + 1; row++) {
        uint32_t const n = (uint32_t)(row + (int)(frame / (uint32_t)line_h));
        snprintf(buf, sizeof(buf), "%06lu HP 100/100 MP 42/80 XP %08lx POS %+05ld,%+05ld Z %03lu OK", (unsigned long)n,
                 (unsigned long)hash32(n), (long)(hash32(n) % 2000) - 1000, (long)(hash32(n + 7) % 2000) - 1000,
                 (unsigned long)(n % 1000));
        pax_col_t const col = (n % 3 == 0) ? 0xFF40FF40 : (n % 3 == 1) ? 0xFFFFD040 : 0xFFE0E0E0;
        pax_draw_text(fb, col, pax_font_sky_mono, 9, 4, (float)(row * line_h - off), buf);
    }
    (void)w;
}

static void motion(pax_buf_t* fb, int w, int h, uint32_t frame) {
    // A checkered floor scrolling diagonally, 4 px a frame.
    int const tile = 40;
    int const ox   = (int)((frame * 4) % (uint32_t)(tile * 2));
    int const oy   = (int)((frame * 2) % (uint32_t)(tile * 2));
    for (int ty = -2; ty < h / tile + 2; ty++) {
        for (int tx = -2; tx < w / tile + 2; tx++) {
            pax_col_t const col = ((tx + ty) & 1) ? 0xFF305070 : 0xFF6090B0;
            pax_simple_rect(fb, col, (float)(tx * tile - ox), (float)(ty * tile - oy), (float)tile, (float)tile);
        }
    }
    // Shapes moving on their own paths, as sprites do.
    for (int i = 0; i < 6; i++) {
        uint32_t const t  = frame * (uint32_t)(3 + i);
        int const      cx = (int)((t + (uint32_t)i * 130) % (uint32_t)(w + 100)) - 50;
        int const      cy = 60 + (int)((hash32((uint32_t)i) % 300) + (uint32_t)((i * 37 + (int)frame * 2) % 60));
        pax_col_t const col = pax_col_hsv((uint8_t)(i * 40), 220, 255);
        pax_simple_circle(fb, col, (float)cx, (float)cy, 30.0f + (float)(i * 4));
        pax_simple_rect(fb, 0xFFFFFFFF, (float)(cx - 5), (float)(cy - 5), 10, 10);
    }
}

// Every pixel from a hash of (frame, index), straight into the buffer:
// orientation does not matter for noise, and pax per pixel would be slow.
static void noise(pax_buf_t* fb, uint32_t frame) {
    uint16_t* const px = pax_buf_get_pixels_rw(fb);
    size_t const    n  = (size_t)pax_buf_get_width_raw(fb) * (size_t)pax_buf_get_height_raw(fb);
    uint32_t        s  = hash32(frame * 2654435761u + 1);
    for (size_t i = 0; i < n; i += 2) {
        s = s * 1664525u + 1013904223u;  // an LCG seeded per frame: same frame, same noise
        px[i]     = (uint16_t)s;
        px[i + 1] = (uint16_t)(s >> 16);
    }
}

void pattern_draw(pax_buf_t* fb, pattern_t p, uint32_t frame, uint32_t fps) {
    int const w = pax_buf_get_width(fb);  // logical, 800
    int const h = pax_buf_get_height(fb);  // logical, 480
    switch (p) {
        case PAT_STATIC:
            pax_background(fb, 0xFF000000);
            bars(fb, w, h);
            break;
        case PAT_BARS:
            pax_background(fb, 0xFF000000);
            bars(fb, w, h);
            pax_simple_rect(fb, 0xFFFF2020, (float)((frame * 8) % (uint32_t)w), 0, 6, (float)h);
            break;
        case PAT_TEXT: text(fb, w, h, frame); break;
        case PAT_MOTION: motion(fb, w, h, frame); break;
        case PAT_NOISE: noise(fb, frame); break;
        default: break;
    }
    // Frame number and clock, big, on a plate so they stay legible.
    char     buf[48];
    uint32_t ms = fps ? (uint32_t)((uint64_t)frame * 1000u / fps) : 0;
    snprintf(buf, sizeof(buf), "F%06lu  %lu.%03lus", (unsigned long)frame, (unsigned long)(ms / 1000),
             (unsigned long)(ms % 1000));
    pax_simple_rect(fb, 0xFF000000, 8, 8, 470, 50);
    pax_draw_text(fb, 0xFFFFFFFF, pax_font_sky_mono, 36, 16, 14, buf);
    // Orientation marker: "TOP LEFT" must come out top left on the PC.
    pax_simple_rect(fb, 0xFF000000, (float)(w - 170), 8, 162, 30);
    pax_draw_text(fb, 0xFFFFFF00, pax_font_sky_mono, 18, (float)(w - 164), 14, "TOP RIGHT");
    pax_simple_rect(fb, 0xFFFFFF00, 0, 0, 8, 8);  // top-left corner mark
}
