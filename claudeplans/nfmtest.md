# Implementation plan: nfmtest — H.264 screen streaming, measured

## Context

The question behind this app (2026-09-25): can the ESP32-P4's hardware H.264
encoder, which `tanmatsu-camera` already uses to record video, also encode
the **screen** of a running app, so a game can be recorded or streamed into OBS
on a Linux PC? And if so, what does it cost the game that is running at the same
time?

What was established before this plan was written (Part E has the evidence):

- The Tanmatsu's USB-C port is **full speed** (12 Mbit/s): one full-speed PHY
  shared between the USB-Serial-JTAG console and the full-speed OTG controller.
  The high-speed controller drives the USB *host* port (F-01).
- Only H.264 fits through full speed: MJPEG at 800×480 does not (F-01).
- Only Linux matters (the user, 2026-09-25). This makes a USB network adapter
  (CDC-NCM, kernel driver `cdc_ncm`) a clean transport. It is also the one that
  shares its whole upper half with a Wi-Fi stream (D-01, D-03).
- OBS receives MPEG-TS over UDP through its "Media Source" (ffmpeg input,
  `udp://@:5000`), with no server in between (D-02).

nfmtest is a **measuring instrument**, not a product. It streams a test picture
and records what streaming costs in **CPU time, PSRAM bandwidth and internal
SRAM**. These are the three budgets a game like SynthMiner is already short of.
Its results decide whether and how streaming goes into SynthEngine3D.

## About this document

This file is committed with the work. It holds:

- the design (Parts A, B, C, M, T, G);
- the step-by-step plan with a status table (Part D);
- the findings and decisions logs (Part E).

Numbering starts at **F-01 / D-01**; this is a new repository. Standing rules,
the same as in the other grace apps:

- **Engine, graceloader or launcher problems: stop and ask.** No workarounds.
  Graceloader changes needed by this app are listed in Part G and are
  **the user's call**; nothing there gets done without a go-ahead.
- Builds run in the **foreground**, never backgrounded with sleep-polling.
- **Commit and push only when the user asks.**
- Tight timeouts; no routine BadgeLink downloads.
- A decision still open is marked **(proposed)** in Part E until the user
  confirms it.

---

## Part A: what the app has to answer

| # | Question | Answered by |
|---|---|---|
| Q1 | Does the chain framebuffer → PPA → HW H.264 → MPEG-TS → USB-NCM → OBS work, from a graceloader app? | T5, T10 |
| Q2 | CPU time per stage (capture, encode, mux, send) and as a share of each core, at 15 and 30 fps | T2-T5, T8 |
| Q3 | PSRAM traffic added by streaming, and how much a game loses to it | T1-T5, T8 |
| Q4 | Internal SRAM taken, stage by stage: static, peak, largest block left | every test's ledger (M3) |
| Q5 | What the USB-C port carries in practice (Mbit/s, loss), and what bitrate that leaves | T5a |
| Q6 | What the same stream costs over lwIP-over-NCM and over Wi-Fi, compared with raw NCM | T6, T7 |
| Q7 | Picture quality at each bitrate for game-like content (text, flat areas, fast motion) | T2 captures, judged on the PC |
| Q8 | Does it survive 10 minutes without leaks, stalls or growing latency? | T9 |

**Exit criteria.** Every Q has a number or a clear no, recorded as findings.
Step 7.1 then turns them into a recommendation for SynthEngine3D.

---

## Part B: the pipeline

```
 core 0 (app/render)          core 1 (stream)                        PC (Linux)
 ───────────────────          ───────────────                        ──────────
 draw frame N into fb[k]
 present: msync C2M, select ─► capture: PPA SRM  fb[k] (RGB565 480x800 portrait)
                                 rotate 90°, RGB565 → YUV420 (O_UYY_E_VYY)
                                 into yuv[j] (800x480, 576 000 B)
                               encode: esp_h264 HW, yuv[j] → NAL units
                               mux: MPEG-TS, 7 × 188 B per datagram
                               tx:  null | sd | ncm_raw | ncm_lwip | wifi ──► OBS Media Source
                               stats: 1 JSON line/s on UDP :5001        ──► tools/nfmrecv.py
```

**Framebuffers.** These are the display driver's three framebuffers, flipped
the way SynthEngine3D 2.2 does it (`se_run.c`: select for the next refresh, no
copy). This needs graceloader 2.6.0 (`graceloader_display_register_callbacks`,
`esp_lcd_dpi_panel_get_frame_buffer`, both exported). The test measures the
conditions a real game runs under, not a friendlier copy of them (D-07).

**Capture point.** Right after the present selects buffer k:

- That buffer is complete, and its cache has been written back (the flip does
  `esp_cache_msync` C2M), so the PPA reads PSRAM directly and sees the frame.
- With three buffers, the renderer comes back to k two presents later, so the
  capture has until then.
- A capture that is not done by then is a **dropped stream frame**, counted and
  never waited for. The game must not slow down because of the stream.

**Stream frame rate.** A fixed rate (15 or 30 fps), independent of the game's
rate:

- The capture takes the most recently presented frame at each tick of its own
  clock.
- A game slower than the stream rate yields repeated frames, which the encoder
  turns into near-empty P-frames.
- A faster game has frames skipped.

**Buffers (all PSRAM unless noted):**
- 2 × YUV420 (576 000 B each), so capture of N+1 overlaps encode of N.
- The encoder's own reference and reconstruction frames.
- The bitstream output buffer (the encoder insists it be at least as large as
  the input, `video.c:106`: 576 000 B).
- A TS ring of ~64 KB.
- The internal allocations the encoder insists on are measured (F-04).

**Tasks (proposed pinning, D-09):**
- `nfm_cap` and `nfm_enc` on core 1, just below where SynthMiner's chunk worker
  sits (`configMAX_PRIORITIES - 6`), so the measurement matches that neighbour.
- TinyUSB's device task on core 1.
- The render loop on core 0.
- Soak tasks (M1) at priority 1 on both cores.

**Parameters** (all on the `RUN` line, Part T):

| Key | Values | Default |
|---|---|---|
| `tx` | `null`, `sd`, `ncm_raw`, `ncm_lwip`, `wifi` | `null` |
| `fps` | 15, 30 | 30 |
| `br` | kbit/s | 3000 |
| `gop` | frames | = fps (one keyframe per second) |
| `qp` | min,max | encoder default |
| `pat` | `static`, `bars`, `text`, `motion`, `noise` | `motion` |
| `load` | `L0`, `L1`, `L2:<MB/frame>` | `L1` |
| `secs` | run length | 30 |
| `host` | PC address (Wi-Fi) | none |

