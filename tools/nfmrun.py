#!/usr/bin/env python3
"""Hands-free nfmtest run over the USB network link (Part T of claudeplans/nfmtest.md).

The app must be running (make cycle starts it). The run takes the badge's
console away for its length, so this script does the round trip:

  1. connects to the debug console, checks the build, sends
     `RUN ncm <args>`, and waits for the START record (run id, MACs);
  2. closes the console: the badge swaps its USB-C PHY over and becomes a
     CDC-NCM network adapter, /dev/ttyACM0 goes away;
  3. waits for the interface (by the MAC in START) and 192.168.77.1 on it,
     measures UDP echo and ping, receives the blast (tools/nfmrecv.py);
  4. waits for the console to come back, collects the PERIOD and RESULT
     records the badge repeats, answers ACK;
  5. writes results/<UTC>-nfm-ncm-<mode>/{console.log,result.json}.

    tools/nfmrun.py --port /dev/ttyACM0 -- mode=blast secs=30

Exit codes as tools/testrun.py: 0 ok, 1 link / timeout, 2 crash or error on
the badge, 3 the run reported bad, 5 usage.
"""

import argparse
import datetime
import glob
import json
import os
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import nfmrecv  # noqa: E402

try:
    import testrun  # noqa: E402  (needs pyserial)
except SystemExit:
    print("pyserial missing (run inside the ESP-IDF environment: source $IDF_SOURCE)", file=sys.stderr)
    sys.exit(5)

EXIT_OK, EXIT_LINK, EXIT_CRASH, EXIT_BAD, EXIT_USAGE = 0, 1, 2, 3, 5


def wait_start(port, log, timeout=15):
    """Collect lines until START (or an early RESULT = error)."""
    buffer = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = testrun.read_line(port, buffer)
        if line is None:
            continue
        log.write(line + "\n")
        kind, rec = testrun.parse_record(line)
        if kind in ("START", "RESULT"):
            return kind, rec
    return None, None


def port_present(url):
    if "://" in url:
        return True  # remote: just try
    return os.path.exists(url)


