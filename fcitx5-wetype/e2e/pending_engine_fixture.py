#!/usr/bin/env python3
"""Controlled protocol responder, used ONLY via QEMU_AARCH64 in private e2e.

Ignores QEMU arguments; does not load the proprietary engine. Real-engine
regressions live in pending_space_test.py. This fixture covers replies that
cannot be produced deterministically by the real engine.
"""
import os
import sys
import time

mode = os.environ["WETYPE_PENDING_FIXTURE"]
if mode == "startup-failure":
    sys.exit(1)
print("READY", flush=True)
buffer = ""
for line in sys.stdin:
    command = line.rstrip("\n")
    if command == "Q":
        break
    if command == "C":
        buffer = ""
        response = "OK"
    elif command.startswith("B "):
        buffer += command[2:]
        time.sleep(0.08)
        # Earlier-prefix candidates must not satisfy the pending Space.
        if len(buffer) < 6:
            response = "CAND\t%d:旧" % len(buffer)
        else:
            response = {"empty": "EMPTY", "error": "ERR", "skip": "SKIP"}[mode]
    else:
        response = "OK"
    print(response, flush=True)
