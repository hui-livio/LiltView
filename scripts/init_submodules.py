#!/usr/bin/env python3
"""Register/pin declared submodules, or check them without mutation or network.

Ordinary clones use git submodule update --init --recursive.
This script never commits or pushes. SPDX-License-Identifier: Apache-2.0
"""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent


def git(*args, cwd=ROOT, capture=False):
    result = subprocess.run(
        ["git", *args], cwd=cwd, check=True, text=True,
        stdout=subprocess.PIPE if capture else None,
    )
    return result.stdout.strip() if capture else ""


def read_dependencies():
    manifest = json.loads((ROOT / "third_party/dependencies.json").read_text())
    if manifest["schema_version"] != 1:
        raise ValueError("Unsupported third-party manifest schema")
    for name, entry in manifest["submodules"].items():
        if not re.fullmatch(r"[A-Za-z0-9_-]+", name):
            raise ValueError(f"Invalid submodule name: {name}")
        if entry["path"] != f"third_party/{name}":
            raise ValueError(f"Unexpected submodule path: {entry['path']}")
        if not re.fullmatch(r"[0-9a-f]{40}", entry["commit"]):
            raise ValueError(f"Invalid pinned commit for {name}")
        for key in ("path", "url"):
            configured = git(
                "config", "--file", ".gitmodules", "--get",
                f"submodule.{name}.{key}", capture=True,
            )
            if configured != entry[key]:
                raise ValueError(f".gitmodules disagrees with manifest: {name}.{key}")
    return manifest["submodules"]


def index_commit(path):
    record = git("ls-files", "--stage", "--", path, capture=True)
    if not record:
        return None
    fields = record.split()
    if len(fields) != 4 or fields[0] != "160000" or fields[2] != "0":
        raise ValueError(f"Expected a submodule gitlink at {path}, found ordinary files or conflict")
    return fields[1]


def require_clean_checkout(path):
    if (path / ".git").exists() and git("status", "--porcelain", capture=True, cwd=path):
        raise ValueError(f"Submodule has local changes; refusing to overwrite: {path}")


def validate_worktrees(dependencies):
    """Review every checkout before registering or changing any submodule."""
    # Validate all existing paths before mutating any dependency.
    for entry in dependencies.values():
        path = ROOT / entry["path"]
        recorded = index_commit(entry["path"])
        if recorded is None and path.exists():
            raise ValueError(f"Unregistered dependency directory already exists: {path}; review it manually")
        require_clean_checkout(path)


def check_dependency(entry):
    relative = entry["path"]
    path = ROOT / relative
    expected = entry["commit"]
    if index_commit(relative) != expected:
        raise ValueError(f"Missing or mismatched gitlink: {relative}; run this script without --check")
    if not (path / ".git").exists():
        raise ValueError(f"Uninitialized submodule: {relative}; run git submodule update --init --recursive")
    actual = git("rev-parse", "HEAD", cwd=path, capture=True)
    if actual != expected:
        raise ValueError(f"Checkout mismatch: {relative}; expected {expected}, got {actual}")


def register_dependency(name, entry):
    relative = entry["path"]
    path = ROOT / relative
    expected = entry["commit"]
    if index_commit(relative) is None:
        git("submodule", "add", "--name", name, "--", entry["url"], relative)
    else:
        git("submodule", "update", "--init", "--recursive", "--", relative)
    require_clean_checkout(path)
    exists = subprocess.run(
        ["git", "cat-file", "-e", f"{expected}^{{commit}}"], cwd=path,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    ).returncode == 0
    if not exists:
        git("fetch", "origin", expected, cwd=path)
    git("checkout", "--detach", expected, cwd=path)
    git("submodule", "update", "--init", "--recursive", cwd=path)
    git("add", "--", ".gitmodules", relative)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Check pins and sources without mutation or network")
    args = parser.parse_args()
    dependencies = read_dependencies()
    validate_worktrees(dependencies)
    for name, entry in dependencies.items():
        if args.check:
            check_dependency(entry)
        else:
            register_dependency(name, entry)
        expected = entry["commit"]
        print(f"OK {name}: {entry['version']} ({expected})", flush=True)
    if not args.check:
        print("Submodule gitlinks staged. Review git diff --cached --submodule before committing.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        print(f"Submodule setup failed: {error}", file=sys.stderr)
        sys.exit(1)
