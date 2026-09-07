from __future__ import annotations

import io
import json
import os
import queue
import socket
import subprocess
import threading
import time
import uuid
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Any
from urllib.parse import urljoin

import requests
from flask import Flask, Response, jsonify, redirect, render_template_string, request
from PIL import Image
from yt_dlp import YoutubeDL

HOST = "0.0.0.0"
PORT = int(os.environ.get("CYD_TV_PORT", "8876"))
DISCOVERY_PORT = 4210
DISCOVERY_MAGIC = b"CYD_TV_DISCOVER"
YT_STREAM_FPS = 10
YT_STREAM_Q = 5
YT_PREBUFFER_FRAMES = 20   # 2 seconds at 10 fps: faster start while keeping a small jitter reserve
YT_BUFFER_FRAMES = 150     # up to 15 seconds of jitter absorption on the PC
HLS_PREBUFFER_FRAMES = 50    # 5 s at 10 FPS; still absorbs typical multi-second HLS segment gaps
LIVE_RETRY_DELAY_SEC = 0.75
LIVE_REFRESH_COOLDOWN_SEC = 3.0
YOUTUBE_RETRY_DELAY_SEC = 0.75
VIDEO_STALL_RESTART_SEC = 12.0  # bound frozen-frame time if FFmpeg stays alive without output
VTV_PLAYBACK_API = "https://api.vtvdigital.org/live-channel/v21.0/playback/source"
VTV_WEB_REFERER = "https://vtvgo.vn/"
VTV_DEVICE_ID = os.environ.get("CYD_VTV_DEVICE_ID") or str(uuid.uuid4())
VTV_API_HEADERS = {
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/152 Safari/537.36",
    "Referer": VTV_WEB_REFERER,
    "Origin": "https://vtvgo.vn",
    "Accept": "application/json, text/plain, */*",
    "Accept-Language": "vi-VN,vi",
    "Content-Type": "application/json",
}

AUDIO_CHUNK_BYTES = 4096
AUDIO_BUFFER_CHUNKS = 32      # ~16 s reserve at 64 kbps; PC RAM only
AUDIO_PREBUFFER_CHUNKS_YT = 4    # ~2.0 s media reserve at 64 kbps
AUDIO_PREBUFFER_CHUNKS_HLS = 10  # ~5.1 s media reserve for live HLS gaps
BASE_DIR = Path(__file__).resolve().parent
YTDLP_EXE = BASE_DIR / "yt-dlp.exe"
CACHE_DIR = BASE_DIR / "cache"
CACHE_DIR.mkdir(exist_ok=True)

app = Flask(__name__)

@dataclass
class VideoItem:
    id: str
    url: str
    title: str
    channel: str
    duration: int
    thumbnail: str

state_lock = threading.Lock()
live_refresh_lock = threading.Lock()
youtube_resolve_lock = threading.Lock()
youtube_refresh_lock = threading.Lock()

# A/V start barrier. Audio transcoding is much cheaper than MJPEG transcoding, so
# its prebuffer can fill first. Without a barrier the speaker can start 1-2 s before
# the first video frame reaches the ESP32. Generation IDs prevent a stale previous
# request from releasing audio for a newly selected video.
playback_sync_cond = threading.Condition()
playback_generation = 0
video_started_generation = -1

def _reset_playback_sync() -> int:
    global playback_generation, video_started_generation
    with playback_sync_cond:
        playback_generation += 1
        video_started_generation = -1
        playback_sync_cond.notify_all()
        return playback_generation

def _current_playback_generation() -> int:
    with playback_sync_cond:
        return playback_generation

def _mark_video_started(generation: int) -> None:
    global video_started_generation
    with playback_sync_cond:
        if generation != playback_generation:
            return
        if video_started_generation != generation:
            video_started_generation = generation
            print(f"[SYNC] video first frame sent generation={generation}", flush=True)
        playback_sync_cond.notify_all()

def _wait_for_video_started(generation: int, timeout: float = 20.0) -> bool:
    deadline = time.monotonic() + timeout
    with playback_sync_cond:
        while generation == playback_generation and video_started_generation != generation:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            playback_sync_cond.wait(timeout=min(0.25, remaining))
        return generation == playback_generation and video_started_generation == generation
live_source_refreshed_at = 0.0
youtube_source_refreshed_at = 0.0
thumb_sources: dict[str, str] = {}
state: dict[str, Any] = {
    "selected": None,
    "source_url": None,
    "audio_url": None,
    "title": "",
    "running": False,
    "audio_running": False,
    "error": "",
    "audio_error": "",
    "started_at": 0.0,
    "source_kind": "youtube",
    "channel_id": "",
    "resume_seconds": 0.0,
    "generation": 0,
}


def _state_generation_is_current(generation: int) -> bool:
    with state_lock:
        return int(state.get("generation", -1)) == generation

