#!/usr/bin/env python3
"""Install a legacy AppDir, upgrade to bionic, and verify data preservation.

All install/uninstall actions use a temporary HOME and a private D-Bus session.
No desktop session, system installation or real user data is touched.
"""
import argparse
from contextlib import contextmanager
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import time

from test_bionic_daemon import Engine, disable_core_dumps


@contextmanager
def isolated_bus(directory):
    # Do not use the system session config: its service directories can activate
    # Fcitx/desktop portals with the build user's environment during reload.
    config = Path(directory) / "bus.conf"
    config.write_text(
        '<busconfig><type>session</type><listen>unix:tmpdir=' + directory + '</listen>'
        '<auth>EXTERNAL</auth><policy context="default">'
        '<allow send_destination="*" eavesdrop="true"/>'
        '<allow eavesdrop="true"/><allow own="*"/>'
        '</policy></busconfig>', encoding="utf-8")
    process = subprocess.Popen(["dbus-daemon", "--config-file=" + str(config),
                                "--nofork", "--print-address"],
                               stdout=subprocess.PIPE, text=True,
                               preexec_fn=disable_core_dumps)
    try:
        address = process.stdout.readline().strip()
        if not address:
            raise RuntimeError("private D-Bus session failed to start")
        yield address
    finally:
        process.terminate()
        process.wait(timeout=10)
        process.stdout.close()


def snapshots(root):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()}


def run(args):
    old = Path(args.old_appdir).resolve()
    new = Path(args.new_appdir).resolve()
    apk = Path(args.apk).resolve()
    with tempfile.TemporaryDirectory(prefix="wetype-upgrade-test.") as directory, isolated_bus(directory) as address:
        base = Path(directory)
        home = base / "home"
        home.mkdir()
        env = os.environ.copy()
        env.update(HOME=str(home), XDG_CONFIG_HOME=str(base / "config"),
                   XDG_DATA_HOME=str(base / "data"), XDG_CACHE_HOME=str(base / "cache"),
                   DBUS_SESSION_BUS_ADDRESS=address)
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
        eng = home / ".local/lib/wetype-ime/arm64"
        data = base / "data/wetype-ime"
        work = data / "dict"
        (work / "userdict/v5").mkdir(parents=True)
        (work / "userdict/user_hot_word").mkdir(parents=True)
        glossary = data / "glossary-en.tsv"
        glossary.write_text("你好\tn. hello\n", encoding="utf-8")

        def action(appdir, name):
            command = ["bash", str(appdir / "usr/lib/wetype-ime/appimage-manage.sh"),
                       str(appdir), name, "--user"]
            if name == "install":
                command += ["--apk", str(apk)]
            subprocess.run(command, env=env, check=True, stdout=subprocess.DEVNULL,
                           cwd=base, preexec_fn=disable_core_dumps, timeout=120)

        action(old, "install")
        assert (eng / "lib/libwetype-shim.so").is_file(), "legacy shim missing"
        assert (eng / "sysroot/lib/ld-linux-aarch64.so.1").exists(), "legacy loader missing"
        with (base / "old-engine.log").open("wb") as log:
            engine = Engine(eng, work, False, None, log)
            try:
                engine.command("OPT spans", "OK")
                words = engine.candidates("nihao")
                rank = next(i for i, item in enumerate(words) if item == (5, "拟好"))
                engine.command("S %d" % rank, "OK")
                time.sleep(0.5)
                engine.command("C", "OK")
                engine.command("SAVE", "OK")
            finally:
                engine.close()
        assert any(work.rglob("*.bin")) or any(p.is_file() for p in work.rglob("*")), \
            "legacy engine did not persist learning data"
        (eng / "dicts/local-preservation-marker").write_bytes(b"keep local dictionary")
        dict_before = snapshots(eng / "dicts")
        data_before = snapshots(data)
        assert data_before, "empty data snapshot"
        action(new, "install")
        assert not (eng / "lib/libwetype-shim.so").exists(), "legacy shim remains"
        assert not (eng / "sysroot/lib/ld-linux-aarch64.so.1").exists(), "legacy loader remains"
        assert (eng / "lib/libandroid.so").is_file()
        assert (eng / "sysroot/system/bin/linker64").is_file()
        assert snapshots(data) == data_before, "upgrade altered learning/glossary data"
        assert snapshots(eng / "dicts") == dict_before, "upgrade altered dictionaries"
        print("PASS upgrade removes glibc/shim and preserves dictionaries, learning and glossary", flush=True)
        with (base / "new-engine.log").open("wb") as log:
            engine = Engine(eng, work, False, None, log)
            try:
                engine.command("OPT spans", "OK")
                words = engine.candidates("nihao")
                learned = next(i for i, item in enumerate(words) if item == (5, "拟好"))
                assert learned < rank, words[:5]
            finally:
                engine.close()
        print("PASS upgraded engine reads legacy learned candidate", flush=True)
        data_before = snapshots(data)
        action(new, "install")
        assert snapshots(data) == data_before
        assert snapshots(eng / "dicts") == dict_before
        print("PASS repeat installation preserves data", flush=True)
        action(new, "uninstall")
        assert not (eng / "wetype-harness").exists()
        assert not (eng / "lib").exists() and not (eng / "sysroot").exists()
        assert snapshots(data) == data_before
        assert snapshots(eng / "dicts") == dict_before
        print("PASS uninstall preserves dictionaries, learning and glossary", flush=True)
    print("E2E_RESULT = PASS", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old-appdir", required=True)
    parser.add_argument("--new-appdir", required=True)
    parser.add_argument("--apk", required=True)
    run(parser.parse_args())
