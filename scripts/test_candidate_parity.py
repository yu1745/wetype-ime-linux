#!/usr/bin/env python3
"""Compare complete CAND protocol lines from two harnesses on a private engine.

Both runs use independent empty user dictionaries. No installed files are changed.
Example (inside make_leak_lab.sh's wrun):
  python3 /wt/scripts/test_candidate_parity.py --engine-dir /ws/engine \
      --before /ws/bin/jinterop-baseline --after /ws/bin/jinterop
"""
import argparse
from pathlib import Path
import tempfile

from test_bionic_daemon import Engine


KEYS = ("nihao", "zhongguo", "nihaozhongguo", "xiao", "n")


def capture(args, harness, base, label):
    work = base / label / "dict"
    (work / "userdict/v5").mkdir(parents=True)
    (work / "userdict/user_hot_word").mkdir(parents=True)
    with (base / (label + ".log")).open("wb") as log:
        engine = Engine(Path(args.engine_dir).resolve(), work, False, args.qemu,
                        log, Path(harness).resolve())
        try:
            engine.command("OPT spans", "OK")
            result = []
            for keys in KEYS:
                engine.command("C", "OK")
                result.append(engine.command("B " + keys, "CAND").encode("utf-8"))
            return result
        finally:
            engine.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-dir", required=True)
    parser.add_argument("--before", required=True, help="baseline harness")
    parser.add_argument("--after", required=True, help="fixed harness")
    parser.add_argument("--qemu")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="wetype-parity.") as directory:
        base = Path(directory)
        before = capture(args, args.before, base, "before")
        after = capture(args, args.after, base, "after")
        for keys, old, new in zip(KEYS, before, after):
            assert old == new, "CAND mismatch for %s:\nbefore=%r\nafter=%r" % (keys, old, new)
            print("PASS %s: %d bytes, complete CAND identical" % (keys, len(new)), flush=True)
    print("PARITY_RESULT = PASS", flush=True)


if __name__ == "__main__":
    main()
