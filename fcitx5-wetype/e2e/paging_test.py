#!/usr/bin/env python3
"""候选翻页 + 漏字(issue #5) + 跟随全局每页候选数(issue #4) 的隔离 e2e。
断言两条链路: ProcessKeyEvent 返回值(按键是否被吞掉) 与插件日志里的
'commit candidate index=N'(数字选词落到了第几个候选)。
"""
import os, re, sys, time
import dbus
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

PS = int(sys.argv[1])
LOG = os.environ["WETYPE_PAGETEST_LOG"]
KEY = {"a": 97, "i": 105, "h": 104, "n": 110, "o": 111, "1": 49, "0": 48,
       "eq": 61, "minus": 45, "esc": 0xFF1B, "down": 0xFF54,
       "up": 0xFF52, "right": 0xFF53, "space": 32, "4": 52}

DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
CTL = dbus.Interface(bus.get_object("org.fcitx.Fcitx5", "/controller"),
                     "org.fcitx.Fcitx.Controller1")
IM = dbus.Interface(bus.get_object("org.fcitx.Fcitx5", "/org/freedesktop/portal/inputmethod"),
                    "org.fcitx.Fcitx.InputMethod1")
for _ in range(20):
    try:
        CTL.CurrentInputMethod()
        break
    except Exception:
        time.sleep(0.5)
else:
    print("FAIL: fcitx5 dbus 未就绪")
    sys.exit(1)

loop = GLib.MainLoop()
commits = []
ui_candidates = []


def record_ui(*args):
    ui_candidates[:] = [(str(c[0]), str(c[1])) for c in args[4]]


bus.add_signal_receiver(record_ui,
                        dbus_interface="org.fcitx.Fcitx.InputContext1",
                        signal_name="UpdateClientSideUI")
bus.add_signal_receiver(lambda *a, **k: commits.append(str(a[0]) if a else ""),
                        dbus_interface="org.fcitx.Fcitx.InputContext1",
                        signal_name="CommitString")
CTL.SetCurrentIM("wetype-im")
time.sleep(0.5)
path, _ = IM.CreateInputContext([("program", "wetype-pagetest"),
                                 ("capability", dbus.String(str(dbus.UInt64(1 | (1 << 39)))))])
ic = dbus.Interface(bus.get_object("org.fcitx.Fcitx5", path),
                    "org.fcitx.Fcitx.InputContext1")
# CreateInputContext's metadata does not set capabilities on all Fcitx5
# versions. Explicitly enable client-side candidate UI for signal assertions.
ic.SetCapability(dbus.UInt64(1 | (1 << 39)))
ic.FocusIn()
time.sleep(0.5)


def pump(seconds):
    end = time.time() + seconds
    while time.time() < end:
        loop.get_context().iteration(False)
        time.sleep(0.02)


def press(name):
    ok = ic.ProcessKeyEvent(dbus.UInt32(KEY[name]), dbus.UInt32(0), dbus.UInt32(0),
                            dbus.Boolean(False),
                            dbus.UInt32(int(time.time() * 1000) & 0xFFFFFFFF))
    pump(0.15)
    return bool(ok)


def indexes():
    with open(LOG, errors="ignore") as f:
        return [int(m) for m in re.findall(r"commit candidate index=(\d+)", f.read())]


def candidate_count(offset=0):
    with open(LOG, errors="ignore") as f:
        f.seek(offset)
        m = re.findall(r"parsed candidates=(\d+) buffer_len=5 preview=0", f.read())
    return int(m[-1]) if m else 0


def fresh():
    press("esc")
    commits.clear()
    offset = os.path.getsize(LOG)
    for ch in "nihao":
        press(ch)
    for _ in range(40):
        pump(0.25)
        if candidate_count(offset) >= 2 * PS + 1:
            break
    return candidate_count(offset)


def set_page_size(size):
    # The runner creates this isolated config; never write the desktop config.
    config = os.environ["WETYPE_PAGETEST_CONFIG"]
    with open(config, "w") as f:
        f.write("[Behavior]\nDefaultPageSize=%d\n" % size)
    CTL.ReloadConfig()
    pump(0.2)


results = []


def check(label, cond, detail=""):
    results.append((label, bool(cond), detail))
    print(("PASS " if cond else "FAIL ") + label + ("  " + detail if detail else ""), flush=True)


n = fresh()
check("engine returned enough candidates (>=2*%d+1)" % PS, n >= 2 * PS + 1, "count=%d" % n)

