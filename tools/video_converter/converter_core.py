from __future__ import annotations

import mmap
import shutil
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable

SOI = b"\xff\xd8"
EOI = b"\xff\xd9"


@dataclass(frozen=True)
class Preset:
    name: str
    width: int
    height: int
    fps: float
    quality: int
    description: str = ""


@dataclass(frozen=True)
class IndexInfo:
    frames: int
    fps: float
    duration: float
    bytes_total: int
    avg_frame_bytes: float
    max_frame_bytes: int


STABLE_PRESET = Preset(
    "á»”n Ä‘á»‹nh - khuyáº¿n nghá»‹",
    320,
    240,
    12.0,
    10,
    "320x240, 12 fps, MJPEG yuvj420p; Æ°u tiÃªn ESP32 Ä‘á»c á»•n Ä‘á»‹nh.",
)
SMOOTH_PRESET = Preset("MÆ°á»£t hÆ¡n", 320, 240, 15.0, 10, "15 fps, cáº§n tháº» SD tá»‘t hÆ¡n.")
QUALITY_PRESET = Preset("Cháº¥t lÆ°á»£ng cao", 320, 240, 12.0, 7, "JPEG Ä‘áº¹p hÆ¡n nhÆ°ng file lá»›n hÆ¡n.")
PRESETS = {p.name: p for p in (STABLE_PRESET, SMOOTH_PRESET, QUALITY_PRESET)}


def find_ffmpeg() -> str:
    found = shutil.which("ffmpeg")
    if found:
        return found
    candidates = [
        Path.home() / "AppData/Local/Microsoft/WinGet/Packages/Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe/ffmpeg-9.0-full_build/bin/ffmpeg.exe",
        Path.home() / "AppData/Local/Microsoft/WinGet/Links/ffmpeg.exe",
    ]
    for p in candidates:
        if p.exists():
            return str(p)
    raise FileNotFoundError("KhÃ´ng tÃ¬m tháº¥y ffmpeg. HÃ£y cÃ i FFmpeg hoáº·c thÃªm ffmpeg vÃ o PATH.")


def find_ffprobe(ffmpeg: str | None = None) -> str:
    found = shutil.which("ffprobe")
    if found:
        return found
    if ffmpeg:
        p = Path(ffmpeg).with_name("ffprobe.exe")
        if p.exists():
            return str(p)
    raise FileNotFoundError("KhÃ´ng tÃ¬m tháº¥y ffprobe.")


def _jpeg_offsets(path: Path) -> tuple[list[int], int]:
    size = path.stat().st_size
    if size <= 0:
        raise ValueError("MJPEG rá»—ng")
    offsets: list[int] = []
    with path.open("rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
        pos = 0
        while True:
            pos = mm.find(SOI, pos)
            if pos < 0:
                break
            offsets.append(pos)
            pos += 2
    if not offsets:
        raise ValueError("KhÃ´ng tÃ¬m tháº¥y frame JPEG trong MJPEG")
    return offsets, size


def verify_mjpeg(path: Path) -> IndexInfo:
    offsets, size = _jpeg_offsets(path)
    if offsets[0] != 0:
        raise ValueError(f"MJPEG cÃ³ dá»¯ liá»‡u rÃ¡c trÆ°á»›c frame Ä‘áº§u: offset={offsets[0]}")
    frame_sizes: list[int] = []
    with path.open("rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
        for i, start in enumerate(offsets):
            end = offsets[i + 1] if i + 1 < len(offsets) else size
            if end <= start + 4:
                raise ValueError(f"Frame {i} quÃ¡ ngáº¯n")
            if mm[end - 2:end] != EOI:
                raise ValueError(f"Frame {i} thiáº¿u JPEG EOI táº¡i offset {end}")
            frame_sizes.append(end - start)
    frames = len(offsets)
    return IndexInfo(frames, 0.0, 0.0, size, size / frames, max(frame_sizes))


def build_index(mjpeg_path: Path, idx_path: Path, fps: float) -> IndexInfo:
    if fps <= 0:
        raise ValueError("FPS pháº£i > 0")
    verified = verify_mjpeg(mjpeg_path)
    offsets, size = _jpeg_offsets(mjpeg_path)
    offsets.append(size)
    idx_path.parent.mkdir(parents=True, exist_ok=True)
    with idx_path.open("wb") as f:
        f.write(b"CYD1")
        f.write(struct.pack("<I", int(round(fps * 1000))))
        f.write(struct.pack("<I", verified.frames))
        for off in offsets:
            if off > 0xFFFFFFFF:
                raise ValueError("File MJPEG >4 GiB khÃ´ng tÆ°Æ¡ng thÃ­ch Ä‘á»‹nh dáº¡ng IDX hiá»‡n táº¡i")
            f.write(struct.pack("<I", off))
    return IndexInfo(
        verified.frames,
        fps,
        verified.frames / fps,
        verified.bytes_total,
        verified.avg_frame_bytes,
        verified.max_frame_bytes,
    )


def parse_ffmpeg_time(line: str) -> float | None:
    if not line.startswith("out_time="):
        return None
    value = line.split("=", 1)[1].strip()
    try:
        hh, mm, ss = value.split(":", 2)
        return int(hh) * 3600 + int(mm) * 60 + float(ss)
    except (ValueError, TypeError):
        return None

def build_ffmpeg_video_args(input_path: Path, output_mjpeg: Path, preset: Preset, ffmpeg: str) -> list[str]:
    scale = (
        f"fps={preset.fps:g},"
        f"scale={preset.width}:{preset.height}:force_original_aspect_ratio=decrease:flags=lanczos,"
        f"pad={preset.width}:{preset.height}:(ow-iw)/2:(oh-ih)/2:black,"
        "format=yuvj420p"
    )
    return [
        ffmpeg, "-y", "-hide_banner", "-i", str(input_path),
        "-map", "0:v:0", "-vf", scale,
        "-c:v", "mjpeg", "-q:v", str(preset.quality),
        "-pix_fmt", "yuvj420p", "-an", "-f", "mjpeg", str(output_mjpeg),
    ]


def build_ffmpeg_audio_args(input_path: Path, output_mp3: Path, ffmpeg: str) -> list[str]:
    return [
        ffmpeg, "-y", "-hide_banner", "-i", str(input_path), "-map", "0:a:0?", "-vn",
        "-c:a", "libmp3lame", "-b:a", "96k", "-ar", "32000", "-ac", "1", str(output_mp3),
    ]


def output_paths(output_dir: Path, base_name: str) -> tuple[Path, Path, Path]:
    safe = "".join(c if c not in '<>:"/\\|?*' else "_" for c in base_name).strip(" .") or "video"
    return output_dir / f"{safe}.mjpeg", output_dir / f"{safe}.idx", output_dir / f"{safe}.mp3"