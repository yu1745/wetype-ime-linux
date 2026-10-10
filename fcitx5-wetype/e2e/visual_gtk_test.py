#!/usr/bin/env python3
"""Send real XTest keys; require a mapped Fcitx candidate window and GTK commit."""
import json
import hashlib
import os
from pathlib import Path
import subprocess
import time

import gi
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk

Gdk.init([])
out = Path(os.environ["WETYPE_VISUAL_OUT"])

def command(*args):
    return subprocess.check_output(args, text=True).strip()

def wait_for(predicate, description, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.1)
    raise AssertionError("Timed out: " + description)

def candidates():
    result = subprocess.run(["xdotool", "search", "--onlyvisible", "--name",
                             "^Fcitx5 Input Window$"], text=True, capture_output=True)
    return result.stdout.strip().splitlines()

def screenshot(name):
    root = Gdk.get_default_root_window()
    pixbuf = Gdk.pixbuf_get_from_window(root, 0, 0, root.get_width(), root.get_height())
    assert pixbuf is not None, "Root screenshot failed"
    pixbuf.savev(str(out / (name + ".png")), "png", [], [])
    (out / (name + "-windows.txt")).write_text(command("xwininfo", "-root", "-tree"))

def candidate_pixels(window, name=None):
    geometry = dict(line.split("=", 1) for line in
                    command("xdotool", "getwindowgeometry", "--shell", window).splitlines())
    root = Gdk.get_default_root_window()
    pixbuf = Gdk.pixbuf_get_from_window(root, int(geometry["X"]), int(geometry["Y"]),
                                      int(geometry["WIDTH"]), int(geometry["HEIGHT"]))
    assert pixbuf is not None, "Candidate window screenshot failed"
    assert pixbuf.get_width() > 30 and pixbuf.get_height() > 30, "Candidate window too small"
    if name:
        pixbuf.savev(str(out / (name + ".png")), "png", [], [])
    return hashlib.sha256(bytes(pixbuf.get_pixels())).hexdigest()

def text():
    return json.loads((out / "entry.json").read_text())["text"]

try:
    wid = wait_for(lambda: subprocess.run(
        ["xdotool", "search", "--onlyvisible", "--name", "^WeType visual E2E$"],
        text=True, capture_output=True).stdout.strip(), "GTK window")
    command("xdotool", "windowfocus", "--sync", wid)
    time.sleep(.5)
    command("fcitx5-remote", "-o")
    command("fcitx5-remote", "-s", "wetype-im")
    time.sleep(.5)
    screenshot("00-empty")
    command("xdotool", "type", "--clearmodifiers", "--delay", "120", "nihao")
    windows = wait_for(candidates, "real Fcitx5 candidate window")
    for window in windows:
        assert command("xdotool", "getwindowpid", window) == os.environ["WETYPE_VISUAL_FCITX_PID"], \
            "Candidate window does not belong to the private Fcitx process"
    time.sleep(1)
    assert text() == "", "Preedit unexpectedly committed to GTK entry"
    screenshot("01-candidates")
    first_page = candidate_pixels(windows[0], "01-candidate-window")
    command("xdotool", "key", "equal")
    wait_for(lambda: candidate_pixels(windows[0]) != first_page, "next page visibly changes")
    time.sleep(.3)
    screenshot("01b-next-page")
    candidate_pixels(windows[0], "01b-candidate-window")
    command("xdotool", "key", "minus")
    wait_for(lambda: candidate_pixels(windows[0]) == first_page, "previous page restored")
    command("xdotool", "key", "space")
    wait_for(lambda: text() == "你好", "GTK entry commit == 你好")
    wait_for(lambda: not candidates(), "candidate window hidden after commit")
    time.sleep(.3)
    screenshot("02-committed")
    report = {"result": "PASS", "typed": "nihao", "committed": text(),
              "architecture": os.uname().machine,
              "fcitx_version": command(os.environ.get("FCITX5_BIN", "fcitx5"), "--version"),
              "candidate_window_ids": windows,
              "checks": ["real candidate window visible", "preedit not committed",
                         "next page changes rendered pixels", "previous page restores rendered pixels",
                         "space commits 你好 into GTK entry", "candidate window hides"]}
    (out / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))
    print(json.dumps(report, ensure_ascii=False))
except Exception:
    try:
        screenshot("failure")
    except Exception as error:
        print("Failure screenshot unavailable:", error)
    raise
