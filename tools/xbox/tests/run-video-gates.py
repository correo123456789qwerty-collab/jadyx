#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the production video decoder against generated, redistributable inputs."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true", help="enable ASan and UBSan")
    parser.add_argument("--output", type=Path, help="write a JSON result after all gates pass")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    compiler = os.environ.get("CXX", "g++")
    for tool in (compiler, "ffmpeg", "pkg-config"):
        if not shutil.which(tool):
            parser.error(f"required tool not found: {tool}")

    def run(command, **kwargs):
        try:
            return subprocess.run(command, check=True, text=True, **kwargs)
        except subprocess.CalledProcessError as error:
            if error.stdout:
                print(error.stdout, end="", file=sys.stderr)
            if error.stderr:
                print(error.stderr, end="", file=sys.stderr)
            raise

    with tempfile.TemporaryDirectory(prefix="jadyx-video-") as folder:
        work = Path(folder)
        common = [compiler, "-std=c++20", "-Isrc"]
        if args.sanitize:
            common += ["-O1", "-g", "-fno-omit-frame-pointer",
                       "-fsanitize=address,undefined", "-fno-sanitize=vptr"]
            # The isolated TU discards Settings at link; vptr metadata would
            # otherwise retain references to unrelated Settings RTTI.
        else:
            common += ["-O2"]
        flags = shlex.split(run(["pkg-config", "--cflags", "--libs", "libavcodec",
                                 "libavformat", "libavutil", "fmt"],
                                capture_output=True).stdout)
        packet_gate = work / "packet"
        decoder_gate = work / "decoder"
        run([*common, "tools/xbox/tests/ffmpeg-packet.cpp", "-o", str(packet_gate),
             *flags], cwd=root)
        run([*common, "-ffunction-sections", "-fdata-sections",
             "tools/xbox/tests/ffmpeg-decoder.cpp", "-Wl,--gc-sections", "-o",
             str(decoder_gate), *flags], cwd=root)
        fixtures = []
        for name, options in (
            ("h264.mkv", ["-c:v", "libx264", "-preset", "ultrafast", "-bf", "2"]),
            ("vp8.ivf", ["-c:v", "libvpx", "-deadline", "realtime", "-cpu-used", "8"]),
            ("vp9.ivf", ["-c:v", "libvpx-vp9", "-deadline", "realtime", "-cpu-used", "8"]),
        ):
            path = work / name
            run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
                 "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=30", "-frames:v", "66",
                 *options, "-g", "30", "-pix_fmt", "yuv420p", str(path)])
            fixtures.append(str(path))
        results = []
        for command in ([str(packet_gate)], [str(decoder_gate), *fixtures]):
            result = run(command, capture_output=True)
            print(result.stdout, end="")
            results.append(result.stdout.strip())
        if args.output:
            versions = run(["pkg-config", "--modversion", "libavcodec", "libavformat",
                            "libavutil", "fmt"], capture_output=True).stdout.splitlines()
            report = {
                "status": "passed", "sanitizers": args.sanitize,
                "vptr_instrumentation": False,
                "base_commit": run(["git", "rev-parse", "HEAD"], cwd=root,
                                   capture_output=True).stdout.strip(),
                "source_sha256": {
                    path: hashlib.sha256((root / path).read_bytes()).hexdigest()
                    for path in (
                        "src/video_core/host1x/ffmpeg.cpp",
                        "src/video_core/host1x/ffmpeg.h",
                        "src/video_core/host1x/ffmpeg_packet.h",
                        "tools/xbox/tests/ffmpeg-packet.cpp",
                        "tools/xbox/tests/ffmpeg-decoder.cpp",
                        "tools/xbox/tests/run-video-gates.py",
                    )
                },
                "compiler": run([compiler, "--version"], capture_output=True).stdout.splitlines()[0],
                "libraries": dict(zip(("libavcodec", "libavformat", "libavutil", "fmt"), versions)),
                "synthetic_frames_per_codec": 66, "results": results,
                "decode_modes": ["cpu", "software fallback with device metadata"],
                "limits": "CPU decoding only; Xbox/UWP, GPU transfer and games not exercised",
            }
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
