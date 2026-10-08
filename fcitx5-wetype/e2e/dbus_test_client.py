"""Small client for the isolated runner, never the desktop D-Bus session."""
from contextlib import contextmanager
import os
from pathlib import Path
import re
import signal
import time

import dbus
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

if not os.environ.get("WETYPE_PAGETEST_LOG") or not os.environ.get("WETYPE_E2E_FCITX_PID"):
    raise RuntimeError("Use the isolated e2e runner, not a desktop D-Bus session")
DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
registry = dbus.Interface(bus.get_object("org.freedesktop.DBus", "/org/freedesktop/DBus"),
                          "org.freedesktop.DBus")
if int(registry.GetConnectionUnixProcessID("org.fcitx.Fcitx5")) != int(os.environ["WETYPE_E2E_FCITX_PID"]):
    raise RuntimeError("D-Bus Fcitx PID does not match the isolated runner")
controller = dbus.Interface(bus.get_object("org.fcitx.Fcitx5", "/controller"),
                            "org.fcitx.Fcitx.Controller1")
input_method = dbus.Interface(bus.get_object("org.fcitx.Fcitx5", "/org/freedesktop/portal/inputmethod"),
                              "org.fcitx.Fcitx.InputMethod1")
context = GLib.MainContext.default()
KEYS = {"esc": 0xFF1B, "enter": 0xFF0D, "backspace": 0xFF08,
        "space": 32, "left": 0xFF51, "right": 0xFF53}
BASE_CAPABILITIES = 1 | (1 << 39)


def pump(seconds=0.1):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        while context.pending():
            context.iteration(False)
        time.sleep(0.005)


class Client:
    def __init__(self, name, capabilities=BASE_CAPABILITIES):
        self.path, _ = input_method.CreateInputContext([("program", name)])
        self.ic = dbus.Interface(bus.get_object("org.fcitx.Fcitx5", self.path),
                                 "org.fcitx.Fcitx.InputContext1")
        self.ic.SetCapability(dbus.UInt64(capabilities))
        self.commits = []
        self.candidates = []
        bus.add_signal_receiver(self._commit, dbus_interface="org.fcitx.Fcitx.InputContext1",
                                signal_name="CommitString", path=self.path)
        bus.add_signal_receiver(self._ui, dbus_interface="org.fcitx.Fcitx.InputContext1",
                                signal_name="UpdateClientSideUI", path=self.path)

    def _commit(self, text):
        self.commits.append(str(text))

    def _ui(self, *args):
        self.candidates[:] = [(str(c[0]), str(c[1])) for c in args[4]]

    def focus(self):
        self.ic.FocusIn()
        controller.Activate()
        controller.SetCurrentIM("wetype-im")
        pump()

    def press(self, key, delay=0.1, states=0, release=False):
        sym = KEYS[key] if key in KEYS else ord(key)
        consumed = self.ic.ProcessKeyEvent(dbus.UInt32(sym), dbus.UInt32(0),
                                           dbus.UInt32(states), dbus.Boolean(release),
                                           dbus.UInt32(int(time.time() * 1000) & 0xFFFFFFFF))
        if delay:
            pump(delay)
        return bool(consumed)

    def type(self, text, wait=True):
        log = Path(os.environ["WETYPE_PAGETEST_LOG"])
        offset = log.stat().st_size
        for char in text:
            assert self.press(char, delay=0.02 if wait else 0), "letter was not consumed"
        if wait:
            end = time.monotonic() + 10
            while time.monotonic() < end:
                pump(0.02)
                with log.open(errors="replace") as stream:
                    stream.seek(offset)
                    lines = stream.read()
                if re.search(r"parsed candidates=[1-9][0-9]* buffer_len=%d preview=0" % len(text), lines):
                    return
            raise AssertionError("current candidates did not arrive")

    def surrounding(self, text, cursor=None):
        cursor = len(text) if cursor is None else cursor
        self.ic.SetSurroundingText(text, cursor, cursor)
        pump()


@contextmanager
def paused_engine():
    # Only signal the harness child of the runner's own private Fcitx process.
    parent = int(os.environ["WETYPE_E2E_FCITX_PID"])
    executable = str(Path(os.environ["WETYPE_ENGINE_DIR"]) / "wetype-harness").encode()
    children = Path("/proc/%d/task/%d/children" % (parent, parent)).read_text().split()
    matches = []
    for child in children:
        try:
            command = Path("/proc/%s/cmdline" % child).read_bytes().split(b"\0")
            if executable in command:
                matches.append(int(child))
        except FileNotFoundError:
            pass
    assert len(matches) == 1, "expected exactly one private harness child"
    pid = matches[0]
    os.kill(pid, signal.SIGSTOP)
    try:
        yield
    finally:
        try:
            command = Path("/proc/%d/cmdline" % pid).read_bytes().split(b"\0")
            if executable in command:
                os.kill(pid, signal.SIGCONT)
        except (FileNotFoundError, ProcessLookupError):
            pass
