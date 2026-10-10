#!/usr/bin/env python3
"""Real GTK entry for the Xvfb E2E; records widget contents, never injects text."""
import json
import os
from pathlib import Path

import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk

out = Path(os.environ["WETYPE_VISUAL_OUT"])
window = Gtk.Window(title="WeType visual E2E")
window.set_default_size(760, 300)
window.move(80, 60)
window.connect("destroy", Gtk.main_quit)
box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=24)
box.set_border_width(32)
window.add(box)
box.pack_start(Gtk.Label(label="WeType / Fcitx5 — real GTK input"), False, False, 0)
entry = Gtk.Entry()
entry.set_placeholder_text("Type pinyin here")
box.pack_start(entry, False, False, 0)
box.pack_start(Gtk.Label(label="ARM64 • Xvfb • GTK IM module • Fcitx5 classic UI"), False, False, 0)

def changed(*_):
    temp = out / "entry.tmp"
    temp.write_text(json.dumps({"text": entry.get_text()}, ensure_ascii=False))
    temp.replace(out / "entry.json")

entry.connect("changed", changed)
window.show_all()
entry.grab_focus()
changed()
Gtk.main()
