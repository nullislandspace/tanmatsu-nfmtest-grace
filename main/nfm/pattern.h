#pragma once
// =====================================================================
//  pattern  --  test pictures, a pure function of the frame number
// ---------------------------------------------------------------------
//  Part B of claudeplans/nfmtest.md. Same frame number, same picture:
//  two runs produce the same video and the same bitrate.
//
//    static  colour bars, a grid, fine text: the encoder's floor
//    bars    the same, with a line sweeping across
//    text    dense HUD-like text, scrolling one pixel a frame
//    motion  a scrolling checkered plane and moving shapes: a game
//    noise   every pixel changes every frame: the worst case
//
//  Each carries the frame number and a millisecond clock (frame / fps)
//  in big digits, for dropped-frame checks and glass-to-glass latency.
//
//  Drawn into an RGB565 pax buffer in its logical (landscape)
//  orientation, as a game draws.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>
#include "pax_gfx.h"

typedef enum {
    PAT_STATIC = 0,
    PAT_BARS,
    PAT_TEXT,
    PAT_MOTION,
    PAT_NOISE,
    PAT_COUNT,
} pattern_t;

// "static", "bars", ... -> the pattern; false if unknown.
bool        pattern_parse(char const* name, pattern_t* out);
char const* pattern_name(pattern_t p);

void pattern_draw(pax_buf_t* fb, pattern_t p, uint32_t frame, uint32_t fps);
