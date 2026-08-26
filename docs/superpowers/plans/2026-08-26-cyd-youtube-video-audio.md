# CYD YouTube Search + Video + Audio Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the CYD search YouTube, browse results, select a video, and play synchronized MJPEG video plus MP3 audio through the onboard FM8002A amplifier.

**Architecture:** Keep the existing PC server as the heavy transcoding layer. Resolve one selected YouTube source once, then expose two paced endpoints derived from the same source: `/stream.mjpg` and `/stream.mp3`. On ESP32, audio is serviced continuously and video may drop late frames. Keep the current Wi-Fi discovery, thumbnails, search keyboard, result list, and back navigation.

**Tech Stack:** Flask server, yt-dlp standalone, ffmpeg, Arduino WiFi/HTTPClient, Arduino_GFX, JPEGDEC, onboard internal-DAC audio service from the SD plan.

**Spec:** `docs/superpowers/specs/2026-08-26-cyd-minitv-media-platform-design.md`

## Global Constraints
- Search must happen from the CYD screen using the existing on-screen keyboard.
- Server performs YouTube extraction/transcoding; ESP32 never decodes H.264/AAC directly.
- Video endpoint: 320x180, 10 fps MJPEG.
- Audio endpoint: mono MP3, 32 kHz, conservative bitrate.
- Both streams must come from the same selected source/session.
- Audio continuity has priority; video may drop frames.
- BACK must stop both streams immediately enough to keep UI responsive.

---

### Task 1: Refactor server selected-media state into a reusable session

**Files:**
- Modify: `tools/youtube_tv_server/server.py`
- Test: `tools/youtube_tv_server/tests/test_media_session.py`

**Interfaces:**
- Produces a state/session structure with selected URL, resolved direct source, title, generation/session id, error, and started timestamp.

- [ ] **Step 1: Write failing test**

Create a test that selects a URL, resolves once using a mocked resolver, then asks both video and audio path builders for source metadata and asserts the resolver was called only once per selected session.

- [ ] **Step 2: Run and verify failure**

Run: `python -m unittest tools.youtube_tv_server.tests.test_media_session -v`
Expected: FAIL because current `mjpeg_generator()` resolves independently and no audio path exists.

- [ ] **Step 3: Implement resolved session cache**

Add a lock-protected session generation. `choose_video()` invalidates old resolution and increments generation. `ensure_resolved_selected()` resolves direct media at most once for that generation and returns a stable source descriptor.

- [ ] **Step 4: Preserve current API behavior**

`POST /api/play` still returns `ok: true`, but include both endpoint paths:

```json
{"ok": true, "video": "/stream.mjpg", "audio": "/stream.mp3"}
```

- [ ] **Step 5: Run tests**

Run: `python -m unittest tools.youtube_tv_server.tests.test_media_session -v`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add tools/youtube_tv_server/server.py tools/youtube_tv_server/tests
git commit -m "refactor: cache one resolved YouTube media session"
```

---

### Task 2: Add paced MP3 stream endpoint

**Files:**
- Modify: `tools/youtube_tv_server/server.py`
- Test: `tools/youtube_tv_server/tests/test_audio_stream_command.py`

**Interfaces:**
- Produces `GET /stream.mp3` with `audio/mpeg` response.

- [ ] **Step 1: Write failing command test**

Test the ffmpeg audio command builder for:
- `-re`
- `-vn`
- `-ac 1`
- `-ar 32000`
- `-b:a 96k` or the chosen stable bitrate
- `-f mp3 pipe:1`

- [ ] **Step 2: Run and verify failure**

Run: `python -m unittest tools.youtube_tv_server.tests.test_audio_stream_command -v`
Expected: FAIL because no audio generator exists.

- [ ] **Step 3: Implement `audio_generator()`**

Build ffmpeg from the same resolved source/session used by video. Drain stderr in a daemon thread exactly as the video path does. Yield moderate chunks, e.g. 4096 bytes, until process ends or client disconnects.

- [ ] **Step 4: Add Flask route**

```python
@app.get('/stream.mp3')
def audio_stream():
    return Response(audio_generator(), mimetype='audio/mpeg')
```

- [ ] **Step 5: Run tests + server smoke test**

Run: `python -m unittest discover tools/youtube_tv_server/tests -v`
Expected: PASS.

Start server and verify `/status`, `/api/search`, `/stream.mjpg`, and `/stream.mp3` respond for a selected video.

- [ ] **Step 6: Commit**

```bash
git add tools/youtube_tv_server/server.py tools/youtube_tv_server/tests
git commit -m "feat: stream YouTube audio as paced mono MP3"
```

---

### Task 3: Harden MJPEG endpoint around the same session

**Files:**
- Modify: `tools/youtube_tv_server/server.py`
- Test: `tools/youtube_tv_server/tests/test_video_stream_command.py`

**Interfaces:**
- `/stream.mjpg` stays multipart MJPEG at 320x180/10 fps.

- [ ] **Step 1: Write failing command test**

Assert ffmpeg video command includes `-re`, `fps=10`, 320x180 scale/pad, `-an`, and MJPEG output.

- [ ] **Step 2: Run and verify current behavior**

Expected: likely PASS for most fields; update test to also verify it consumes the shared resolved session rather than resolving independently.

- [ ] **Step 3: Refactor generator**

Remove direct call to the old resolver from `mjpeg_generator()` and use `ensure_resolved_selected()`.

- [ ] **Step 4: Run server tests**

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/youtube_tv_server/server.py tools/youtube_tv_server/tests/test_video_stream_command.py
git commit -m "refactor: bind MJPEG stream to selected media session"
```

