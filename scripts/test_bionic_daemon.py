#!/usr/bin/env python3
"""Headless engine regression, using only an isolated temporary user dictionary.

Usage: python3 scripts/test_bionic_daemon.py --engine-dir /path/to/installed/arm64
Add --native on ARM64 Linux to run the harness without QEMU. It uses a temporary copy
whose ELF interpreter is set with patchelf to the bundled bionic linker64.
Do not change /system or patch an installed production harness.
Add --idle-seconds 85 to cover delayed/background engine work.
The same test can exercise the legacy glibc engine through its bundled QEMU.
"""
import argparse
import os
from pathlib import Path
import selectors
import resource
import shutil
import subprocess
import tempfile
import time


def disable_core_dumps():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


class Engine:
    def __init__(self, root, work, native, qemu, log):
        env = os.environ.copy()
        lib = root / "lib"
        sysroot = root / "sysroot"
        env.update(LD_LIBRARY_PATH=str(lib), WETYPE_DICT_DIR=str(root / "dicts"),
                   WETYPE_ASSET_DIR=str(root / "dicts"), WETYPE_WORK_DIR=str(work))
        harness = root / "wetype-harness"
        if native:
            # Install-time step on ARM64 hosts: the harness interpreter points at the
            # bundled bionic linker. Patch a temporary copy, never the installed harness.
            env["LD_LIBRARY_PATH"] += ":" + str(sysroot / "system/lib64")
            harness = work.parent / "wetype-harness"
            shutil.copy2(root / "wetype-harness", harness)
            subprocess.run(["patchelf", "--set-interpreter", str(sysroot / "system/bin/linker64"),
                            str(harness)], check=True)
            command = []
        else:
            launcher = qemu or str(root / "qemu-aarch64-static")
            command = [launcher, "-L", str(sysroot)]
        command += [str(harness), str(lib / "libwxhld_jni.so"), "--daemon"]
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=log, env=env, cwd="/tmp", bufsize=0,
                                        preexec_fn=disable_core_dumps)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.buffer = b""
        self.log = log
        try:
            self.wait_for("READY", 30)
        except BaseException:
            self.process.kill()
            self.process.wait()
            self.selector.close()
            self.process.stdin.close()
            self.process.stdout.close()
            raise

    def line(self, timeout):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not self.selector.select(remaining):
                raise AssertionError("engine response timed out")
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if not chunk:
                raise AssertionError("engine exited before response, rc=%s" % self.process.poll())
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return line.decode("utf-8", errors="strict").rstrip("\r")

    def wait_for(self, prefix, timeout=15):
        deadline = time.monotonic() + timeout
        while True:
            line = self.line(max(0, deadline - time.monotonic()))
            if line == prefix or line.startswith(prefix + "\t"):
                return line
            self.log.write(("[stdout] " + line + "\n").encode())
            self.log.flush()
            if line in ("ERR", "EMPTY", "BYE"):
                raise AssertionError("unexpected response: " + line)

    def command(self, command, expected):
        self.process.stdin.write((command + "\n").encode())
        self.process.stdin.flush()
        return self.wait_for(expected)

    def candidates(self, keys):
        line = self.command("B " + keys, "CAND")
        candidates = []
        for item in line.split("\t")[1:]:
            cover, text = item.split(":", 1)
            candidates.append((int(cover), text))
        assert candidates, "no candidates"
        return candidates

    def close(self):
        try:
            if self.process.poll() is None:
                self.command("Q", "BYE")
                assert self.process.wait(timeout=15) == 0
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.selector.close()
            self.process.stdin.close()
            self.process.stdout.close()


def run(args):
    root = Path(args.engine_dir).resolve()
    with tempfile.TemporaryDirectory(prefix="wetype-daemon-test.") as directory:
        work = Path(directory) / "dict"
        (work / "userdict/v5").mkdir(parents=True)
        (work / "userdict/user_hot_word").mkdir(parents=True)
        log_path = Path(args.log).resolve() if args.log else Path(directory) / "engine.log"
        with log_path.open("wb") as log:
            engine = Engine(root, work, args.native, args.qemu, log)
            try:
                engine.command("PING", "PONG")
                engine.command("OPT spans", "OK")
                # Sequential requests exercise every intermediate JNI callback.
                words = []
                for key in "nihao":
                    words = engine.candidates(key)
                assert words[0] == (5, "你好"), words[:3]
                print("PASS startup, ping, sequential nihao", flush=True)

                engine.command("C", "OK")
                words = engine.candidates("zhongguo")
                assert words[0] == (8, "中国"), words[:3]
                engine.command("S 0", "OK")
                print("PASS batch input, full selection", flush=True)

                engine.command("C", "OK")
                words = engine.candidates("nihaozhongguo")
                partial = next((i for i, (cover, text) in enumerate(words)
                                if cover == 5 and text == "你好"), None)
                assert partial is not None, "expected a partial 你好 candidate"
                remaining = engine.command("S %d" % partial, "CAND")
                assert "8:中国" in remaining.split("\t")[1:], remaining
                engine.command("S 0", "OK")
                print("PASS partial selection and remaining candidates", flush=True)

                engine.command("C", "OK")
                words = engine.candidates("nihao")
                learned = next((i for i, (cover, text) in enumerate(words)
                                if cover == 5 and text == "拟好"), None)
                assert learned is not None, "expected learnable 拟好 candidate"
                engine.command("S %d" % learned, "OK")
                # Selection acknowledgment precedes asynchronous learning work.
                time.sleep(0.5)
                engine.command("C", "OK")
                time.sleep(0.5)
                words = engine.candidates("nihao")
                learned_rank = next((i for i, item in enumerate(words)
                                     if item == (5, "拟好")), None)
                assert learned_rank is not None and learned_rank < learned, words[:5]
                engine.command("SAVE", "OK")
                print("PASS selection learning, reset, SAVE", flush=True)

                if args.idle_seconds:
                    engine.command("C", "OK")
                    time.sleep(args.idle_seconds)
                    engine.command("PING", "PONG")
                    assert engine.candidates("zhongguo")[0] == (8, "中国")
                    print("PASS idle %gs and subsequent input" % args.idle_seconds, flush=True)
            finally:
                engine.close()
            # Restart against the same isolated data to prove persistence.
            engine = Engine(root, work, args.native, args.qemu, log)
            try:
                engine.command("OPT spans", "OK")
                words = engine.candidates("nihao")
                persisted_rank = next((i for i, item in enumerate(words)
                                       if item == (5, "拟好")), None)
                assert persisted_rank is not None and persisted_rank <= learned_rank, words[:5]
                print("PASS restart preserves learned candidate", flush=True)
            finally:
                engine.close()
        print("E2E_RESULT = PASS", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-dir", required=True)
    parser.add_argument("--native", action="store_true")
    parser.add_argument("--qemu")
    parser.add_argument("--idle-seconds", type=float, default=0)
    parser.add_argument("--log")
    arguments = parser.parse_args()
    if arguments.idle_seconds < 0:
        parser.error("idle-seconds must be nonnegative")
    run(arguments)