**Test patterns** are pure functions of the frame number, so two runs produce
the same video and the same bitrate:

- `static`: a still frame; the encoder's floor.
- `bars`: colour bars with a moving line.
- `text`: dense HUD-like text and fine lines, which stress chroma subsampling and
  quantisation.
- `motion`: scrolling textured planes, the typical game case.
- `noise`: every macroblock changes; the worst case and a bitrate stress test.

Each pattern carries a frame counter and a millisecond clock as big digits.
They let a PC check for dropped frames (M4), and filmed side by side with OBS
they give a glass-to-glass latency.

**Load levels** (what "the game" costs):
- `L0`: the app presents a static frame. This measures streaming alone.
- `L1`: animated pattern only. Cheap for the CPU; streaming as seen by a light app.
- `L2:<MB>`: a synthetic renderer. Each frame, the CPU writes the whole
  framebuffer and reads `<MB>` from a PSRAM texture pool in a scattered order,
  as a rasteriser does. Its **frame time with streaming off versus on** is the
  practical cost to a game (T8). `L2:4` is about SynthMiner's per-frame PSRAM
  traffic; step 3.7 checks that against the engine's own figures.

---

## Part C: transports

One interface, so every transport shares the pipeline above it:

```c
typedef struct {
    char const* name;
    esp_err_t (*open)(nfm_tx_cfg_t const* cfg);
    esp_err_t (*send)(uint8_t const* dgram, size_t len);  /* one UDP payload, <= 1316 B */
    void      (*poll)(void);                               /* RX side: ARP, ICMP, DHCP */
    void      (*close)(void);
    void      (*stats)(nfm_tx_stats_t* out);               /* sent, dropped, busy_us */
} nfm_tx_t;
```

| Transport | Needs | What it isolates |
|---|---|---|
| `null` | nothing | Capture + encode + mux, with no I/O |
| `sd` | nothing | Writes `/sd/nfmtest/<run>.ts`: the stream checked offline with ffprobe/ffplay before any network exists |
| `ncm_raw` | TinyUSB in the app | USB cost alone: Ethernet/IPv4/UDP headers built by hand, no lwIP |
| `ncm_lwip` | graceloader exports (G1) | The real network stack over USB, the same code path as Wi-Fi |
| `wifi` | graceloader exports + Wi-Fi (G1, G2) | The radio: ESP32-C6 over SDIO, via esp-hosted |

**The NCM device** (steps 1.3, 4.1):

- **PHY switch.** Taking over the USB-C port is the same dance as the launcher's
  and Renze's MSC app: pull-down override, `usb_serial_jtag_ll_phy_select(1)`,
  pull-up. Returning is the reverse with `select(0)`.
- **The console is gone for the run** (F-07); Part T is built around that.
- **Descriptors.**
  - Vendor/product ID: MCH2022 badge VID 0x16D0 as the launcher uses. The PID is
    a **(proposed)** choice, D-10.
  - `iMACAddress` is derived from the chip MAC with the locally-administered bit
    set, so the PC's interface name (`enx…`) is stable per badge.
- **Addressing in `ncm_raw` (no DHCP):**
  - The badge is `192.168.77.2`.
  - The PC is `192.168.77.1/24`, from a NetworkManager profile keyed to that MAC,
    set up once by `tools/setup_host_ncm.sh`. After that, no sudo is needed per run.
  - The badge answers ARP and ICMP echo, so `ping` works as a first check.
  - It sends to the PC's MAC, which it learns from the PC's first frame.
    Until then it sends to broadcast `192.168.77.255`; `udp://@:5000` receives both.
- **Wire format.**
  - MPEG-TS: PAT, PMT, H.264 on PID 0x100 (stream_type 0x1B), with the PCR on the
    video PID.
  - Each access unit gets a PES packet with PTS, prefixed with AUD, and SPS/PPS
    before every IDR.
  - 7 TS packets make one 1316-byte UDP payload, the usual size, which fits one
    1500-byte Ethernet frame.
- **Stats.** One JSON line per second to UDP port 5001, with the same fields as the
  device records (Part M), so a run can be watched live on the PC.

**Wi-Fi** (prepared, not built before G1/G2):
- It uses the launcher's stored credentials (the launcher's wifi-manager
  `wifi_connect_try_all()` pattern).
- The target is the `host=` parameter. UDP is the default; TCP is a variant for
  lossy air (`tx=wifi proto=tcp`).
- Everything above `send()` is shared, so the only new code is the connect/teardown
  and the socket.

---

## Part M: how the three budgets are measured

Each method below says what it sees and what it is blind to. Where two methods
overlap, they have to agree before a number becomes a finding.

### M1 — CPU

1. **Stage timers.** Cycle counter (`rdcycle`) around each stage of each stream
   frame: capture submit, PPA wait, encode submit, encoder wait, mux, send. Wait
   time is kept apart from busy time: a task blocked on the PPA costs nothing.
   Reported per stage as mean, p95 and max, in µs.
2. **Soak tasks: CPU share per core.**
   - One task per core at priority 1 spins on a counter and gives the core up for
     one tick every 10 ms. That leaves the IDLE task and its watchdog alone, and
     costs the same 10% of spin time in calibration and in the run, so it cancels.
   - A 3 s calibration with everything stopped gives counts/s at 0% load.
   - During a run, `load = 1 - rate/baseline`.
   - This needs no graceloader change. It sees everything: ISRs, TinyUSB's task,
     lwIP, the radio's tasks.
3. **FreeRTOS run-time stats (G3, D-14): the per-task breakdown.**
   - `uxTaskGetSystemState()` snapshots every task's run-time counter at the
     start and end of each `PERIOD`. The differences give each task's share of
     its core: ours, TinyUSB's, `tiT` (lwIP), esp-hosted's, the PPA's.
   - The idle tasks' shares (`xTaskGetIdleTaskHandleForCore` +
     `ulTaskGetRunTimeCounter`) give per-core load **without** the soak tasks. The
     two methods check each other.
   - Interrupt time is charged to whichever task was interrupted. So per-task
     figures can blame a task for an ISR's cost, and the soak method stays the
     reference for totals.
   - Until the graceloader change (6.1) lands, this path compiles out
     (`NFM_HAVE_RUNTIME_STATS`) and M1.2 alone answers Q2.

