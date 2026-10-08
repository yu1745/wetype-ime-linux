#!/usr/bin/env python3
"""Real-engine pending-Space regressions; only the private runner is allowed."""
import traceback
import os
import signal
import time
from pathlib import Path
from dbus_test_client import Client, controller, paused_engine, pump

if os.environ.get("WETYPE_E2E_KEYBOARD") != "1":
    raise RuntimeError("Set WETYPE_E2E_KEYBOARD=1 for the isolated IM-switch fixture")

failures = []


def check(condition, detail):
    assert condition, detail


def settle(seconds=1.5):
    pump(seconds)


def fresh():
    c = Client("wetype-pending-space")
    c.focus()
    # Warm up the isolated harness before SIGSTOP.
    c.type("nihao")
    c.press("esc")
    c.commits.clear()
    c.events.clear()
    c.forwarded.clear()
    return c


def pending(c, text="nihao"):
    c.type(text, wait=False)
    check(c.press("space", delay=0), "initial Space must be consumed")


def run(name, test):
    c = None
    try:
        c = fresh()
        test(c)
        print("PASS " + name, flush=True)
    except Exception:
        failures.append(name)
        print("FAIL " + name, flush=True)
        traceback.print_exc()
        if c is not None:
            print("FAIL candidate snapshot serial=%d: %r" % (c.ui_serial, c.candidates), flush=True)
        print("--- private Fcitx diagnostic tail ---", flush=True)
        print("\n".join(Path(os.environ["WETYPE_PAGETEST_LOG"]).read_text(errors="replace").splitlines()[-100:]), flush=True)
    finally:
        if c is not None:
            try:
                c.ic.DestroyIC()
                pump()
            except Exception:
                pass


def baseline(c):
    c.type("nihao")
    c.press("space")
    c.type("zhongguo")
    c.press("space")
    settle()
    expected = list(c.commits)
    check(len(expected) == 2, "baseline needs two selected segments: %r" % expected)
    c.commits.clear()
    with paused_engine():
        pending(c)
        c.type("zhongguo", wait=False)
        c.press("space", delay=0)
        check(not c.commits, "pending must not select stale/preview candidates")
    settle(3)
    check(c.commits == expected, "pending versus baseline: %r != %r" % (c.commits, expected))


def raw(c, action, expected="nihao zhongguo"):
    with paused_engine():
        pending(c)
        c.type("zhongguo", wait=False)
        action(c)
        settle(.2)
        check(c.commits == [expected], "raw fallback: %r" % c.commits)
    settle()
    check(c.commits == [expected], "late reply duplicated commit: %r" % c.commits)


def cancel(c, action):
    with paused_engine():
        pending(c)
        c.type("zhongguo", wait=False)
        action(c)
    settle()
    check(not c.commits, "cancel committed: %r" % c.commits)
    c.focus()
    c.type("nihao")
    c.press("space")
    check(c.commits == ["你好"], "new composition retained pending state: %r" % c.commits)


def backspace(c, queued):
    with paused_engine():
        pending(c)
        if queued:
            c.type("ab", wait=False)
        c.press("backspace", delay=0)
        c.press("enter")
        expected = "nihao a" if queued else "niha "
        check(c.commits == [expected], "Backspace raw result: %r" % c.commits)
    settle()
    check(c.commits == [expected], "late response after Backspace")


def repeats(c):
    with paused_engine():
        pending(c)
        for _ in range(3):
            c.press("space", delay=0)
    settle(3)
    check(c.commits == ["你好"], "repeated Space selected extra segments: %r" % c.commits)
    check([e[0] for e in c.forwarded] == [32, 32, 32],
          "literal spaces must be forwarded FIFO: %r" % c.forwarded)
    check(c.events[0] == ("commit", "你好"), "forwarding preceded selection")


def releases(c):
    with paused_engine():
        pending(c)
        c.press("space", delay=0)
        c.press("space", delay=0, release=True)
    settle(3)
    check(c.commits == ["你好"], "release caused selection")
    check(len(c.forwarded) == 2 and all(e[0] == 32 for e in c.forwarded),
          "queued press/release lost: %r" % c.forwarded)
    check(not c.forwarded[0][-1] and c.forwarded[1][-1],
          "press/release order: %r" % c.forwarded)


def timeout(c):
    with paused_engine():
        pending(c)
        c.type("abc", wait=False)
        settle(5.7)
        check(c.commits == ["nihao abc"], "timeout raw fallback: %r" % c.commits)
    settle()
    check(c.commits == ["nihao abc"], "late timeout reply selected")