---

### Task 4: Keep CYD search/browser flow and adapt display APIs

**Files:**
- Modify: `src/main.cpp`
- Test: `tests/test_youtube_search_ui.py`

**Interfaces:**
- Existing endpoints: `/api/feed`, `/api/search`, `/thumb/<id>.jpg`, `/api/play`.
- Existing screens: YT_BROWSER, YT_KEYBOARD, YT_PLAYER.

- [ ] **Step 1: Write failing regression test**

Assert source retains strings/endpoints for `/api/search`, `/api/feed`, `/thumb/`, `/api/play`, and screen states `YT_BROWSER`, `YT_KEYBOARD`, `YT_PLAYER` after the Arduino_GFX migration.

- [ ] **Step 2: Run and verify failures caused by display refactor**

Expected: failures only where previous TFT_eSPI assumptions were removed.

- [ ] **Step 3: Port thumbnails and text rendering**

Decode thumbnails through the same JPEGDEC big-endian callback used by video. Keep touch hitboxes in calibrated screen coordinates.

- [ ] **Step 4: Validate result selection**

`POST /api/play` response should be parsed for `video` and `audio` fields. Fall back to `/stream.mjpg` and `/stream.mp3` if fields are absent for compatibility.

- [ ] **Step 5: Run tests + build**

Run: `python tests/test_youtube_search_ui.py`
Expected: PASS.

Run: `platformio run -e cyd`
Expected: SUCCESS.

- [ ] **Step 6: Hardware acceptance**

Search a query from the on-screen keyboard, scroll results, open one result, and verify thumbnail/title hitboxes match visible UI after touch calibration.

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp tests/test_youtube_search_ui.py
git commit -m "feat: preserve YouTube search UI on Arduino_GFX"
```

---

### Task 5: Start YouTube video and audio together on ESP32

**Files:**
- Create: `src/media/youtube_player.h`
- Create: `src/media/youtube_player.cpp`
- Modify: `src/main.cpp`
- Test: `tests/test_youtube_av_player.py`

**Interfaces:**
- Produces:
  - `bool youtubePlayerStart(const String &videoUrl, const String &audioUrl, const String &title)`
  - `void youtubePlayerLoop()`
  - `void youtubePlayerStop()`
  - `YoutubePlayerStatus youtubePlayerStatus()`

- [ ] **Step 1: Write failing policy test**

Assert player source calls `audioLoop()` continuously, has an MJPEG parser, has a video-drop counter, and exposes stop/back behavior for both HTTP streams.

- [ ] **Step 2: Run and verify failure**

Run: `python tests/test_youtube_av_player.py`
Expected: FAIL.

- [ ] **Step 3: Implement start sequence**

Start audio stream first and allow a short configurable prebuffer. Then open MJPEG stream and initialize parser. Record common player start time.

- [ ] **Step 4: Implement non-blocking loop**

Each loop iteration:
1. service `audioLoop()`;
2. process a bounded amount of MJPEG network data;
3. when a complete JPEG arrives, decode/render it only if not excessively late;
4. process touch/UART frequently.

Do not use long blocking reads or delays.

- [ ] **Step 5: Implement stop/back**

Close video HTTPClient, stop audio decoder/network client, clear parser buffers, return to YT_BROWSER, and redraw.

- [ ] **Step 6: Run tests + build**

Run: `python tests/test_youtube_av_player.py`
Expected: PASS.

Run: `platformio run -e cyd`
Expected: SUCCESS.

- [ ] **Step 7: Commit**

```bash
git add src/media/youtube_player.* src/main.cpp tests/test_youtube_av_player.py
git commit -m "feat: play YouTube MJPEG and MP3 concurrently"
```

---

### Task 6: Add YouTube player controls and A/V diagnostics

**Files:**
- Modify: `src/media/youtube_player.*`
- Modify: `src/main.cpp`
- Test: `tests/test_youtube_player_controls.py`

**Interfaces:**
- Controls: BACK, pause when feasible, volume -, volume +, OSD toggle.
- Diagnostics: FPS, dropped video frames, audio running state, average JPEG size, decode/render timings.

- [ ] **Step 1: Write failing control test**

Assert source contains back, volume, status, and dropped-frame metrics.

- [ ] **Step 2: Run and verify failure**

Expected: FAIL.

- [ ] **Step 3: Implement OSD**

Display title, `YouTube via LAN`, volume, audio state, and video FPS/drop metrics on demand. Keep OSD timeout behavior.

- [ ] **Step 4: Add UART diagnostics**

Commands:
- `ytstatus`
- `fps`
- `audioinfo`
- `vol <n>`

- [ ] **Step 5: Run tests + build**

Expected: PASS and successful firmware build.

- [ ] **Step 6: End-to-end hardware test**

Verify:
- server discovery;
- search query from CYD;
- 10+ result list;
- one selected video plays correct colors;
- audio is audible through FM8002A/SPEAK;
- 5-minute playback without freeze;
- BACK exits promptly;
- disconnecting server shows recoverable error;
- SD mode still works afterward.

- [ ] **Step 7: Commit**

```bash
git add src/media/youtube_player.* src/main.cpp tests/test_youtube_player_controls.py
git commit -m "feat: finish YouTube AV controls and diagnostics"
```