VTVGO_CHANNELS = [
    {"id": "vtv1", "api_id": "1", "name": "VTV1", "url": "https://vtvgo.vn/channel/vtv1-1,1.html"},
    {"id": "vtv2", "api_id": "2", "name": "VTV2", "url": "https://vtvgo.vn/channel/vtv2-1,2.html"},
    {"id": "vtv3", "api_id": "3", "name": "VTV3", "url": "https://vtvgo.vn/channel/vtv3-1,3.html"},
    {"id": "vtv4", "api_id": "4", "name": "VTV4", "url": "https://vtvgo.vn/channel/vtv4-1,4.html"},
    {"id": "vtv5", "api_id": "5", "name": "VTV5", "url": "https://vtvgo.vn/channel/vtv5-1,5.html"},
    {"id": "vtv6", "api_id": "13", "name": "VTV6", "url": "https://vtvgo.vn/channel/vtv6-1,13.html"},
    {"id": "vtv7", "api_id": "27", "name": "VTV7", "url": "https://vtvgo.vn/channel/vtv7-1,27.html"},
    {"id": "vtv8", "api_id": "36", "name": "VTV8", "url": "https://vtvgo.vn/channel/vtv8-1,36.html"},
    {"id": "vtv9", "api_id": "39", "name": "VTV9", "url": "https://vtvgo.vn/channel/vtv9-1,39.html"},
    {"id": "vtv10", "api_id": "6", "name": "VTV10", "url": "https://vtvgo.vn/channel/vtv10-1,6.html"},
    {"id": "vietnamtoday", "api_id": "vietnamtoday", "name": "Vietnam Today", "url": "https://vtvgo.vn/channel/vietnam-today-1,vietnamtoday.html"},
    {"id": "antv", "api_id": "89", "name": "ANTV", "url": "https://vtvgo.vn/channel/truyen-hinh-cong-an-nhan-dan-1,89.html"},
    {"id": "qpvn", "api_id": "103", "name": "QPVN", "url": "https://vtvgo.vn/channel/truyen-hinh-quoc-phong-viet-nam-1,103.html"},
    {"id": "hanoi1", "api_id": "111", "name": "Hà Nội 1", "url": "https://vtvgo.vn/channel/truyen-hinh-ha-noi-1-1,111.html"},
    {"id": "hanoi2", "api_id": "17", "name": "Hà Nội 2", "url": "https://vtvgo.vn/channel/truyen-hinh-ha-noi-2-1,17.html"},
]


def _find_vtvgo_channel(channel_id: str) -> dict[str, str]:
    for item in VTVGO_CHANNELS:
        if item["id"] == channel_id:
            return item
    raise KeyError(f"unknown TV channel: {channel_id}")


def _hls_non_comment_lines(text: str) -> list[str]:
    return [line.strip() for line in text.splitlines() if line.strip() and not line.lstrip().startswith("#")]


def _hls_variants(master_url: str, text: str) -> list[tuple[int, str]]:
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    variants: list[tuple[int, str]] = []
    for i, line in enumerate(lines):
        if not line.startswith("#EXT-X-STREAM-INF:"):
            continue
        bandwidth = 2**31 - 1
        marker = "BANDWIDTH="
        if marker in line:
            raw = line.split(marker, 1)[1].split(",", 1)[0].strip()
            try:
                bandwidth = int(raw)
            except ValueError:
                pass
        for nxt in lines[i + 1:]:
            if nxt.startswith("#"):
                continue
            variants.append((bandwidth, urljoin(master_url, nxt)))
            break
    variants.sort(key=lambda item: item[0])
    return variants


def _probe_vtv_hls_source(url: str) -> str | None:
    """Return a verified media-playlist URL only after a real media segment is readable."""
    headers = {"User-Agent": VTV_API_HEADERS["User-Agent"], "Referer": VTV_WEB_REFERER}
    try:
        master = requests.get(url, headers=headers, timeout=(3, 5))
        if master.status_code != 200 or not bytes(master.content[:128]).lstrip().startswith(b"#EXTM3U"):
            print(f"[VTV] master rejected HTTP={master.status_code}", flush=True)
            return None
        variants = _hls_variants(url, master.text)
        media_candidates = [variants[0][1]] if variants else [url]
        for media_url in media_candidates:
            try:
                if media_url == url and not variants:
                    media_text = master.text
                else:
                    media = requests.get(media_url, headers=headers, timeout=(3, 5))
                    if media.status_code != 200 or not bytes(media.content[:128]).lstrip().startswith(b"#EXTM3U"):
                        continue
                    media_text = media.text
                segments = _hls_non_comment_lines(media_text)
                if not segments:
                    continue
                segment_url = urljoin(media_url, segments[0])
                segment_headers = dict(headers)
                segment_headers["Range"] = "bytes=0-2047"
                seg = requests.get(segment_url, headers=segment_headers, timeout=(3, 5), stream=True)
                try:
                    if seg.status_code not in (200, 206):
                        continue
                    first = next(seg.iter_content(chunk_size=1024), b"")
                    if not first:
                        continue
                finally:
                    seg.close()
                print(f"[VTV] verified media segment via {media_url}", flush=True)
                return media_url
            except requests.RequestException as exc:
                print(f"[VTV] variant probe failed: {type(exc).__name__}", flush=True)
                continue
        return None
    except requests.RequestException as exc:
        print(f"[VTV] candidate probe failed: {type(exc).__name__}", flush=True)
        return None


def resolve_vtvgo_source(channel: dict[str, str]) -> str:
    """Resolve the current official non-DRM HLS source through VTV Go v21 playback API.

    The 2026 web player no longer exposes HLS during initial page load. It calls
    /live-channel/v21.0/playback/source and can return multiple CDN candidates.
    Probe them in order so a signed-but-stalled CDN does not freeze the TV player.
    """
    api_id = str(channel.get("api_id") or "").strip()
    if not api_id:
        raise RuntimeError(f"VTV Go channel has no playback API id: {channel.get('name', channel.get('id', '?'))}")

    payload = {"channelId": api_id, "platform": "webPC", "deviceId": VTV_DEVICE_ID}
    try:
        r = requests.post(VTV_PLAYBACK_API, json=payload, headers=VTV_API_HEADERS, timeout=(4, 10))
        r.raise_for_status()
        data = (r.json() or {}).get("data") or {}
    except (requests.RequestException, ValueError) as exc:
        raise RuntimeError(f"VTV Go playback API failed for {channel['name']}: {exc}") from exc

    candidates: list[str] = []
    for mode in data.get("sourceModes") or []:
        if int(mode.get("isVip") or 0) != 0:
            continue
        for source_group in mode.get("multiSource") or []:
            manifest_type = str(source_group.get("manifestType") or "").lower()
            drm_info = source_group.get("drmInfo") or {}
            drm_type = str(source_group.get("drmTypeID") or drm_info.get("drmType") or "").lower()
            if manifest_type != "hls" or drm_type not in ("", "none"):
                continue
            for source in source_group.get("sources") or []:
                url = str(source.get("url") or "").strip()
                if url and url not in candidates:
                    candidates.append(url)

    if not candidates:
        raise RuntimeError(f"VTV Go returned no free non-DRM HLS source for {channel['name']}")

    print(f"[VTV] playback API returned {len(candidates)} HLS candidate(s) for {channel['name']}", flush=True)
    for index, url in enumerate(candidates, 1):
        playable = _probe_vtv_hls_source(url)
        if playable:
            print(f"[VTV] selected live CDN {index}/{len(candidates)} for {channel['name']}", flush=True)
            return playable

    raise RuntimeError(f"VTV Go returned {len(candidates)} HLS source(s) but none responded for {channel['name']}")


