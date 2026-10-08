#!/usr/bin/env python3
"""Probe WeType's candidate/engine behavior as pinyin composition grows.

Each case gets a fresh daemon/session and isolated user dictionary. Characters
are sent incrementally using the same B protocol as the Fcitx addon. Results
are printed as CSV; daemon stderr is saved beside the CSV for crash diagnosis.
"""

import argparse
import csv
import os
import select
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def repeated(text: str, length: int) -> str:
    return (text * ((length + len(text) - 1) // len(text)))[:length]


def read_line(proc: subprocess.Popen, timeout: float) -> str:
    ready, _, _ = select.select([proc.stdout], [], [], timeout)
    if not ready:
        raise TimeoutError(f"no protocol response after {timeout:.1f}s")
    line = proc.stdout.readline()
    if not line:
        raise EOFError(f"daemon exited (returncode={proc.poll()})")
    return line.rstrip("\r\n")


def classify(response: str):
    if response == "OK":
        return "OK", 0
    if response.startswith("CAND\t"):
        return "CAND", len(response.split("\t")) - 1
    if response == "EMPTY":
        return "EMPTY", 0
    return f"UNEXPECTED:{response[:40]}", 0


def run_case(name: str, sequence: str, batch_size: int, args, result_rows: list) -> None:
    with tempfile.TemporaryDirectory(prefix=f"wetype-{name}-") as work_dir:
        log_path = Path(args.output).with_name(f"{Path(args.output).stem}-{name}.stderr.log")
        env = os.environ.copy()
        env.update({
            "LD_LIBRARY_PATH": f"{args.engine_dir}/lib",
            "WETYPE_DICT_DIR": f"{args.engine_dir}/dicts",
            "WETYPE_ASSET_DIR": f"{args.engine_dir}/dicts",
            "WETYPE_WORK_DIR": work_dir,
            "WETYPE_HARNESS_LOG": str(log_path),
        })
        env.setdefault("QEMU_CPU", "cortex-a72")   # same default as the plugin
        command = [args.qemu, "-L", args.sysroot, str(args.harness),
                   str(args.engine_dir / "lib/libwxhld_jni.so"), "--daemon"]
        started = time.monotonic()
        with open(log_path, "w", encoding="utf-8") as stderr_file:
            proc = subprocess.Popen(command, stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE, stderr=stderr_file,
                                    text=True, bufsize=1, env=env)
            try:
                ready = read_line(proc, args.timeout)
                if ready != "READY":
                    raise RuntimeError(f"expected READY, received {ready!r}")
                print(f"# case={name} chars={len(sequence)} batch={batch_size}", flush=True)
                length = 0

                def request(command: str, phase: str, prefix: str) -> str:
                    nonlocal length
                    before = time.monotonic()
                    proc.stdin.write(command + "\n")
                    proc.stdin.flush()
                    response = read_line(proc, args.timeout)
                    elapsed_ms = (time.monotonic() - before) * 1000
                    status, count = classify(response)
                    row = [name, batch_size, length, phase, prefix, status, count,
                           f"{elapsed_ms:.1f}", ""]
                    result_rows.append(row)
                    print(",".join(map(str, row[:-1])))
                    return status

                if args.backspace_every_length:
                    for length, char in enumerate(sequence, 1):
                        prefix = sequence[:length]
                        status = request(f"B {char}", "TYPE", prefix)
                        if status.startswith("UNEXPECTED:"):
                            break
                        if request("C", "BACKSPACE_CLEAR", sequence[:length - 1]) != "OK":
                            raise RuntimeError("C did not return OK during backspace simulation")
                        if length > 1:
                            status = request(f"B {sequence[:length - 1]}",
                                             "BACKSPACE_REPLAY", sequence[:length - 1])
                            if status.startswith("UNEXPECTED:"):
                                break
                        status = request(f"B {char}", "RETYPE", prefix)
                        if status.startswith("UNEXPECTED:"):
                            break
                else:
                    while length < len(sequence):
                        chunk = sequence[length:length + batch_size]
                        length += len(chunk)
                        status = request(f"B {chunk}", "TYPE", sequence[:length])
                        if status.startswith("UNEXPECTED:"):
                            break
                proc.stdin.write("Q\n")
                proc.stdin.flush()
                try:
                    read_line(proc, args.timeout)
                except (EOFError, TimeoutError):
                    pass
                proc.wait(timeout=args.timeout)
            except (EOFError, TimeoutError, BrokenPipeError, OSError) as exc:
                code = proc.poll()
                result_rows.append([name, batch_size, length, "CRASH_OR_TIMEOUT",
                                    sequence[:length], "CRASH_OR_TIMEOUT", 0, "", str(exc)])
                print(f"# {name}: CRASH_OR_TIMEOUT: {exc}; returncode={code}; log={log_path}",
                      file=sys.stderr, flush=True)
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
            finally:
                if proc.stdin and not proc.stdin.closed:
                    proc.stdin.close()
        print(f"# {name}: elapsed={time.monotonic() - started:.1f}s log={log_path}",
              flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-dir", type=Path,
                        default=Path("/usr/lib/wetype-ime/arm64"))
    parser.add_argument("--harness", type=Path, default=None)
    parser.add_argument("--qemu", default="qemu-aarch64-static")
    parser.add_argument("--sysroot", default=None,
                        help="bionic runtime passed to qemu -L (default: <engine-dir>/sysroot)")
    parser.add_argument("--max-length", type=int, default=32)
    parser.add_argument("--batch-sizes", type=int, nargs="+", default=[1, 4],
                        help="characters sent per B request (addon normally caps batches at 4)")
    parser.add_argument("--backspace-every-length", action="store_true",
                        help="at each length, simulate Backspace (C + replay) and retype")
    parser.add_argument("--cases", nargs="+",
                        choices=["nihao_repeat", "ni_repeat", "sentence_ni_juede_xiaogouruhe"],
                        default=None)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--output", default="/tmp/wetype-length-threshold.csv")
    args = parser.parse_args()
    args.engine_dir = args.engine_dir.resolve()
    args.harness = (args.harness or args.engine_dir / "wetype-harness").resolve()
    args.sysroot = args.sysroot or str(args.engine_dir / "sysroot")
    if args.max_length < 1:
        parser.error("--max-length must be positive")
    for required in (args.harness, args.engine_dir / "lib/libwxhld_jni.so",
                     args.engine_dir / "dicts"):
        if not required.exists():
            parser.error(f"required engine path does not exist: {required}")

    cases = {
        "nihao_repeat": repeated("nihao", args.max_length),
        "ni_repeat": repeated("ni", args.max_length),
        "sentence_ni_juede_xiaogouruhe": repeated("nijuede" + "xiaogouruhe",
                                                  args.max_length),
    }
    rows = []
    print("case,batch_size,length,phase,prefix,status,candidates,elapsed_ms")
    for batch_size in args.batch_sizes:
        if batch_size < 1 or batch_size > 16:
            parser.error("batch sizes must be between 1 and 16")
        selected_cases = args.cases or list(cases)
        for name in selected_cases:
            run_case(name, cases[name], batch_size, args, rows)
    with open(args.output, "w", newline="", encoding="utf-8") as output_file:
        writer = csv.writer(output_file)
        writer.writerow(["case", "batch_size", "length", "phase", "prefix", "status",
                         "candidates", "elapsed_ms", "detail"])
        writer.writerows(rows)
    print(f"# CSV: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
