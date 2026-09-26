#!/usr/bin/env python3
"""PC side of nfmtest's USB network link (M4, Part M of claudeplans/nfmtest.md).

The badge (RUN ncm, or a key on the badge) turns its USB-C port into a CDC-NCM
network adapter: Linux names it enx<MAC>, the badge hands the PC 192.168.77.1/24
by DHCP (no route, no DNS) and is itself 192.168.77.2.

This script:
  - waits for that interface and its address (optionally by MAC, --mac);
  - if the PC has no DHCP client on it, can set the address itself (--sudo-ip);
  - receives the badge's UDP datagrams on :5000 and checks them: sequence
    numbers (loss, duplicates, reordering), the fill pattern (corruption),
    throughput, and the spread of one-way delay (from the badge's clock);
  - prints the badge's own once-a-second PERIOD lines from :5001 alongside;
  - optionally pings and measures UDP echo round trips (--ping, --echo);
  - optionally ends the run early (--stop sends "STOP" to :5002);
  - writes a JSON summary (--json).

Stand-alone use, for a run started on the badge by hand:

    tools/nfmrecv.py --ping --echo 50

tools/nfmrun.py uses it for the hands-free round trip.
"""

import argparse
import glob
import json
import os
import select
import socket
import statistics
import struct
import subprocess
import sys
import threading
import time

DEV_IP = "192.168.77.2"
HOST_IP = "192.168.77.1"
PORT_DATA, PORT_STATS, PORT_CTRL, PORT_ECHO = 5000, 5001, 5002, 7
HDR = struct.Struct("<4sIII Q")  # magic, run hash, seq, len, badge microseconds
assert HDR.size == 24


def fnv1a(text):
    h = 2166136261
    for b in text.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


# --- the interface ---------------------------------------------------------------

def find_interface(mac):
    """Name of the interface with this MAC, or None."""
    for path in glob.glob("/sys/class/net/*/address"):
        try:
            with open(path, encoding="ascii") as fh:
                if fh.read().strip().lower() == mac.lower():
                    return path.split("/")[-2]
        except OSError:
            pass
    return None


def ncm_interfaces():
    """Interfaces bound to the cdc_ncm driver."""
    out = []
    for path in glob.glob("/sys/class/net/*/device/driver"):
        if os.path.basename(os.path.realpath(path)) == "cdc_ncm":
            out.append(path.split("/")[-4])
    return out


def has_address(ifname, ip=HOST_IP):
    try:
        res = subprocess.run(["ip", "-4", "-o", "addr", "show", "dev", ifname], capture_output=True, text=True,
                             timeout=5)
        return f" {ip}/" in res.stdout
    except (OSError, subprocess.SubprocessError):
        return False


def wait_link(mac, timeout, sudo_ip, say=print):
    """Wait for the badge's interface and 192.168.77.1 on it. Returns the
    interface name (or None) and how long each step took."""
    t0 = time.time()
    info = {"ifname": None, "if_s": None, "addr_s": None, "addr_by": None}
    ifname = None
    while time.time() - t0 < timeout:
        ifname = find_interface(mac) if mac else (ncm_interfaces() or [None])[0]
        if ifname:
            break
        time.sleep(0.1)
    if not ifname:
        say(f"no {'interface with MAC ' + mac if mac else 'cdc_ncm interface'} after {timeout:.0f} s "
            "(is the badge in network mode? dmesg | tail)")
        return None, info
    info["ifname"], info["if_s"] = ifname, round(time.time() - t0, 2)
    say(f"interface {ifname} after {info['if_s']} s, waiting for {HOST_IP} (DHCP from the badge)...")
    dhcp_wait = min(timeout, 12.0) if sudo_ip else timeout
    t1 = time.time()
    while time.time() - t1 < dhcp_wait:
        if has_address(ifname):
            info["addr_s"], info["addr_by"] = round(time.time() - t0, 2), "dhcp/existing"
            say(f"{ifname} has {HOST_IP} after {info['addr_s']} s")
            return ifname, info
        time.sleep(0.2)
    if sudo_ip:
        say(f"no DHCP on {ifname}: setting {HOST_IP}/24 by hand (sudo)")
        subprocess.run(["sudo", "ip", "link", "set", ifname, "up"], check=False)
        subprocess.run(["sudo", "ip", "addr", "add", f"{HOST_IP}/24", "dev", ifname], check=False)
        if has_address(ifname):
            info["addr_s"], info["addr_by"] = round(time.time() - t0, 2), "sudo-ip"
            return ifname, info
    say(f"{ifname} never got {HOST_IP}. Either the PC runs no DHCP client on new interfaces, or it lost the "
        f"race: try --sudo-ip, or once:  sudo ip addr add {HOST_IP}/24 dev {ifname} && sudo ip link set {ifname} up")
    return ifname, info


# --- probes ----------------------------------------------------------------------------

