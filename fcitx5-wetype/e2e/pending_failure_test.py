"""Private protocol-fixture tests for pending Space failure paths."""
import os
import time
from dbus_test_client import Client, pump

mode = os.environ["WETYPE_PENDING_FIXTURE"]
client = Client("pending-space-fixture")
client.focus()
client.type("nihaoa", wait=False)
assert client.press("space", delay=0)
client.type("x", wait=False)
assert client.press("space", delay=0)
pump(0.1)
if mode != "startup-failure":
    assert not client.commits, "prefix preview committed prematurely"
end = time.monotonic() + 8
while not client.commits and time.monotonic() < end:
    pump(0.05)
expected = "nihaoa x "
if mode == "startup-failure":
    # Exhausted restarts are known before typing; immediate per-word fallback
    # is valid, and must retain both Space boundaries without waiting again.
    assert "".join(client.commits) == expected, (mode, client.commits)
else:
    assert client.commits == [expected], (mode, client.commits)
commits = list(client.commits)
pump(0.8)
assert client.commits == commits, "late response duplicated raw fallback"
print("PASS controlled pending failure:", mode)
