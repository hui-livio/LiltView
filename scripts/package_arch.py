#!/usr/bin/env python3
"""Convenience wrapper for the checked-in native Arch PKGBUILD.

Direct usage is supported: cd packaging/arch && makepkg -si.
SPDX-License-Identifier: Apache-2.0
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
RECIPE_DIR = ROOT / "packaging/arch"


def positive_integer(text):
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "build", "install"), nargs="?", default="build")
    parser.add_argument("--manager", choices=("pacman", "paru"), default="pacman")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/arch")
    parser.add_argument("--jobs", type=positive_integer)
    parser.add_argument("--syncdeps", action="store_true")
    parser.add_argument("--tests", action="store_true", help="build and run Release UT (disabled by default)")
    args = parser.parse_args()
    if not hasattr(os, "geteuid") or os.geteuid() == 0:
        parser.error("run on Arch Linux as a normal user; makepkg requests privilege only for pacman")
    for tool in ("makepkg",) + (("paru",) if args.manager == "paru" and args.action != "prepare" else ()):
        if not shutil.which(tool):
            parser.error(f"missing command: {tool}")
    if args.manager == "paru" and args.syncdeps:
        parser.error("paru resolves dependencies itself; --syncdeps is for makepkg")
    output = args.output_dir.resolve()
    if output == ROOT or ROOT.is_relative_to(output):
        parser.error("output directory must not contain the source tree")
    output.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    (output / "work").mkdir(exist_ok=True)
    environment.update(PKGDEST=str(output), BUILDDIR=str(output / "work"), LILTVIEW_SOURCE_DIR=str(ROOT))
    if args.tests:
        environment["LILTVIEW_RUN_TESTS"] = "1"
    if args.jobs:
        environment["CMAKE_BUILD_PARALLEL_LEVEL"] = str(args.jobs)
    if args.action == "prepare":
        metadata = subprocess.check_output(["makepkg", "--printsrcinfo"], cwd=RECIPE_DIR, env=environment)
        (output / ".SRCINFO").write_bytes(metadata)
        print(f"PKGBUILD (not generated): {RECIPE_DIR / 'PKGBUILD'}")
        print(f"Metadata: {output / '.SRCINFO'}")
        print(f"Build: cd {shlex.quote(str(RECIPE_DIR))} && makepkg -s")
        return
    command = ["paru", "--build", str(RECIPE_DIR)] if args.manager == "paru" else ["makepkg"]
    if args.tests or environment.get("LILTVIEW_RUN_TESTS") == "1":
        command.append("--mflags=--check" if args.manager == "paru" else "--check")
    if args.syncdeps:
        command.append("--syncdeps")
    if args.action == "install":
        command.append("--install")
    subprocess.run(command, cwd=RECIPE_DIR, env=environment, check=True)
    paths = subprocess.check_output(["makepkg", "--packagelist"], cwd=RECIPE_DIR, env=environment, text=True)
    for line in paths.splitlines():
        if Path(line).is_file():
            print(f"Package: {line}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Arch packaging failed: {error}", file=sys.stderr)
        sys.exit(1)
