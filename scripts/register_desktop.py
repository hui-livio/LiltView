#!/usr/bin/env python3
"""Register an existing LiltView build in the current user's desktop menu.

Keeps the desktop filename, Wayland app_id, X11 WM_CLASS and icon name aligned.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
APP_ID = "io.github.hui_livio.LiltView"


def desktop_exec(path):
    """Escape one executable argument for desktop-entry string + Exec parsing."""
    value = str(path)
    if any(character in value for character in ("\n", "\r", "\t", "=", "%")):
        raise ValueError("Executable path contains a character unsupported by this desktop launcher")
    for character in ("\\", '"', "`", "$"):
        value = value.replace(character, "\\" + character)
    return '"' + value.replace("\\", "\\\\") + '"'


def write_launcher(executable, data_root):
    desktop = data_root / f"applications/{APP_ID}.desktop"
    template = (ROOT / f"packaging/linux/{APP_ID}.desktop").read_text()
    contents = template.replace("Exec=liltview\n", f"Exec={desktop_exec(executable)}\n")
    # Validate before replacing a user's launcher; this script only owns its app ID.
    validator = shutil.which("desktop-file-validate")
    if desktop.exists() and "StartupWMClass=" + APP_ID not in desktop.read_text():
        raise ValueError(f"Existing unrelated launcher found: {desktop}")
    desktop.parent.mkdir(parents=True, exist_ok=True)
    desktop.write_text(contents)
    if validator:
        subprocess.run([validator, str(desktop)], check=True)
    return desktop


def install_icons(data_root):
    icons = data_root / "icons/hicolor"
    target = icons / f"scalable/apps/{APP_ID}.svg"
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / "assets/icons/liltview-color.svg", target)
    for size in (16, 24, 32, 48, 64, 128, 256, 512):
        target = icons / f"{size}x{size}/apps/{APP_ID}.png"
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / f"assets/icons/png/liltview-color-{size}.png", target)


def refresh_desktop_cache(desktop):
    for tool, arguments in (
        ("update-desktop-database", [str(desktop.parent)]),
        ("kbuildsycoca6", ["--noincremental"]),
    ):
        command = shutil.which(tool)
        if command:
            # Cache refresh is best effort; resource files remain valid if no session is running.
            subprocess.run([command, *arguments], check=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=ROOT / "build/dev/app/liltview")
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise ValueError(f"Build the application first: {executable}")
    data_root = Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share")))
    if not data_root.is_absolute():
        raise ValueError("XDG_DATA_HOME must be an absolute directory")
    desktop = write_launcher(executable, data_root)
    install_icons(data_root)
    refresh_desktop_cache(desktop)
    print(f"Registered desktop launcher: {desktop}")
    print(f"Application ID / Dock icon: {APP_ID}")
    print("Restart LiltView, then pin its new launcher if desired.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Desktop registration failed: {error}", file=sys.stderr)
        sys.exit(1)