def _format_item(entry: dict[str, Any]) -> VideoItem:
    video_id = str(entry.get("id") or "")
    webpage_url = entry.get("webpage_url") or entry.get("url") or (f"https://www.youtube.com/watch?v={video_id}" if video_id else "")
    thumb = entry.get("thumbnail") or ""
    if not thumb:
        thumbs = entry.get("thumbnails") or []
        if thumbs:
            thumb = thumbs[-1].get("url", "")
    return VideoItem(
        id=video_id,
        url=str(webpage_url),
        title=str(entry.get("title") or "Untitled")[:160],
        channel=str(entry.get("channel") or entry.get("uploader") or "YouTube")[:80],
        duration=int(entry.get("duration") or 0),
        thumbnail=str(thumb),
    )


def _run_ytdlp_json(args: list[str], timeout: int = 35) -> dict[str, Any]:
    if YTDLP_EXE.exists():
        cmd = [str(YTDLP_EXE), "--no-warnings", "--no-progress", "--js-runtimes", "node", *args]
        cp = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
        if cp.returncode != 0:
            raise RuntimeError((cp.stderr or cp.stdout or "yt-dlp failed").strip()[-1200:])
        return json.loads(cp.stdout)
    # Fallback for development only. On Python 3.9 pip may install an old yt-dlp.
    raise RuntimeError("yt-dlp.exe not found. Run start_server.bat or download the latest standalone yt-dlp.exe")


def search_youtube(query: str, limit: int = 12) -> list[VideoItem]:
    query = (query or "").strip() or "Tran Dang Khoa"
    info = _run_ytdlp_json([
        "--flat-playlist",
        "--dump-single-json",
        f"ytsearch{limit}:{query}",
    ], timeout=45)
    entries = (info or {}).get("entries") or []
    items: list[VideoItem] = []
    for e in entries:
        if not e:
            continue
        vid = str(e.get("id") or "")
        if vid and not str(e.get("webpage_url") or "").startswith("http"):
            e["webpage_url"] = f"https://www.youtube.com/watch?v={vid}"
        item = _format_item(e)
        if item.id and item.thumbnail:
            thumb_sources[item.id] = item.thumbnail
        items.append(item)
    return items


def _codec_present(value: Any) -> bool:
    return bool(value and str(value).lower() != "none")


def resolve_direct_streams(url: str) -> tuple[str, str, str]:
    """Resolve one YouTube page once and return independent video/audio URLs."""
    info = _run_ytdlp_json([
        "--dump-single-json",
        "--no-playlist",
        "--socket-timeout", "8",
        "--retries", "1",
        "--fragment-retries", "1",
        "-f",
        "bestvideo[height<=480][ext=mp4][vcodec^=avc1][protocol=https]+bestaudio[protocol=https]/"
        "bestvideo[height<=480][protocol=https]+bestaudio[protocol=https]/"
        "best[height<=480][protocol=https]/best",
        url,
    ], timeout=35)

    title = str(info.get("title") or url)
    video_url: str | None = None
    audio_url: str | None = None

    requested = info.get("requested_formats") or []
    for fmt in requested:
        direct = fmt.get("url")
        if not direct:
            continue
        if not video_url and _codec_present(fmt.get("vcodec")):
            video_url = str(direct)
        if not audio_url and _codec_present(fmt.get("acodec")):
            audio_url = str(direct)

    # A progressive fallback can carry both codecs in one URL.
    direct = info.get("url")
    if direct:
        if not video_url and (_codec_present(info.get("vcodec")) or not requested):
            video_url = str(direct)
        if not audio_url and (_codec_present(info.get("acodec")) or not requested):
            audio_url = str(direct)

    # Last-resort extraction from the format catalog keeps unusual extractors usable.
    formats = info.get("formats") or []
    if not video_url:
        for fmt in reversed(formats):
            if fmt.get("url") and _codec_present(fmt.get("vcodec")):
                video_url = str(fmt["url"])
                break
    if not audio_url:
        for fmt in reversed(formats):
            if fmt.get("url") and _codec_present(fmt.get("acodec")):
                audio_url = str(fmt["url"])
                break

    if not video_url:
        raise RuntimeError("yt-dlp did not return a playable video URL")
    if not audio_url:
        raise RuntimeError("yt-dlp did not return a playable audio URL")
    return video_url, audio_url, title


def resolve_direct_media(url: str) -> tuple[str, str]:
    """Backward-compatible video-only wrapper used by older tooling/tests."""
    video_url, _audio_url, title = resolve_direct_streams(url)
    return video_url, title


def _state_snapshot() -> dict[str, Any]:
    with state_lock:
        return dict(state)


def _set_error(msg: str) -> None:
    with state_lock:
        state["error"] = msg
        state["running"] = False


def choose_video(url: str, title: str = "") -> None:
    generation = _reset_playback_sync()
    print(f"[SYNC] new YouTube session generation={generation}", flush=True)
    with state_lock:
        state["selected"] = url
        state["source_url"] = None
        state["audio_url"] = None
        state["title"] = title
        state["error"] = ""
        state["audio_error"] = ""
        state["running"] = False
        state["audio_running"] = False
        state["started_at"] = 0.0
        state["source_kind"] = "youtube"
        state["channel_id"] = ""
        state["resume_seconds"] = 0.0
        state["generation"] = generation


