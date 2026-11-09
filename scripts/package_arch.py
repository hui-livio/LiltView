#!/usr/bin/env python3
"""Prepare, build or install an Arch package from the current LiltView checkout.

Uses makepkg/pacman or paru's local PKGBUILD mode; no AUR publication is needed.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent.parent
TEMPLATE = ROOT / "packaging/arch/PKGBUILD.in"


def git(*arguments):
    return subprocess.run(
        ["git", "-C", str(ROOT), *arguments], check=True, capture_output=True
    ).stdout


def source_identity(output):
    """Keep package versions traceable without depending on upstream Git tags."""
    text = (ROOT / "CMakeLists.txt").read_text()
    match = re.search(r"project\(LiltView\s+VERSION\s+(\d+\.\d+\.\d+)", text)
    if not match:
        raise ValueError("Cannot read the LiltView version from CMakeLists.txt")
    modules = git("submodule", "status", "--recursive").decode().splitlines()
    for line in modules:
        if not line.startswith(" "):
            raise ValueError(
                "Submodules must match the recorded commits. Check local changes, then run "
                "git submodule update --init --recursive.\n" + line
            )
    revision = git("rev-parse", "HEAD").decode().strip()
    count = git("rev-list", "--count", "HEAD").decode().strip()
    paths = ["."]
    if output.is_relative_to(ROOT):
        paths.append(f":(exclude,literal){output.relative_to(ROOT)}")
    dirty = bool(git("status", "--porcelain", "--untracked-files=normal", "--", *paths))
    version = f"{match[1]}.r{count}.g{revision[:12]}" + (".dirty" if dirty else "")
    identity = {
        "git_revision": revision,
        "dirty": dirty,
        "submodules": {line.split()[1]: line.split()[0] for line in modules},
    }
    epoch = int(git("show", "-s", "--format=%ct", "HEAD").strip())
    return version, identity, epoch


def source_files(output):
    """Include working files and recursive tracked dependencies, never Git data."""
    tracked = git("ls-files", "--cached", "--recurse-submodules", "-z")
    untracked = git("ls-files", "--others", "--exclude-standard", "-z")
    files = []
    for raw in sorted(set(tracked.split(b"\0") + untracked.split(b"\0")) - {b""}):
        name = Path(os.fsdecode(raw))
        path = ROOT / name
        # Explicitly exclude custom output directories even if not Git-ignored.
        if path.is_relative_to(output) or ".git" in name.parts:
            continue
        if path.is_symlink() or path.is_file():
            files.append((name, path))
    return files


def normalize(info, epoch):
    info.uid = info.gid = 0
    info.uname = info.gname = "root"
    info.mtime = epoch
    info.pax_headers = {}
    info.mode = 0o755 if info.mode & 0o111 else 0o644
    return info


def write_archive(destination, prefix, files, identity, epoch):
    # Stream files into gzip rather than loading the source tree into memory.
    with destination.open("wb") as output:
        with gzip.GzipFile(filename="", fileobj=output, mode="wb", mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w|") as archive:
                for name, path in files:
                    info = normalize(archive.gettarinfo(str(path), arcname=f"{prefix}/{name}"), epoch)
                    if info.isfile():
                        with path.open("rb") as source:
                            archive.addfile(info, source)
                    else:
                        archive.addfile(info)
                data = (json.dumps(identity, indent=2, sort_keys=True) + "\n").encode()
                info = normalize(tarfile.TarInfo(f"{prefix}/LILTVIEW_SOURCE.json"), epoch)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))


def prepare(output, release):
    version, identity, epoch = source_identity(output)
    files = source_files(output)
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".snapshot-", dir=output) as temporary:
        filename = f"liltview-{version}.tar.gz"
        archive = Path(temporary) / filename
        write_archive(archive, f"liltview-{version}", files, identity, epoch)
        with archive.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        directory = output / f"{version}-{release}-{digest[:12]}"
        directory.mkdir(exist_ok=True)
        # Repeated preparation of unchanged input keeps makepkg's source cache valid.
        if not (directory / filename).exists():
            shutil.move(archive, directory / filename)
    recipe = TEMPLATE.read_text().replace("@PKGVER@", version).replace("@PKGREL@", str(release)).replace("@SHA256@", digest)
    (directory / "PKGBUILD").write_text(recipe)
    environment = os.environ.copy()
    for name in ("PKGDEST", "SRCDEST", "SRCPKGDEST", "LOGDEST", "BUILDDIR"):
        environment[name] = str(directory)
    metadata = subprocess.run(
        ["makepkg", "--printsrcinfo"], cwd=directory, env=environment, check=True, capture_output=True, text=True
    )
    (directory / ".SRCINFO").write_text(metadata.stdout)
    return directory, environment


def positive_integer(text):
    number = int(text)
    if number < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return number


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "build", "install"), nargs="?", default="build")
    parser.add_argument("--manager", choices=("pacman", "paru"), default="pacman")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/arch")
    parser.add_argument("--pkgrel", type=positive_integer, default=1)
    parser.add_argument("--jobs", type=positive_integer, help="parallel CMake build jobs")
    parser.add_argument("--syncdeps", action="store_true", help="let makepkg install missing dependencies using pacman")
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("run as a normal user; makepkg/paru request privilege only for pacman operations")
    for tool in ("git", "makepkg") + (("paru",) if args.manager == "paru" and args.action != "prepare" else ()):
        if not shutil.which(tool):
            parser.error(f"missing command: {tool}")
    args.output_dir = args.output_dir.resolve()
    if args.output_dir == ROOT or ROOT.is_relative_to(args.output_dir):
        parser.error("output directory must not contain the source checkout")
    if args.manager == "paru" and args.syncdeps:
        parser.error("--syncdeps is for makepkg/pacman; paru resolves dependencies itself")
    return args


def main():
    args = parse_arguments()
    directory, environment = prepare(args.output_dir, args.pkgrel)
    if args.jobs:
        environment["CMAKE_BUILD_PARALLEL_LEVEL"] = str(args.jobs)
    print(f"Prepared local PKGBUILD and source snapshot: {directory}", flush=True)
    if args.action == "prepare":
        print(f"Build: cd {shlex.quote(str(directory))} && makepkg --syncdeps")
        print(f"Or: paru --build --install {shlex.quote(str(directory))}")
        return
    if args.manager == "paru":
        command = ["paru", "--build", str(directory)]
    else:
        command = ["makepkg"]
        if args.syncdeps:
            command.append("--syncdeps")
    if args.action == "install":
        command.append("--install")
    subprocess.run(command, cwd=directory, env=environment, check=True)
    result = subprocess.run(
        ["makepkg", "--packagelist"], cwd=directory, env=environment, capture_output=True, text=True, check=True
    )
    packages = [path for path in result.stdout.splitlines() if Path(path).is_file()]
    for package in packages:
        print(f"Package: {package}")
    if args.action == "build" and packages:
        print("Install: sudo pacman -U " + " ".join(shlex.quote(path) for path in packages))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Arch packaging failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr, file=sys.stderr)
        sys.exit(1)
