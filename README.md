# nfmtest: USB-C networking and H.264 screen streaming, measured

A measuring instrument for one question: can a graceloader game stream its screen
(hardware H.264, MPEG-TS over UDP) into OBS on a Linux PC, and what does that cost
the game? The plan, the findings and the status are in
[`claudeplans/nfmtest.md`](claudeplans/nfmtest.md).

## What works so far: the USB-C port as a network adapter

The app turns the Tanmatsu's USB-C port into a CDC-NCM USB network adapter
(TinyUSB, compiled into the app). Linux picks it up with its stock `cdc_ncm`
driver as `enx<MAC>`.

| | |
|---|---|
| Badge | `192.168.77.2`: answers ARP, ping, UDP echo (port 7) |
| PC | `192.168.77.1/24`, handed out by the badge's own DHCP server, with **no router and no DNS**, so the PC's other networks are untouched |
| Data | UDP to the PC's port 5000 (blast test), badge statistics once a second to port 5001, `STOP` to the badge's port 5002 ends a run |

While the link is up, the USB-Serial-JTAG console (`/dev/ttyACM0`) is **gone**:
the two share one full-speed PHY. The app swaps it back at the end of a run.

**By hand:** start the app. On the badge, press Enter to blast for 30 s, `B` for
5 minutes, or `I` for a link-only test (DHCP, ping). On the PC:

```sh
tools/nfmrecv.py --ping --echo 50      # waits for the interface, receives, checks
```

**Hands-free:** `make nfmcycle NFM="mode=blast secs=30"` builds, installs, starts
the app, runs the test through the console round trip (`tools/nfmrun.py`) and
writes `results/<UTC>-nfm-ncm-<mode>/result.json`. `make tscheck` runs the
host-side tests of the network code, with no badge involved.

If the PC runs no DHCP client on new interfaces, `tools/nfmrecv.py --sudo-ip`
(or `tools/setup_host_ncm.sh --now`) sets the address by hand.

## Streaming the screen into OBS

The badge encodes what it draws as H.264 (the hardware encoder, built into
graceloader) and sends it as MPEG-TS over the USB network link. OBS plays it
directly, with no server in between.

**On the badge:** start the app and press **S**, which streams for 5 minutes.
From the console, `RUN ncm mode=stream secs=<s> fps=<n> br=<kbit/s>
gop=<frames> pat=<pattern>` sets the parameters. The defaults are 30 fps,
3000 kbit/s, a keyframe every second and the `motion` test pattern. The
display shows the frames being streamed; the frame counter on it, filmed next
to OBS, gives the glass-to-glass latency.

**On the PC:** wait until the `enx…` interface has `192.168.77.1`, as for the
other network tests. Then, in OBS:

1. Sources → **+** → **Media Source**, give it a name.
2. Untick **Local File**.
3. **Input:** `udp://@:5000`
4. **Input Format:** `mpegts`
5. OK.

The picture appears within about a second of the link coming up, because
the player waits for the next keyframe. For a quick check without OBS:
`ffplay -fflags nobuffer udp://@:5000`.

What is sent: 800×480 H.264 (Constrained Baseline) in MPEG-TS. PAT/PMT go
before every keyframe; the PCR and the video are on PID 0x100; each frame
starts with an access unit delimiter. Datagrams are 7 × 188 bytes and are
sent as soon as a frame is encoded. The badge also sends one JSON statistics
line per second to port 5001 (`tools/nfmrecv.py` prints them).

## 3D: SynthEngine3D