def resolve_selected() -> tuple[str, str, str]:
    snap = _state_snapshot()
    video_url = snap.get("source_url")
    audio_url = snap.get("audio_url")
    if video_url and audio_url:
        return str(video_url), str(audio_url), str(snap.get("title") or "Video")
    if snap.get("source_kind") == "direct_hls" and video_url:
        return str(video_url), str(audio_url or video_url), str(snap.get("title") or "Live TV")
    selected = snap.get("selected")
    if not selected:
        raise RuntimeError("No video selected")

    with youtube_resolve_lock:
        snap = _state_snapshot()
        video_url = snap.get("source_url")
        audio_url = snap.get("audio_url")
        if video_url and audio_url:
            return str(video_url), str(audio_url), str(snap.get("title") or "Video")
        video_url, audio_url, title = resolve_direct_streams(str(selected))
        with state_lock:
            state["source_url"] = video_url
            state["audio_url"] = audio_url
            if title:
                state["title"] = title
            state["error"] = ""
            state["audio_error"] = ""
        return video_url, audio_url, title


def refresh_youtube_sources(stale_video: str | None = None, stale_audio: str | None = None, generation: int | None = None) -> tuple[str, str, str]:
    """Refresh expiring GoogleVideo URLs once and let video/audio branches share the result."""
    global youtube_source_refreshed_at
    snap = _state_snapshot()
    if generation is not None and int(snap.get("generation", -1)) != generation:
        raise RuntimeError("stale generation YouTube refresh")
    if snap.get("source_kind") != "youtube":
        raise RuntimeError("YouTube refresh requested for non-YouTube media")
    selected = str(snap.get("selected") or "")
    if not selected:
        raise RuntimeError("YouTube refresh has no selected page URL")

    with youtube_refresh_lock:
        snap = _state_snapshot()
        if generation is not None and int(snap.get("generation", -1)) != generation:
            raise RuntimeError("stale generation YouTube refresh")
        current_video = str(snap.get("source_url") or "")
        current_audio = str(snap.get("audio_url") or "")
        if stale_video and current_video and current_video != stale_video and current_audio:
            return current_video, current_audio, str(snap.get("title") or "Video")
        if stale_audio and current_audio and current_audio != stale_audio and current_video:
            return current_video, current_audio, str(snap.get("title") or "Video")

        video_url, audio_url, title = resolve_direct_streams(selected)
        with state_lock:
            state["source_url"] = video_url
            state["audio_url"] = audio_url
            if title:
                state["title"] = title
            state["error"] = ""
            state["audio_error"] = ""
        youtube_source_refreshed_at = time.monotonic()
        print("[YT][REFRESH] refreshed direct video/audio URLs", flush=True)
        return video_url, audio_url, title


def refresh_live_source(generation: int | None = None) -> str:
    """Refresh the signed VTV HLS URL without ending existing HTTP clients.

    Video and audio generators can fail independently. A short cooldown lets the
    second branch reuse URLs just refreshed by the first instead of launching two
    yt-dlp refresh operations at once.
    """
    global live_source_refreshed_at
    snap = _state_snapshot()
    if generation is not None and int(snap.get("generation", -1)) != generation:
        raise RuntimeError("stale generation live refresh")
    if snap.get("source_kind") != "direct_hls":
        raise RuntimeError("live source refresh requested for non-live media")
    channel_id = str(snap.get("channel_id") or "")
    if not channel_id:
        raise RuntimeError("live source has no channel id")

    with live_refresh_lock:
        snap = _state_snapshot()
        if generation is not None and int(snap.get("generation", -1)) != generation:
            raise RuntimeError("stale generation live refresh")
        now = time.monotonic()
        cached = str(snap.get("source_url") or "")
        if cached and live_source_refreshed_at and now - live_source_refreshed_at < LIVE_REFRESH_COOLDOWN_SEC:
            return cached

        channel = _find_vtvgo_channel(channel_id)
        fresh = resolve_vtvgo_source(channel)
        with state_lock:
            state["source_url"] = fresh
            state["audio_url"] = fresh
            state["error"] = ""
            state["audio_error"] = ""
        live_source_refreshed_at = time.monotonic()
        print(f"[LIVE] refreshed {channel_id} HLS source", flush=True)
        return fresh


def build_video_command(direct: str, resume_seconds: float = 0.0) -> list[str]:
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "2",
        "-rw_timeout", "10000000",
    ]
    if resume_seconds > 0.5:
        cmd += ["-ss", f"{resume_seconds:.3f}"]
    cmd += [
        "-i", direct,
        "-an",
        "-vf", f"scale=320:180:force_original_aspect_ratio=decrease:flags=lanczos,pad=320:180:(ow-iw)/2:(oh-ih)/2:black,fps={YT_STREAM_FPS}",
        "-q:v", str(YT_STREAM_Q),
        "-f", "mjpeg",
        "pipe:1",
    ]
    return cmd


def _drain_ffmpeg_stderr(pipe) -> None:
    """Drain ffmpeg stderr continuously so its pipe can never stall video output."""
    try:
        for raw in iter(pipe.readline, b""):
            msg = raw.decode("utf-8", errors="replace").strip()
            if msg:
                print(f"[FFMPEG] {msg}", flush=True)
    except Exception as exc:
        print(f"[FFMPEG] stderr drain stopped: {exc}", flush=True)