### M2 — PSRAM bandwidth

1. **The byte budget (analytic)** is the lower bound, per stream frame at
   800×480. The encoder's reference traffic is the unknown; M2.2/M2.3 measure it.

   | Stage | Reads | Writes |
   |---|---|---|
   | PPA capture (RGB565 in, YUV420 out) | 768 000 | 576 000 |
   | Encoder input | 576 000 | — |
   | Encoder reference / reconstruction | ≥ 576 000 | 576 000 |
   | Bitstream + TS + USB copies (3 Mbit/s) | ~40 000 | ~40 000 |
   | **Total** | **≥ 1.96 MB** | **≥ 1.19 MB** |

   That is **≥ 3.1 MB per frame: ~95 MB/s at 30 fps, ~47 MB/s at 15 fps**. For
   scale, SynthMiner's page flip removed a 768 KB copy per frame and that
   mattered (G6 in its plan).
2. **L2 cache counters (CPU side only).**
   - The P4's L2 cache counts its next-level reads and writes per bus:
     `CACHE_L2_DBUS0..3_ACS_NXTLVL_RD_CNT_REG` / `…_WR_CNT_REG`, plus hit, miss
     and conflict counters, enabled via `CACHE_L2_CACHE_ACS_CNT_CTRL_REG`
     (`soc/esp32p4/register/hw_ver1/soc/cache_reg.h`, F-06).
   - This is the CPU's own PSRAM traffic (renderer, mux, TinyUSB copies).
   - **DMA bypasses the cache**, so the PPA, the encoder and USB DMA are invisible
     here.
   - Step 3.3 settles units (lines vs bytes), width and wrap by reading a known
     buffer.
3. **Remaining-bandwidth probes (everything, indirectly).** Two probes, each run
   for 2 s at the start (idle) and in the middle of a run, and repeated in bursts
   so they do not dominate the run:
   - a **DMA probe**: AXI GDMA `esp_async_memcpy`, 1 MB PSRAM → PSRAM in a loop,
     MB/s;
   - a **CPU probe**: a 4 MB `memcpy` striding past the 128 KB L2, MB/s.

   The drop from idle to during-run is the bandwidth streaming occupies,
   DMA engines included. A probe perturbs what it measures, so it is **off**
   during the M1 and M4 figures of the same run.
4. **The practical number (T8).** The `L2` synthetic renderer's frame time with
   streaming off and on. This is what a game feels, and the number Step 7.1
   leans on.

### M3 — internal SRAM

- A **ledger**: `heap_caps_get_free_size`,
  `heap_caps_get_largest_free_block` and `heap_caps_get_minimum_free_size` for
  `MALLOC_CAP_INTERNAL` and for `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`,
  snapshotted:
  - at app start;
  - after the display and PPA client;
  - after encoder open;
  - after TinyUSB and PHY;
  - after netif and socket (lwIP transports);
  - after Wi-Fi connect;
  - at the end, after teardown, where everything must be back (a leak otherwise).
- Every snapshot goes into the result, and the deltas are the answer to Q4.
- The **largest free block** is reported beside the free total. A game's SRAM
  failures have been about fragmentation (SynthMiner's band tiles: 37-38 KB
  largest block), not the total.
- **Stack high-water marks** for the stream tasks need
  `uxTaskGetStackHighWaterMark`, which graceloader does not export (G1). Until
  it does, stacks are sized generously and the ledger shows their cost.
- The encoder's internal allocations (F-04) are measured **as shipped** and,
  in step 5.4, with the deblocking buffer pushed to PSRAM. PSRAM is the only
  option once a game holds the SRAM.

### M4 — stream health (PC side, `tools/nfmrecv.py`)

- Datagrams and bytes per second, plus TS continuity-counter errors (= loss) per
  PID.
- Frames received versus the badge's frame counter, from the PTS sequence;
  duplicated and dropped frames are counted separately.
- The capture is saved as `capture.ts`. `ffprobe` then gives:
  - the resolution;
  - the frame count;
  - decoder errors, which must be 0.
- The badge's JSON stats lines are logged next to the PC-side numbers, joined on
  the frame counter.
- **Optional:** one-way latency.
  - The badge stamps each frame's encode-done time into the stats.
  - The PC estimates the clock offset from ICMP round trips, halved.
  - Filming the badge next to OBS gives glass-to-glass latency as a manual check.

---

## Part T: tests and the harness

### The problem the harness has to solve

The testkit (`main/testkit/`) talks over the USB-Serial-JTAG console. The NCM
transports **take that PHY away** for the length of the run (F-07). So:

1. The PC finds the app (`READY`) and sends `RUN nfm tx=ncm_raw secs=30 …`.
2. The app answers with a `START` record carrying the run id and the parameters.
   It drains the console and switches the PHY to OTG. `/dev/ttyACM0` disappears
   and `enx…` appears.
3. `tools/nfmrecv.py` does the following:
   - waits for the interface, matched by the MAC in the `START` record;
   - brings it up from the NetworkManager profile;
   - receives, measuring M4;
   - optionally opens `ffplay` so the run can be watched.
4. At `secs` the app stops the stream, tears down TinyUSB and switches the PHY
   back. It then **repeats its final records** (`NFM-RESULT`, `NFM-LEDGER`) every
   2 s for 20 s, or until the PC answers `ACK`, and returns to the launcher.
5. `testrun.py` waits for `/dev/ttyACM0` to come back, collects the final records
   and runs ffprobe. It merges the badge side and the PC side into
   `results/<UTC>-nfm-<tx>-<pat>/result.json`.
6. The badge also writes the same records to `/sd/nfmtest/<run>.json`. If step 5
   loses the console, the run is not lost.

For the `null`, `sd` and `wifi` transports the console stays, and steps 2 and 4
reduce to "run, then report". A crash in OTG mode reboots the badge, and the ROM
brings USJ back, so `make recover` works as it does for any app.

### The tests

Each test is a `RUN` line. `make cycle TEST="…"` builds, installs, runs and
collects as the template's testkit does.

