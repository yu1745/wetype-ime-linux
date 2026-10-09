#!/usr/bin/env python3
"""Memory-leak regression for the wetype engine daemon.

Drives the daemon with repeated batch input while sampling the anonymous
RSS of the emulator (or native) process, then reports per-request growth.

Usage:
  python3 scripts/test_memory_leak.py --engine-dir ~/.local/lib/wetype-ime/arm64
Add --harness path to test a freshly built harness/jinterop.
Add --rounds/--keys to scale. Default reproduces the ~1.1 MB/request leak.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).parent))
from test_bionic_daemon import Engine, disable_core_dumps  # noqa: E402


def anon_kb(pid):
    try:
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                if line.startswith("RssAnon:"):
                    return int(line.split()[1])
    except OSError:
        return None
    return None


def sample(pid):
    kb = anon_kb(pid)
    return kb


def run(args):
    root = Path(args.engine_dir).resolve()
    harness = Path(args.harness).resolve() if args.harness else root / "wetype-harness"
    with tempfile.TemporaryDirectory(prefix="wetype-memtest.") as directory:
        work = Path(directory) / "dict"
        (work / "userdict/v5").mkdir(parents=True)
        (work / "userdict/user_hot_word").mkdir(parents=True)
        log_path = Path(args.log).resolve() if args.log else Path(directory) / "engine.log"
        with log_path.open("wb") as log:
            engine = Engine(root, work, args.native, args.qemu, log, harness=harness)
            pid = engine.process.pid
            try:
                engine.command("PING", "PONG")
                engine.command("OPT spans", "OK")
                # Warm-up: load dictionaries, settle background init.
                engine.candidates(args.keys)
                engine.command("C", "OK")
                time.sleep(args.idle)

                reqs = 0
                marks = []   # (requests, anon_kb)
                marks.append((reqs, sample(pid)))
                rounds = args.rounds
                per_round = len(args.keys)   # one B command per key in batch mode? No: one B per key
                for r in range(rounds):
                    engine.candidates(args.keys)
                    reqs += 1
                    marks.append((reqs, sample(pid)))
                engine.command("C", "OK")
                settle = [sample(pid) for _ in range(args.idle_samples)]
                time.sleep(1)
                after_reset = sample(pid)

                base = marks[0][1]
                last = marks[-1][1]
                growth = last - base
                per_req = growth / max(1, reqs)
                print("rounds=%d requests=%d" % (rounds, reqs))
                print("anon: start=%d kB end=%d kB growth=%d kB (%.0f kB/req)"
                      % (base, last, growth, per_req))
                print("after C reset: %d kB" % after_reset)
                # first vs second half slope, to show linear (leak) vs flattening (cache)
                half = len(marks) // 2
                h1 = (marks[half][1] - marks[0][1]) / max(1, marks[half][0])
                h2 = (marks[-1][1] - marks[half][1]) / max(1, marks[-1][0] - marks[half][0])
                print("slope first half=%.0f kB/req second half=%.0f kB/req" % (h1, h2))
                verdict = "LEAK" if per_req > args.threshold else "OK"
                print("MEMTEST_RESULT = %s (%.0f kB/req, threshold %d)"
                      % (verdict, per_req, args.threshold))
                return 0 if verdict == "OK" else 1
            finally:
                engine.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-dir", required=True)
    parser.add_argument("--harness", help="override harness binary under test")
    parser.add_argument("--native", action="store_true")
    parser.add_argument("--qemu")
    parser.add_argument("--rounds", type=int, default=60)
    parser.add_argument("--keys", default="nihao")
    parser.add_argument("--idle", type=float, default=3.0,
                        help="settle time after warm-up before measuring")
    parser.add_argument("--idle-samples", type=int, default=0)
    parser.add_argument("--threshold", type=int, default=100,
                        help="kB per request above which the run is flagged as a leak")
    parser.add_argument("--log")
    args = parser.parse_args()
    sys.exit(run(args))
