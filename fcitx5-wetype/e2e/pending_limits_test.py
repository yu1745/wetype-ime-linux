"""Bounded pending FIFO must flush, not discard, intercepted text."""
import os
from dbus_test_client import Client, pump

assert os.environ.get("WETYPE_PENDING_FIXTURE") == "skip"
c = Client("pending-space-limit")
c.focus()
c.type("nihaoa", wait=False)
assert c.press("space", delay=0)
for _ in range(256):
    assert c.press("x", delay=0)
pump(0.1)
expected = "nihaoa " + "x" * 256
assert c.commits == [expected], [len(s) for s in c.commits]
pump(0.8)
assert c.commits == [expected], "late reply committed after bounded fallback"
print("PASS pending FIFO 256-event limit preserves all input")
