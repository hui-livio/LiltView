#!/usr/bin/env python3
"""Optional desktop integration tests: Linux D-Bus/SDL offscreen or Windows pixels.

The chrome_pixels case also runs on Windows with its native OpenGL backend
at 100% display scale (fractional scales are covered by the headless C++ tests).

No real desktop settings, compositor configuration or launcher files are changed.
SPDX-License-Identifier: Apache-2.0
"""
import math
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


def test_dialog_failure(executable, folder):
    # A private session and explicit invalid driver fail without showing a real dialog.
    log = folder / "dialog.log"
    environment = dict(os.environ, SDL_FILE_DIALOG_DRIVER="liltview-invalid-driver")
    with log.open("w") as output:
        app = subprocess.Popen([str(executable), "--config-dir", str(folder)], env=environment,
                               stdout=output, stderr=subprocess.STDOUT)
        try:
            service = find_service(app.pid)
            for count in (1, 2):
                bus_call(service, "Event", "10", "clicked", "<0>", "0")
                deadline = time.monotonic() + 5
                while log.read_text().count("File dialog failed: File dialog driver unsupported") < count:
                    assert app.poll() is None, log.read_text()
                    assert time.monotonic() < deadline, log.read_text()
                    time.sleep(0.02)
            bus_call(service, "Event", "12", "clicked", "<0>", "0")
            assert app.wait(timeout=10) == 0, log.read_text()
        finally:
            if app.poll() is None:
                app.terminate()
                app.wait(timeout=5)


def test_navigation(executable, folder):
    # Exercise exported actions all the way through Viewer and actual OpenGL display.
    sample = Path(__file__).resolve().parents[1] / "fixtures/red.bmp"
    for name in ("image1.bmp", "image2.BMP", "image10.bmp"):
        shutil.copy2(sample, folder / name)
    (folder / "settings.yaml").write_text("language: en-US\nlog_level: debug\n")
    log = folder / "navigation.log"
    with log.open("w") as output:
        app = subprocess.Popen([str(executable), "--config-dir", str(folder), str(folder / "image1.bmp")],
                               stdout=output, stderr=subprocess.STDOUT)
        try:
            service = find_service(app.pid)

            def wait_for(predicate):
                deadline = time.monotonic() + 10
                while not predicate():
                    assert app.poll() is None, log.read_text()
                    assert time.monotonic() < deadline, log.read_text()
                    time.sleep(0.02)

            def enabled(item, value):
                return str(value).lower() in bus_call(service, "GetProperty", str(item), "enabled").stdout

            wait_for(lambda: "Image displayed: image1.bmp" in log.read_text() and enabled(22, True))
            assert enabled(21, False)
            bus_call(service, "Event", "22", "clicked", "<0>", "0")
            wait_for(lambda: "Image displayed: image2.BMP" in log.read_text() and enabled(21, True))
            # A target removed after enumeration fails normally; its cursor still permits returning.
            (folder / "image10.bmp").unlink()
            bus_call(service, "Event", "22", "clicked", "<0>", "0")
            wait_for(lambda: enabled(22, False))
            assert enabled(21, True)
            bus_call(service, "Event", "21", "clicked", "<0>", "0")
            wait_for(lambda: log.read_text().count("Image displayed: image2.BMP") == 2)
            bus_call(service, "Event", "21", "clicked", "<0>", "0")
            wait_for(lambda: log.read_text().count("Image displayed: image1.bmp") == 2 and enabled(21, False))
            assert enabled(22, True)
            bus_call(service, "Event", "12", "clicked", "<0>", "0")
            assert app.wait(timeout=10) == 0, log.read_text()
        finally:
            if app.poll() is None:
                app.terminate()
                app.wait(timeout=5)


