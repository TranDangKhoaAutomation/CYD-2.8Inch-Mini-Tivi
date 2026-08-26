from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from converter_core import PRESETS, STABLE_PRESET, build_ffmpeg_audio_args, build_ffmpeg_video_args, build_index, find_ffmpeg, output_paths, parse_ffmpeg_time


def run_progress(args: list[str], duration: float):
    args = list(args)
    args[1:1] = ["-loglevel", "warning", "-progress", "pipe:1", "-nostats"]
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    assert p.stdout is not None
    last = -1
    for line in p.stdout:
        sec = parse_ffmpeg_time(line.strip())
        if sec is not None and duration > 0:
            pct = int(min(sec / duration, 1.0) * 100)
            if pct >= last + 5 or pct == 100:
                print(f"[convert] {pct:3d}%  {sec:.1f}/{duration:.1f}s", flush=True)
                last = pct
    rc = p.wait()
    if rc != 0:
        raise SystemExit(f"FFmpeg failed: {rc}")


def probe_duration(path: Path, ffmpeg: str) -> float:
    ffprobe = str(Path(ffmpeg).with_name("ffprobe.exe"))
    r = subprocess.run([ffprobe, "-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1", str(path)], capture_output=True, text=True, check=True)
    return float(r.stdout.strip())


def main():
    ap = argparse.ArgumentParser(description="Convert long video to CYD-friendly MJPEG + IDX")
    ap.add_argument("input", type=Path)
    ap.add_argument("-o", "--output-dir", type=Path, required=True)
    ap.add_argument("--name")
    ap.add_argument("--preset", choices=["stable", "smooth", "quality"], default="stable")
    ap.add_argument("--audio", action="store_true")
    ns = ap.parse_args()
    preset = {"stable": STABLE_PRESET, "smooth": list(PRESETS.values())[1], "quality": list(PRESETS.values())[2]}[ns.preset]
    if not ns.input.is_file():
        raise SystemExit(f"Input not found: {ns.input}")
    ns.output_dir.mkdir(parents=True, exist_ok=True)
    ffmpeg = find_ffmpeg()
    duration = probe_duration(ns.input, ffmpeg)
    mjpeg, idx, mp3 = output_paths(ns.output_dir, ns.name or ns.input.stem)
    print(f"Input: {ns.input}")
    print(f"Duration: {duration:.2f}s")
    print(f"Preset: {preset.width}x{preset.height} @ {preset.fps:g}fps q={preset.quality}")
    run_progress(build_ffmpeg_video_args(ns.input, mjpeg, preset, ffmpeg), duration)
    if ns.audio:
        run_progress(build_ffmpeg_audio_args(ns.input, mp3, ffmpeg), duration)
    info = build_index(mjpeg, idx, preset.fps)
    print(f"VERIFY OK: frames={info.frames} duration={info.duration:.2f}s avg={info.avg_frame_bytes/1024:.1f}KB max={info.max_frame_bytes/1024:.1f}KB")
    print(f"MJPEG={mjpeg} ({mjpeg.stat().st_size/1048576:.1f}MB)")
    print(f"IDX={idx} ({idx.stat().st_size/1024:.1f}KB)")


if __name__ == "__main__":
    main()