| Test | What | Transport | Answers |
|---|---|---|---|
| **T0** | Calibration: soak baselines, both PSRAM probes idle, SRAM ledger at rest | — | the baselines everything is compared to |
| **T1** | Capture only: PPA RGB565→YUV420 with rotation, at 15/30 fps; the YUV discarded | — | Q2, Q3 for the PPA alone |
| **T2** | Capture + encode, `null` transport, fps × br {1000, 2000, 3000, 4000} × pat {static, text, motion, noise} | `null` | Q2, Q3, Q4 for the encoder; bitrate adherence per pattern; each cell also saved via `sd` for Q7 |
| **T3** | T2 + mux | `null` | Mux cost (expected small) |
| **T4** | Full stream to SD, played back on the PC | `sd` | Stream validity before the network exists |
| **T5a** | **USB ceiling**: raw UDP datagrams as fast as TinyUSB takes them, no encoder | `ncm_raw` | Q5: usable Mbit/s and CPU per Mbit |
| **T5** | Full stream over raw NCM, fps × br × pat as in T2 | `ncm_raw` | Q1, Q2, Q3, Q4 end to end |
| **T6** | T5 over lwIP (after G1) | `ncm_lwip` | Q6: lwIP's cost against raw |
| **T7** | T5 over Wi-Fi, UDP and TCP (after G1, G2) | `wifi` | Q6: radio cost, loss, SRAM |
| **T8** | `load=L2:{2,4,6}`, streaming off / 15 fps / 30 fps, for each working transport | best of T5/T6/T7 | Q3 in game terms: frame-time increase |
| **T9** | 10 minutes, `pat=motion`, 30 fps, 3000 kbit/s | best transport | Q8: ledger end equals start, min free stable, no drift in drops or latency |
| **T10** | OBS acceptance (manual): Media Source `udp://@:5000`, the frame counter must advance smoothly; latency read from a phone video | `ncm_raw` | Q1 in the real consumer |

### Records

The records use the testkit format (`@@NFM-<KIND>@@ <json> @@<crc32>@@`,
`REPORT_PREFIX="NFM"`):

- `START`: run id, parameters, MAC.
- `PERIOD`, once a second:
  - stage µs (mean/p95/max);
  - soak load per core;
  - cache next-level reads and writes;
  - frames captured, encoded, sent and dropped;
  - bytes;
  - encoder QP (if exposed).
- `PROBE`: the idle and during-run MB/s of both probes.
- `LEDGER`: every SRAM snapshot.
- `RESULT`: the verdict of the run.
  - **Bad:** decoder errors, drops above 1%, a ledger leak, or a stall over 1 s.
  - **Ok:** otherwise.

---

## Part G: what graceloader has and what it lacks

Checked against the template's `fakelib/liball.so` (graceloader 2.6.0) on
2026-09-25.

**Exported already, all of step 1-5 needs:**
- PPA: `ppa_register_client`, `ppa_do_scale_rotate_mirror`.
- `esp_intr_alloc` / `esp_intr_free`.
- `esp_cache_msync`.
- `usb_new_phy` / `usb_del_phy`.
- `esp_async_memcpy` (+ `_install_gdma_axi`).
- The heap statistics functions.
- `esp_timer_get_time`.
- `esp_lcd_dpi_panel_get_frame_buffer` and `graceloader_display_register_callbacks`.
- NVS.

The USB-Serial-JTAG PHY switch is header-only (`hal/usb_serial_jtag_ll.h`).

**Missing:**

| Id | What | Needed by | Note |
|---|---|---|---|
| **G1** | Sockets (`lwip_socket`, `lwip_bind`, `lwip_sendto`, `lwip_recvfrom`, `lwip_close`, `lwip_setsockopt`, `lwip_fcntl`), `esp_netif_init`, `esp_netif_new`, `esp_netif_attach`, `esp_netif_receive`, `esp_netif_set_ip_info`, `esp_netif_dhcps_start/option`, `esp_netif_get_ip_info`, `uxTaskGetStackHighWaterMark` | `ncm_lwip`, `wifi`, M3 stacks | lwIP is linked into graceloader already (for esp-hosted), only a few `lwip_*`/`esp_netif_*` symbols are exported |
| **G2** | Wi-Fi bring-up for apps: `wifi_remote_initialize`, `wifi_remote_get_initialized` (tanmatsu-wifi, linked in, not exported) and what `wifi-manager` calls: `esp_netif_init`, `esp_netif_create_default_wifi_sta`, `esp_event_loop_create_default`, `esp_event_handler_register`, `esp_wifi_init`, `esp_wifi_set_mode`, `esp_wifi_set_config`, `esp_wifi_start`, `esp_wifi_connect`, `esp_wifi_disconnect`, `esp_wifi_sta_enterprise_enable/disable` | `wifi` | Graceloader depends on `tanmatsu-wifi` but not on `wifi-manager`. Rather than add wifi-manager to the loader, the **app compiles wifi-manager's two files** (`wifi_connection.c`, `wifi_settings.c`, 541 lines) against these exports. The loader stays small, and the credentials are the launcher's (NVS, already exported). F-11 |
| **G3** | Config: `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y`, `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` (counter clock: esp_timer, the default). Exports: `uxTaskGetSystemState`, `ulTaskGetRunTimeCounter`, `ulTaskGetRunTimePercent`, `ulTaskGetIdleRunTimeCounter`, `xTaskGetIdleTaskHandleForCore`, `uxTaskGetNumberOfTasks` | M1.3 | Included (D-14). **Changes the size of FreeRTOS's static structs** (F-13): every app must be built against the synced template before it uses `x…CreateStatic`. None of the user's apps does, and they are the only graceloader apps. Cost: one timer read per context switch, a few bytes per TCB/queue. |

**How they land (the user, 2026-09-25, D-13):**
1. Add all the exports to graceloader.
2. `make sync-template`.
3. Commit graceloader and the template.
4. Pull the template into the apps that need it, this one first.

Graceloader is **not pushed for this**: GitHub already holds many changes since
the last release, and the release is cut later. Until then this app builds
against the local template. Old apps must keep running on the new loader. No
check is needed the other way round, because new apps never run on an old loader.

**TinyUSB and the H.264 encoder are compiled into the app**, not into
graceloader:
- Both are source code (F-02, F-08).
- Both allocate non-IRAM interrupts (F-03), which is legal for code in PSRAM: the
  interrupt is masked while the cache is off, and the encoder waits with a timeout.

If this ever ships in a game, moving them into graceloader is a separate
decision (D-11).

---

## Part L: source layout (proposed)

