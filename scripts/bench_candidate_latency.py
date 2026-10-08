#!/usr/bin/env python3
"""Benchmark WeType candidate response time for 1..N pinyin characters.

Talks to the ARM64 harness daemon through QEMU user mode directly (no Fcitx,
no D-Bus), using the same line protocol as the addon:
  B <keys>  -> CAND\t... | EMPTY
  C         -> OK   (reset composition by recreating the session)
  Q         -> quit

Modes:
  incremental  type one character per B request; the latency at length N is
               the time to get candidates after the N-th keystroke.
  whole        reset, then send all N characters in a single B request.

Daemon startup and C resets are excluded from the measured latency. The
daemon waits at most 2500 ms for a candidate callback, so an EMPTY response
near that value is reported as a callback timeout rather than real latency.
"""

import argparse
import csv
import os
import select
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

CALLBACK_TIMEOUT_MS = 2500
CASES = {
    "nihao": "nihao",
    "ni": "ni",
    "sentence": "nijuedexiaogouruhe",
}


def repeated(text: str, length: int) -> str:
    return (text * ((length + len(text) - 1) // len(text)))[:length]


class Daemon:
    def __init__(self, args, work_dir: str, log_path: Path):
        env = os.environ.copy()
        lib_path = f"{args.engine_dir}/lib"
        if args.native:
            # Same launch as the ARM64 plugin: the harness runs directly, its interpreter
            # already points at the bundled bionic linker (set at install time).
            lib_path += f":{args.sysroot}/system/lib64"
        env.update({
            "LD_LIBRARY_PATH": lib_path,
            "WETYPE_DICT_DIR": f"{args.engine_dir}/dicts",
            "WETYPE_ASSET_DIR": f"{args.engine_dir}/dicts",
            "WETYPE_WORK_DIR": work_dir,
            "WETYPE_HARNESS_LOG": str(log_path),
        })
        launcher = [] if args.native else [args.qemu, "-L", args.sysroot]
        command = launcher + [str(args.harness),
                              str(args.engine_dir / "lib/libwxhld_jni.so"), "--daemon"]
        self.timeout = args.timeout
        self.stderr_file = open(log_path, "a", encoding="utf-8")
        started = time.monotonic()
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.stderr_file,
                                     text=True, bufsize=1, env=env)
        ready = self.read_line(args.startup_timeout)
        if ready != "READY":
            self.close()
            raise RuntimeError(f"expected READY, received {ready!r}")
        self.startup_ms = (time.monotonic() - started) * 1000

    def read_line(self, timeout: float) -> str:
        ready, _, _ = select.select([self.proc.stdout], [], [], timeout)
        if not ready:
            raise TimeoutError(f"no protocol response after {timeout:.1f}s")
        line = self.proc.stdout.readline()
        if not line:
            raise EOFError(f"daemon exited (returncode={self.proc.poll()})")
        return line.rstrip("\r\n")

    def request(self, command: str):
        before = time.perf_counter()
        self.proc.stdin.write(command + "\n")
        self.proc.stdin.flush()
        response = self.read_line(self.timeout)
        return response, (time.perf_counter() - before) * 1000

    def reset(self) -> None:
        response, _ = self.request("C")
        if response != "OK":
            raise RuntimeError(f"C returned {response!r}")

    def close(self) -> None:
        if self.proc.poll() is None:
            try:
                self.proc.stdin.write("Q\n")
                self.proc.stdin.flush()
                self.proc.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                self.proc.kill()
                self.proc.wait()
        self.stderr_file.close()


def classify(response: str, elapsed_ms: float):
    if response.startswith("CAND\t"):
        return "CAND", len(response.split("\t")) - 1
    if response == "EMPTY":
        return ("EMPTY_TIMEOUT" if elapsed_ms >= CALLBACK_TIMEOUT_MS * 0.95 else "EMPTY"), 0
    return f"UNEXPECTED:{response[:40]}", 0


class Bench:
    def __init__(self, args):
        self.args = args
        self.rows = []
        self.daemon = None
        self.restarts = 0
        self.tmp = tempfile.TemporaryDirectory(prefix="wetype-bench-")
        self.log_path = Path(args.output).with_suffix(".stderr.log")
        self.log_path.write_text("", encoding="utf-8")

    def ensure_daemon(self) -> Daemon:
        if self.daemon is None:
            self.daemon = Daemon(self.args, self.tmp.name, self.log_path)
            print(f"# daemon ready in {self.daemon.startup_ms:.0f} ms", flush=True)
        return self.daemon

    def drop_daemon(self, exc: Exception) -> None:
        print(f"# daemon failure: {exc}; restarting (log={self.log_path})",
              file=sys.stderr, flush=True)
        if self.daemon:
            self.daemon.close()
        self.daemon = None
        self.restarts += 1

    def record(self, mode, case, rep, length, status, count, elapsed_ms):
        self.rows.append([mode, case, rep, length, status, count,
                          "" if elapsed_ms is None else f"{elapsed_ms:.2f}"])

    def run_incremental(self, case: str, text: str, rep: int, measured: bool) -> None:
        length = 0
        try:
            daemon = self.ensure_daemon()
            daemon.reset()
            for length, char in enumerate(text, 1):
                response, ms = daemon.request(f"B {char}")
                status, count = classify(response, ms)
                if measured:
                    self.record("incremental", case, rep, length, status, count, ms)
                if status.startswith("UNEXPECTED"):
                    break
        except (EOFError, TimeoutError, BrokenPipeError, OSError, RuntimeError) as exc:
            if measured:
                self.record("incremental", case, rep, length, "CRASH_OR_TIMEOUT", 0, None)
            self.drop_daemon(exc)

    def run_whole(self, case: str, text: str, rep: int, measured: bool) -> None:
        for length in range(1, len(text) + 1):
            try:
                daemon = self.ensure_daemon()
                daemon.reset()
                response, ms = daemon.request(f"B {text[:length]}")
                status, count = classify(response, ms)
            except (EOFError, TimeoutError, BrokenPipeError, OSError, RuntimeError) as exc:
                status, count, ms = "CRASH_OR_TIMEOUT", 0, None
                self.drop_daemon(exc)
            if measured:
                self.record("whole", case, rep, length, status, count, ms)

    def run(self) -> None:
        args = self.args
        runners = {"incremental": self.run_incremental, "whole": self.run_whole}
        for mode in args.modes:
            for case in args.cases:
                text = repeated(CASES[case], args.max_length)
                print(f"# mode={mode} case={case} chars={len(text)} "
                      f"warmup={args.warmup} reps={args.reps}", flush=True)
                for rep in range(-args.warmup, args.reps):
                    runners[mode](case, text, rep, measured=rep >= 0)
        if self.daemon:
            self.daemon.close()
        self.tmp.cleanup()


def percentile(values, q):
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, round(q * (len(ordered) - 1))))
    return ordered[index]


