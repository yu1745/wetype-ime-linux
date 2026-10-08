"""Consumed pending text remains visible before frontend-controlled focus-out."""
from dbus_test_client import Client, bus, paused_engine, pump

c = Client("pending-space-preedit")
c.focus()
c.type("nihao")
c.press("esc")
c.commits.clear()
preedits = []


def ui(*args):
    preedits.append("".join(str(part[0]) for part in args[0]))


bus.add_signal_receiver(ui, dbus_interface="org.fcitx.Fcitx.InputContext1",
                        signal_name="UpdateClientSideUI", path=c.path)
with paused_engine():
    c.type("nihao", wait=False)
    assert c.press("space", delay=0)
    c.type("zhongguo", wait=False)
    pump(0.1)
    assert preedits and preedits[-1] == "nihao zhongguo", preedits
    assert not c.commits
    c.ic.FocusOut()
pump(1)
assert not c.commits, "addon made an extra focus-out commit"
print("PASS pending preedit includes Space and all queued text without extra focus-out commit")
