#pragma once
// =====================================================================
//  tsmux  --  H.264 access units -> MPEG-TS, 7 packets per UDP datagram
// ---------------------------------------------------------------------
//  Part C of claudeplans/nfmtest.md (D-02): what OBS's Media Source (an
//  ffmpeg input, udp://@:5000) plays without a server in between.
//
//    - PAT (PID 0) and PMT (PID 0x1000) before every keyframe, and at
//      least every TSMUX_TABLE_INTERVAL access units, so a receiver that
//      joins late finds the program quickly;
//    - H.264 on PID 0x100 (stream_type 0x1B), one PES per access unit,
//      with PTS (no B-frames, so DTS = PTS and is left out);
//    - the PCR on the video PID, in the first packet of every access
//      unit, TSMUX_PCR_LEAD before its PTS;
//    - an access unit delimiter in front of every access unit (the
//      encoder does not write one; ffmpeg's parser likes to have it);
//    - random_access_indicator on keyframes.
//
//  Packets are gathered into datagrams of TSMUX_DGRAM_PACKETS x 188 =
//  1316 bytes, the usual size, one Ethernet frame each. The last
//  datagram of an access unit goes out short rather than waiting for the
//  next frame: latency matters more than the few bytes.
//
//  Plain C, no ESP headers: tools/tscheck.c tests it on the PC and
//  tools/tsmux_file.c muxes a whole .h264 for ffprobe.
// =====================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TSMUX_PACKET          188
#define TSMUX_DGRAM_PACKETS   7
#define TSMUX_DGRAM_MAX       (TSMUX_PACKET * TSMUX_DGRAM_PACKETS)  // 1316
#define TSMUX_PID_PMT         0x1000
#define TSMUX_PID_VIDEO       0x0100
#define TSMUX_TABLE_INTERVAL  15      // access units between PAT/PMT at most
#define TSMUX_PCR_LEAD        9000    // 100 ms, in 90 kHz ticks

// Called for every finished datagram (1..7 packets); returns false if it
// could not be sent (counted, the muxer carries on).
typedef bool (*tsmux_emit_t)(void* ctx, uint8_t const* dgram, size_t len);

typedef struct {
    uint8_t      dgram[TSMUX_DGRAM_MAX];
    size_t       fill;
    uint8_t      cc_pat, cc_pmt, cc_video;  // continuity counters
    uint32_t     since_tables;
    tsmux_emit_t emit;
    void*        ctx;
    // statistics
    uint32_t     access_units;
    uint32_t     packets;
    uint32_t     dgrams;
    uint32_t     dgrams_failed;
    uint64_t     bytes;  // datagram bytes handed to emit
} tsmux_t;

void tsmux_init(tsmux_t* m, tsmux_emit_t emit, void* ctx);

// Mux one access unit: Annex-B NAL units (start codes included), as the
// encoder produced it. `pts` is in 90 kHz ticks. Ends with any partial
// datagram flushed.
void tsmux_write(tsmux_t* m, uint8_t const* au, size_t len, uint64_t pts, bool keyframe);

// The MPEG-2 CRC32 of PSI sections (poly 0x04C11DB7, not reflected).
uint32_t tsmux_crc32(uint8_t const* data, size_t len);
