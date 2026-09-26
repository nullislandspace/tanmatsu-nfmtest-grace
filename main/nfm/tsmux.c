// =====================================================================
//  tsmux  --  H.264 access units -> MPEG-TS (see tsmux.h)
// =====================================================================

#include "tsmux.h"
#include <string.h>

#define PID_PAT        0x0000
#define PROGRAM_NUMBER 1
#define STREAM_H264    0x1B

// An access unit delimiter: nal_unit_type 9, primary_pic_type 7 ("any"),
// then the rbsp stop bit.
static uint8_t const AUD[] = {0x00, 0x00, 0x00, 0x01, 0x09, 0xF0};

uint32_t tsmux_crc32(uint8_t const* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int b = 0; b < 8; b++) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

void tsmux_init(tsmux_t* m, tsmux_emit_t emit, void* ctx) {
    memset(m, 0, sizeof(*m));
    m->emit         = emit;
    m->ctx          = ctx;
    m->since_tables = TSMUX_TABLE_INTERVAL;  // tables go out with the first access unit
}

static void flush(tsmux_t* m) {
    if (m->fill == 0) return;
    m->dgrams++;
    m->bytes += m->fill;
    if (!m->emit(m->ctx, m->dgram, m->fill)) m->dgrams_failed++;
    m->fill = 0;
}

// A fresh 188-byte packet in the datagram, full datagrams sent first.
static uint8_t* next_packet(tsmux_t* m) {
    if (m->fill + TSMUX_PACKET > sizeof(m->dgram)) flush(m);
    uint8_t* p = m->dgram + m->fill;
    m->fill += TSMUX_PACKET;
    m->packets++;
    return p;
}

static void header(uint8_t* p, uint16_t pid, bool pusi, uint8_t afc, uint8_t cc) {
    p[0] = 0x47;
    p[1] = (uint8_t)((pusi ? 0x40 : 0) | ((pid >> 8) & 0x1F));
    p[2] = (uint8_t)pid;
    p[3] = (uint8_t)((afc << 4) | (cc & 0x0F));
}

// One PSI section in one packet (both tables are tiny): pointer field,
// section, 0xFF padding.
static void section_packet(tsmux_t* m, uint16_t pid, uint8_t* cc, uint8_t const* sec, size_t len) {
    uint8_t* p = next_packet(m);
    header(p, pid, true, 1, (*cc)++);
    p[4] = 0;  // pointer_field
    memcpy(p + 5, sec, len);
    memset(p + 5 + len, 0xFF, TSMUX_PACKET - 5 - len);
}

static void write_tables(tsmux_t* m) {
    // PAT: one program, its PMT on TSMUX_PID_PMT.
    uint8_t pat[16];
    size_t  n = 0;
    pat[n++]  = 0x00;  // table_id
    pat[n++]  = 0xB0;  // section_syntax_indicator, '0', reserved, length hi = 0
    pat[n++]  = 13;    // section_length: 5 header + 4 program + 4 CRC
    pat[n++]  = 0x00;
    pat[n++]  = 0x01;  // transport_stream_id 1
    pat[n++]  = 0xC1;  // version 0, current_next 1
    pat[n++]  = 0x00;  // section_number
    pat[n++]  = 0x00;  // last_section_number
    pat[n++]  = PROGRAM_NUMBER >> 8;
    pat[n++]  = PROGRAM_NUMBER & 0xFF;
    pat[n++]  = (uint8_t)(0xE0 | (TSMUX_PID_PMT >> 8));
    pat[n++]  = TSMUX_PID_PMT & 0xFF;
    uint32_t crc = tsmux_crc32(pat, n);
    pat[n++]     = (uint8_t)(crc >> 24);
    pat[n++]     = (uint8_t)(crc >> 16);
    pat[n++]     = (uint8_t)(crc >> 8);
    pat[n++]     = (uint8_t)crc;
    section_packet(m, PID_PAT, &m->cc_pat, pat, n);

    // PMT: PCR and H.264 on the video PID.
    uint8_t pmt[21];
    n        = 0;
    pmt[n++] = 0x02;  // table_id
    pmt[n++] = 0xB0;
    pmt[n++] = 18;  // section_length: 9 header + 5 stream + 4 CRC
    pmt[n++] = PROGRAM_NUMBER >> 8;
    pmt[n++] = PROGRAM_NUMBER & 0xFF;
    pmt[n++] = 0xC1;
    pmt[n++] = 0x00;
    pmt[n++] = 0x00;
    pmt[n++] = (uint8_t)(0xE0 | (TSMUX_PID_VIDEO >> 8));  // PCR_PID
    pmt[n++] = TSMUX_PID_VIDEO & 0xFF;
    pmt[n++] = 0xF0;  // program_info_length 0
    pmt[n++] = 0x00;
    pmt[n++] = STREAM_H264;
    pmt[n++] = (uint8_t)(0xE0 | (TSMUX_PID_VIDEO >> 8));
    pmt[n++] = TSMUX_PID_VIDEO & 0xFF;
    pmt[n++] = 0xF0;  // ES_info_length 0
    pmt[n++] = 0x00;
    crc      = tsmux_crc32(pmt, n);
    pmt[n++] = (uint8_t)(crc >> 24);
    pmt[n++] = (uint8_t)(crc >> 16);
    pmt[n++] = (uint8_t)(crc >> 8);
    pmt[n++] = (uint8_t)crc;
    section_packet(m, TSMUX_PID_PMT, &m->cc_pmt, pmt, n);
}