# 1) 未翻页时按数字 1 -> 第一个候选
before = len(indexes())
press("1")
pump(0.8)
got = indexes()
check("page 1 digit-1 commits index 0", len(got) > before and got[-1] == 0,
      "index=%s" % (got[-1] if got else None))
check("digit-1 emits a nonempty CommitString", bool(commits) and bool(commits[-1]),
      "commits=%s" % commits)

# 2) issue #5: 第一页按 '-' 无法后退, 但按键必须被吞掉且不产生上屏
fresh()
leak_before = len(commits)
consumed = press("minus")
pump(0.8)
check("issue#5 '-' on first page is consumed", consumed, "consumed=%s" % consumed)
check("issue#5 '-' leaks nothing into the document", len(commits) == leak_before,
      "commits=%s" % commits[leak_before:])

# 3) issue #4: 一次 '=' 前进 PS 个候选
fresh()
before = len(indexes())
press("eq")
press("1")
pump(0.8)
got = indexes()
check("one '=' then digit-1 commits index %d" % PS, len(got) > before and got[-1] == PS,
      "index=%s" % (got[-1] if got else None))

# 4) 两次 '=' 前进 2*PS
fresh()
before = len(indexes())
press("eq")
press("eq")
press("1")
pump(0.8)
got = indexes()
check("two '=' then digit-1 commits index %d" % (2 * PS),
      len(got) > before and got[-1] == 2 * PS, "index=%s" % (got[-1] if got else None))

# 5) 末尾页反复 '=' / '-' 不越界、不漏字, 回到首页后数字 1 仍是 index 0
n = fresh()
steps = (n + PS - 1) // PS + 2
tail = all(press("eq") for _ in range(steps))
check("last page shows the expected remaining candidates",
      len(ui_candidates) == n - ((n - 1) // PS) * PS)
check("paging forward leaks no CommitString", not commits)
before = len(indexes())
press("1")
pump(0.8)
got = indexes()
check("last page digit-1 selects the expected index",
      len(got) > before and got[-1] == ((n - 1) // PS) * PS)
fresh()
for _ in range(steps):
    press("eq")
head = all(press("minus") for _ in range(steps))
check("paging keys stay consumed at both boundaries", tail and head)
check("paging back leaks no CommitString", not commits)
before = len(indexes())
press("1")
pump(0.8)
got = indexes()
check("after paging back, digit-1 commits index 0",
      len(got) > before and got[-1] == 0, "index=%s" % (got[-1] if got else None))

# 6) 每页 10 个时, '0' 选第 10 个
if PS >= 10:
    fresh()
    before = len(indexes())
    press("0")
    pump(0.8)
    got = indexes()
    check("digit-0 commits the 10th candidate (index 9)",
          len(got) > before and got[-1] == 9, "index=%s" % (got[-1] if got else None))

# 7) Changing global configuration mid-composition must not produce negative
# indices (3 -> 10 -> Up), or leave the selected index outside the new page.
set_page_size(3)
fresh()
press("eq")  # windowStart = 3
set_page_size(10)
consumed = press("up")
before = len(indexes())
press("1")
pump(0.8)
got = indexes()
check("live page size 3->10 then Up stays consumed and selects index 0",
      consumed and len(got) > before and got[-1] == 0 and bool(commits))

set_page_size(10)
fresh()
press("eq")  # windowStart = 10
set_page_size(3)
press("up")  # align to 9, then previous page starts at 6
before = len(indexes())
press("1")
pump(0.8)
got = indexes()
check("live page size 10->3 normalizes page and selects index 6",
      len(got) > before and got[-1] == 6 and bool(commits))
# No navigation key between reload and selection: unchanged indices still
# require a UI refresh so displayed candidates match the digit-key limit.
set_page_size(10)
fresh()
check("page size 10 is rendered", len(ui_candidates) == 10)
set_page_size(3)
consumed = press("4")
check("live homepage 10->3 refreshes before direct digit handling",
      not consumed and len(ui_candidates) == 3 and not commits)

set_page_size(10)
fresh()
for _ in range(9):
    press("right")
set_page_size(3)
before = len(indexes())
press("space")
pump(0.8)
got = indexes()
check("live shrink clamps selected index before Space",
      len(got) > before and got[-1] == 2 and bool(commits))
set_page_size(PS)

failed = [r for r in results if not r[1]]
print("SUMMARY page_size=%d total=%d failed=%d" % (PS, len(results), len(failed)), flush=True)
print("E2E_RESULT =", "FAIL" if failed else "PASS", flush=True)
sys.exit(1 if failed else 0)