def _mjpeg_producer(proc: subprocess.Popen[bytes], frame_queue: "queue.Queue[bytes]", stop_event: threading.Event) -> None:
    """Read/transcode ahead of playback and keep an ordered PC-side frame reservoir."""
    assert proc.stdout is not None
    buf = bytearray()
    produced = 0
    last_frame_at = time.monotonic()
    try:
        while not stop_event.is_set():
            chunk = proc.stdout.read(8192)
            if not chunk:
                break
            buf.extend(chunk)
            while not stop_event.is_set():
                soi = buf.find(b"\xff\xd8")
                if soi < 0:
                    if len(buf) > 16384:
                        del buf[:-2]
                    break
                eoi = buf.find(b"\xff\xd9", soi + 2)
                if eoi < 0:
                    if soi > 0:
                        del buf[:soi]
                    break
                frame = bytes(buf[soi:eoi + 2])
                del buf[:eoi + 2]
                gap = time.monotonic() - last_frame_at
                last_frame_at = time.monotonic()
                if gap > 1.0:
                    print(f"[BUFFER] source gap {gap:.2f}s after frame {produced}", flush=True)
                while not stop_event.is_set():
                    try:
                        frame_queue.put(frame, timeout=0.25)
                        produced += 1
                        break
                    except queue.Full:
                        # A full queue is intentional: it means the PC has a deep reserve.
                        continue
    except Exception as exc:
        print(f"[BUFFER] producer stopped: {exc}", flush=True)
    finally:
        print(f"[BUFFER] producer exit frames={produced} queued={frame_queue.qsize()}", flush=True)


def mjpeg_generator():
    proc: subprocess.Popen[bytes] | None = None
    stop_event = threading.Event()
    producer: threading.Thread | None = None
    frame_queue: "queue.Queue[bytes]" = queue.Queue(maxsize=YT_BUFFER_FRAMES)
    sync_generation: int | None = None
    try:
        direct, _audio_direct, title = resolve_selected()
        sync_generation = _current_playback_generation()
        source_snap = _state_snapshot()
        if int(source_snap.get("generation", -1)) != sync_generation:
            print(f"[BUFFER] stale generation before video start generation={sync_generation}", flush=True)
            return
        is_youtube_session = source_snap.get("source_kind") == "youtube"

        # Flush HTTP headers immediately. Live VTV/FFmpeg prebuffer may need many
        # seconds before the first JPEG; the ESP32 parser ignores this MIME preamble.
        yield b"\r\n"
        sent = 0
        resume_seconds = float(source_snap.get("resume_seconds") or 0.0) if is_youtube_session else 0.0
        next_emit = time.monotonic()
        session = 0

        while True:
            if not _state_generation_is_current(sync_generation):
                print(f"[BUFFER] stale generation video exit generation={sync_generation}", flush=True)
                break
            session += 1
            proc = subprocess.Popen(
                build_video_command(direct, resume_seconds if is_youtube_session else 0.0),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                bufsize=0,
            )
            with state_lock:
                if state.get("generation") != sync_generation:
                    print(f"[BUFFER] stale generation after video spawn generation={sync_generation}", flush=True)
                    break
                state["running"] = True
                if not state.get("started_at"):
                    state["started_at"] = time.time()
                state["title"] = title
                state["error"] = ""

            if proc.stderr is not None:
                threading.Thread(target=_drain_ffmpeg_stderr, args=(proc.stderr,), daemon=True).start()
            producer = threading.Thread(target=_mjpeg_producer, args=(proc, frame_queue, stop_event), daemon=True)
            producer.start()

            snap = _state_snapshot()
            prebuffer_target = HLS_PREBUFFER_FRAMES if snap.get("source_kind") == "direct_hls" else YT_PREBUFFER_FRAMES
            prebuffer_started = time.monotonic()
            stale_generation = False
            while frame_queue.qsize() < prebuffer_target and producer.is_alive():
                if not _state_generation_is_current(sync_generation):
                    stale_generation = True
                    break
                if time.monotonic() - prebuffer_started > 12.0:
                    break
                time.sleep(0.05)
            if stale_generation:
                print(f"[BUFFER] stale generation during video prebuffer generation={sync_generation}", flush=True)
                break
            print(f"[BUFFER] playback start session={session} queued={frame_queue.qsize()}/{YT_BUFFER_FRAMES}", flush=True)

            period = 1.0 / YT_STREAM_FPS
            underrun_started: float | None = None
            stalled_source = False
            while producer.is_alive() or not frame_queue.empty():
                if not _state_generation_is_current(sync_generation):
                    stale_generation = True
                    print(f"[BUFFER] stale generation during video stream generation={sync_generation}", flush=True)
                    break
                try:
                    frame = frame_queue.get(timeout=2.0)
                except queue.Empty:
                    now = time.monotonic()
                    if underrun_started is None:
                        underrun_started = now
                    gap = now - underrun_started
                    print(f"[BUFFER] underrun: no frame for {gap:.1f}s", flush=True)
                    if gap >= VIDEO_STALL_RESTART_SEC:
                        stalled_source = True
                        print(f"[BUFFER] video stall {gap:.1f}s -> terminate ffmpeg", flush=True)
                        if proc and proc.poll() is None:
                            proc.terminate()
                        break
                    continue

                underrun_started = None
                now = time.monotonic()
                if next_emit > now:
                    time.sleep(next_emit - now)
                else:
                    next_emit = now
                first_frame = sent == 0
                yield b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + str(len(frame)).encode() + b"\r\n\r\n" + frame + b"\r\n"
                if first_frame:
                    _mark_video_started(sync_generation)
                sent += 1
                if is_youtube_session:
                    resume_seconds += period
                    if sent % YT_STREAM_FPS == 0:
                        with state_lock:
                            if state.get("generation") == sync_generation:
                                state["resume_seconds"] = resume_seconds
                next_emit += period
                if sent % (YT_STREAM_FPS * 5) == 0:
                    print(f"[BUFFER] sent={sent} queued={frame_queue.qsize()}", flush=True)

            if stop_event.is_set() or stale_generation:
                break
            if not _state_generation_is_current(sync_generation):
                print(f"[BUFFER] stale generation before video recovery generation={sync_generation}", flush=True)
                break

            if stalled_source and proc and proc.poll() is None:
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
            if stalled_source and producer and producer.is_alive():
                producer.join(timeout=1)

            snap = _state_snapshot()
            if int(snap.get("generation", -1)) != sync_generation:
                print(f"[BUFFER] stale generation at video recovery generation={sync_generation}", flush=True)
                break
            if snap.get("source_kind") == "youtube":
                if proc and proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                if producer and producer.is_alive():
                    producer.join(timeout=1)
                print(f"[YT][VIDEO] source ended/stalled at {resume_seconds:.1f}s -> refresh in same HTTP stream", flush=True)
                time.sleep(YOUTUBE_RETRY_DELAY_SEC)
                if not _state_generation_is_current(sync_generation):
                    break
                try:
                    direct, _fresh_audio, title = refresh_youtube_sources(stale_video=direct, generation=sync_generation)
                except Exception as exc:
                    if not _state_generation_is_current(sync_generation):
                        break
                    print(f"[YT][VIDEO] refresh failed: {exc}; retrying", flush=True)
                    time.sleep(max(1.0, YOUTUBE_RETRY_DELAY_SEC * 2))
                next_emit = time.monotonic()
                continue

            if snap.get("source_kind") != "direct_hls":
                break

            if proc and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
            print(f"[LIVE][VIDEO] restarting ffmpeg session={session}", flush=True)
            time.sleep(LIVE_RETRY_DELAY_SEC)
            if not _state_generation_is_current(sync_generation):
                break
            try:
                direct = refresh_live_source(generation=sync_generation)
            except Exception as exc:
                if not _state_generation_is_current(sync_generation):
                    break
                print(f"[LIVE][VIDEO] refresh failed: {exc}; retry cached source", flush=True)
                direct = str(_state_snapshot().get("source_url") or direct)
            # Live HLS always restarts at the live edge; never carry YouTube seek state.
            resume_seconds = 0.0
            next_emit = time.monotonic()
    except GeneratorExit:
        pass
    except Exception as exc:
        if sync_generation is None:
            _set_error(str(exc))
        else:
            with state_lock:
                if state.get("generation") == sync_generation:
                    state["error"] = str(exc)
                    state["running"] = False
        print(f"[VIDEO] stream error: {exc}", flush=True)
    finally:
        stop_event.set()
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        if producer and producer.is_alive():
            producer.join(timeout=1)
        with state_lock:
            if sync_generation is None or state.get("generation") == sync_generation:
                state["running"] = False