def summarize(rows, modes, cases, max_length):
    print()
    for mode in modes:
        for case in cases:
            print(f"== mode={mode} case={case} (ms) ==")
            print(f"{'len':>3} {'min':>8} {'median':>8} {'p90':>8} {'max':>8} "
                  f"{'cands':>5} {'ok/n':>6}  notes")
            for length in range(1, max_length + 1):
                sel = [r for r in rows if r[0] == mode and r[1] == case and r[3] == length]
                if not sel:
                    continue
                ok = [float(r[6]) for r in sel if r[4] == "CAND"]
                counts = [r[5] for r in sel if r[4] == "CAND"]
                bad = sorted({r[4] for r in sel if r[4] != "CAND"})
                if ok:
                    print(f"{length:>3} {min(ok):>8.1f} {statistics.median(ok):>8.1f} "
                          f"{percentile(ok, 0.9):>8.1f} {max(ok):>8.1f} "
                          f"{round(statistics.median(counts)):>5} {len(ok):>3}/{len(sel):<2}  "
                          f"{','.join(bad)}")
                else:
                    print(f"{length:>3} {'-':>8} {'-':>8} {'-':>8} {'-':>8} {'-':>5} "
                          f"{0:>3}/{len(sel):<2}  {','.join(bad)}")
            print()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--engine-dir", type=Path, default=Path("/usr/lib/wetype-ime/arm64"))
    parser.add_argument("--harness", type=Path, default=None)
    parser.add_argument("--qemu", default="qemu-aarch64-static")
    parser.add_argument("--native", action="store_true",
                        help="run the harness directly on an ARM64 host (no QEMU)")
    parser.add_argument("--sysroot", default=None,
                        help="bionic runtime passed to qemu -L (default: <engine-dir>/sysroot)")
    parser.add_argument("--min-length", type=int, default=1,
                        help="shortest length included in the summary")
    parser.add_argument("--max-length", type=int, default=60)
    parser.add_argument("--modes", nargs="+", choices=["incremental", "whole"],
                        default=["incremental", "whole"])
    parser.add_argument("--cases", nargs="+", choices=list(CASES), default=list(CASES))
    parser.add_argument("--reps", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="per-request protocol timeout in seconds")
    parser.add_argument("--startup-timeout", type=float, default=60.0)
    parser.add_argument("--output", default="/tmp/wetype-candidate-latency.csv")
    args = parser.parse_args()
    args.engine_dir = args.engine_dir.resolve()
    args.harness = (args.harness or args.engine_dir / "wetype-harness").resolve()
    args.sysroot = args.sysroot or str(args.engine_dir / "sysroot")
    if not 1 <= args.min_length <= args.max_length <= 500:
        parser.error("need 1 <= --min-length <= --max-length <= 500")
    if args.reps < 1 or args.warmup < 0:
        parser.error("--reps must be >= 1 and --warmup >= 0")
    for required in (args.harness, args.engine_dir / "lib/libwxhld_jni.so",
                     args.engine_dir / "dicts"):
        if not required.exists():
            parser.error(f"required engine path does not exist: {required}")

    bench = Bench(args)
    bench.run()
    rows = [r for r in bench.rows if r[3] >= args.min_length]
    with open(args.output, "w", newline="", encoding="utf-8") as output_file:
        writer = csv.writer(output_file)
        writer.writerow(["mode", "case", "rep", "length", "status", "candidates", "elapsed_ms"])
        writer.writerows(rows)
    summarize(rows, args.modes, args.cases, args.max_length)
    print(f"# daemon restarts: {bench.restarts}")
    print(f"# CSV: {args.output}")
    print(f"# stderr log: {bench.log_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
