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
    for item, label, shortcut in (
        (10, "Open", ("Control", "o")),
        (21, "Previous", ("Left",)),
        (22, "Next", ("Right",)),
        (24, "Fit", ("f",)),
        (25, "100%", ("Control", "1")),
        (20, "Fullscreen", ("F11",)),
        (50, "Pan left", ("Shift", "Left")),
        (51, "Pan right", ("Shift", "Right")),
        (52, "Pan up", ("Shift", "Up")),
        (53, "Pan down", ("Shift", "Down")),
        (54, "Zoom in", ("Control", "Up")),
        (55, "Zoom out", ("Control", "Down")),
    ):
        assert label in bus_call(service, "GetProperty", str(item), "label").stdout
        keys = bus_call(service, "GetProperty", str(item), "shortcut").stdout
        for key in shortcut:
            assert f"'{key}'" in keys, keys
    for item in (21, 22, 24, 25, 50, 51, 52, 53, 54, 55):
        assert "false" in bus_call(service, "GetProperty", str(item), "enabled").stdout
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

    # Loaded-image state must also reach the exported system menu.
    sample = Path(__file__).resolve().parents[1] / "fixtures/red.bmp"
    with log.open("w") as output:
        app = subprocess.Popen([str(executable), "--config-dir", str(folder), "--language", "en-US", str(sample)],
                               stdout=output, stderr=subprocess.STDOUT)
        try:
            service = find_service(app.pid)
            deadline = time.monotonic() + 10
            for item in (24, 25, 50, 51, 52, 53, 54, 55):
                while "true" not in bus_call(service, "GetProperty", str(item), "enabled").stdout:
                    assert app.poll() is None, log.read_text()
                    assert time.monotonic() < deadline, log.read_text()
                    time.sleep(0.025)
            for item in (25, 54, 50, 51, 52, 53, 55, 24):
                bus_call(service, "Event", str(item), "clicked", "<0>", "0")
            bus_call(service, "Event", "12", "clicked", "<0>", "0")
            assert app.wait(timeout=10) == 0, log.read_text()
            assert "[error]" not in log.read_text() and "[critical]" not in log.read_text(), log.read_text()
        finally:
            if app.poll() is None:
                app.terminate()
                app.wait(timeout=5)


