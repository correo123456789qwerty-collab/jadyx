#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Preserve checkout mtimes only for byte-identical inputs of a cached Ninja build.

The workflow's cache key pins toolchain, configuration, dependencies and this script. This
manifest additionally pins the workspace path and hashes every tracked, regular source file.
Changed inputs always get a timestamp newer than every cached output, even if their size and
checkout timestamp happen to match. CMake/Ninja still decide and perform the actual build.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

FORMAT = 1


def tracked(root):
    output = subprocess.check_output(["git", "ls-files", "-z"], cwd=root)
    return [Path(os.fsdecode(name)) for name in output.split(b"\0") if name]


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def regular(path):
    return not path.is_symlink() and path.is_file()


def save(root, build):
    root, build = root.resolve(), build.resolve()
    if not build.is_relative_to(root) or build == root:
        raise ValueError("Build directory must be inside the workspace")
    inputs = {}
    for relative in tracked(root):
        path = root / relative
        if regular(path):
            inputs[relative.as_posix()] = {
                "sha256": digest(path), "mtime_ns": path.stat().st_mtime_ns,
            }
    highwater = time.time_ns()
    for directory, _, names in os.walk(build):
        for name in names:
            highwater = max(highwater, (Path(directory) / name).lstat().st_mtime_ns)
    data = {"format": FORMAT, "root": str(root), "highwater_ns": highwater, "inputs": inputs}
    target = build / "input-state.json"
    temporary = target.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(data, separators=(",", ":")), encoding="utf-8")
    temporary.replace(target)
    print(f"Saved incremental input state: {len(inputs)} files")


def restore(root, build):
    root, build = root.resolve(), build.resolve()
    if not build.is_relative_to(root) or build == root:
        raise ValueError("Build directory must be inside the workspace")
    data = json.loads((build / "input-state.json").read_text(encoding="utf-8"))
    if data.get("format") != FORMAT or Path(data["root"]).resolve() != root:
        raise ValueError("Incremental build format/workspace mismatch")
    inputs = data["inputs"]
    # Validate the entire manifest before changing a timestamp.
    for name, record in inputs.items():
        relative = Path(name)
        if relative.is_absolute() or relative.drive or ".." in relative.parts or not relative.parts:
            raise ValueError("Invalid incremental input path")
        if len(record["sha256"]) != 64 or not isinstance(record["mtime_ns"], int):
            raise ValueError("Invalid incremental input record")
    changed_time = max(time.time_ns(), int(data["highwater_ns"]) + 1_000_000)
    reused = changed = skipped = 0
    for relative in tracked(root):
        path = root / relative
        if not regular(path):
            skipped += 1
            continue
        record = inputs.get(relative.as_posix())
        if record and digest(path) == record["sha256"]:
            stamp = record["mtime_ns"]
            reused += 1
        else:
            stamp = changed_time
            changed += 1
        os.utime(path, ns=(path.stat().st_atime_ns, stamp))
    print(f"Incremental inputs: {reused} unchanged, {changed} changed/new, {skipped} skipped")
    return reused, changed, skipped


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("save", "restore"))
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--build", type=Path, default=Path("build-uwp"))
    args = parser.parse_args()
    if not args.build.is_absolute():
        args.build = args.root / args.build
    if args.action == "save":
        save(args.root, args.build)
    else:
        restore(args.root, args.build)


if __name__ == "__main__":
    main()
