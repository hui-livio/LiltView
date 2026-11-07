#!/usr/bin/env python3
"""Optional Linux integration tests: private D-Bus session + SDL offscreen.

No real desktop settings, compositor configuration or launcher files are changed.
SPDX-License-Identifier: Apache-2.0
"""
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def bus_call(service, method, *arguments, ok=True):
    result = subprocess.run([
        "gdbus", "call", "--session", "--dest", service, "--object-path", "/org/liltview/MenuBar",
        "--method", "com.canonical.dbusmenu." + method, "--", *arguments,
    ], text=True, capture_output=True, timeout=5)
    assert not ok or result.returncode == 0, result.stderr
    return result


def find_service(pid):
    for _ in range(100):
        result = subprocess.check_output([
            "gdbus", "call", "--session", "--dest", "org.freedesktop.DBus", "--object-path", "/org/freedesktop/DBus",
            "--method", "org.freedesktop.DBus.ListNames",
        ], text=True, timeout=5)
        for name in re.findall(r"'(:[0-9.]+)'", result):
            owner = subprocess.run([
                "gdbus", "call", "--session", "--dest", "org.freedesktop.DBus", "--object-path", "/org/freedesktop/DBus",
                "--method", "org.freedesktop.DBus.GetConnectionUnixProcessID", name,
            ], text=True, capture_output=True, timeout=5)
            if owner.returncode == 0 and f"uint32 {pid}," in owner.stdout:
                if bus_call(name, "GetLayout", "0", "-1", "[]", ok=False).returncode == 0:
                    return name
        time.sleep(0.025)
    raise AssertionError("Application did not export its menu")


def check_menu(app, folder):
    service = find_service(app.pid)
    all_items = bus_call(service, "GetGroupProperties", "[]", "[]").stdout
    assert "File" in all_items and "Settings" in all_items and "Quit" in all_items, all_items
    filtered = bus_call(service, "GetGroupProperties", "[]", '["label"]').stdout
    assert "label" in filtered and "enabled" not in filtered, filtered
    assert "standard" in bus_call(service, "GetProperty", "1", "type").stdout
    assert "-1" in bus_call(service, "GetProperty", "1", "toggle-state").stdout
    assert "[]" in bus_call(service, "GetProperty", "1", "shortcut").stdout
    assert "[]" in bus_call(service, "GetProperty", "1", "icon-data").stdout
    assert bus_call(service, "GetProperty", "1", "unsupported", ok=False).returncode != 0
    bus_call(service, "Event", "36", "clicked", "<0>", "0")
    bus_call(service, "Event", "12", "clicked", "<0>", "0")
    assert app.wait(timeout=10) == 0
    assert "theme: light" in (folder / "settings.yaml").read_text()


def test_menu(executable, folder):
    log = folder / "app.log"
    with log.open("w") as output:
        app = subprocess.Popen([str(executable), "--config-dir", str(folder), "--language", "en-US"],
                               stdout=output, stderr=subprocess.STDOUT)
        try:
            check_menu(app, folder)
            assert "CRITICAL" not in log.read_text(), log.read_text()
        finally:
            if app.poll() is None:
                app.terminate()
                app.wait(timeout=5)