def collect_final(url, log, timeout):
    """Wait for the console to come back, collect PERIOD + RESULT, ACK."""
    deadline = time.time() + timeout
    periods, result = {}, None
    while time.time() < deadline and result is None:
        if not port_present(url):
            time.sleep(0.2)
            continue
        try:
            port = testrun.open_console(url, timeout=0.5)
        except Exception as exc:  # noqa: BLE001 - it may vanish again while enumerating
            log.write(f"### reopen: {exc}\n")
            time.sleep(0.5)
            continue
        log.write("### console back\n")
        buffer = bytearray()
        try:
            while time.time() < deadline:
                line = testrun.read_line(port, buffer)
                if line is None:
                    continue
                log.write(line + "\n")
                kind, rec = testrun.parse_record(line)
                if kind == "PERIOD":
                    periods[rec["i"]] = rec
                elif kind == "RESULT":
                    result = rec
                    port.write(b"\nACK\n")
                    port.flush()
                    # A few more lines: BYE, as the app leaves.
                    end = time.time() + 2
                    while time.time() < end:
                        line = testrun.read_line(port, buffer)
                        if line is not None:
                            log.write(line + "\n")
                    break
        except Exception as exc:  # noqa: BLE001
            log.write(f"### console lost again: {exc}\n")
        finally:
            try:
                port.close()
            except Exception:  # noqa: BLE001
                pass
    return [periods[k] for k in sorted(periods)], result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.environ.get("PORT"), help="debug console, e.g. /dev/ttyACM0")
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "results"))
    ap.add_argument("--connect-timeout", type=float, default=12)
    ap.add_argument("--no-git-check", action="store_true")
    ap.add_argument("--sudo-ip", action="store_true", help="set 192.168.77.1/24 with sudo if DHCP does not")
    ap.add_argument("--echo", type=int, default=50, help="UDP echo round trips (0: none)")
    ap.add_argument("--no-ping", action="store_true")
    ap.add_argument("args", nargs="*", help="RUN ncm arguments, e.g. mode=blast secs=30")
    a = ap.parse_args()
    testrun.set_prefix("NFM")
    if not a.port:
        print("no --port and no $PORT", file=sys.stderr)
        return EXIT_USAGE

    params = dict(p.split("=", 1) for p in a.args if "=" in p)
    mode = params.get("mode", "blast")
    secs = float(params.get("secs", 30))
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run_id = f"{stamp}-nfm-ncm-{mode}"
    if "run" not in params:
        a.args.append(f"run={stamp}")
    run_dir = os.path.join(a.out_dir, run_id)
    os.makedirs(run_dir, exist_ok=True)
    log = open(os.path.join(run_dir, "console.log"), "w", encoding="utf-8", buffering=1)
    result = {"meta": {"host_time": stamp, "args": a.args, "local_git": testrun.local_git_id()},
              "ready": None, "start": None, "link": None, "echo": None, "ping": None, "pc": None,
              "periods": [], "badge": None, "verdict": None}

    def finish(verdict, code):
        result["verdict"] = verdict
        log.close()
        with open(os.path.join(run_dir, "result.json"), "w", encoding="utf-8") as fh:
            json.dump(result, fh, indent=1)
        print(f"\nverdict: {verdict}  ->  {run_dir}/")
        return code

    # 1. Console, build check, RUN, START.
    print(f"Connecting to {a.port}...")
    port, ready = testrun.connect(a.port, a.connect_timeout, log)
    if port is None:
        return finish("no-connection", EXIT_LINK)
    result["ready"] = ready
    want = result["meta"]["local_git"]
    if not a.no_git_check and want and ready.get("git") != want:
        print(f"Badge runs build {ready.get('git')!r}, this checkout is {want!r} -- install first "
              "(make cycle), or --no-git-check", file=sys.stderr)
        port.close()
        return finish("wrong-build", EXIT_USAGE)
    port.write(b"\n" * 64 + f"RUN ncm {' '.join(a.args)}\n".encode())
    port.flush()
    kind, rec = wait_start(port, log)
    try:
        port.close()
    except Exception:  # noqa: BLE001
        pass
    if kind is None:
        return finish("no-start", EXIT_LINK)
    if kind == "RESULT":
        result["badge"] = rec
        print(f"badge refused: {rec}")
        return finish("error", EXIT_CRASH)
    result["start"] = rec
    print(f"START: run {rec['run']}, PC interface MAC {rec['host_mac']}, badge {rec['dev_mac']}")

    # 3. The network side, while the console is away.
    recv = nfmrecv.Receiver(run=rec["run"])
    ifname, link = nfmrecv.wait_link(rec["host_mac"], 20, a.sudo_ip)
    result["link"] = link
    t_link = time.time()
    if ifname is not None and (link.get("addr_s") is not None):
        if a.echo:
            result["echo"] = nfmrecv.udp_echo(a.echo)
            print(f"UDP echo: {result['echo']}")
        ping_out = {}
        pinger = None
        if not a.no_ping:
            pinger = threading.Thread(target=lambda: ping_out.update(nfmrecv.ping()), daemon=True)
            pinger.start()
        # Until the badge's run is over: it started roughly when START came.
        remaining = max(1.0, secs - (time.time() - t_link) + 3)
        recv.run(secs=remaining, idle=4.0 if mode == "blast" else None)
        if pinger:
            pinger.join(timeout=10)
            result["ping"] = ping_out
            print(f"ping: {ping_out}")
    else:
        print("no usable link; waiting for the badge to finish its run anyway")
        time.sleep(max(0.0, secs - (time.time() - t_link)))
    result["pc"] = recv.summary()
    nfmrecv.print_summary(result["pc"])

    # 4. The console comes back.
    print("waiting for the console to come back...")
    periods, badge = collect_final(a.port, log, timeout=40)
    result["periods"], result["badge"] = periods, badge
    if badge is None:
        return finish("no-result", EXIT_LINK)
    print(f"badge: status {badge.get('status')}, {badge.get('udp_mbit')} Mbit/s sent, mount {badge.get('mount_ms')} "
          f"ms, dhcp {badge.get('dhcp_ms')} ms, usb task {badge.get('usb_busy_pct')}%, "
          f"switch on/off {badge.get('switch_on_ms')}/{badge.get('switch_off_ms')} ms")
    if badge.get("tx_udp") and result["pc"].get("received") is not None:
        sent = badge.get("seq", 0)
        print(f"end to end: badge generated {sent}, TinyUSB took {badge['tx_udp']}, PC received "
              f"{result['pc']['received']}")
    led = badge.get("ledger", {})
    if led.get("before") and led.get("after"):
        leak = led["before"]["sram"] - led["after"]["sram"]
        print(f"SRAM: before {led['before']['sram']}, up {(led.get('up') or {}).get('sram')}, after "
              f"{led['after']['sram']} (difference {leak} B)")
    if badge.get("status") != "ok":
        return finish("bad", EXIT_BAD)
    return finish("ok", EXIT_OK)


if __name__ == "__main__":
    sys.exit(main())
