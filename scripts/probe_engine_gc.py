#!/usr/bin/env python3
"""Probe which engine exports reclaim guest memory after a leaking workload.

Leaks N requests, prints RSS, then calls candidate cleanup APIs one by one
(via the daemon's debug 'X <name>' command) and reports RSS after each.
"""
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from test_bionic_daemon import Engine


def rss_kb(pid):
    with open("/proc/%d/status" % pid) as f:
        for line in f:
            if line.startswith("RssAnon:"):
                return int(line.split()[1])
    return 0


def run(args):
    root = Path(args.engine_dir).resolve()
    from tempfile import TemporaryDirectory
    with TemporaryDirectory(prefix="wetype-probe.") as d:
        work = Path(d) / "dict"
        (work / "userdict/v5").mkdir(parents=True)
        (work / "userdict/user_hot_word").mkdir(parents=True)
        with (Path(d) / "engine.log").open("wb") as log:
            engine = Engine(root, work, False, args.qemu, log, harness=args.harness)
            pid = engine.process.pid
            try:
                engine.command("PING", "PONG")
                engine.command("OPT spans", "OK")
                print("rss start %d kB" % rss_kb(pid))
                for i in range(args.requests):
                    engine.candidates("nihao")
                print("rss after %d requests: %d kB" % (args.requests, rss_kb(pid)))
                for name in args.apis.split(","):
                    engine.command("X " + name, "OK")
                    time.sleep(1.0)
                    print("rss after %s: %d kB" % (name, rss_kb(pid)))
                engine.candidates("nihao")
                print("rss after 1 more request: %d kB" % rss_kb(pid))
            finally:
                engine.close()


if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--engine-dir", required=True)
    p.add_argument("--harness")
    p.add_argument("--qemu")
    p.add_argument("--log")
    p.add_argument("--requests", type=int, default=15)
    p.add_argument("--apis", default="gc_sessions,read_and_clean_last_emit_log,warm_up_dict,reset_user_dict,enrich_user_dict,clear_cell_dicts,warm_up_session,reset_session,has_session")
    run(p.parse_args())