[SynthEngine3D](https://github.com/nullislandspace/synthengine3D) is the 3D engine for
graceloader apps: a software rasteriser (z-buffer and raycast), PPA compositing, meshes,
textures, lighting, audio and UI helpers, with its own `se_run()` main loop.

The engine is **not** shipped with the template, so apps that do not want it are not
carrying it around. What the template does ship is the build wiring, which sits idle until
an app adds the engine. In a new app that wants 3D:

```sh
make engine     # git submodule add -b main git@github.com:nullislandspace/synthengine3D.git synthengine3D
git add .gitmodules synthengine3D && git commit -m "Add SynthEngine3D"
```

`CMakeLists.txt` picks it up by itself: when `synthengine3D/CMakeLists.txt` exists it builds
the engine, propagates its include directory (so app sources can `#include "synthengine3d.h"`)
and folds its objects into `app.so`. When it does not, the link line is exactly the plain
one, which is why every app can keep this template as `upstream` whether it uses 3D or not.

Two things follow from it being a submodule:

* clone such an app with `git clone --recursive`, or run `git submodule update --init` in it;
* `git submodule update --remote synthengine3D` moves it to the newest engine, and an app
  that wants a fixed version pins it (`ENGINE_REF=V2.0 make engine`, or check out the tag
  inside `synthengine3D/` and commit the new pointer).

Engine settings (list caps and the like) are compile definitions that must reach the
`synthengine3d` target, so set them with `add_compile_definitions()` **before**
`add_subdirectory(synthengine3D)` — see the engine's `docs/configuration.md`.

## Automated device tests

`main/testkit/` is a ready-made test loop for an app on real hardware. The host
sends a command over the debug console, the app runs it inside its own frame
loop, reports machine-readable records, and returns to the launcher by itself —
so `make cycle` builds, installs, runs, tests and comes back with a verdict
without anyone touching the badge.

```sh
make cycle       TEST="perf  scene=title secs=20"       # frame rate, phase split, primitive counts
make cycle       TEST="shots scene=title ms=0,1500,4000" # render exact instants, save PNGs + hashes
make testrefs    TEST="shots scene=title ms=0,1500,4000" # store those hashes as the references
make testcompare TEST="shots scene=title ms=0,1500,4000" # compare against them: a regression test
make recover                                             # after a crash or a hang
```

Results land in `results/<UTC>-<test>-<scene>/` as `console.log` + `result.json`;
references live in `tests/refs/manifest.json`. Exit codes: 0 ok, 1 link, 2 crash,
3 the test reported bad, 4 an image mismatch, 5 usage.

### What it is

| File | What |
|---|---|
| `testkit/debugcon.*` | The console listener: `PING`, `RUN <test> k=v`, `EXIT`, `BADGELINK`, read through the USB-serial/JTAG **driver** (graceloader 2.4.0+ exports it). Emits a `READY` record every 2 s while idle, so the host can find the app without sending anything. |
| `testkit/report.*` | The record format: `@@SR-<KIND>@@ <json> @@<crc32>@@`, one line, CRC'd so a line another task interleaved into it is dropped rather than believed. |
| `testkit/devtest.*` | The two tests (`perf`, `shots`) and the runner that drives them. |
| `testkit/profile.*` | Per-phase frame timing (`prof_begin`/`prof_end`), reported in each `PERF` record. The phase names are yours. |
| `testkit/screenshot.*` | Framebuffer → PNG on the SD card, with a deflate *stored* stream so it needs 64 KB of PSRAM rather than a ~130 KB compressor in scarce internal RAM. |
| `testkit/showtime.*` | The clock everything hangs off: real time, or fixed steps of 1/fps, or set outright. |
| `tools/testrun.py` | The host side: connect, identify, refuse a stale build, run, collect, compare, write results. |
| `tools/recover.py` | Get a wedged badge back. |

### Wiring it into an app

1. Add `main/testkit/*.c` to `APP_SOURCES`, and `main` to `APP_INCLUDES` (it is
   there already). Set the app's paths and name while you are there:

   ```cmake
   add_compile_definitions(SCREENSHOT_DIR="/sd/myapp")
   # REPORT_PREFIX="SR" by default; change it only if two apps' logs mix,
   # and pass the same to testrun.py with --prefix.
   ```

2. Tell the kit how to address your content — one struct, five functions:

   ```c
   static devtest_content_t const CONTENT = {
       .select    = level_select,     // play this one from its start; false if unknown
       .duration  = level_duration,   // seconds, <= 0 for endless
       .started   = level_started,    // show time at which it began
       .name      = level_name,       // what is selected now
       .shot_name = level_section,    // sub-section for per-shot stats, or ""
   };
   static devtest_config_t const TEST = {
       .app = "tld.username.myapp", .shot_dir = "/sd/myapp/test", .content = &CONTENT,
   };
   ```

3. Call it from the frame loop:

   ```c
   on_init:    devtest_start(&TEST);
   on_update:  showtime_frame(); devtest_update(); /* then advance your own content */
   on_render:  /* draw the frame */ devtest_after_render(fb, rast_us);
   per second: devtest_period(fps, frame_ms);
   ```

An app without SynthEngine3D compiles the kit with `TESTKIT_NO_ENGINE`: it then
reports timings and heap, and the primitive counts read zero.

### The one precondition

The `shots` test renders *chosen instants*: it sets the clock instead of running
it. That is only meaningful if what you draw is a **pure function of
`showtime_now()`** — same t, same picture. Get that right and a stored hash is a
real regression test, a host-side checker can replay the same instant (see the
engine's `docs/testing.md`), and a video export can render far slower than real
time without changing a frame. An app that accumulates per-frame `dt`, or draws
from unseeded randomness, can still use `perf`, but its shot hashes will wobble
and the references mean nothing.

## License

This software is under the [MIT license](https://opensource.org/license/mit). The MIT license allows others to build upon your work without restrictions while also making sure you retain your attribution.

(C) 2026 Rene Schickbauer
