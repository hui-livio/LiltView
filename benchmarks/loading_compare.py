"""Reproducible 8K first-load pipeline vs batch comparison (Pillow required).
Run from repository root. Inputs/reports live under ignored build/, never assets/.
Each sample is a fresh process. OS page cache is warmed, not claimed cold.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import sys
from PIL import Image, __version__ as pillow_version


def generate(directory):
    seed = 20261113
    edge, block = 8192, 16
    rng = random.Random(seed)
    grid = edge // block
    pixels = bytearray()
    for y in range(grid):
        for x in range(grid):
            low, high = (24, 127) if (x + y) % 2 == 0 else (128, 239)
            pixels.extend(rng.randint(low, high) for _ in range(3))
    image = Image.frombytes("RGB", (grid, grid), bytes(pixels)).resize((edge, edge), Image.Resampling.NEAREST)
    path = directory / "checker-random-8192.png"
    image.save(path, compress_level=6)
    # Validate dimensions and actual decode, rather than trusting a filename.
    with Image.open(path) as check:
        check.load()
        assert check.size == (edge, edge) and check.mode == "RGB"
    cpu = os.environ.get("PROCESSOR_IDENTIFIER", platform.processor())
    if sys.platform == "win32":
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
            cpu = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
    manifest = dict(cpu=cpu, file=path.name, sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                    width=edge, height=edge, block=block, seed=seed, pillow=pillow_version,
                    python=sys.version, platform=platform.platform(), file_bytes=path.stat().st_size,
                    rgba_bytes=edge * edge * 4, note="Alternating luminance, independently random RGB per 16px cell; not uniform Core tiles")
    (directory / "input.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return path


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("build/loading-comparison"))
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--vsync", choices=["0", "1"], default="1")
    parser.add_argument("--cadence-ms", default="16.667")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    image = generate(args.output)
    for mode in ("pipeline", "batch"):
        (args.output / (mode + ".yaml")).write_text("static_upload_mode: " + mode + "\nlog_level: info\n", encoding="utf-8")
    # Warm file cache before both groups; alternate order to reduce time/order bias.
    image.read_bytes()
    reports = []
    for run in range(args.runs):
        for mode in (("pipeline", "batch") if run % 2 == 0 else ("batch", "pipeline")):
            stem = args.output / (mode + "-" + str(run + 1).zfill(2))
            with stem.with_suffix(".log").open("w", encoding="utf-8") as log:
                result = subprocess.run([str(args.exe.resolve()), str(image.resolve()),
                    str((args.output / (mode + ".yaml")).resolve()), str(stem.with_suffix(".json").resolve()),
                    args.vsync, args.cadence_ms], stdout=log, stderr=subprocess.STDOUT, timeout=150)
            if result.returncode:
                raise RuntimeError(str(stem) + " failed: " + str(result.returncode))
            report = json.loads(stem.with_suffix(".json").read_text(encoding="utf-8"))
            assert report["mode"] == mode
            assert report["partial_frames"] == 0
            assert report["tiles"] == 256
            assert report["input_peak"] <= 8
            assert report["first_upload_ms"] < report["decode_done_ms"] if mode == "pipeline" else report["first_upload_ms"] >= report["decode_done_ms"]
            reports.append(report)
            print(mode, run + 1, "ready_ms=", report["ready_ms"], "work_max_ms=", report["work_max_ms"], flush=True)
    assert len({r["pixel_hash"] for r in reports}) == 1, "Modes produced different images"
    summary = {"cache_condition": "fresh process, warmed OS input cache; fresh anonymous tile cache", "runs_per_mode": args.runs,
               "input": json.loads((args.output / "input.json").read_text()), "modes": {}}
    keys = ["ready_ms", "presented_ms", "core_ms", "decode_done_ms", "prepare_done_ms", "first_upload_ms", "last_upload_ms",
            "upload_cpu_ms", "max_upload_slice_ms", "pump_max_ms", "work_p95_ms", "work_max_ms", "event_p95_ms", "event_max_ms", "output_peak", "cache_bytes", "peak_rss_bytes", "process_cpu_ms"]
    for mode in ("pipeline", "batch"):
        group = [r for r in reports if r["mode"] == mode]
        summary["modes"][mode] = {key: {"median": statistics.median(r[key] for r in group), "min": min(r[key] for r in group), "max": max(r[key] for r in group), "p95": sorted(r[key] for r in group)[math.ceil(len(group)*.95)-1]} for key in keys}
        summary["modes"][mode].update(driver=group[0]["driver"], vsync=group[0]["vsync"], pixel_hash=group[0]["pixel_hash"])
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
