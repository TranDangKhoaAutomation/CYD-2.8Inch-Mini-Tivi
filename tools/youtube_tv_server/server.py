from __future__ import annotations

import io
import json
import os
import queue
import socket
import subprocess
import threading
import time
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Any

import requests
from flask import Flask, Response, jsonify, redirect, render_template_string, request
from PIL import Image
from yt_dlp import YoutubeDL

HOST = "0.0.0.0"
PORT = int(os.environ.get("CYD_TV_PORT", "8876"))
DISCOVERY_PORT = 4210
DISCOVERY_MAGIC = b"CYD_TV_DISCOVER"
YT_STREAM_FPS = 6
YT_STREAM_Q = 6
YT_PREBUFFER_FRAMES = 24   # 4 seconds at 6 fps before playback starts
YT_BUFFER_FRAMES = 120     # up to 20 seconds of jitter absorption on the PC
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
thumb_sources: dict[str, str] = {}
state: dict[str, Any] = {
    "selected": None,
    "source_url": None,
    "title": "",
    "running": False,
    "error": "",
    "started_at": 0.0,
    "source_kind": "youtube",
    "channel_id": "",
}

VTVGO_CHANNELS = [
    {"id": "vtv1", "name": "VTV1", "url": "https://vtvgo.vn/channel/vtv1-1,1.html"},
    {"id": "vtv2", "name": "VTV2", "url": "https://vtvgo.vn/channel/vtv2-1,2.html"},
    {"id": "vtv3", "name": "VTV3", "url": "https://vtvgo.vn/channel/vtv3-1,3.html"},
    {"id": "vtv4", "name": "VTV4", "url": "https://vtvgo.vn/channel/vtv4-1,4.html"},
    {"id": "vtv5", "name": "VTV5", "url": "https://vtvgo.vn/channel/vtv5-1,5.html"},
    {"id": "vtv6", "name": "VTV6", "url": "https://vtvgo.vn/channel/vtv6-1,13.html"},
    {"id": "vtv7", "name": "VTV7", "url": "https://vtvgo.vn/channel/vtv7-1,27.html"},
    {"id": "vtv8", "name": "VTV8", "url": "https://vtvgo.vn/channel/vtv8-1,36.html"},
    {"id": "vtv9", "name": "VTV9", "url": "https://vtvgo.vn/channel/vtv9-1,39.html"},
    {"id": "vtv10", "name": "VTV10", "url": "https://vtvgo.vn/channel/vtv10-1,6.html"},
    {"id": "vietnamtoday", "name": "Vietnam Today", "url": "https://vtvgo.vn/channel/vietnam-today-1,vietnamtoday.html"},
    {"id": "antv", "name": "ANTV", "url": "https://vtvgo.vn/channel/truyen-hinh-cong-an-nhan-dan-1,89.html"},
    {"id": "qpvn", "name": "QPVN", "url": "https://vtvgo.vn/channel/truyen-hinh-quoc-phong-viet-nam-1,103.html"},
    {"id": "hanoi1", "name": "Hà Nội 1", "url": "https://vtvgo.vn/channel/truyen-hinh-ha-noi-1-1,111.html"},
    {"id": "hanoi2", "name": "Hà Nội 2", "url": "https://vtvgo.vn/channel/truyen-hinh-ha-noi-2-1,17.html"},
]


def _find_vtvgo_channel(channel_id: str) -> dict[str, str]:
    for item in VTVGO_CHANNELS:
        if item["id"] == channel_id:
            return item
    raise KeyError(f"unknown TV channel: {channel_id}")