def test_prefetch(executable, folder):
    sample = Path(__file__).resolve().parents[1] / "fixtures/red.bmp"
    for name in ("1.bmp", "2.bmp", "3.bmp"):
        shutil.copy2(sample, folder / name)
    (folder / "settings.yaml").write_text("language: en-US\nlog_level: debug\n")
    log = folder / "prefetch.log"
    with log.open("w") as output:
        app = subprocess.Popen([str(executable), "--config-dir", str(folder), str(folder / "2.bmp")],
                               stdout=output, stderr=subprocess.STDOUT)
        try:
            service = find_service(app.pid)

            def wait_for(predicate):
                deadline = time.monotonic() + 10
                while not predicate():
                    assert app.poll() is None, log.read_text()
                    assert time.monotonic() < deadline, log.read_text()
                    time.sleep(0.02)

            wait_for(lambda: log.read_text().count("Neighbor prefetch completed:") >= 2)
            bus_call(service, "Event", "22", "clicked", "<0>", "0")
            wait_for(lambda: re.search(r"Image displayed: 3\.bmp, .*static_source_bytes_read=0", log.read_text()))
            bus_call(service, "Event", "21", "clicked", "<0>", "0")
            bus_call(service, "Event", "21", "clicked", "<0>", "0")
            wait_for(lambda: re.search(r"Image displayed: 1\.bmp, .*static_source_bytes_read=0", log.read_text()))
            bus_call(service, "Event", "12", "clicked", "<0>", "0")
            assert app.wait(timeout=10) == 0, log.read_text()
            assert "[critical]" not in log.read_text() and "[error]" not in log.read_text(), log.read_text()
        finally:
            if app.poll() is None:
                app.terminate()
                app.wait(timeout=5)


def test_info(executable, folder):
    sample = Path(__file__).resolve().parents[1] / "fixtures/red.bmp"
    first = folder / ("1-中文-%s-" + "x" * 100 + ".bmp")
    second = folder / "2-next.bmp"
    shutil.copy2(sample, first)
    shutil.copy2(sample, second)
    (folder / "settings.yaml").write_text("language: en-US\nlog_level: debug\nwidth: 640\nheight: 480\n")
    log = folder / "information.log"
    with log.open("w") as output:
        app = subprocess.Popen([str(executable), "--config-dir", str(folder), str(first)],
                               stdout=output, stderr=subprocess.STDOUT)
        try:
            service = find_service(app.pid)

            def property_is(item, key, expected):
                return expected in bus_call(service, "GetProperty", str(item), key).stdout

            def wait_for(predicate):
                deadline = time.monotonic() + 10
                while not predicate():
                    assert app.poll() is None, log.read_text()
                    assert time.monotonic() < deadline, log.read_text()
                    time.sleep(0.02)

            wait_for(lambda: property_is(57, "enabled", "true") and property_is(22, "enabled", "true"))
            assert property_is(57, "shortcut", "'i'")
            assert property_is(57, "toggle-state", "0")
            bus_call(service, "Event", "57", "clicked", "<0>", "0")
            wait_for(lambda: property_is(57, "toggle-state", "1"))
            assert "Image information panel: open=true" in log.read_text()
            # The panel stays open while a real directory navigation changes the source.
            bus_call(service, "Event", "22", "clicked", "<0>", "0")
            wait_for(lambda: "Image displayed: 2-next.bmp" in log.read_text())
            assert property_is(57, "toggle-state", "1")
            bus_call(service, "Event", "33", "clicked", "<0>", "0")
            wait_for(lambda: property_is(57, "label", "图片信息"))
            bus_call(service, "Event", "20", "clicked", "<0>", "0")
            wait_for(lambda: property_is(20, "toggle-state", "1"))
            bus_call(service, "Event", "57", "clicked", "<0>", "0")
            wait_for(lambda: property_is(57, "toggle-state", "0"))
            assert property_is(20, "toggle-state", "1")
            bus_call(service, "Event", "12", "clicked", "<0>", "0")
            assert app.wait(timeout=10) == 0, log.read_text()
            assert "[critical]" not in log.read_text() and "[error]" not in log.read_text(), log.read_text()
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
            (directory / "settings.yaml").write_text(f"theme: {theme}\nlanguage: {language}\nsystem_titlebar: true\n")
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