def thumb_cache_path(video_id: str) -> Path:
    safe = "".join(c for c in video_id if c.isalnum() or c in "-_" )[:64] or "thumb"
    return CACHE_DIR / f"{safe}_96x54.jpg"


def build_thumbnail(video_id: str, url: str) -> bytes:
    path = thumb_cache_path(video_id)
    if path.exists() and path.stat().st_size > 100:
        return path.read_bytes()
    if not url:
        raise RuntimeError("No thumbnail URL")
    r = requests.get(url, timeout=10)
    r.raise_for_status()
    img = Image.open(io.BytesIO(r.content)).convert("RGB")
    img.thumbnail((96, 54), Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", (96, 54), "black")
    canvas.paste(img, ((96 - img.width) // 2, (54 - img.height) // 2))
    out = io.BytesIO()
    canvas.save(out, format="JPEG", quality=72, optimize=True)
    data = out.getvalue()
    path.write_bytes(data)
    return data


@app.get("/")
def index():
    return render_template_string(
        """
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>CYD Mini TV</title><style>
body{font-family:system-ui;background:#101216;color:#eee;max-width:760px;margin:auto;padding:24px}input,button{font-size:16px;padding:10px;border-radius:8px;border:1px solid #444;background:#1d222a;color:#fff}input{width:70%}button{cursor:pointer}.card{padding:14px;background:#181c22;border-radius:12px;margin:12px 0}code{color:#8be9fd}
</style></head><body>
<h1>CYD Mini TV</h1>
<div class="card"><form method="post" action="/play"><input name="url" placeholder="Dán link YouTube"><button>Phát</button></form></div>
<div class="card"><b>Trạng thái:</b> {{s}}</div>
<div class="card">CYD trong cùng Wi-Fi sẽ tự tìm server. Endpoint video: <code>/stream.mjpg</code></div>
</body></html>
        """,
        s=json.dumps(_state_snapshot(), ensure_ascii=False),
    )


def build_audio_command(direct: str, resume_seconds: float = 0.0) -> list[str]:
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "2",
        "-rw_timeout", "10000000",
    ]
    if resume_seconds > 0.5:
        cmd += ["-ss", f"{resume_seconds:.3f}"]
    cmd += [
        "-i", direct,
        "-vn", "-ac", "1", "-ar", "48000",
        "-af", "highpass=f=220,lowpass=f=6500,equalizer=f=2800:t=q:w=1:g=3,loudnorm=I=-8:TP=-2:LRA=4",
        "-c:a", "libmp3lame", "-b:a", "64k",
        "-id3v2_version", "0", "-write_xing", "0",
        "-f", "mp3", "pipe:1",
    ]
    return cmd



def _audio_producer(proc: subprocess.Popen[bytes], audio_queue: "queue.Queue[bytes]", stop_event: threading.Event) -> None:
    """Drain FFmpeg into fixed-size MP3 blocks so queue depth maps to real time.

    An unbuffered pipe may return ~192-byte MP3-frame short reads even when read(4096)
    is requested. Counting those as 4096-byte reservoir slots made the nominal 16 s
    queue hold less than a second. Coalesce short reads before enqueueing.
    """
    assert proc.stdout is not None
    produced_bytes = 0
    last_data_at = time.monotonic()
    pending = bytearray()

    def put_block(chunk: bytes) -> bool:
        nonlocal produced_bytes
        while not stop_event.is_set():
            try:
                audio_queue.put(chunk, timeout=0.25)
                produced_bytes += len(chunk)
                return True
            except queue.Full:
                continue
        return False

    try:
        while not stop_event.is_set():
            raw = proc.stdout.read(AUDIO_CHUNK_BYTES)
            if not raw:
                break
            gap = time.monotonic() - last_data_at
            last_data_at = time.monotonic()
            if gap > 1.0:
                print(f"[AUDIO-BUF] source gap {gap:.2f}s queued={audio_queue.qsize()}", flush=True)
            pending.extend(raw)
            while len(pending) >= AUDIO_CHUNK_BYTES and not stop_event.is_set():
                chunk = bytes(pending[:AUDIO_CHUNK_BYTES])
                del pending[:AUDIO_CHUNK_BYTES]
                if not put_block(chunk):
                    break

        if pending and not stop_event.is_set():
            put_block(bytes(pending))
    except Exception as exc:
        print(f"[AUDIO-BUF] producer stopped: {exc}", flush=True)
    finally:
        print(f"[AUDIO-BUF] producer exit bytes={produced_bytes} queued={audio_queue.qsize()} pending={len(pending)}", flush=True)


def audio_generator():
    proc: subprocess.Popen[bytes] | None = None
    stop_event = threading.Event()
    producer: threading.Thread | None = None
    audio_queue: "queue.Queue[bytes]" = queue.Queue(maxsize=AUDIO_BUFFER_CHUNKS)
    sync_generation: int | None = None
    try:
        _video_direct, audio_direct, _title = resolve_selected()
        sync_generation = _current_playback_generation()
        source_snap = _state_snapshot()
        if int(source_snap.get("generation", -1)) != sync_generation:
            print(f"[AUDIO-BUF] stale generation before audio start generation={sync_generation}", flush=True)
            return
        is_youtube_session = source_snap.get("source_kind") == "youtube"
        sent_bytes = 0
        resume_seconds = float(source_snap.get("resume_seconds") or 0.0) if is_youtube_session else 0.0
        last_log = time.monotonic()
        session = 0

        while True:
            if not _state_generation_is_current(sync_generation):
                print(f"[AUDIO-BUF] stale generation audio exit generation={sync_generation}", flush=True)
                break
            session += 1
            proc = subprocess.Popen(
                build_audio_command(audio_direct, resume_seconds if is_youtube_session else 0.0),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                bufsize=0,
            )
            with state_lock:
                if state.get("generation") != sync_generation:
                    print(f"[AUDIO-BUF] stale generation after audio spawn generation={sync_generation}", flush=True)
                    break
                state["audio_running"] = True
                state["audio_error"] = ""
            if proc.stderr is not None:
                threading.Thread(target=_drain_ffmpeg_stderr, args=(proc.stderr,), daemon=True).start()

            producer = threading.Thread(target=_audio_producer, args=(proc, audio_queue, stop_event), daemon=True)
            producer.start()

            snap = _state_snapshot()
            audio_prebuffer_target = (AUDIO_PREBUFFER_CHUNKS_HLS
                                      if snap.get("source_kind") == "direct_hls"
                                      else AUDIO_PREBUFFER_CHUNKS_YT)
            audio_prebuffer_started = time.monotonic()
            stale_generation = False
            while audio_queue.qsize() < audio_prebuffer_target and producer.is_alive():
                if not _state_generation_is_current(sync_generation):
                    stale_generation = True
                    break
                if time.monotonic() - audio_prebuffer_started > 15.0:
                    break
                time.sleep(0.05)
            if stale_generation:
                print(f"[AUDIO-BUF] stale generation during audio prebuffer generation={sync_generation}", flush=True)
                break
            print(f"[AUDIO-BUF] prebuffer ready session={session} queued={audio_queue.qsize()}/{AUDIO_BUFFER_CHUNKS} target={audio_prebuffer_target}", flush=True)

            # Never let a fast audio transcode outrun startup of the heavier MJPEG path.
            if not _wait_for_video_started(sync_generation, timeout=20.0):
                print(f"[SYNC] audio aborted: video did not start generation={sync_generation}", flush=True)
                break
            print(f"[SYNC] audio released generation={sync_generation}", flush=True)

            while producer.is_alive() or not audio_queue.empty():
                if not _state_generation_is_current(sync_generation):
                    stale_generation = True
                    print(f"[AUDIO-BUF] stale generation during audio stream generation={sync_generation}", flush=True)
                    break
                try:
                    chunk = audio_queue.get(timeout=1.0)
                except queue.Empty:
                    continue

                sent_bytes += len(chunk)
                if time.monotonic() - last_log >= 5.0:
                    print(f"[AUDIO-BUF] sent={sent_bytes} queued={audio_queue.qsize()}/{AUDIO_BUFFER_CHUNKS}", flush=True)
                    last_log = time.monotonic()
                yield chunk

            if stop_event.is_set() or stale_generation:
                break
            if not _state_generation_is_current(sync_generation):
                print(f"[AUDIO-BUF] stale generation before audio recovery generation={sync_generation}", flush=True)
                break

            snap = _state_snapshot()
            if int(snap.get("generation", -1)) != sync_generation:
                print(f"[AUDIO-BUF] stale generation at audio recovery generation={sync_generation}", flush=True)
                break
            if snap.get("source_kind") == "youtube":
                if proc and proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                if producer and producer.is_alive():
                    producer.join(timeout=1)
                resume_seconds = float(snap.get("resume_seconds") or resume_seconds) if is_youtube_session else 0.0
                print(f"[YT][AUDIO] source ended at {resume_seconds:.1f}s -> refresh in same HTTP stream", flush=True)
                time.sleep(YOUTUBE_RETRY_DELAY_SEC)
                if not _state_generation_is_current(sync_generation):
                    break
                try:
                    _fresh_video, audio_direct, _fresh_title = refresh_youtube_sources(stale_audio=audio_direct, generation=sync_generation)
                except Exception as exc:
                    if not _state_generation_is_current(sync_generation):
                        break
                    print(f"[YT][AUDIO] refresh failed: {exc}; retrying", flush=True)
                    time.sleep(max(1.0, YOUTUBE_RETRY_DELAY_SEC * 2))
                continue

            if snap.get("source_kind") != "direct_hls":
                break

            if proc and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
            print(f"[LIVE][AUDIO] restarting ffmpeg session={session}", flush=True)
            time.sleep(LIVE_RETRY_DELAY_SEC)
            if not _state_generation_is_current(sync_generation):
                break
            try:
                audio_direct = refresh_live_source(generation=sync_generation)
            except Exception as exc:
                if not _state_generation_is_current(sync_generation):
                    break
                print(f"[LIVE][AUDIO] refresh failed: {exc}; retry cached source", flush=True)
                audio_direct = str(_state_snapshot().get("audio_url") or audio_direct)
            # Live HLS always restarts at the live edge.
            resume_seconds = 0.0
    except GeneratorExit:
        pass
    except Exception as exc:
        with state_lock:
            if sync_generation is None or state.get("generation") == sync_generation:
                state["audio_error"] = str(exc)
        print(f"[AUDIO] stream error: {exc}", flush=True)
    finally:
        stop_event.set()
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        if producer and producer.is_alive():
            producer.join(timeout=1)
        with state_lock:
            if sync_generation is None or state.get("generation") == sync_generation:
                state["audio_running"] = False


@app.post("/play")
def play_form():
    url = (request.form.get("url") or "").strip()
    if url:
        choose_video(url, url)
    return redirect("/")


@app.post("/api/play")
def api_play():
    data = request.get_json(silent=True) or {}
    url = str(data.get("url") or "").strip()
    title = str(data.get("title") or "").strip()
    if not url:
        return jsonify({"ok": False, "error": "missing url"}), 400
    choose_video(url, title)
    return jsonify({"ok": True, "stream": "/stream.mjpg", "video": "/stream.mjpg", "audio": "/stream.mp3"})


@app.get("/api/tv/channels")
def api_tv_channels():
    return jsonify({"ok": True, "items": VTVGO_CHANNELS})


@app.post("/api/tv/play")
def api_tv_play():
    data = request.get_json(silent=True) or {}
    channel_id = str(data.get("id") or "").strip().lower()
    try:
        channel = _find_vtvgo_channel(channel_id)
        direct = resolve_vtvgo_source(channel)
    except KeyError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 404
    except Exception as exc:
        _set_error(str(exc))
        return jsonify({"ok": False, "error": str(exc)}), 502
    generation = _reset_playback_sync()
    print(f"[SYNC] new live session generation={generation}", flush=True)
    with state_lock:
        state["selected"] = channel["url"]
        state["source_url"] = direct
        state["audio_url"] = direct
        state["title"] = channel["name"]
        state["source_kind"] = "direct_hls"
        state["channel_id"] = channel_id
        state["error"] = ""
        state["audio_error"] = ""
        state["running"] = False
        state["audio_running"] = False
        state["started_at"] = 0.0
        state["resume_seconds"] = 0.0
        state["generation"] = generation
    return jsonify({"ok": True, "stream": "/stream.mjpg", "video": "/stream.mjpg", "audio": "/stream.mp3", "title": channel["name"]})


@app.get("/api/feed")
def api_feed():
    q = request.args.get("q", "Tran Dang Khoa")
    try:
        items = search_youtube(q, 12)
        return jsonify({"ok": True, "query": q, "items": [asdict(x) for x in items]})
    except Exception as exc:
        return jsonify({"ok": False, "error": str(exc), "items": []}), 502


@app.get("/api/search")
def api_search():
    q = request.args.get("q", "")
    try:
        items = search_youtube(q, 15)
        return jsonify({"ok": True, "query": q, "items": [asdict(x) for x in items]})
    except Exception as exc:
        return jsonify({"ok": False, "error": str(exc), "items": []}), 502


@app.get("/thumb/<video_id>.jpg")
def thumb(video_id: str):
    url = request.args.get("u", "") or thumb_sources.get(video_id, "")
    try:
        data = build_thumbnail(video_id, url)
        return Response(data, mimetype="image/jpeg", headers={"Cache-Control": "public, max-age=86400"})
    except Exception as exc:
        return Response(str(exc), status=404, mimetype="text/plain")


@app.get("/status")
def status():
    # ESP32 discovery intentionally reads only a bounded prefix of /status.
    # Keep the full state for diagnostics, but put the stable validation keys
    # first so long signed media URLs cannot push them past that prefix.
    snap = _state_snapshot()
    payload = {
        "running": snap.get("running", False),
        "selected": snap.get("selected"),
    }
    payload.update(snap)
    return Response(
        json.dumps(payload, ensure_ascii=False, separators=(",", ":"), sort_keys=False),
        mimetype="application/json",
    )


@app.get("/stream.mjpg")
def stream():
    return Response(mjpeg_generator(), mimetype="multipart/x-mixed-replace; boundary=frame")


@app.get("/stream.mp3")
def audio_stream():
    return Response(
        audio_generator(),
        mimetype="audio/mpeg",
        headers={
            "Cache-Control": "no-store",
            "X-Audio-Format": "mp3;rate=48000;channels=1;bitrate=64k",
        },
    )


def discovery_loop() -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", DISCOVERY_PORT))
    while True:
        try:
            data, addr = sock.recvfrom(256)
            if data.strip() == DISCOVERY_MAGIC:
                msg = f"CYD_TV_SERVER|{PORT}".encode()
                sock.sendto(msg, addr)
        except Exception:
            time.sleep(0.2)


if __name__ == "__main__":
    threading.Thread(target=discovery_loop, daemon=True).start()
    print(f"CYD Mini TV server: http://0.0.0.0:{PORT}")
    print(f"UDP discovery: {DISCOVERY_PORT}")
    app.run(host=HOST, port=PORT, threaded=True, debug=False)
