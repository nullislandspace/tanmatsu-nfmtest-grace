#pragma once
// =====================================================================
//  stream  --  framebuffer -> PPA -> H.264 -> MPEG-TS -> UDP :5000
// ---------------------------------------------------------------------
//  Part B of claudeplans/nfmtest.md, over the ncm_raw link (usbnet).
//
//    render task (core 0)  draws the test pattern into one of three
//                          RGB565 framebuffers and publishes it; the
//                          display shows it too (frame counter on screen
//                          next to OBS gives glass-to-glass latency);
//    stream task (core 1)  at a fixed rate, takes the newest published
//                          frame: PPA to YUV420 (rotated upright), the
//                          graceloader encoder, tsmux, usbnet_send_udp.
//
//  A slow renderer means repeated frames (cheap P-frames); a stream
//  frame whose work overruns its slot is skipped and counted, never
//  waited for.
//
//  On the PC: OBS Media Source, input udp://@:5000, format mpegts
//  (or: ffplay -fflags nobuffer udp://@:5000).
// =====================================================================

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "pattern.h"
#include "pax_gfx.h"

typedef struct {
    int       fps;      // stream rate
    uint32_t  br_kbit;  // encoder target
    int       gop;      // frames between keyframes
    pattern_t pat;
} stream_cfg_t;

typedef struct {
    uint32_t frames;       // encoded and muxed
    uint32_t skipped;      // stream slots missed (work overran)
    uint32_t enc_errors;
    uint32_t keyframes;
    uint32_t rendered;     // frames the render task drew
    uint64_t es_bytes;     // H.264 bytes out of the encoder
    uint32_t dgrams;
    uint32_t dgrams_failed;  // usbnet did not take them (no link, ring full)
    uint64_t ts_bytes;
    uint32_t ppa_us_max, enc_us_max, mux_us_max;
    uint64_t ppa_us_sum, enc_us_sum, mux_us_sum;
} stream_stats_t;

// Buffers, PPA client and encoder. `tmpl` is the app's framebuffer: the
// three stream framebuffers copy its size, format and orientation.
// `blit` shows a finished frame (may be NULL). Call before the link is up
// so errors still reach the console.
esp_err_t stream_prepare(stream_cfg_t const* cfg, pax_buf_t const* tmpl, void (*blit)(void const* pixels));
void      stream_start(void);
void      stream_stop(void);  // stops the tasks, frees everything
void      stream_get_stats(stream_stats_t* out);
