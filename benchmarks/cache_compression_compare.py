"""Same-build compression on/off benchmark. Source/crops stay outside git.

Requires Pillow only when --source is used. Runs fresh processes serially,
alternates mode order per repetition and never flushes the OS file cache.
"""
import argparse
import hashlib
import json
import os
import platform
import statistics
import struct
import subprocess
from datetime import datetime, timezone
from pathlib import Path

SIZES = [("4k", 3840, 2160), ("8k", 7680, 4320), ("16k", 15360, 8640)]
METRICS = [
    "ready_ms", "presented_ms", "core_ms", "first_region_ms", "decode_done_ms",
    "prepare_done_ms", "pack_ms", "lod_ms", "compress_ms", "store_ms",
    "upload_cpu_ms", "max_upload_slice_ms", "work_p95_ms", "work_max_ms",
    "steady_fit_submit_p95_ms", "steady_fit_work_p95_ms", "actual_ready_ms",
    "actual_work_p95_ms", "cache_decode_ms", "cache_max_decode_ms", "cache_bytes",
    "load_peak_rss_bytes", "peak_rss_bytes", "process_cpu_ms", "source_bytes",
    "uploaded_bytes", "workers", "worker_peak", "input_peak", "output_peak",
]


def generate(source, directory):
    from PIL import Image
    # Explicitly trusted local input; still bounded before decode.
    Image.MAX_IMAGE_PIXELS = 240_000_000
    with Image.open(source) as image:
        if image.width * image.height > 240_000_000:
            raise ValueError("Source exceeds trusted test pixel limit")
        if image.width < 15360 or image.height < 8640:
            raise ValueError("Source must cover all three crops without resampling")
        image.load()
        for label, width, height in SIZES:
            x, y = (image.width - width) // 2, (image.height - height) // 2
            crop = image.crop((x, y, x + width, y + height)).convert("RGB")
            crop.save(directory / (label + ".png"), compress_level=1)
            crop.close()


def digest(path):
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source", type=Path, help="Generate centered PNG crops, no resize")
    parser.add_argument("--inputs", type=Path, help="Existing 4k.png/8k.png/16k.png")
    parser.add_argument("--runs", type=int, default=5)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    exe, output = args.exe.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    inputs = args.inputs.resolve() if args.inputs else output / "inputs"
    inputs.mkdir(parents=True, exist_ok=True)
    if args.source:
        generate(args.source, inputs)
    for label, width, height in SIZES:
        with (inputs / (label + ".png")).open("rb") as stream:
            header = stream.read(24)
        if header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR" or struct.unpack(">II", header[16:24]) != (width, height):
            raise ValueError("Wrong PNG dimensions for " + label)
    manifest = [dict(label=label, width=w, height=h, bytes=(inputs / (label + ".png")).stat().st_size,
                     sha256=digest(inputs / (label + ".png"))) for label, w, h in SIZES]
    (output / "inputs.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    for mode in ("on", "off"):
        (output / (mode + ".yaml")).write_text(
            "static_upload_mode: pipeline\nstatic_cache_compression: " +
            ("true" if mode == "on" else "false") + "\nlog_level: info\n", encoding="utf-8")
    reports = {label: {mode: [] for mode in ("on", "off")} for label, _, _ in SIZES}
    for repetition in range(args.runs):
        for label, _, _ in SIZES:
            for mode in (("on", "off") if repetition % 2 == 0 else ("off", "on")):
                name = label + "-" + mode + "-" + str(repetition + 1).zfill(2)
                report = output / (name + ".json")
                command = [str(exe), str(inputs / (label + ".png")), str(output / (mode + ".yaml")),
                           str(report), "1", "16.667"]
                with (output / (name + ".stdout.log")).open("w") as stdout, (output / (name + ".stderr.log")).open("w") as stderr:
                    subprocess.run(command, stdout=stdout, stderr=stderr, check=True, timeout=150,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
                data = json.loads(report.read_text(encoding="utf-8"))
                if bool(data["cache_compression"]) != (mode == "on"):
                    raise AssertionError("Configuration did not take effect")
                if (data["cache_disk_bytes"] != 0 or data["worker_peak"] > 2 or data["input_peak"] > 8 or
                        data["output_peak"] > 8 * 1024 * 1024 or data["scratch_limit_bytes"] > 8 * 1024 * 1024):
                    raise AssertionError("Cache/worker bounds violated")
                if mode == "off" and (data["compress_ms"] != 0 or data["cache_decode_ms"] != 0):
                    raise AssertionError("Disabled compression still ran codec")
                if data["partial_frames"] != 0:
                    raise AssertionError("Incomplete first screen displayed")
                reports[label][mode].append(data)
                print(name + ": ready=" + str(data["ready_ms"]) + "ms RSS=" + str(round(data["peak_rss_bytes"] / 2**20, 1)) + "MiB", flush=True)
    summary = dict(timestamp=datetime.now(timezone.utc).isoformat(), platform=platform.platform(),
                   processor=platform.processor(), binary_sha256=digest(exe), runs_per_mode=args.runs,
                   methodology="Fresh process; same binary/settings/input; alternating order; OS cache not flushed. Hidden 1600x1000 SDL_Render window, VSync requested, cadence 16.667ms. CPU wall times, not GPU scanout. Stage times summed across <=2 workers; do not add them to end-to-end time.",
                   inputs=manifest, results={})
    for label, _, _ in SIZES:
        hashes = {data["pixel_hash"] for mode in ("on", "off") for data in reports[label][mode]}
        if len(hashes) != 1:
            raise AssertionError("Pixel hash mismatch for " + label)
        result = dict(pixel_hash=next(iter(hashes)))
        for mode in ("on", "off"):
            result[mode] = {metric: dict(median=statistics.median(data[metric] for data in reports[label][mode]),
                                        min=min(data[metric] for data in reports[label][mode]),
                                        max=max(data[metric] for data in reports[label][mode])) for metric in METRICS}
        result["off_ready_change_percent"] = (result["off"]["ready_ms"]["median"] / result["on"]["ready_ms"]["median"] - 1) * 100
        summary["results"][label] = result
    (output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary["results"], indent=2), flush=True)


if __name__ == "__main__":
    main()
