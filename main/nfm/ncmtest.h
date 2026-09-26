#pragma once
// =====================================================================
//  ncmtest  --  steps 1.3/1.4, 4.2 and T5a: the USB network link alone
// ---------------------------------------------------------------------
//  RUN ncm [mode=blast|idle] [secs=30] [len=1316] [rate=<kbit/s>] [poll=0|1] [run=<id>]
//
//    mode=idle   bring the link up and keep it up: enumeration, DHCP,
//                ping and UDP echo (port 7) are tested from the PC.
//    mode=blast  the same, plus UDP datagrams of `len` bytes to the PC's
//                port 5000, as fast as TinyUSB takes them (T5a, the USB
//                ceiling) or at `rate` kbit/s of payload.
//
//  Every datagram starts with a 24-byte header the PC checks
//  (tools/nfmrecv.py): "NFMB", run id, sequence number, length, and the
//  badge's microsecond clock; the rest is a pattern of the sequence
//  number. Once a second a PERIOD record (JSON) goes to the PC's port
//  5001 as well, so a run can be watched live.
//
//  The PC ends a run early with the datagram "STOP" to port 5002.
//
//  The console is gone while the link is up (F-07). Records are kept and
//  emitted when it is back: START before the switch; then PERIOD x n and
//  RESULT, repeated every 2 s until the host answers ACK (or 20 s pass).
//  The same records go to /sd/nfmtest/<run>.json.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#define NCMTEST_PORT_DATA  5000
#define NCMTEST_PORT_STATS 5001
#define NCMTEST_PORT_CTRL  5002

typedef struct {
    bool     blast;
    int      secs;
    uint16_t len;
    uint32_t rate_kbit;  // 0: as fast as possible
    bool     poll;       // usbnet task polls instead of sleeping (F-19)
    char     run[40];
} ncmtest_params_t;

// Parse the k=v words after "RUN ncm". False (with a message in `err`)
// on an unknown key or a bad value.
bool ncmtest_parse(char const* args, ncmtest_params_t* p, char* err, int err_len);

// A few lines for the screen, refreshed about twice a second while a run
// is going; `lines` holds `n` strings.
typedef void (*ncmtest_hud_t)(char const* const* lines, int n);

// Run it: takes the console away and gives it back. Blocks for the
// length of the run. `console` says whether a host is on the console to
// ACK the final records (a RUN from the host) or not (started from the
// keyboard: emit them once and return).
void ncmtest_run(ncmtest_params_t const* p, ncmtest_hud_t hud, bool console);
