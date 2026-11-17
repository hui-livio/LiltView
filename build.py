#!/usr/bin/env python3
"""Build LiltView with Python 3.11+ and the existing Release CMake presets.

Default: configure, build and run Release UT. Requires CMake, Ninja and a C23
compiler (Windows: an x64 MSVC developer environment). No Python packages needed.
SPDX-License-Identifier: Apache-2.0
"""
import argparse
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parent


def positive_integer(text):
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return value


def tool_output(*command):
    return subprocess.check_output(command, cwd=ROOT, text=True).strip()


def platform_definitions():
    """Select the same native compilers used by the release pipeline."""
    if platform.system() == "Windows":
        return [f"-DCMAKE_C_COMPILER={os.environ.get('CC') or 'cl.exe'}",
                f"-DCMAKE_CXX_COMPILER={os.environ.get('CXX') or 'cl.exe'}"]
    if platform.system() == "Darwin":
        # Homebrew LLVM supports the C23 core; Apple compilers retain the system
        # libc++ and Cocoa/Metal Objective-C framework integration.
        definitions = [
            "-DCMAKE_C_COMPILER=" + (os.environ.get("CC") or tool_output("brew", "--prefix", "llvm@20") + "/bin/clang"),
            "-DCMAKE_CXX_COMPILER=" + (os.environ.get("CXX") or tool_output("xcrun", "-f", "clang++")),
            "-DCMAKE_OBJC_COMPILER=" + (os.environ.get("OBJC") or tool_output("xcrun", "-f", "clang")),
            "-DCMAKE_OSX_SYSROOT=" + tool_output("xcrun", "--show-sdk-path"),
        ]
        if os.environ.get("MACOSX_DEPLOYMENT_TARGET"):
            definitions.append("-DCMAKE_OSX_DEPLOYMENT_TARGET=" + os.environ["MACOSX_DEPLOYMENT_TARGET"])
        return definitions
    return []


def run(command, *, cwd=ROOT, env=None):
    # Lists, never shell strings: cl.exe and paths containing spaces must remain
    # single arguments on both PowerShell and POSIX runners.
    print("+ " + shlex.join(map(str, command)), flush=True)
    subprocess.run(command, cwd=cwd, env=env, check=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("all", "configure", "build", "test", "arch"),
                        nargs="?", default="all",
                        help="build configures and compiles; all also runs UT; arch uses native makepkg with UT")
    parser.add_argument("--preset", choices=("release-ci", "app-release"), default="release-ci")
    parser.add_argument("--build-dir", type=Path, help="override the preset build directory")
    parser.add_argument("--parallel", type=positive_integer,
                        default=os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL") or str(min(os.cpu_count() or 1, 8)))
    parser.add_argument("--test-parallel", type=positive_integer, default=1 if os.name == "nt" else 2)
    parser.add_argument("--junit", type=Path, help="write CTest JUnit XML to this path")
    parser.add_argument("--version-suffix", default=os.environ.get("VERSION_SUFFIX"),
                        help="RC suffix; by default use VERSION_SUFFIX or the source RELEASE_VERSION marker")
    parser.add_argument("-D", dest="definitions", action="append", default=[], metavar="NAME=VALUE",
                        help="additional CMake cache definition (repeatable)")
    args = parser.parse_args(argv)
    for definition in args.definitions:
        if "=" not in definition or not definition.split("=", 1)[0]:
            parser.error("-D requires NAME=VALUE")
    if args.action in ("all", "test") and args.preset != "release-ci":
        parser.error("running UT requires --preset release-ci; use build for an application-only build")
    if args.action == "arch":
        if args.build_dir or args.junit or args.definitions or args.preset != "release-ci":
            parser.error("arch uses the native PKGBUILD layout and does not accept preset/directory/CMake overrides")
        environment = dict(os.environ, LILTVIEW_RUN_TESTS="1", CMAKE_BUILD_PARALLEL_LEVEL=str(args.parallel))
        if args.version_suffix is not None:
            # PKGBUILD validates the full version and preserves RC package ordering.
            from scripts.package_release import version
            environment["LILTVIEW_RELEASE_VERSION"] = version(ROOT) + args.version_suffix
        run(["makepkg", "--check", "--noconfirm"], cwd=ROOT / "packaging/arch", env=environment)
        return 0
    build_dir = args.build_dir.resolve() if args.build_dir else ROOT / "build" / args.preset
    if args.action in ("all", "configure", "build"):
        command = ["cmake", "--preset", args.preset, "-B", str(build_dir)] + platform_definitions()
        if args.version_suffix is not None:
            command.append("-DLILTVIEW_VERSION_SUFFIX=" + args.version_suffix)
        command.extend("-D" + value for value in args.definitions)
        run(command)
    if args.action in ("all", "build"):
        run(["cmake", "--build", str(build_dir), "--config", "Release", "--parallel", str(args.parallel)])
    if args.action in ("all", "test"):
        command = ["ctest", "--test-dir", str(build_dir), "--build-config", "Release",
                   "--parallel", str(args.test_parallel), "--output-on-failure", "--no-tests=error"]
        if args.junit:
            destination = args.junit.resolve()
            destination.parent.mkdir(parents=True, exist_ok=True)
            command.extend(["--output-junit", str(destination)])
        run(command)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        print(f"Build failed: {error}", file=sys.stderr)
        sys.exit(error.returncode if error.returncode > 0 else 1)
    except (OSError, ValueError) as error:
        print(f"Build failed: {error}", file=sys.stderr)
        sys.exit(1)