def test_chrome_pixels(executable, folder):
    for theme in ("light", "dark"):
        for square in (False, True):
            directory = folder / f"{theme}-{square}"
            directory.mkdir()
            (directory / "settings.yaml").write_text(f"theme: {theme}\nlanguage: en-US\nlog_level: debug\ndisable_rounding: {str(square).lower()}\n")
            screenshot = directory / "chrome.ppm"
            result = subprocess.run([str(executable), "--smoke-test", "--config-dir", str(directory), "--screenshot", str(screenshot)],
                                    text=True, capture_output=True, timeout=10)
            assert result.returncode == 0, result.stdout + result.stderr
            header, dimensions, maximum, pixels = screenshot.read_bytes().split(b"\n", 3)
            assert header == b"P6" and maximum == b"255"
            width, height = map(int, dimensions.split())
            alpha_header, alpha_dimensions, alpha_max, alpha = (directory / "chrome-alpha.pgm").read_bytes().split(b"\n", 3)
            assert alpha_header == b"P5" and alpha_dimensions == dimensions and alpha_max == b"255"

            def pixel(x, y):
                offset = (y * width + x) * 3
                return pixels[offset:offset + 3]

            # Theme-derived outline; verify visual properties rather than
            # coupling the regression to a fixed accent RGB triplet.
            border = pixel(width // 2, 0)
            start, end = (0, 0) if square else (14, 14)
            for y in (0, height - 1):
                assert all(pixel(x, y) == border for x in range(start, width - end)), (theme, "incomplete horizontal border", y)
            for x in (0, width - 1):
                assert all(pixel(x, y) == border for y in range(start, height - end)), (theme, "incomplete vertical border", x)
            assert pixel(width // 2, 1) != border, (theme, "top outline is thicker than one pixel")
            assert pixel(width // 2, height - 2) != border, (theme, "bottom outline is thicker than one pixel")
            assert pixel(1, height // 2) != border, (theme, "left outline is thicker than one pixel")
            assert pixel(width - 2, height // 2) != border, (theme, "right outline is thicker than one pixel")
            corners = (0, width - 1, (height - 1) * width, width * height - 1)
            assert all(alpha[index] == (255 if square else 0) for index in corners), (theme, square, "incorrect corner alpha")
            assert alpha[(height // 2) * width + width // 2] == 255
            if not square:
                # Pixel-center samples around the entire outer AA fringe, not
                # just the fully transparent corner or straight border pixels.
                # Transparent framebuffers contain premultiplied RGB; opaque
                # X11 visuals must instead retain the border-colored clear RGB.
                transparent = "transparent=true" in result.stdout
                radius = 12  # This offscreen/smoke fixture uses 100% display scale.
                for corner in range(4):
                    checked = 0
                    for y in range(radius):
                        for x in range(radius):
                            distance = math.hypot(radius - x - 0.5, radius - y - 0.5)
                            if not radius - 0.25 <= distance <= radius + 0.75:
                                continue
                            px = width - 1 - x if corner & 1 else x
                            py = height - 1 - y if corner & 2 else y
                            coverage = alpha[py * width + px]
                            if coverage < 8:
                                continue
                            factor = coverage / 255 if transparent else 1
                            expected = [round(channel * factor) for channel in border]
                            actual = pixel(px, py)
                            assert all(abs(a - b) <= 2 for a, b in zip(actual, expected)), (
                                theme, corner, x, y, "background halo", actual, expected, coverage)
                            checked += 1
                    assert checked > 0, (theme, corner, "missing rounded edge pixels")
            _, _, _, about_alpha = (directory / "chrome-about-alpha.pgm").read_bytes().split(b"\n", 3)
            assert all(about_alpha[index] == (255 if square else 0) for index in corners), (theme, square, "modal painted transparent corners")
            title = pixel(width // 2, 20)
            menu = pixel(width // 2, 50)
            canvas = pixel(width // 2, 100)
            assert title != menu != canvas, (theme, "missing title/menu/canvas rows")
            assert pixel(2, 69) == canvas and pixel(width - 3, 69) == canvas, (theme, "padded canvas")
            footer = pixel(width // 2, height - 3)
            assert footer != border and footer != canvas, (theme, "missing status bar")
            def luminance(color):
                rgb = [v / 255 for v in color]
                linear = [v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in rgb]
                return sum(a * b for a, b in zip(linear, (0.2126, 0.7152, 0.0722)))

            border_light = luminance(border)
            # Stay clear of the foreground stroke's inner AA fringe.
            status_background = pixel(width // 2, height - 8)
            for surface in (title, menu, canvas, status_background):
                surface_light = luminance(surface)
                contrast = (max(border_light, surface_light) + 0.05) / (min(border_light, surface_light) + 0.05)
                assert contrast >= 4.5, (theme, "low-contrast outline", border, surface, contrast)
                assert (border_light > surface_light) == (theme == "dark"), (theme, "wrong outline polarity")


def test_chrome(executable, folder):
    test_chrome_pixels(executable, folder)

    # Native -> custom -> native must retain the same process, menu and loaded image.
    directory = folder / "toggle"
    directory.mkdir()
    settings = directory / "settings.yaml"
    settings.write_text("system_titlebar: true\nlog_level: debug\nlanguage: en-US\n")
    sample = Path(__file__).resolve().parents[1] / "fixtures/red.bmp"
    log = directory / "app.log"
    with log.open("w") as output:
        app = subprocess.Popen([str(executable), "--config-dir", str(directory), str(sample)], stdout=output, stderr=subprocess.STDOUT)
        try:
            service = find_service(app.pid)
            deadline = time.monotonic() + 5
            while "Image displayed: red.bmp" not in log.read_text():
                assert app.poll() is None and time.monotonic() < deadline, log.read_text()
                time.sleep(0.01)
            assert "checkmark" in bus_call(service, "GetProperty", "38", "toggle-type").stdout
            assert "checkmark" in bus_call(service, "GetProperty", "41", "toggle-type").stdout
            assert "false" in bus_call(service, "GetProperty", "41", "enabled").stdout
            assert "Enable system titlebar" in bus_call(service, "GetProperty", "38", "label").stdout
            for enabled in (False, True, False):
                bus_call(service, "Event", "38", "clicked", "<0>", "0")
                deadline = time.monotonic() + 5
                while f"<{int(enabled)}>" not in bus_call(service, "GetProperty", "38", "toggle-state").stdout:
                    assert app.poll() is None and time.monotonic() < deadline, log.read_text()
                    time.sleep(0.01)
            bus_call(service, "Event", "41", "clicked", "<0>", "0")
            deadline = time.monotonic() + 5
            while "<1>" not in bus_call(service, "GetProperty", "41", "toggle-state").stdout:
                assert app.poll() is None and time.monotonic() < deadline, log.read_text()
                time.sleep(0.01)
            bus_call(service, "Event", "12", "clicked", "<0>", "0")
            assert app.wait(timeout=10) == 0, log.read_text()
            text = log.read_text()
            assert len(re.findall(r"LiltView [^\n]+ started,", text)) == 1, text
            assert text.count("Image displayed: red.bmp") == 1, text
            windows = re.findall(r"Window created: system_titlebar=(true|false), transparent=(true|false), native_window_id=(\d+)", text)
            alpha = "false" if os.environ.get("SDL_VIDEODRIVER") == "x11" else "true"
            assert [(native, transparent) for native, transparent, _ in windows] == [("true", alpha)], text
            reused = re.findall(r"native_window_id=(\d+), surface_reused=true, gl_context_reused=true", text)
            assert reused == [windows[0][2]] * 3, text
            assert "Window handoff:" not in text and "Window placement:" not in text, text
            assert text.count("Window decoration applied:") == 3, text
            assert "[error]" not in text and "[critical]" not in text, text
            assert "system_titlebar: false" in settings.read_text(), settings.read_text()
            assert "disable_rounding: true" in settings.read_text(), settings.read_text()
        finally:
            if app.poll() is None:
                app.terminate()
                app.wait(timeout=5)


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
    if sys.platform != "win32":
        os.environ["SDL_VIDEODRIVER"] = "offscreen"
    with tempfile.TemporaryDirectory(prefix="liltview-desktop-regression-") as temporary:
        {"menu": test_menu, "font": test_font, "save": test_save_failure, "jpeg": test_jpeg_render,
         "settings": test_settings_yaml, "logging": test_logging, "formats": test_formats_render,
         "animation_app": test_animation_app, "status": test_status, "close": test_close, "chrome": test_chrome,
         "chrome_pixels": test_chrome_pixels,
         "navigation": test_navigation, "info": test_info, "prefetch": test_prefetch,
         "dialog_failure": test_dialog_failure}[case](executable, Path(temporary))
    print(f"PASS desktop regression: {case}")


if __name__ == "__main__":
    main()