// A 33-bit PTS in the 5-byte PES form, with prefix '0010'.
static void put_pts(uint8_t* p, uint64_t pts) {
    pts &= 0x1FFFFFFFFull;
    p[0] = (uint8_t)(0x21 | ((pts >> 29) & 0x0E));
    p[1] = (uint8_t)(pts >> 22);
    p[2] = (uint8_t)(0x01 | ((pts >> 14) & 0xFE));
    p[3] = (uint8_t)(pts >> 7);
    p[4] = (uint8_t)(0x01 | ((pts << 1) & 0xFE));
}

// Copies payload from the PES header, then the AUD, then the access unit,
// as if they were one buffer.
typedef struct {
    uint8_t const* part[3];
    size_t         len[3];
    int            idx;
    size_t         off;
} src_t;

static size_t src_left(src_t const* s) {
    size_t n = 0;
    for (int i = s->idx; i < 3; i++) n += s->len[i] - (i == s->idx ? s->off : 0);
    return n;
}

static void src_copy(src_t* s, uint8_t* out, size_t n) {
    while (n > 0) {
        size_t const avail = s->len[s->idx] - s->off;
        size_t const k     = avail < n ? avail : n;
        memcpy(out, s->part[s->idx] + s->off, k);
        out += k;
        n -= k;
        s->off += k;
        if (s->off == s->len[s->idx]) {
            s->idx++;
            s->off = 0;
        }
    }
}

void tsmux_write(tsmux_t* m, uint8_t const* au, size_t len, uint64_t pts, bool keyframe) {
    if (keyframe || m->since_tables >= TSMUX_TABLE_INTERVAL) {
        write_tables(m);
        m->since_tables = 0;
    }
    m->since_tables++;
    m->access_units++;

    // PES header: video stream 0xE0, unbounded length (allowed for video
    // in TS), PTS only.
    uint8_t pes[14];
    pes[0] = 0x00;
    pes[1] = 0x00;
    pes[2] = 0x01;
    pes[3] = 0xE0;
    pes[4] = 0x00;
    pes[5] = 0x00;  // PES_packet_length 0: unbounded
    pes[6] = 0x80;  // '10', no scrambling, no priority, no alignment flag, not copyright, copy
    pes[7] = 0x80;  // PTS only
    pes[8] = 5;     // PES_header_data_length
    put_pts(pes + 9, pts);

    src_t src = {.part = {pes, AUD, au}, .len = {sizeof(pes), sizeof(AUD), len}};

    bool first = true;
    while (src_left(&src) > 0) {
        size_t const left = src_left(&src);
        uint8_t*     p    = next_packet(m);
        // Adaptation field: PCR (+ random access) in the first packet;
        // stuffing in the last one if the payload does not fill it.
        size_t af_len = 0;  // bytes after the adaptation_field_length byte
        bool   has_af = false;
        if (first) {
            has_af = true;
            af_len = 1 + 6;  // flags + PCR
        }
        size_t room = TSMUX_PACKET - 4 - (has_af ? 1 + af_len : 0);
        if (left < room) {
            // Pad with adaptation field stuffing so the payload ends the packet.
            size_t const pad = room - left;
            if (!has_af) {
                has_af = true;
                af_len = pad - 1;  // the length byte itself takes one
                if (pad == 1) af_len = 0;
            } else {
                af_len += pad;
            }
            room = left;
        }
        header(p, TSMUX_PID_VIDEO, first, has_af ? 3 : 1, m->cc_video++);
        size_t o = 4;
        if (has_af) {
            p[o++] = (uint8_t)af_len;
            if (af_len > 0) {
                size_t const af_start = o;
                uint8_t      flags    = 0;
                if (first) flags |= 0x10 | (keyframe ? 0x40 : 0);  // PCR_flag, random_access_indicator
                p[o++] = flags;
                if (first) {
                    uint64_t const base = (pts - TSMUX_PCR_LEAD) & 0x1FFFFFFFFull;
                    p[o++]              = (uint8_t)(base >> 25);
                    p[o++]              = (uint8_t)(base >> 17);
                    p[o++]              = (uint8_t)(base >> 9);
                    p[o++]              = (uint8_t)(base >> 1);
                    p[o++]              = (uint8_t)(((base & 1) << 7) | 0x7E);  // reserved 6 bits, extension hi
                    p[o++]              = 0x00;                                 // extension lo
                }
                memset(p + o, 0xFF, af_start + af_len - o);
                o = af_start + af_len;
            }
        }
        src_copy(&src, p + o, TSMUX_PACKET - o);
        first = false;
    }
    flush(m);  // the rest of this access unit goes now, not with the next one
}
