#!/usr/bin/env python3
"""Regression for native iterator ownership in the bundled engine's daemon.

Uses the engine's allocation/deletion log records (wxime.cpp/c_callback_holder.h)
for this build. A missing JNI deleter or a second, ignored native listener fails
this test even when a short RSS test could mistake the leak for allocator cache.
All user data lives in a private temporary directory.
"""
import argparse
from collections import Counter
from pathlib import Path
import re
import tempfile

from test_bionic_daemon import Engine
from test_candidate_parity import KEYS


ALLOC = re.compile(r"malloc new_iterator: (\d+), cloud_iterator: (\d+)")
FREE = re.compile(r"\{ wxime_delete_candidate_iterator \[(\d+)\]")


def check_log(text):
    allocated = Counter(int(value) for pair in ALLOC.findall(text) for value in pair
                        if int(value))
    released = Counter(map(int, FREE.findall(text)))
    assert allocated, "engine allocation records missing; check engine version/log configuration"
    assert allocated == released, "iterator ownership mismatch: outstanding=%s excess_deletes=%s" % (
        allocated - released, released - allocated)
    return sum(allocated.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-dir", required=True)
    parser.add_argument("--harness")
    parser.add_argument("--qemu")
    parser.add_argument("--rounds", type=int, default=20)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    with tempfile.TemporaryDirectory(prefix="wetype-iterator.") as directory:
        base = Path(directory)
        work = base / "dict"
        (work / "userdict/v5").mkdir(parents=True)
        (work / "userdict/user_hot_word").mkdir(parents=True)
        log_path = base / "engine.log"
        with log_path.open("wb") as log:
            engine = Engine(Path(args.engine_dir).resolve(), work, False, args.qemu, log,
                            Path(args.harness).resolve() if args.harness else None)
            try:
                engine.command("OPT spans", "OK")
                for i in range(args.rounds):
                    engine.command("C", "OK")
                    engine.candidates(KEYS[i % len(KEYS)])
                engine.command("C", "OK")
            finally:
                engine.close()
        total = check_log(log_path.read_text())
    print("ITERATOR_RESULT = PASS (allocated=%d deleted=%d, %d requests)" % (
        total, total, args.rounds), flush=True)


if __name__ == "__main__":
    main()