def _system_chrome_path() -> str:
    candidates = [
        Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe"),
        Path(r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe"),
        Path(r"C:\Program Files\Microsoft\Edge\Application\msedge.exe"),
        Path(r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"),
    ]
    for path in candidates:
        if path.exists():
            return str(path)
    raise RuntimeError("Chrome/Edge not found for VTV Go source resolver")


def resolve_vtvgo_source(channel: dict[str, str]) -> str:
    """Use the public VTV Go web player to obtain its current non-DRM HLS URL."""
    from playwright.sync_api import sync_playwright

    found: list[str] = []
    with sync_playwright() as pw:
        browser = pw.chromium.launch(
            headless=True,
            executable_path=_system_chrome_path(),
            args=["--autoplay-policy=no-user-gesture-required", "--disable-blink-features=AutomationControlled"],
        )
        try:
            page = browser.new_page(
                user_agent="Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/151.0.0.0 Safari/537.36",
                locale="vi-VN",
                viewport={"width": 1280, "height": 720},
            )

            def on_request(req) -> None:
                url = req.url
                low = url.lower()
                if "vtvgolive-" in low and "/hls/" in low and low.endswith("master.m3u8"):
                    if url not in found:
                        found.append(url)

            page.on("request", on_request)
            page.goto(channel["url"], wait_until="domcontentloaded", timeout=60000)
            page.wait_for_timeout(1000)
            consent = page.get_by_role("button", name="Đồng ý và tiếp tục")
            if consent.count():
                consent.click()
            deadline = time.monotonic() + 14.0
            while time.monotonic() < deadline and not found:
                page.wait_for_timeout(250)
        finally:
            browser.close()

    if not found:
        raise RuntimeError(f"VTV Go did not expose a non-DRM HLS source for {channel['name']}")
    return found[0]


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


def resolve_direct_media(url: str) -> tuple[str, str]:
    info = _run_ytdlp_json([
        "--dump-single-json",
        "--no-playlist",
        "-f", "bestvideo[height<=480][ext=mp4][vcodec^=avc1][protocol=https]/bestvideo[height<=480][protocol=https]/bestvideo[height<=480]",
        url,
    ], timeout=60)
    direct = info.get("url")
    title = info.get("title") or url
    if not direct:
        # Some extractors return requested_formats rather than a top-level URL.
        fmts = info.get("requested_formats") or []
        for f in fmts:
            if f.get("url"):
                direct = f["url"]
                break
    if not direct:
        raise RuntimeError("yt-dlp did not return a playable media URL")
    return str(direct), str(title)


def _state_snapshot() -> dict[str, Any]:
    with state_lock:
        return dict(state)


def _set_error(msg: str) -> None:
    with state_lock:
        state["error"] = msg
        state["running"] = False


def choose_video(url: str, title: str = "") -> None:
    with state_lock:
        state["selected"] = url
        state["source_url"] = None
        state["title"] = title
        state["error"] = ""
        state["running"] = False
        state["started_at"] = 0.0
        state["source_kind"] = "youtube"
        state["channel_id"] = ""


def resolve_selected() -> tuple[str, str]:
    snap = _state_snapshot()
    if snap.get("source_kind") == "direct_hls" and snap.get("source_url"):
        return str(snap["source_url"]), str(snap.get("title") or "Live TV")
    selected = snap.get("selected")
    if not selected:
        raise RuntimeError("No video selected")
    direct, title = resolve_direct_media(str(selected))
    with state_lock:
        state["source_url"] = direct
        if title:
            state["title"] = title
        state["error"] = ""
    return direct, title


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
    try:
        direct, title = resolve_selected()
        cmd = [
            "ffmpeg",
            "-hide_banner",
            "-loglevel", "error",
            "-reconnect", "1",
            "-reconnect_streamed", "1",
            "-reconnect_delay_max", "2",
            "-rw_timeout", "10000000",
            "-i", direct,
            "-an",
            # 320x180 is the panel's actual video area. Decode a modest progressive source,
            # transcode ahead on the PC, then pace the already-buffered JPEGs to the ESP32.
            "-vf", f"scale=320:180:force_original_aspect_ratio=decrease:flags=fast_bilinear,pad=320:180:(ow-iw)/2:(oh-ih)/2:black,fps={YT_STREAM_FPS}",
            "-q:v", str(YT_STREAM_Q),
            "-f", "mjpeg",
            "pipe:1",
        ]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        with state_lock:
            state["running"] = True
            state["started_at"] = time.time()
            state["title"] = title
            state["error"] = ""

        if proc.stderr is not None:
            threading.Thread(target=_drain_ffmpeg_stderr, args=(proc.stderr,), daemon=True).start()
        producer = threading.Thread(target=_mjpeg_producer, args=(proc, frame_queue, stop_event), daemon=True)
        producer.start()

        # Build a few seconds of reserve before the first frame is emitted. Without -re,
        # ffmpeg can continue filling the deeper queue in the background while playback runs.
        snap = _state_snapshot()
        prebuffer_target = 12 if snap.get("source_kind") == "direct_hls" else YT_PREBUFFER_FRAMES
        prebuffer_started = time.monotonic()
        while frame_queue.qsize() < prebuffer_target and producer.is_alive():
            if time.monotonic() - prebuffer_started > 12.0:
                break
            time.sleep(0.05)
        print(f"[BUFFER] playback start queued={frame_queue.qsize()}/{YT_BUFFER_FRAMES}", flush=True)

        period = 1.0 / YT_STREAM_FPS
        next_emit = time.monotonic()
        sent = 0
        while True:
            if not producer.is_alive() and frame_queue.empty():
                break
            try:
                frame = frame_queue.get(timeout=2.0)
            except queue.Empty:
                print("[BUFFER] underrun: no frame for 2.0s", flush=True)
                continue

            now = time.monotonic()
            if next_emit > now:
                time.sleep(next_emit - now)
            else:
                # Do not accumulate timing debt after an occasional slow client write.
                next_emit = now
            yield b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + str(len(frame)).encode() + b"\r\n\r\n" + frame + b"\r\n"
            sent += 1
            next_emit += period
            if sent % (YT_STREAM_FPS * 5) == 0:
                print(f"[BUFFER] sent={sent} queued={frame_queue.qsize()}", flush=True)
    except GeneratorExit:
        pass
    except Exception as exc:
        _set_error(str(exc))
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
            state["running"] = False


def thumb_cache_path(video_id: str) -> Path:
    safe = "".join(c for c in video_id if c.isalnum() or c in "-_" )[:64] or "thumb"
    return CACHE_DIR / f"{safe}.jpg"


def build_thumbnail(video_id: str, url: str) -> bytes:
    path = thumb_cache_path(video_id)
    if path.exists() and path.stat().st_size > 100:
        return path.read_bytes()
    if not url:
        raise RuntimeError("No thumbnail URL")
    r = requests.get(url, timeout=10)
    r.raise_for_status()
    img = Image.open(io.BytesIO(r.content)).convert("RGB")
    img.thumbnail((112, 63), Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", (112, 63), "black")
    canvas.paste(img, ((112 - img.width) // 2, (63 - img.height) // 2))
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
    return jsonify({"ok": True, "stream": "/stream.mjpg"})


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
    with state_lock:
        state["selected"] = channel["url"]
        state["source_url"] = direct
        state["title"] = channel["name"]
        state["source_kind"] = "direct_hls"
        state["channel_id"] = channel_id
        state["error"] = ""
        state["running"] = False
        state["started_at"] = 0.0
    return jsonify({"ok": True, "stream": "/stream.mjpg", "title": channel["name"]})


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
    return jsonify(_state_snapshot())


@app.get("/stream.mjpg")
def stream():
    return Response(mjpeg_generator(), mimetype="multipart/x-mixed-replace; boundary=frame")


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
