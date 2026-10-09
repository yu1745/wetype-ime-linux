#!/usr/bin/env python3
"""用 malloc 拦截器驱动引擎 N 次搜索，捕获 [mtrace] 输出。

用法（在 wrun 容器里）:
  python3 /ws/tmp/drive_mtrace.py --rounds 20 --every 20000
输出 /ws/tmp/mtrace.log（stderr），stdout 协议行打进日志。
"""
import argparse, os, subprocess, sys, time
from pathlib import Path

root = Path("/ws/engine")
harness = Path("/ws/bin/jinterop")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=20)
    ap.add_argument("--every", type=str, default="20000")
    ap.add_argument("--keys", default="nihao")
    ap.add_argument("--stackdump", default="")
    ap.add_argument("--owners", default="")
    ap.add_argument("--probe", default="")
    ap.add_argument("--xcmds", default="")
    args = ap.parse_args()

    env = os.environ.copy()
    lib = root / "lib"
    env.update(LD_LIBRARY_PATH=str(lib), WETYPE_DICT_DIR=str(root / "dicts"),
               WETYPE_ASSET_DIR=str(root / "dicts"),
               WETYPE_WORK_DIR="/ws/tmp/work")
    Path("/ws/tmp/work/userdict/v5").mkdir(parents=True, exist_ok=True)
    Path("/ws/tmp/work/userdict/user_hot_word").mkdir(parents=True, exist_ok=True)
    env["MTRACE_EVERY"] = args.every

    logf = open("/ws/tmp/mtrace.log", "wb")
    cmd = [str(root / "qemu-aarch64-static"), "-L", str(root / "sysroot"),
           "-E", "LD_PRELOAD=/ws/engine/lib/libmtrace2.so",
           "-E", "MTRACE_EVERY=" + args.every]
    if args.stackdump:
        cmd += ["-E", "MTRACE_STACKDUMP=" + args.stackdump]
    if args.owners:
        cmd += ["-E", "MTRACE_OWNERS=" + args.owners]
    if args.probe:
        cmd += ["-E", "MTRACE_PROBE=" + args.probe]
    cmd += [str(harness), str(lib / "libwxhld_jni.so"), "--daemon"]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=logf, env=env, cwd="/tmp", bufsize=0)

    def wait(prefix, timeout=60):
        deadline = time.monotonic() + timeout
        buf = b""
        while time.monotonic() < deadline:
            r = select_reader(p.stdout, 0.2)
            if r:
                buf += r
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if line.startswith(prefix.encode()):
                        return line.decode()
        raise SystemExit("timeout waiting " + prefix)

    import select as _select
    def select_reader(f, t):
        r, _, _ = _select.select([f], [], [], t)
        if r:
            return os.read(f.fileno(), 65536)
        return b""

    wait("READY", 400)
    wait("PONG") if False else None
    p.stdin.write(b"PING\n"); p.stdin.flush(); wait("PONG")
    p.stdin.write(b"OPT spans\n"); p.stdin.flush(); wait("OK")
    # warm-up
    p.stdin.write(("B " + args.keys + "\n").encode()); p.stdin.flush(); wait("CAND")
    p.stdin.write(b"C\n"); p.stdin.flush(); wait("OK")
    time.sleep(2)
    for i in range(args.rounds):
        p.stdin.write(("B " + args.keys + "\n").encode()); p.stdin.flush(); wait("CAND")
        # no C reset (official leak-test conditions)
    # 逐个试清理 API，各跟一次搜索触发 dump
    if args.xcmds:
        for api in args.xcmds.split(","):
            p.stdin.write(("X " + api + "\n").encode()); p.stdin.flush()
            print("=== X %s -> %s" % (api, wait("OK")), flush=True)
            time.sleep(1.5)
            p.stdin.write(("B " + args.keys + "\n").encode()); p.stdin.flush(); wait("CAND")
            time.sleep(0.5)
    p.stdin.write(b"Q\n"); p.stdin.flush()
    try:
        p.wait(timeout=30)
    except subprocess.TimeoutExpired:
        p.kill()
    logf.close()
    print("done, see /ws/tmp/mtrace.log")

main()