```
main/main.c                  app shell: display, input, testkit hook, RUN dispatch
main/nfm/stream.c|h          orchestration: tasks, clock, capture point, drop policy
main/nfm/pattern.c|h         test patterns, pure f(frame)
main/nfm/load.c|h            L0 / L1 / L2 synthetic renderer
main/nfm/present.c|h         three driver framebuffers, page flip (after se_run.c)
main/nfm/capture.c|h         PPA SRM RGB565 → YUV420 + rotation
main/nfm/encoder.c|h         esp_h264 HW wrapper
main/nfm/tsmux.c|h           MPEG-TS muxer (host-buildable, no ESP headers)
main/nfm/tx.h                transport interface (Part C)
main/nfm/tx_null.c tx_sd.c tx_ncm_raw.c tx_ncm_lwip.c tx_wifi.c
main/nfm/usbnet.c|h          TinyUSB NCM device, PHY switch there and back
main/nfm/netraw.c|h          Ethernet/ARP/IPv4/ICMP/UDP/DHCP for ncm_raw (host-buildable)
main/nfm/ncmtest.c|h         RUN ncm: the link alone, UDP blast (steps 1.3/1.4, 4.2, T5a)
main/measure/stagetime.c|h   M1.1
main/measure/cpuload.c|h     M1.2 soak tasks
main/measure/cachecnt.c|h    M2.2 L2 counters
main/measure/psramprobe.c|h  M2.3 probes
main/measure/ledger.c|h      M3
main/testkit/                from the template (NFM prefix, TESTKIT_NO_ENGINE)
components/esp_h264/         vendored subset: interface/, hw/, port/ (no sw/ libs), provenance + version
components/tinyusb/          vendored subset: src/ core + class/net + portable/synopsys/dwc2, provenance + version
components/tusb_config.h     hand-written (F-08)
tools/nfmrecv.py             PC receiver, M4
tools/nfmrun.py              hands-free round trip: RUN -> network side -> console back -> ACK
tools/tscheck.c              host test for tsmux.c + netraw.c
tools/setup_host_ncm.sh      NetworkManager profile for the badge's MAC
claudeplans/nfmtest.md       this file
```

---

## Part D: step-by-step plan with status tracking

**Status values:** `todo` / `in progress` / `done` / `blocked (why)` /
`skipped (why)`. The Notes column records the result, with links to findings
(F-n) and decisions (D-n).

