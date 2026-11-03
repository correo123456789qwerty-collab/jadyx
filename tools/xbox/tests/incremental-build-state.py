#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""A real compiler/make differential: no-op checkout and same-size header changes."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile

spec = importlib.util.spec_from_file_location("incremental", Path(__file__).parents[1] / "incremental-build-state.py")
incremental = importlib.util.module_from_spec(spec)
spec.loader.exec_module(incremental)

with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    build = root / "build"
    build.mkdir()
    def run(*args):
        return subprocess.check_output(args, cwd=root, text=True).strip()
    run("git", "init", "-q")
    (root / "main.cpp").write_text('#include "value.h"\n#include <cstdio>\nint main(){std::printf("%d", VALUE);}\n')
    header = root / "value.h"
    header.write_text("#define VALUE 21\n")
    (root / "Makefile").write_text("build/program: main.cpp value.h Makefile\n\tg++ -O2 main.cpp -o build/program\n")
    run("git", "add", "main.cpp", "value.h", "Makefile")
    run("make", "-s")
    assert run("./build/program") == "21"
    incremental.save(root, build)
    original_stamp = (build / "program").stat().st_mtime_ns
    old_header_stamp = header.stat().st_mtime_ns
    # A fresh checkout touches every source: unchanged bytes must not trigger a build.
    for name in ["main.cpp", "value.h", "Makefile"]:
        os.utime(root / name, None)
    assert incremental.restore(root, build) == (3, 0, 0)
    run("make", "-s")
    assert (build / "program").stat().st_mtime_ns == original_stamp
    # Same-size edit with an OLD timestamp must still rebuild and execute the new code.
    header.write_text("#define VALUE 42\n")
    os.utime(header, ns=(old_header_stamp, old_header_stamp))
    assert incremental.restore(root, build) == (2, 1, 0)
    assert header.stat().st_mtime_ns > original_stamp
    run("make", "-s")
    assert run("./build/program") == "42"
    # The next saved state is reusable too; a newly tracked source is always changed.
    incremental.save(root, build)
    (root / "new.h").write_text("// new input\n")
    run("git", "add", "new.h")
    assert incremental.restore(root, build) == (3, 1, 0)
    manifest = build / "input-state.json"
    good = json.loads(manifest.read_text())
    for mutation in ["root", "format", "traversal"]:
        data = json.loads(json.dumps(good))
        if mutation == "root":
            data["root"] = str(root / "elsewhere")
        elif mutation == "format":
            data["format"] = -1
        else:
            data["inputs"]["../outside"] = next(iter(data["inputs"].values()))
        manifest.write_text(json.dumps(data))
        before = header.stat().st_mtime_ns
        try:
            incremental.restore(root, build)
            raise AssertionError("Invalid cache state accepted")
        except ValueError:
            assert header.stat().st_mtime_ns == before
print("PASS real build reuse, changed same-size header, stale timestamps, new inputs and invalid state")
