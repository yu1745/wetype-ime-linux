#!/usr/bin/env python3
"""Smart punctuation regression on the runner's private D-Bus session."""
import sys

from dbus_test_client import Client, BASE_CAPABILITIES, paused_engine, pump

results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name + (" " + detail if detail else ""), flush=True)


def raw(client, text):
    client.press("esc")
    client.commits.clear()
    client.type(text, wait=False)
    client.press("enter")
    check("Enter commits Latin text literally", client.commits == [text], str(client.commits))
    client.commits.clear()


first = Client("wetype-punctuation-A")
first.focus()
check("unknown context keeps comma passthrough", not first.press(",") and not first.commits)
raw(first, "hello")
check("Latin context keeps period passthrough", not first.press(".") and not first.commits)

first.type("nihao")
first.press(",")
check("English history does not affect current Chinese candidate",
      first.commits == ["你好，"], str(first.commits))
first.commits.clear()
first.press(".")
check("standalone Chinese punctuation is fullwidth", first.commits == ["。"])
raw(first, "nihao")
check("Chinese history does not affect raw Latin commit", not first.press(",") and not first.commits)

first.type("nihao")
first.press("space")
first.commits.clear()
check("Ctrl-comma shortcut remains passthrough", not first.press(",", states=4) and not first.commits)
check("punctuation release remains passthrough", not first.press(",", release=True) and not first.commits)
first.ic.FocusOut()
second = Client("wetype-punctuation-B")
second.focus()
check("new context does not inherit Chinese history", not second.press(",") and not second.commits)
raw(second, "hello")
second.ic.FocusOut()
first.focus()
first.commits.clear()
first.press(",")
check("returning to first context keeps its own Chinese history", first.commits == ["，"])
first.ic.FocusOut()
second.focus()
second.commits.clear()
check("returning to Latin context stays independent", not second.press(".") and not second.commits)

second.ic.FocusOut()
surrounding = Client("wetype-punctuation-surrounding", BASE_CAPABILITIES | (1 << 6))
surrounding.focus()
surrounding.surrounding("hello你好", 5)
check("cursor before Chinese word uses preceding Latin", not surrounding.press(",") and not surrounding.commits)
surrounding.surrounding("hello你好", 7)
surrounding.press(",")
check("cursor after Chinese word uses fullwidth", surrounding.commits == ["，"])
surrounding.commits.clear()
surrounding.surrounding("你好", 0)
check("cursor at beginning is unknown, not previous history",
      not surrounding.press(".") and not surrounding.commits)
surrounding.surrounding("hello")
surrounding.type("nihao")
surrounding.press(",")
check("current Chinese candidate overrides stale surrounding text",
      surrounding.commits == ["你好，"], str(surrounding.commits))
surrounding.commits.clear()
surrounding.press(".")
check("latest commit wins until surrounding text is refreshed", surrounding.commits == ["。"])
surrounding.commits.clear()
surrounding.surrounding("abc")
check("fresh document context replaces previous commit history",
      not surrounding.press(".") and not surrounding.commits)
surrounding.surrounding("中文")
check("idle digits pass through", not surrounding.press("3"))
check("decimal period after a digit stays ASCII", not surrounding.press(".") and not surrounding.commits)

surrounding.ic.FocusOut()
structured = Client("wetype-punctuation-email", BASE_CAPABILITIES | (1 << 7))
structured.focus()
structured.type("nihao")
structured.press(",")
check("structured Email field keeps ASCII separator", structured.commits == ["你好,"])
structured.ic.FocusOut()
first.focus()
first.type("nihao")
first.press("space")
first.commits.clear()
# Ordinary IC with known Chinese history: no Email guard can mask a bug.
# No response can arrive during this case: the engine is deliberately paused.
with paused_engine():
    first.type("nihao", wait=False)
    first.press(",")
    check("candidate not yet available overrides Chinese history with ASCII pinyin punctuation",
          first.commits == ["nihao,"], str(first.commits))
pump(0.5)
check("late responses do not add punctuation commits", first.commits == ["nihao,"])

print("SUMMARY punctuation total=%d failed=%d" % (len(results), results.count(False)), flush=True)
print("E2E_RESULT =", "PASS" if all(results) else "FAIL", flush=True)
sys.exit(0 if all(results) else 1)