def test_status(executable, folder):
    # Cover square native-window corners, edge-to-edge canvas layout, font scripts/themes, and a long filename.
    sample = folder / ("long-image-" + "x" * 180 + ".bmp")
    shutil.copy2(Path(__file__).resolve().parents[1] / "fixtures/red.bmp", sample)
    for theme in ("light", "dark"):
        for language in ("en-US", "zh-CN"):
            directory = folder / f"{theme}-{language}"
            directory.mkdir()
            (directory / "settings.yaml").write_text(f"theme: {theme}\nlanguage: {language}\n")
            screenshot = directory / "status.ppm"
            command = [str(executable), "--smoke-test", "--config-dir", str(directory), "--screenshot", str(screenshot)]
            if theme == "dark":
                command.append(str(sample))
            result = subprocess.run(command, text=True, capture_output=True, timeout=10)
            assert result.returncode == 0, result.stdout + result.stderr
            header, dimensions, maximum, pixels = screenshot.read_bytes().split(b"\n", 3)
            assert header == b"P6" and maximum == b"255", "invalid screenshot"
            width, height = map(int, dimensions.split())

            def pixel(x, y):
                offset = (y * width + x) * 3
                return pixels[offset:offset + 3]

            background = pixel(width // 2, height - 1)
            # The first full background row is the canvas/footer boundary. Sampling
            # both edges avoids mistaking dense long-filename glyphs for the canvas.
            background_rows = [y for y in range(height - 100, height)
                               if all(pixel(x, y) == background for x in (18, width // 2, width - 19))]
            assert background_rows, (theme, language, "missing status background")
            top = background_rows[0]
            assert 24 <= height - top <= 28, (theme, language, "footer height", height - top)
            canvas_color = pixel(0, height // 2)
            assert canvas_color != background, (theme, language, "canvas has side padding")
            canvas_top = next(y for y in range(top) if pixel(width // 2, y) == canvas_color)
            menu_color = pixel(width // 2, 0)
            # A native-decorated window must fill its rectangular client area,
            # not expose the renderer's clear color outside custom-rounded corners.
            for x in (*range(8), *range(width - 8, width)):
                for y in range(8):
                    assert pixel(x, y) == menu_color, (theme, language, "unpainted top corner", x, y)
                for y in range(height - 8, height):
                    assert pixel(x, y) == background, (theme, language, "unpainted bottom corner", x, y)
            menu_bottom = next(y for y in range(top) if pixel(width // 2, y) != menu_color)
            assert canvas_top == menu_bottom, (theme, language, "canvas has top padding", menu_bottom, canvas_top)
            for y in (canvas_top, (canvas_top + top) // 2, top - 1):
                assert pixel(0, y) == canvas_color, (theme, language, "canvas has left padding", y)
                assert pixel(width - 1, y) == canvas_color, (theme, language, "canvas has right padding", y)
            ink_bounds = []
            for left, right in ((18, width // 3), (width - 75, width - 18)):
                rows = [y for y in range(top, height)
                        if any(max(abs(a - b) for a, b in zip(pixel(x, y), background)) > 15
                               for x in range(left, right))]
                assert rows, (theme, language, "missing status text")
                above, below = min(rows) - top, height - 1 - max(rows)
                assert min(above, below) >= 3, (theme, language, "clipped status text", above, below)
                # Allow up to 2px of optical offset from glyph ascenders/descenders.
                assert abs(above - below) <= 4, (theme, language, "text not centered", above, below)
                ink_bounds.append((min(rows), max(rows)))
            # Chinese glyphs and Latin descenders share a baseline, not identical ink bounds.
            assert all(abs(a - b) <= 3 for a, b in zip(*ink_bounds)), (theme, language, "status baselines differ", ink_bounds)


def test_close(executable, folder):
    fixtures = Path(__file__).resolve().parents[1] / "fixtures"

    def check_order(text, route):
        requests = re.findall(r"Close requested: window_hidden=true, frames=(\d+), hide_ms=([0-9.]+)", text)
        stops = re.findall(r"Shutdown: stopping image work, window_hidden=true, frames=(\d+)", text)
        completed = re.findall(r"Window closed cleanly after (\d+) frames", text)
        assert len(requests) == len(stops) == len(completed) == 1, text
        assert requests[0][0] == stops[0] == completed[0], "frame submitted after close request: " + text
        assert text.index("Close requested:") < text.index("Shutdown: stopping image work") < text.index("Window closed cleanly"), text
        assert "[error]" not in text and "[critical]" not in text, text
        latencies = re.findall(
            r"Close latency: source=(\w+), origin=(\w+), event_to_handler_ms=(-?[0-9.]+), "
            r"handler_to_request_ms=(-?[0-9.]+), event_to_request_ms=(-?[0-9.]+), timing_valid=(true|false)", text)
        assert len(latencies) == 1, text
        source, origin, queued, dispatched, total, valid = latencies[0]
        expected = {"menu": ("global_menu", "dbus_activation"), "signal": ("sdl_quit", "sdl_event"),
                    "window": ("window_close", "sdl_event")}
        assert (source, origin) == expected[route], latencies
        assert valid == "true" and min(float(queued), float(dispatched), float(total)) >= 0, latencies
        assert abs(float(queued) + float(dispatched) - float(total)) <= 0.003, latencies
        print(f"latency {route}: event-to-handler={queued}ms, handler-to-request={dispatched}ms, origin={origin}")
        return float(requests[0][1])

    for route in ("menu", "signal", "window"):
        for filename in (None, "red.bmp", "red-blue-loop.gif"):
            directory = folder / f"{route}-{filename or 'empty'}"
            directory.mkdir()
            settings = directory / "settings.yaml"
            settings.write_text("width: 777\nheight: 555\nmaximized: false\nlog_level: debug\nlanguage: en-US\n")
            command = [str(executable), "--config-dir", str(directory)]
            if route == "window":
                # Smoke mode injects an actual SDL_EVENT_WINDOW_CLOSE_REQUESTED.
                command.append("--smoke-test")
            if filename:
                command.append(str(fixtures / filename))
            if route == "window":
                result = subprocess.run(command, text=True, capture_output=True, timeout=10)
                assert result.returncode == 0, result.stdout + result.stderr
                check_order(result.stdout, route)
                continue

            log = directory / "app.log"
            with log.open("w") as output:
                app = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
                try:
                    service = find_service(app.pid)
                    deadline = time.monotonic() + 5
                    ready = f"Image displayed: {filename}" if filename else "started,"
                    while ready not in log.read_text():
                        assert app.poll() is None and time.monotonic() < deadline, log.read_text()
                        time.sleep(0.005)
                    # Exercise the idle wait path too, not only startup's active frames.
                    time.sleep(0.25)
                    started = time.monotonic()
                    if route == "menu":
                        bus_call(service, "Event", "12", "clicked", "<0>", "0")
                    else:
                        app.send_signal(signal.SIGINT)
                    assert app.wait(timeout=5) == 0, log.read_text()
                    elapsed_ms = (time.monotonic() - started) * 1000
                    hide_ms = check_order(log.read_text(), route)
                    print(f"close {route}/{filename or 'empty'}: hide={hide_ms:.3f}ms, request-to-exit={elapsed_ms:.1f}ms")
                    saved = settings.read_text()
                    assert "width: 777" in saved and "height: 555" in saved, saved
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
    assert "Image displayed: testorig.jpg" in result.stdout, result.stdout
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


def test_formats_render(executable, folder):
    fixtures = Path(__file__).resolve().parents[1] / "fixtures"
    (folder / "settings.yaml").write_text("log_level: info\nlanguage: en-US\n")
    for filename in ("red-alpha.png", "red-alpha.webp", "red.bmp", "red-alpha.svg", "red-first-frame.gif", "red.tiff"):
        screenshot = folder / (filename + ".ppm")
        result = subprocess.run([str(executable), "--smoke-test", "--config-dir", str(folder), "--screenshot",
                                 str(screenshot), str(fixtures / filename)], text=True, capture_output=True, timeout=15)
        assert result.returncode == 0, result.stdout + result.stderr
        assert f"Image displayed: {filename}" in result.stdout, result.stdout
        header, dimensions, maximum, pixels = screenshot.read_bytes().split(b"\n", 3)
        assert header == b"P6" and maximum == b"255", "invalid screenshot"
        width, height = map(int, dimensions.split())
        red = 0
        for y in range(height // 3, height * 2 // 3):
            for x in range(width // 3, width * 2 // 3):
                offset = (y * width + x) * 3
                r, g, b = pixels[offset:offset + 3]
                red += r > 120 and r > g * 1.4 and r > b * 1.4
        assert red > 500, f"{filename} pixels were not rendered in the canvas"


def test_animation_app(executable, folder):
    sample = Path(__file__).resolve().parents[1] / "fixtures/red-blue-loop.gif"
    (folder / "settings.yaml").write_text("log_level: debug\nlanguage: en-US\n")
    log = folder / "animation.log"
    with log.open("w") as output:
        app = subprocess.Popen([str(executable), "--config-dir", str(folder), str(sample)],
                               stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while "epoch=2, frame=1" not in log.read_text():
                assert app.poll() is None, log.read_text()
                assert time.monotonic() < deadline, log.read_text()
                time.sleep(0.025)
            app.send_signal(signal.SIGINT)
            assert app.wait(timeout=10) == 0, log.read_text()
            text = log.read_text()
            assert "epoch=1, frame=0" in text and "epoch=1, frame=1" in text, text
            assert "[error]" not in text and "[critical]" not in text, text
        finally:
            if app.poll() is None:
                app.terminate()
                app.wait(timeout=5)


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
         "settings": test_settings_yaml, "logging": test_logging, "formats": test_formats_render,
         "animation_app": test_animation_app, "status": test_status, "close": test_close}[case](executable, Path(temporary))
    print(f"PASS desktop regression: {case}")


if __name__ == "__main__":
    main()