def partial(c):
    def locate():
        c.type("nihaoshijie")
        seen = set()
        while tuple(c.candidates) not in seen:
            seen.add(tuple(c.candidates))
            for i, (_, text) in enumerate(c.candidates):
                if text.split() in (["你好"], [str(i + 1), "你好"]):
                    return i
            c.press("=")
            c.synchronize_ui()
        raise AssertionError("fixture setup: no partial 你好 candidate on any page: %r" % (seen,))

    index = locate()
    c.press(str(index + 1))
    settle()
    check(c.commits == ["你好"], "partial numeric selection: %r" % c.commits)
    c.press("space")
    settle()
    expected = list(c.commits)
    check(len(expected) == 2, "fixture remainder did not select: %r" % expected)
    c.commits.clear()
    index = locate()
    with paused_engine():
        c.synchronize_ui()
        check(c.candidates[index][1].split() in (["你好"], [str(index + 1), "你好"]),
              "candidate changed before mouse selection: %r" % c.candidates)
        c.ic.SelectCandidate(index)
        pump(.1)
        check(c.commits == ["你好"], "partial mouse selection: %r" % c.commits)
        c.press("space", delay=0)
        check(c.commits == ["你好"], "Space used preview before S remainder arrived")
    settle(3)
    check(c.commits == expected, "pending remainder versus baseline: %r != %r" % (c.commits, expected))


run("baseline / pending FIFO and preview rejection", baseline)
run("repeated spaces become literal", repeats)
run("queued press/release ordering", releases)
run("partial selection preserves remainder", partial)
run("queued Backspace", lambda c: backspace(c, True))
run("frozen-buffer Backspace", lambda c: backspace(c, False))
run("Enter raw fallback", lambda c: raw(c, lambda x: x.press("enter")))
run("navigation raw fallback", lambda c: raw(c, lambda x: check(not x.press("left"), "navigation must pass")))
run("shortcut raw fallback", lambda c: raw(c, lambda x: check(not x.press("a", states=4), "Ctrl-A must pass")))
run("Escape cancels", lambda c: cancel(c, lambda x: x.press("esc")))
run("Reset cancels", lambda c: cancel(c, lambda x: x.ic.Reset()))
run("FocusOut cancels / new context state", lambda c: cancel(c, lambda x: x.ic.FocusOut()))


def destroy(c):
    with paused_engine():
        pending(c)
        c.type("abc", wait=False)
        c.ic.DestroyIC()
    settle()
    check(not c.commits, "Destroy committed pending sequence")
    other = fresh()
    try:
        other.type("nihao")
        other.press("space")
        check(other.commits == ["你好"], "destroy poisoned new context")
    finally:
        other.ic.DestroyIC()


run("Destroy / new context", destroy)
run("IM switch raw fallback", lambda c: raw(c, lambda x: controller.SetCurrentIM("keyboard-us")))
run("five-second timeout while stopped", timeout)
def unrelated_release(c):
    with paused_engine():
        pending(c)
        c.press("!", delay=0)
        c.press("b", delay=0)
        c.press("!", delay=0, release=True)
        c.press("b", delay=0, release=True)
        c.press("backspace", delay=0)
    settle(3)
    check(c.commits == ["你好"], "deleted queued b leaked: %r" % c.commits)
    check(any(e[0] == ord("!") and not e[-1] for e in c.forwarded),
          "literal ! press lost: %r" % c.forwarded)
    check(any(e[0] == ord("!") and e[-1] for e in c.forwarded),
          "unrelated ! release lost: %r" % c.forwarded)
    check(not any(e[0] == ord("b") for e in c.forwarded),
          "deleted b press/release forwarded: %r" % c.forwarded)


def printable(c, key, text):
    with paused_engine():
        pending(c)
        check(c.press(key, delay=0), "printable must queue")
        pump(.1)
        check(not c.commits, "printable flushed pending early: %r" % c.commits)
        c.press("enter")
        check(c.commits == ["nihao " + text], "printable raw fallback: %r" % c.commits)


def recovery(c):
    with paused_engine() as pid:
        pending(c)
        c.type("zhongguo", wait=False)
        c.press("space", delay=0)
        # paused_engine verified exact argv and runner parent before yielding.
        os.kill(pid, signal.SIGKILL)
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline and len(c.commits) < 2:
        pump(.1)
    check(c.commits == ["你好", "中国"], "kill/recovery must select current Chinese, not raw: %r" % c.commits)
    c.type("nihao")
    c.press("space")
    check(c.commits == ["你好", "中国", "你好"], "engine did not recover normally")


run("Backspace retains unrelated release", unrelated_release)
run("Unicode printable queues without early flush", lambda c: printable(c, "é", "é"))
run("keypad printable queues", lambda c: printable(c, 0xFFB1, "1"))
run("private engine kill/recovery", recovery)
print("Pending Space: %d failures: %s" % (len(failures), ", ".join(failures)), flush=True)
raise SystemExit(bool(failures))
