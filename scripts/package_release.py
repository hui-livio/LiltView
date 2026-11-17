#!/usr/bin/env python3
"""Create complete, reproducible release sources and native package inputs.

Only repository contents and pinned recursive dependencies are included; no Git
metadata, build products or untracked files. SPDX-License-Identifier: Apache-2.0
"""
import argparse
import gzip
import hashlib
import io
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parent.parent


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args])


def version(root=ROOT):
    match = re.search(r"project\(LiltView\s+VERSION\s+(\d+\.\d+\.\d+)",
                      (root / "CMakeLists.txt").read_text(encoding="utf-8"))
    if not match:
        raise ValueError("Cannot read project version")
    return match[1]


def release_version(root=ROOT, requested=None):
    base = version(root)
    marker = root / "RELEASE_VERSION"
    recorded = marker.read_text(encoding="utf-8").strip() if marker.is_file() else None
    value = requested or os.environ.get("LILTVIEW_RELEASE_VERSION") or recorded or base
    if not re.fullmatch(re.escape(base) + r"(?:-rc\.[1-9][0-9]*)?", value):
        raise ValueError(f"Release version must be {base} or {base}-rc.N")
    return value


def version_for_ref(ref, root=ROOT):
    if ref.startswith("refs/tags/"):
        tag = ref.removeprefix("refs/tags/")
        if not tag.startswith("v"):
            raise ValueError("Release tags must start with v")
        return release_version(root, tag[1:])
    # Manual/PR builds are previews by default, never mislabel them stable.
    return release_version(root, version(root) + "-rc.1")


def excluded(name):
    # Provenance/license source.json files are retained; Git administrative files
    # and hosted CI configuration are not needed to build the source distribution.
    return any(part == ".github" or part == ".git" or part.startswith(".git") for part in name.parts)


def source_entries(root=ROOT):
    entries = []
    for record in git(root, "ls-files", "--stage", "--recurse-submodules", "-z").split(b"\0"):
        if not record:
            continue
        metadata, raw_name = record.split(b"\t", 1)
        mode, _, stage = metadata.split()
        name = PurePosixPath(os.fsdecode(raw_name))
        if excluded(name):
            continue
        if stage != b"0" or mode == b"160000":
            raise ValueError(f"Unmerged or uninitialized dependency: {name}")
        if mode not in (b"100644", b"100755", b"120000"):
            raise ValueError(f"Unsupported source entry: {name}")
        path = root.joinpath(*name.parts)
        if not path.is_file() and not path.is_symlink():
            raise ValueError(f"Missing tracked source: {name}")
        if path.is_symlink():
            target = os.readlink(path)
            if Path(target).is_absolute() or not path.resolve().is_relative_to(root.resolve()):
                raise ValueError(f"Unsafe source symlink: {name}")
        entries.append((name.as_posix(), path, int(mode, 8)))
    return sorted(entries)


def validate_checkout(root=ROOT, allow_dirty=False):
    for line in git(root, "submodule", "status", "--recursive").decode().splitlines():
        if not line.startswith(" "):
            raise ValueError("Initialize all pinned dependencies: git submodule update --init --recursive\n" + line)
    if not allow_dirty and git(root, "status", "--porcelain", "--untracked-files=normal").strip():
        raise ValueError("Release sources require a clean checkout")


def write_source(destination, root=ROOT, allow_dirty=False):
    validate_checkout(root, allow_dirty)
    entries = source_entries(root)
    prefix = f"liltview-{release_version(root)}"
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH") or git(root, "show", "-s", "--format=%ct", "HEAD"))
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("wb") as output:
        with gzip.GzipFile(filename="", mode="wb", fileobj=output, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w|") as archive:
                # Preserve the RC identity when building the archive offline.
                recorded = (release_version(root) + "\n").encode()
                marker = tarfile.TarInfo(f"{prefix}/RELEASE_VERSION")
                marker.mode, marker.mtime, marker.size = 0o644, epoch, len(recorded)
                marker.uname = marker.gname = "root"
                archive.addfile(marker, io.BytesIO(recorded))
                for name, path, mode in entries:
                    if name == "RELEASE_VERSION":
                        continue
                    info = archive.gettarinfo(str(path), f"{prefix}/{name}")
                    info.uid = info.gid = 0
                    info.uname = info.gname = "root"
                    info.mtime = epoch
                    info.mode = 0o755 if mode == 0o100755 else 0o644
                    info.pax_headers = {}
                    if info.isfile():
                        with path.open("rb") as source:
                            archive.addfile(info, source)
                    else:
                        archive.addfile(info)
    return destination


def sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def checksums(directory):
    files = sorted(path for path in directory.iterdir() if path.is_file() and path.name != "SHA256SUMS")
    if not files:
        raise ValueError("No release assets to checksum")
    (directory / "SHA256SUMS").write_text(
        "".join(f"{sha256(path)}  {path.name}\n" for path in files), encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("version", "source", "checksums"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/dist")
    parser.add_argument("--ref", help="Optional GitHub tag ref; must match project version")
    args = parser.parse_args()
    try:
        selected_version = version_for_ref(args.ref) if args.ref else release_version()
    except ValueError as error:
        parser.error(str(error))
    os.environ["LILTVIEW_RELEASE_VERSION"] = selected_version
    if args.action == "version":
        print(selected_version)
    elif args.action == "source":
        print(write_source(args.output / f"liltview-{selected_version}-source.tar.gz"))
    else:
        checksums(args.output)


if __name__ == "__main__":
    main()