def ping(count=20, interval=0.2):
    try:
        res = subprocess.run(["ping", "-c", str(count), "-i", str(interval), "-W", "1", DEV_IP],
                             capture_output=True, text=True, timeout=count * interval + 10)
    except (OSError, subprocess.SubprocessError) as exc:
        return {"error": str(exc)}
    out = {"raw_tail": res.stdout.strip().splitlines()[-2:]}
    for line in res.stdout.splitlines():
        if "packets transmitted" in line:
            parts = line.split(",")
            out["sent"] = int(parts[0].split()[0])
            out["received"] = int(parts[1].split()[0])
        if line.startswith("rtt") or line.startswith("round-trip"):
            vals = line.split("=")[1].split()[0].split("/")
            out["rtt_min_ms"], out["rtt_avg_ms"], out["rtt_max_ms"] = (float(v) for v in vals[:3])
    return out


def udp_echo(count, size=64, timeout=0.5):
    """Round trips to the badge's UDP echo (port 7), in ms."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    rtts, lost = [], 0
    for i in range(count):
        payload = struct.pack("<I", i) + bytes(size - 4)
        t = time.perf_counter()
        s.sendto(payload, (DEV_IP, PORT_ECHO))
        try:
            while True:
                data, _ = s.recvfrom(2048)
                if data[:4] == payload[:4]:
                    rtts.append((time.perf_counter() - t) * 1000)
                    break
        except socket.timeout:
            lost += 1
        time.sleep(0.02)
    s.close()
    if not rtts:
        return {"sent": count, "lost": lost}
    rtts.sort()
    return {"sent": count, "lost": lost, "min_ms": round(rtts[0], 3), "p50_ms": round(statistics.median(rtts), 3),
            "p95_ms": round(rtts[int(len(rtts) * 0.95) - 1 if len(rtts) > 1 else 0], 3), "max_ms": round(rtts[-1], 3)}


def send_stop():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for _ in range(3):
        s.sendto(b"STOP", (DEV_IP, PORT_CTRL))
        time.sleep(0.05)
    s.close()


# --- the receiver --------------------------------------------------------------------------

class Receiver:
    """Counts and checks the blast datagrams on :5000 and collects the
    badge's PERIOD JSON from :5001."""

    def __init__(self, run=None, bind="0.0.0.0", quiet=False):
        self.run_hash = fnv1a(run) if run else None
        self.quiet = quiet
        self.data = self._sock(bind, PORT_DATA)
        self.stats = self._sock(bind, PORT_STATS)
        self.patterns = {}
        self.reset()

    @staticmethod
    def _sock(bind, port):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
        s.bind((bind, port))
        s.setblocking(False)
        return s

    def reset(self):
        self.count = self.bytes = self.dups = self.reordered = self.corrupt = self.other = self.foreign = 0
        self.seen = set()
        self.max_seq = -1
        self.first_seq = None
        self.first_t = self.last_t = None
        self.offsets = []  # local receive time - badge send time, microseconds
        self.periods = []
        self.per_second = []
        self._sec_start = None
        self._sec_count = self._sec_bytes = 0

    def _pattern(self, seq, length):
        key = (seq & 0xFF, length)
        p = self.patterns.get(key)
        if p is None:
            p = bytes((seq + i) & 0xFF for i in range(24, length))
            self.patterns[key] = p
        return p

    def _data(self, pkt, now):
        if len(pkt) < 24 or pkt[:4] != b"NFMB":
            self.other += 1
            return
        _, run_hash, seq, length, t_us = HDR.unpack_from(pkt)
        if self.run_hash is not None and run_hash != self.run_hash:
            self.foreign += 1  # an older run still draining
            return
        if length != len(pkt) or pkt[24:] != self._pattern(seq, length):
            self.corrupt += 1
        if seq in self.seen:
            self.dups += 1
            return
        self.seen.add(seq)
        if seq < self.max_seq:
            self.reordered += 1
        self.max_seq = max(self.max_seq, seq)
        if self.first_seq is None:
            self.first_seq = seq
            self.first_t = now
        self.last_t = now
        self.count += 1
        self.bytes += len(pkt)
        self.offsets.append(now * 1e6 - t_us)
        if self._sec_start is None:
            self._sec_start = now
        self._sec_count += 1
        self._sec_bytes += len(pkt)

    def _stats(self, pkt):
        try:
            rec = json.loads(pkt.decode())
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        self.periods.append(rec)
        if not self.quiet:
            print(f"  badge t={rec.get('t', 0):6.1f}s mounted={rec.get('mounted')} udp {rec.get('udp_mbit', 0):6.3f} "
                  f"Mbit/s  blocked {rec.get('blocked')}  full {rec.get('ring_full')}  usb task "
                  f"{rec.get('usb_busy_pct', 0):5.1f}%  rx {rec.get('rx')} ping {rec.get('icmp')} "
                  f"dhcp {rec.get('dhcp')}  wake ev/to/poll {rec.get('wk_ev')}/{rec.get('wk_to')}/"
                  f"{rec.get('wk_poll')}  xfer {rec.get('xfer')}")

    def _tick(self, now):
        if self._sec_start is not None and now - self._sec_start >= 1.0:
            dt = now - self._sec_start
            mbit = self._sec_bytes * 8 / dt / 1e6
            lost = (self.max_seq - self.first_seq + 1 - self.count) if self.first_seq is not None else 0
            self.per_second.append({"t": round(now - self.first_t, 3), "pkts": self._sec_count,
                                    "mbit": round(mbit, 3), "lost_total": lost})
            if not self.quiet:
                print(f"PC: {self._sec_count:5d} pkt/s  {mbit:6.3f} Mbit/s payload  lost so far {lost}  "
                      f"dup {self.dups}  corrupt {self.corrupt}")
            self._sec_start, self._sec_count, self._sec_bytes = now, 0, 0

    def run(self, secs=None, idle=5.0, stop_event=None):
        """Receive until `secs` pass, or `idle` seconds without data after
        the first datagram, or stop_event is set."""
        t0 = time.time()
        last_rx = None
        while True:
            now = time.time()
            if secs is not None and now - t0 >= secs:
                break
            if last_rx is not None and idle and now - last_rx >= idle:
                break
            if stop_event is not None and stop_event.is_set():
                break
            ready, _, _ = select.select([self.data, self.stats], [], [], 0.2)
            now = time.time()
            for s in ready:
                while True:
                    try:
                        pkt = s.recv(65536)
                    except BlockingIOError:
                        break
                    if s is self.data:
                        self._data(pkt, now)
                        last_rx = now
                    else:
                        self._stats(pkt)
            self._tick(now)

    def summary(self):
        out = {"received": self.count, "bytes": self.bytes, "dups": self.dups, "reordered": self.reordered,
               "corrupt": self.corrupt, "other": self.other, "foreign": self.foreign,
               "periods_from_badge": len(self.periods), "per_second": self.per_second}
        if self.first_seq is not None:
            span = self.max_seq - self.first_seq + 1
            dur = (self.last_t - self.first_t) if self.last_t > self.first_t else 0
            out.update({"first_seq": self.first_seq, "last_seq": self.max_seq, "expected": span,
                        "lost": span - self.count, "loss_pct": round(100.0 * (span - self.count) / span, 4),
                        "secs": round(dur, 3),
                        "payload_mbit": round(self.bytes * 8 / dur / 1e6, 3) if dur else None})
            base = min(self.offsets)
            rel = sorted((o - base) / 1000 for o in self.offsets)  # ms above the fastest datagram
            out["delay_spread_ms"] = {"p50": round(rel[len(rel) // 2], 3), "p95": round(rel[int(len(rel) * 0.95)], 3),
                                      "p99": round(rel[int(len(rel) * 0.99)], 3), "max": round(rel[-1], 3)}
        return out


def print_summary(s):
    if not s.get("received"):
        print("no blast datagrams received")
        return
    print(f"received {s['received']} of {s['expected']} (lost {s['lost']}, {s['loss_pct']}%), dup {s['dups']}, "
          f"reordered {s['reordered']}, corrupt {s['corrupt']}")
    print(f"payload {s['payload_mbit']} Mbit/s over {s['secs']} s; one-way delay spread "
          f"p50 {s['delay_spread_ms']['p50']} / p95 {s['delay_spread_ms']['p95']} / max "
          f"{s['delay_spread_ms']['max']} ms")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mac", help="the badge's PC-side MAC (on its screen, or in the START record)")
    ap.add_argument("--run", help="only count datagrams of this run id")
    ap.add_argument("--link-timeout", type=float, default=30)
    ap.add_argument("--no-link-wait", action="store_true", help="just listen")
    ap.add_argument("--sudo-ip", action="store_true", help=f"set {HOST_IP}/24 with sudo if DHCP does not")
    ap.add_argument("--secs", type=float, help="receive this long (default: until 5 s of silence)")
    ap.add_argument("--idle", type=float, default=5.0)
    ap.add_argument("--ping", action="store_true", help="ping the badge 20 times while receiving")
    ap.add_argument("--echo", type=int, default=0, help="UDP echo round trips to measure first")
    ap.add_argument("--stop", action="store_true", help="send STOP to the badge when done")
    ap.add_argument("--json", help="write the summary here")
    args = ap.parse_args()

    result = {"link": None}
    if not args.no_link_wait:
        ifname, info = wait_link(args.mac, args.link_timeout, args.sudo_ip)
        result["link"] = info
        if ifname is None:
            return 1
    recv = Receiver(run=args.run)
    if args.echo:
        result["echo"] = udp_echo(args.echo)
        print(f"UDP echo: {result['echo']}")
    ping_out = {}
    pinger = None
    if args.ping:
        pinger = threading.Thread(target=lambda: ping_out.update(ping()), daemon=True)
        pinger.start()
    print(f"receiving on :{PORT_DATA} (data) and :{PORT_STATS} (badge stats)... Ctrl-C ends")
    try:
        recv.run(secs=args.secs, idle=args.idle)
    except KeyboardInterrupt:
        pass
    if args.stop:
        send_stop()
    if pinger:
        pinger.join(timeout=10)
        result["ping"] = ping_out
        print(f"ping: {ping_out}")
    result["recv"] = recv.summary()
    print_summary(result["recv"])
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(result, fh, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