| # | Step | Status | Notes |
|---|---|---|---|
| **0** | **Setup** | | |
| 0.1 | Clone `tanmatsu-template-grace` as `tanmatsu-nfmtest-grace`; `upstream` = template, `origin` = `nullislandspace/tanmatsu-nfmtest-grace` | done | 2026-09-25, at template `0c62ac4` (graceloader 2.6.0 symbols). The GitHub repo exists and is empty; nothing pushed yet. |
| 0.2 | This plan | done | 2026-09-25 |
| 0.3 | Identity: `APP_SLUG_NAME ?= at.cavac.nfmtest`, `metadata.json` (name "NFM Test", category tools), README intro; testkit enabled with `TESTKIT_NO_ENGINE`, `REPORT_PREFIX="NFM"`, `SCREENSHOT_DIR="/sd/nfmtest"`; build, install, run, `make cycle TEST="perf secs=5"` passes | in progress | 2026-09-25: slug, metadata, README, `REPORT_PREFIX="NFM"`, build id (`app_version.h`) done; only the testkit's console half (`debugcon`, `report`) is built in, since nfmtest runs its own test (`RUN ncm`) rather than perf/shots. Builds, `make verify` clean. Not yet run on hardware. |
| 0.4 | First push to `origin` | done | The plan commit was on `origin` already; the USB-NCM work was pushed 2026-09-25 when the user asked. |
| **1** | **Spikes: can each piece live in a grace app at all?** | | Small and throwaway, one question each; results become findings. |
| 1.0 | **Next: first hardware test** of 1.3/1.4/4.2/4.3 (the user, evening of 2026-09-25): press `I` on the badge (link only): `enx…` appears in dmesg, gets 192.168.77.1, `ping 192.168.77.2` works. Enter (blast) + `tools/nfmrecv.py --ping --echo 50`. `/dev/ttyACM0` comes back afterwards. A second run in the same session works. Then `make nfmcycle`. | todo | Everything touching USB hardware is written blind; see the README for what to check if it fails. |
| 1.1 | Vendor the esp_h264 HW encoder (interface/, hw/, port/) from the version the camera uses, build it `-fPIC` into app.so; open an 800×480 encoder, encode one flat frame, close | todo | Check that it links (only source, F-02) and that the ISR fires from PSRAM code (F-03). **Ledger:** SRAM before/after open, per allocation (F-04). Check whether the H.264 DMA collides with the PPA's 2D-DMA channels. |
| 1.2 | PPA SRM: the raw 480×800 RGB565 framebuffer → 800×480 YUV420 with 90° rotation (or 270°, whichever makes it upright), 1:1 scale; encode that; write `.h264` to SD; decode on the PC and compare with a PNG screenshot of the same frame | todo | Also check colour range and standard (BT.601 limited, as the camera does). The even-scale mask (F-10) is irrelevant at 1:1; confirm. |
| 1.3 | Vendor TinyUSB (core, class/net NCM, dwc2 port); hand-written `tusb_config.h`; PHY switch to OTG; enumerate as NCM; `ip link` on the PC shows `enx…` with the expected MAC | in progress (built, untested on hardware) | 2026-09-25, written blind: TinyUSB 0.21.0 from the launcher (`components/tinyusb/PROVENANCE.md`), `components/tusb_config.h`, `main/nfm/usbnet.c`. Links against graceloader 2.6.0 with nothing missing. F-14..F-17. | **Decision D-12:** DWC2 slave (FIFO) mode first, because the app's static buffers are in PSRAM (F-09). Try DMA mode with `heap_caps` internal buffers only if slave-mode CPU cost (T5a) is too high. |
| 1.4 | The way back: stop TinyUSB, free the PHY, `usb_serial_jtag_ll_phy_select(0)`, pull-up; the console answers `PING` again; the app exits to the launcher cleanly | in progress (built, untested on hardware) | `usbnet_stop()`: `tud_disconnect`, `tusb_deinit`, `usb_del_phy`, PHY back. Also to check: a second run in the same app session (keyboard) re-inits TinyUSB cleanly. | Also: what does a crash in OTG mode leave behind? It should be a reboot with USJ back. Confirm, and note how `make recover` behaves. |
| **2** | **Pipeline, `null` and `sd` transports** | | |
| 2.1 | `present.c`: three driver framebuffers, flip as in `se_run.c`; `pattern.c` with all five patterns; `load.c` with L0/L1 | todo | Patterns are pure f(frame) (host check: same frame, same hash). |
| 2.2 | `capture.c`: PPA non-blocking into `yuv[2]`, the capture point after the present's select; drop instead of wait (Part B) | todo | |
| 2.3 | `encoder.c`: esp_h264 HW, parameters from the RUN line; output ring; frame types recorded | todo | |
| 2.4 | `tsmux.c` + `tools/tscheck.c`: mux a known Annex-B `.h264` on the PC, the result passes `ffprobe -v error` with 0 errors, correct PTS spacing and continuity counters | todo | Host-built, no ESP headers. |
| 2.5 | `tx_null`, `tx_sd`; `stream.c` ties 2.1-2.4 together; T4 passes (a .ts from the badge plays in ffplay/mpv with the frame counter advancing) | todo | |
| **3** | **Measurement** | | |
| 3.1 | `stagetime`: rdcycle around every stage, mean/p95/max per second | todo | |
| 3.2 | `cpuload`: soak tasks, calibration, load per core; check it reads ~0% on an idle app and ~50% with a known busy-wait of half a core | todo | The known load is the method's own test. |
| 3.3 | `cachecnt`: enable and read the L2 next-level counters; calibrate by reading and writing a 4 MB PSRAM buffer of known size and checking the counts match | todo | Settles units and wrap (F-06). |
| 3.4 | `psramprobe`: DMA and CPU probes; repeatability (5 idle runs within ±3%) | todo | |
| 3.5 | `ledger`: the snapshots from M3, as records and in `/sd/nfmtest/<run>.json` | todo | |
| 3.6 | Records (`START`/`PERIOD`/`PROBE`/`LEDGER`/`RESULT`), the stats line on UDP 5001, an on-screen HUD (fps, drops, Mbit/s, load per core, SRAM free/largest) | todo | |
| 3.7 | `load.c` L2 synthetic renderer; check `L2:4` against SynthMiner's measured per-frame PSRAM traffic (its `PERF` records) and adjust the default | todo | |
| 3.8 | Run **T0, T1, T2, T3**; findings | todo | The first real numbers: encoder cost with no USB in the picture. |
| **4** | **Raw NCM** | | |
| 4.1 | `netraw.c`: Ethernet, ARP reply, IPv4, ICMP echo, UDP send, learn the PC's MAC; host-tested in `tscheck` with captured frames | done | 2026-09-25. Plus a one-lease DHCP server and UDP echo on port 7 (D-15). `make tscheck` passes (ASan/UBSan), and `tscheck --pcap` output decodes cleanly in tcpdump (ARP, ICMP, DHCP OFFER/ACK/NAK, UDP): a check against an independent parser, not only against our own reading of the RFCs. |
| 4.2 | `tx_ncm_raw` over `usbnet`; `tools/setup_host_ncm.sh`; `ping 192.168.77.2` works | in progress (built, untested on hardware) | `usbnet_send_udp()` + ring for now; the `nfm_tx_t` wrapper comes with the stream (2.5). `setup_host_ncm.sh` is only the fallback now (D-15). |
| 4.3 | **T5a**, the USB ceiling | in progress (ready to run) | Q5; decides the bitrate range for T5. `RUN ncm mode=blast [rate=<kbit/s>] [len=…]`, `main/nfm/ncmtest.c`. CPU for now is the usbnet task's own busy time only (no ISR time); the soak method (3.2) comes later. |
| 4.4 | `tools/nfmrecv.py` (M4) and the harness round trip of Part T (START → PHY away → receive → PHY back → final records → merge) in `testrun.py`; `make cycle TEST="nfm tx=ncm_raw …"` is hands-free | in progress | A separate `tools/nfmrun.py` (importing `testrun.py`'s console helpers) rather than more branches in `testrun.py`; `make nfmcycle NFM="…"`. The receiver was checked against a simulated sender (loss, a duplicate and a corrupted datagram were all counted), and the whole console round trip against a fake badge on a pty. Both were dry runs on the dev server, with no USB. |
| 4.5 | **T5** matrix, **T10** in OBS | todo | Q1 answered here. |
| **5** | **What streaming costs: the analysis runs** | | |
| 5.1 | **T8** with `ncm_raw`: L2 frame time, streaming off / 15 / 30 fps | todo | |
| 5.2 | **T9** endurance | todo | |
| 5.3 | Cross-check M2: analytic bytes versus probe drop versus cache counters; explain any gap before recording a PSRAM finding | todo | |
| 5.4 | Encoder SRAM variant: its deblocking buffer and NAL buffer forced to PSRAM (a one-line change in the vendored copy, recorded in its provenance); SRAM saved versus encode time lost | todo | F-04 |
| 5.5 | Bitrate × pattern quality review on the PC (Q7): stills from each T2 capture side by side with the source PNG | todo | The user judges. |
| **6** | **lwIP and Wi-Fi** (needs G1/G2) | | |
| 6.1 | Graceloader: the G1 + G2 exports and G3 (config + exports); grep the user's grace apps again for static FreeRTOS objects (F-13); check that SynthMiner and one plain app still load and run; `make sync-template` (`--check` OK); commit graceloader and template, **no graceloader push** (D-13); pull the template into this app, then turn on `NFM_HAVE_RUNTIME_STATS` and check M1.3 against M1.2 on T2 | todo (approved) | The user's go-ahead 2026-09-25, G3 included the same day (D-14). Independent of steps 1-5, so it can happen any time before 6.2. |
| 6.2 | `tx_ncm_lwip`: esp_netif with a custom driver on top of `usbnet`; DHCP server so the PC needs no profile; **T6** | todo | |
| 6.3 | `tx_wifi`: vendor wifi-manager's `wifi_connection.c` + `wifi_settings.c` (provenance, licence checked); bring-up in the order the full-app Discord client and the template use (`wifi_remote_initialize` → `wifi_connection_init_stack` → `wifi_connect_try_all`); UDP to `host=`, TCP variant; **T7**; T8 again with Wi-Fi | todo | Radio SRAM measured in the ledger (the camera saw ~40 KB, `c67f796` there). |
| **7** | **Conclusions** | | |
| 7.1 | Summary table answering Q1-Q8; recommendation for SynthEngine3D: transport(s), stream rate, bitrate, where the capture hooks into `se_present()`, whether TinyUSB/encoder belong in graceloader (D-11), and what SynthMiner can afford | todo | |
| 7.2 | Optional: audio. Game mixer output → Shine MP3 22.05 kHz mono (the camera's setting: ~7-8 ms per 26 ms frame) → a second TS PID; CPU and SRAM measured the same way | todo | Only if 7.1 says video fits. |

---

## Part E: findings and decisions log

### Findings

| # | Finding | Evidence |
|---|---|---|
| F-01 | The USB-C port is **full speed only**. One internal full-speed PHY is shared between USB-Serial-JTAG and the full-speed OTG controller and switched by `usb_serial_jtag_ll_phy_select()`. The high-speed controller with its UTMI PHY is used for USB **host** (keyboards) on the port powered by the 5 V boost converter. MJPEG at 800×480/30 fps (30-60 Mbit/s) cannot fit through full speed; H.264 at 2-4 Mbit/s can. | `tanmatsu-launcher/main/usb_device.c:143-155,253` (`TINYUSB_CONFIG_FULL_SPEED`), `sdkconfigs/tanmatsu` `CONFIG_TINYUSB_RHPORT_FS=y`, `espressif__usb/src/usb_host.c:655,710-712` (default host peripheral = HS/UTMI), `tanmatsu-usb-msc/README.md` ("USB Full-Speed (USB 1.1)", "two USB controllers sharing one physical port") |
| F-02 | The esp_h264 **hardware** encoder is source code (`hw/src/*.c`, `hw/hal/esp32p4/*.c`, `interface/src`, `port/src`, ~2.7k lines); only the software codecs are prebuilt `.a`. It can be compiled `-fPIC` into app.so. | `tanmatsu-camera/managed_components/espressif__esp_h264/` |
| F-03 | Its interrupt is allocated with flags `0` (`esp_h264_enc_single_hw.c:280`), so it is not IRAM. That is legal for PSRAM code: masked while the cache is off, and the encoder waits for frame-done with a timeout. The display's IRAM problem (graceloader 2.6.0) does not repeat here. | as above |
| F-04 | The encoder **prefers internal RAM** for: the rate-control state (`h264_rc.c:57`), the SPS/PPS buffer (`esp_h264_enc_hw_param.c:376`), the **deblocking buffer**, sized by `mb_width × mb_height` (`:384`, 50 × 30 macroblocks at 800×480), and its DMA descriptors (`:388`, internal only). It falls back to PSRAM when internal allocation fails; the descriptors have no fallback. | as above; measured in 1.1 and 5.4 |
| F-05 | Graceloader 2.6.0 exports everything steps 1-5 need; it lacks sockets/netif, Wi-Fi bring-up, stack high-water marks and run-time stats (Part G). | `nm -D fakelib/liball.so` |
| F-06 | The P4's L2 cache counts next-level reads and writes per bus (`CACHE_L2_DBUSn_ACS_NXTLVL_RD_CNT_REG`/`_WR_`, plus hit/miss/conflict), i.e. the CPU's PSRAM traffic. DMA bypasses the cache and is not counted. | `esp-idf v6.0.2 components/soc/esp32p4/register/hw_ver1/soc/cache_reg.h` |
| F-07 | The testkit's console is USB-Serial-JTAG, the same PHY NCM needs, so the console is gone for the length of an NCM run. | `main/testkit/debugcon.c`; F-01 |
| F-08 | A grace app cannot configure components through Kconfig: it compiles against graceloader's `sdkconfig.h`, which has no `CONFIG_TINYUSB_*`. TinyUSB needs a hand-written `tusb_config.h` instead of `esp_tinyusb`'s generated one. | `include/sdkconfig.h`; template `CMakeLists.txt` |
| F-09 | An app's `.bss`/`.data` live in PSRAM (kbelf loads app.so there). TinyUSB's static endpoint and NCM buffers would therefore be in PSRAM, which matters if the DWC2 controller runs in DMA mode. Slave (FIFO) mode avoids the question at some CPU cost. | graceloader memory layout; D-12 |
| F-10 | With YUV420 output the PPA silently forces even scale fractions (`ppa_srm.c`); the camera got a green block from it. At 1:1 it does not bite. | `tanmatsu-camera/main/video.c:615-632` |
| F-11 | No grace app uses Wi-Fi or sockets today. The Discord client does, but it is a full `appfs` app with its own Wi-Fi stack. Graceloader links `tanmatsu-wifi` and lwIP but exports neither `wifi_remote_initialize` nor the 12 `esp_wifi_*`/`esp_netif_*`/`esp_event_*` functions `wifi-manager` calls; of wifi-manager's calls only `esp_wifi_stop` and the `nvs_*` ones are exported. | `tanmatsu-discord/metadata/metadata.json` (`"type": "appfs"`); `comm` of wifi-manager's calls against `fakelib/liball.so` |
| F-12 | USB IDs. Linux binds `cdc_ncm` by interface class, not by VID:PID, so any IDs work for testing. The launcher's `0x16D0:0x0F9A` must **not** be reused: BadgeLink tools match on it and would try to talk to the stream. Free sources of a real PID: **Espressif's usb-pids** (PIDs under VID `0x303A` for projects on Espressif chips, requested by pull request on GitHub), and **pid.codes** (PIDs under VID `0x1209` for open-source projects, also by pull request, with a few test PIDs anyone may use privately). | USB-IF VID policy; D-10 |
| F-13 | FreeRTOS's static object structs change size with the config: `GENERATE_RUN_TIME_STATS` adds a counter to `StaticTask_t`; `USE_TRACE_FACILITY` adds fields to `StaticTask_t`, `StaticQueue_t` (= `StaticSemaphore_t`), `StaticEventGroup_t`, `StaticTimer_t` and `StaticStreamBuffer_t`. An app built with the smaller struct and passing it to `x…CreateStatic` makes the kernel write past its buffer: silent corruption. Dynamic creation is unaffected. No `tanmatsu-*-grace` app (nor `tanmatsu-simd-tests`) uses static creation, and no grace app uses the network either (grep, 2026-09-25). | `esp-idf v6.0.2 components/freertos/FreeRTOS-Kernel/include/freertos/FreeRTOS.h:1282-1454`; graceloader `sdkconfig_tanmatsu:3151,3155` (both off today) |

| F-14 | TinyUSB's FreeRTOS OSAL creates its queue and semaphores **statically** (`xQueueCreateStatic`, `xSemaphoreCreate…Static`, because `configSUPPORT_STATIC_ALLOCATION` is on). So F-13's "no app uses static creation" no longer holds for this app. It is safe as long as nfmtest is built against headers that match the loader's FreeRTOS config: after G3 lands, rebuild against the synced template **before** running it on the new loader. | `components/tinyusb/src/osal/osal_freertos.h:44,195,231,258` |
| F-15 | Graceloader exports neither `esp_read_mac`/`esp_efuse_mac_get_default` nor `esp_log_set_vprintf`. The MAC is read from the eFuse registers (`efuse_ll_get_mac0/1`, `EFUSE` is exported), and the console is kept quiet during the run with `esp_log_level_set("*", ESP_LOG_NONE)`. | `nm -D fakelib/liball.so` |
| F-16 | FreeRTOS runs at 100 Hz here, so a `tud_task` that polls with a 1 ms timeout would really poll every 10 ms, or spin. Instead, TinyUSB's `tud_event_hook_cb` (called for every queued event, from the ISR too) wakes the usbnet task; it then runs `tud_task_ext(0)` and drains the TX ring. An IN transfer finishing is such an event, so a full NTB queue refills without waiting a tick. | `include/sdkconfig.h` `CONFIG_FREERTOS_HZ 100`; `usbd.c:432` |
| F-17 | TinyUSB compiled for full speed (`CFG_TUD_MAX_SPEED=FULL`) and the launcher's high-speed build pick the same PHY on the OTG 1.1 controller: `dwc2_core_is_highspeed_phy()` returns false either way, because that core has no HS PHY. The launcher's working setup is therefore a valid reference for ours. | `dwc2_common.c:186-197` |

### Decisions

| # | Decision | By / when |
|---|---|---|
| D-01 | Linux only. Windows and macOS are out of scope, so CDC-NCM and UVC-with-H.264 both qualify, and NCM is chosen for its shared path with Wi-Fi and for carrying audio in the same stream later. | The user, 2026-09-25 |
| D-02 | Wire format: MPEG-TS over UDP, 7 × 188 B per datagram, received by OBS's Media Source (`udp://@:5000`). | The user asked for NCM streaming into OBS, 2026-09-25 |
| D-03 | Two NCM stacks: `ncm_raw` (hand-built frames, no graceloader change, measures USB alone) first, then `ncm_lwip` (shares the Wi-Fi path). | (proposed) |
| D-04 | Wi-Fi is prepared (transport interface, tests T6-T8 defined, graceloader needs listed) but built only after G1/G2. | The user: "prepared to test the WiFi version", 2026-09-25 |
| D-05 | Video only until 7.2. | (proposed) |
| D-06 | Native 800×480, 30 and 15 fps, GOP = one second, 1-4 Mbit/s. | (proposed) |
| D-07 | Framebuffers are the display driver's three, flipped like SynthEngine3D 2.2, so the measurement sees a game's real conditions. Requires graceloader 2.6.0. | (proposed) |
| D-08 | CPU share by soak tasks (M1.2, the reference for totals), plus FreeRTOS run-time stats for the per-task breakdown once G3 is in (M1.3). | (proposed); run-time stats by D-14 |
| D-09 | Stream tasks on core 1 below SynthMiner's chunk worker priority; render on core 0. | (proposed) |
| D-10 | USB IDs: a pid.codes **test PID** while this is a test app (F-12); implemented as `0x1209:0x0001`. If streaming ships in a game, request a PID from Espressif's usb-pids or pid.codes (this app is MIT). Never the launcher's `0x16D0:0x0F9A`. | (proposed) |
| D-11 | Whether the encoder and TinyUSB move into graceloader for games is decided in 7.1, on these numbers. | open |
| D-12 | DWC2 in slave mode first (F-09). Implemented (`tusb_config.h`). | (proposed) |
| D-13 | The graceloader exports (G1, G2) get added, synced to the template and committed in both, then pulled into the apps that need them, this one first. Graceloader is not pushed for it; the next release carries it. | The user, 2026-09-25 |
| D-15 | `ncm_raw` has its own one-lease **DHCP server**: the PC always gets `192.168.77.1/24`, with no router and no DNS, and the badge is `192.168.77.2`. The laptop moves between networks, so the link must configure itself without touching the default route. NetworkManager's automatic profile picks it up; `setup_host_ncm.sh` and `nfmrecv.py --sudo-ip` are fallbacks for PCs with no DHCP client. | The user, 2026-09-25 |
| D-14 | G3 is in: graceloader gets FreeRTOS run-time stats **and** the trace facility, with their exports. The static-struct size change (F-13) is accepted because the user is the only graceloader user and none of their apps create static FreeRTOS objects. | The user, 2026-09-25 |

---

## Critical files

| File | Why |
|---|---|
| `tanmatsu-camera/main/video.c`, `camera_pipeline.c` | The working encoder and PPA-to-YUV setup this app starts from |
| `tanmatsu-camera/managed_components/espressif__esp_h264/` | The source to vendor (F-02) |
| `tanmatsu-launcher/main/usb_device.c`, `tanmatsu-usb-msc/main/usb_mode.c` | The PHY switch there and back |
| `tanmatsu-launcher/managed_components/espressif__tinyusb/` (0.21.0), `espressif__esp_tinyusb/tinyusb_net.c` | TinyUSB NCM, and Espressif's lwIP glue as a reference for 6.2 |
| `synthengine3D/src/se_run.c` (in SynthMiner) | The page flip copied by `present.c` |
| `tanmatsu-graceloader/main/exported_symbols.cmake`, `sdkconfigs/tanmatsu` | Where G1-G3 go |
| `tanmatsu-template/managed_components/nicolaielectronics__wifi-manager/`, `tanmatsu-discord/main/main.c` | Wi-Fi bring-up for step 6.3 (F-11) |
| `esp-idf/components/soc/esp32p4/register/hw_ver1/soc/cache_reg.h` | L2 counters (F-06) |
| `hardware_docs/esp32-p4_trm_v1.3.pdf` | PPA, H.264, cache and USB chapters |
