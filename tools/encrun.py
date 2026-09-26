#!/usr/bin/env python3
"""Run nfmtest's encoder test (RUN enc, main/nfm/enctest.h) and check its output.

    tools/encrun.py --port rfc2217://localhost:4001 -- frames=150 pat=motion br=3000

Prints the ENCBEGIN/ENCPERIOD/RESULT records, writes
results/<UTC>-nfm-enc-<pat>/{console.log,result.json}. With --fetch it also
downloads /sd/nfmtest/<run>.h264 and <run>_f0.png over BadgeLink, and runs
ffprobe on the video (frame count, resolution, decoder errors) and ffmpeg to
extract the decoded frame 0 as frame0_decoded.png, to hold next to the
badge's own frame-0 PNG (step 1.2).
"""

import argparse
import datetime
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
try:
    import testrun  # noqa: E402  (needs pyserial)
except SystemExit:
    print("pyserial missing (run inside the ESP-IDF environment: source $IDF_SOURCE)", file=sys.stderr)
    sys.exit(5)


def ffcheck(path, out_dir):
    res = {}
    probe = subprocess.run(["ffprobe", "-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries",
                            "stream=codec_name,profile,width,height,nb_read_frames,pix_fmt", "-of", "json", path],
                           capture_output=True, text=True)
    res["ffprobe_errors"] = probe.stderr.strip().splitlines()
    try:
        res["stream"] = json.loads(probe.stdout)["streams"][0]
    except (ValueError, KeyError, IndexError):
        res["stream"] = None
    dec = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "null", "-"], capture_output=True, text=True)
    res["decode_errors"] = dec.stderr.strip().splitlines()
    png = os.path.join(out_dir, "frame0_decoded.png")
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", path, "-frames:v", "1", png], capture_output=True)
    res["frame0_decoded"] = png if os.path.exists(png) else None
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.environ.get("PORT"))
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "results"))
    ap.add_argument("--no-git-check", action="store_true")
    ap.add_argument("--fetch", action="store_true", help="download the .h264 and PNG, check with ffprobe")
    ap.add_argument("--timeout", type=float, default=300)
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    testrun.set_prefix("NFM")
    if not a.port:
        print("no --port and no $PORT", file=sys.stderr)
        return 5
    params = dict(p.split("=", 1) for p in a.args if "=" in p)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    if "run" not in params:
        a.args.append(f"run={stamp}")
        params["run"] = stamp
    run_dir = os.path.join(a.out_dir, f"{stamp}-nfm-enc-{params.get('pat', 'motion')}")
    os.makedirs(run_dir, exist_ok=True)
    log = open(os.path.join(run_dir, "console.log"), "w", encoding="utf-8", buffering=1)
    result = {"args": a.args, "ready": None, "begin": None, "periods": [], "badge": None, "files": None}

    def finish(verdict, code):
        result["verdict"] = verdict
        log.close()
        with open(os.path.join(run_dir, "result.json"), "w", encoding="utf-8") as fh:
            json.dump(result, fh, indent=1)
        print(f"\nverdict: {verdict}  ->  {run_dir}/")
        return code

    port, ready = testrun.connect(a.port, 12, log)
    if port is None:
        return finish("no-connection", 1)
    result["ready"] = ready
    want = testrun.local_git_id()
    if not a.no_git_check and want and ready.get("git") != want:
        print(f"badge runs {ready.get('git')!r}, checkout is {want!r} (install first, or --no-git-check)")
        return finish("wrong-build", 5)
    port.write(b"\n" * 64 + f"RUN enc {' '.join(a.args)}\n".encode())
    port.flush()
    buffer = bytearray()
    deadline = time.time() + a.timeout
    while time.time() < deadline and result["badge"] is None:
        line = testrun.read_line(port, buffer)
        if line is None:
            continue
        log.write(line + "\n")
        kind, rec = testrun.parse_record(line)
        if kind == "ENCBEGIN":
            result["begin"] = rec
            print(f"begin: encoder took {rec['enc_sram']} B of internal SRAM, PPA client {rec['ppa_sram']} B")
        elif kind == "ENCPERIOD":
            result["periods"].append(rec)
            print(f"  n={rec['n']:4d} {rec['fps']:5.1f} fps {rec['kbit']:7.1f} kbit/s  enc max {rec['enc_max_us']} us")
        elif kind == "RESULT":
            result["badge"] = rec
    port.close()
    b = result["badge"]
    if b is None:
        return finish("timeout", 1)
    print(json.dumps(b, indent=1))
    if b.get("status") != "ok":
        return finish(b.get("status", "error"), 3)

    if a.fetch:
        bl = os.path.join(ROOT, "badgelink", "tools", "badgelink.sh")
        conn = os.environ.get("BADGELINKPORT", "")
        conn = ["--tcp", conn] if ":" in conn else ["--port", conn or a.port]
        files = {}
        time.sleep(3)  # the app returns to the launcher; BadgeLink answers there
        for name in (f"{params['run']}.h264", f"{params['run']}_f0.png"):
            local = os.path.join(run_dir, name)
            ok = subprocess.run([bl, *conn, "fs", "download", f"/sd/nfmtest/{name}", local]).returncode == 0
            files[name] = local if ok else None
        result["files"] = files
        h264 = files.get(f"{params['run']}.h264")
        if h264:
            result["check"] = ffcheck(h264, run_dir)
            print(json.dumps(result["check"], indent=1))
    return finish("ok", 0)


if __name__ == "__main__":
    sys.exit(main())
