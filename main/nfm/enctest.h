#pragma once
// =====================================================================
//  enctest  --  steps 1.1/1.2: RGB565 framebuffer -> PPA -> HW H.264
// ---------------------------------------------------------------------
//  RUN enc [frames=90] [fps=30] [br=<kbit/s>] [gop=<frames>] [qp=<min>,<max>]
//          [pat=static|bars|text|motion|noise] [rot=0|90|180|270]
//          [pace=0|1] [show=0|1] [save=0|1] [run=<id>]
//
//  Per frame:
//    draw   the pattern into the app's RGB565 framebuffer (480x800
//           portrait in memory, 800x480 landscape as drawn), as a game
//           renders into its own;
//    sync   write the frame back from the cache (the PPA reads memory);
//    ppa    PPA SRM: RGB565 -> YUV420 (O_UYY_E_VYY, BT.601 limited, as
//           the camera), rotated `rot` degrees counter-clockwise to
//           800x480 landscape (90 undoes pax's PAX_O_ROT_CW);
//    enc    esp_h264 hardware encoder, blocking;
//    write  the Annex-B bitstream appended to /sd/nfmtest/<run>.h264.
//
//  pace=1 (default) holds `fps`, as a stream would; pace=0 runs flat out
//  to see what the chain can do. show=1 also puts each frame on the
//  display (a copy the game would do anyway, timed separately).
//
//  Records: ENCBEGIN (parameters, SRAM ledger around the PPA client and
//  the encoder, F-04), one ENCPERIOD a second, RESULT (per-stage time
//  mean/p95/max in microseconds, I- and P-frame sizes, the bitrate it
//  came out at, the ledger again after teardown). Frame 0 is also saved
//  as /sd/nfmtest/<run>_f0.png, to compare with the decoded video (1.2).
//
//  The console stays up throughout: this test takes nothing away.
// =====================================================================

#include <stdbool.h>
#include "pax_gfx.h"

// Run the test described by the k=v words after "RUN enc". `fb` is the
// app's framebuffer: RGB565, 64-byte aligned, in PSRAM. `blit` shows it.
void enctest_run(char const* args, pax_buf_t* fb, void (*blit)(void));