def test_font(executable, folder):
    binary = folder / "liltview"
    shutil.copy2(executable, binary)
    resources = folder / "assets/fonts"
    resources.mkdir(parents=True)
    (resources / "HarmonyOS_Sans_SC_Regular.ttf").write_bytes(b"")
    (folder / "settings.yaml").write_text("log_level: info\n")
    result = subprocess.run([str(binary), "--smoke-test", "--config-dir", str(folder)], text=True, capture_output=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "Bundled font is unreadable or invalid" in result.stdout, result.stdout


def test_save_failure(executable, folder):
    (folder / "settings.yaml").symlink_to("/dev/full")
    log = folder / "app.log"
    with log.open("w") as output:
        app = subprocess.Popen([str(executable), "--config-dir", str(folder)], stdout=output, stderr=subprocess.STDOUT)
        try:
            for _ in range(100):
                if "started," in log.read_text():
                    break
                assert app.poll() is None, log.read_text()
                time.sleep(0.025)
            app.send_signal(signal.SIGINT)
            assert app.wait(timeout=10) == 0, log.read_text()
            assert "Cannot save application settings" in log.read_text(), log.read_text()
        finally:
            if app.poll() is None:
                app.terminate()
                app.wait(timeout=5)


def test_jpeg_render(executable, folder):
    sample = Path(__file__).resolve().parents[2] / "third_party/libjpeg-turbo/testimages/testorig.jpg"
    screenshot = folder / "jpeg.ppm"
    (folder / "settings.yaml").write_text("log_level: info\n")
    result = subprocess.run([str(executable), "--smoke-test", "--config-dir", str(folder), "--screenshot", str(screenshot), str(sample)],
                            text=True, capture_output=True, timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "JPEG displayed: testorig.jpg" in result.stdout, result.stdout
    header, dimensions, maximum, pixels = screenshot.read_bytes().split(b"\n", 3)
    assert header == b"P6" and maximum == b"255", "invalid screenshot"
    width, height = map(int, dimensions.split())
    red = 0
    for y in range(height // 3, height * 2 // 3):
        for x in range(width // 3, width * 2 // 3):
            offset = (y * width + x) * 3
            r, g, b = pixels[offset:offset+3]
            red += r > 120 and r > g * 1.4 and r > b * 1.4
    assert red > 500, "JPEG rose pixels were not rendered in the canvas"


def test_settings_yaml(executable, folder):
    settings = folder / "settings.yaml"
    settings.write_text("# User configuration\nwidth: 1280\nheight: 720\n"
                        "maximized: false\ntheme: light\nlanguage: en-US\nlog_level: warn\n")
    log = folder / "app.log"
    with log.open("w") as output:
        app = subprocess.Popen([str(executable), "--config-dir", str(folder)], stdout=output, stderr=subprocess.STDOUT)
        try:
            service = find_service(app.pid)
            assert "File" in bus_call(service, "GetProperty", "1", "label").stdout
            assert "1" in bus_call(service, "GetProperty", "36", "toggle-state").stdout
            assert "1" in bus_call(service, "GetProperty", "32", "toggle-state").stdout
            bus_call(service, "Event", "12", "clicked", "<0>", "0")
            assert app.wait(timeout=10) == 0, log.read_text()
        finally:
            if app.poll() is None:
                app.terminate()
                app.wait(timeout=5)
    saved = settings.read_text()
    for value in ("width: 1280", "height: 720", "theme: light", "language: en-US", "log_level: warn"):
        assert value in saved, saved
    # Parse the writer's actual output on a second launch, with CLI override.
    with log.open("w") as output:
        app = subprocess.Popen([str(executable), "--config-dir", str(folder), "--language=zh-CN"],
                               stdout=output, stderr=subprocess.STDOUT)
        try:
            service = find_service(app.pid)
            assert "文件" in bus_call(service, "GetProperty", "1", "label").stdout
            bus_call(service, "Event", "12", "clicked", "<0>", "0")
            assert app.wait(timeout=10) == 0, log.read_text()
        finally:
            if app.poll() is None:
                app.terminate()
                app.wait(timeout=5)
    assert "language: zh-CN" in settings.read_text()


def test_logging(executable, folder):
    default_level = sys.argv[3] if len(sys.argv) > 3 else "debug"
    for level in (None, "warn", "debug", "off", "invalid"):
        directory = folder / (level or "default")
        directory.mkdir()
        if level is not None:
            (directory / "settings.yaml").write_text(f"log_level: {level}\n")
        result = subprocess.run([str(executable), "--smoke-test", "--config-dir", str(directory)],
                                text=True, capture_output=True, timeout=10)
        assert result.returncode == 0, result.stdout + result.stderr
        expected = default_level if level in (None, "invalid") else level
        if expected == "debug":
            assert "[debug] Log level: debug" in result.stdout, result.stdout
            assert "started," in result.stdout, result.stdout
        else:
            assert "[debug]" not in result.stdout and "[info]" not in result.stdout, result.stdout
        if expected == "off":
            assert not result.stdout, result.stdout


def main():
    executable = Path(sys.argv[1]).resolve()
    case = sys.argv[2]
    os.environ["SDL_VIDEODRIVER"] = "offscreen"
    with tempfile.TemporaryDirectory(prefix="liltview-desktop-regression-") as temporary:
        {"menu": test_menu, "font": test_font, "save": test_save_failure, "jpeg": test_jpeg_render,
         "settings": test_settings_yaml, "logging": test_logging}[case](executable, Path(temporary))
    print(f"PASS desktop regression: {case}")


if __name__ == "__main__":
    main()
