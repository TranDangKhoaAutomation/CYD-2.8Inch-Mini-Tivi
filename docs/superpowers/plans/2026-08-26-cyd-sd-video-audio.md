# CYD SD Video + Audio Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Play long SD-card MJPEG video with indexed seeking and synchronized MP3 audio through the CYD onboard FM8002A amplifier on GPIO26.

**Architecture:** Keep MJPEG + CYD1 index as the video format and read index offsets directly from SD. Add MP3 decoding through ESP32 internal DAC channel on GPIO26. Treat audio as the continuous playback clock; video follows the same elapsed-time target and may drop late frames.

**Tech Stack:** Arduino_GFX display foundation, JPEGDEC, SD/FS, Preferences, ESP32-audioI2S or another library proven to support internal DAC/I2S_DAC_CHANNEL_LEFT_EN on the installed ESP32 Arduino core.

**Spec:** `docs/superpowers/specs/2026-08-26-cyd-minitv-media-platform-design.md`

## Global Constraints
- Audio output must use onboard FM8002A via GPIO26/internal DAC; no external DAC board.
- SD media set is `.mjpeg + .idx + optional .mp3` under `/videos`.
- Long `.idx` files stay on SD; never allocate all offsets in RAM.
- Missing MP3 must not block silent video playback.
- Pause/seek/back must affect both video and audio.
- SD player must stay responsive to touch/UART.

---

### Task 1: Extend converter for CYD audio profile

**Files:**
- Modify: `tools/video_converter/converter_core.py`
- Modify: `tools/video_converter/gui.py` or current GUI entry file
- Test: `tools/video_converter/tests/test_audio_profile.py`

**Interfaces:**
- Produces MP3 profile: mono, 32 kHz sample rate, 96 kbps default.

- [ ] **Step 1: Write failing test**

```python
from converter_core import build_ffmpeg_audio_args

def test_audio_profile_is_cyd_friendly(tmp_path):
    args = build_ffmpeg_audio_args(tmp_path/'in.mp4', tmp_path/'out.mp3', 'ffmpeg')
    joined = ' '.join(map(str, args))
    assert '-ac 1' in joined
    assert '-ar 32000' in joined
    assert '-b:a 96k' in joined
```

- [ ] **Step 2: Run and verify failure if current profile differs**

Run: `python -m unittest tools.video_converter.tests.test_audio_profile -v`
Expected: FAIL if profile is not exact.

- [ ] **Step 3: Implement exact audio profile**

```python
return [ffmpeg, '-y', '-hide_banner', '-i', str(input_path),
        '-map', '0:a:0?', '-vn', '-c:a', 'libmp3lame',
        '-b:a', '96k', '-ar', '32000', '-ac', '1', str(output_mp3)]
```

- [ ] **Step 4: Run converter tests and smoke conversion**

Run: `python -m unittest discover tools/video_converter/tests -v`
Expected: all PASS.

Smoke convert 30 seconds of the VTV1 source and verify `.mjpeg/.idx/.mp3` all exist.

- [ ] **Step 5: Commit**

```bash
git add tools/video_converter
git commit -m "feat: generate CYD-friendly mono MP3 tracks"
```

---

### Task 2: Add onboard FM8002A audio service

**Files:**
- Create: `src/audio/audio_player.h`
- Create: `src/audio/audio_player.cpp`
- Modify: `platformio.ini`
- Test: `tests/test_audio_hardware_path.py`

**Interfaces:**
- Produces:
  - `bool audioBegin()`
  - `bool audioOpenFile(const char *path)`
  - `bool audioOpenUrl(const String &url)`
  - `void audioLoop()`
  - `void audioPause(bool paused)`
  - `void audioStop()`
  - `void audioSetVolume(uint8_t volume)`
  - `bool audioIsRunning()`
  - `uint32_t audioPositionMs()` when available, otherwise a software monotonic playback clock.

- [ ] **Step 1: Write failing test**

```python
from pathlib import Path
CPP = Path('src/audio/audio_player.cpp').read_text(encoding='utf-8')

def test_audio_uses_internal_dac_left_channel():
    assert 'GPIO26' in CPP or 'I2S_DAC_CHANNEL_LEFT_EN' in CPP
    assert 'FM8002A' in CPP
```

- [ ] **Step 2: Run and verify failure**

Run: `python tests/test_audio_hardware_path.py`
Expected: FAIL because service does not exist.

- [ ] **Step 3: Add audio dependency proven for ESP32 internal DAC**

Prefer the same `ESP32-audioI2S` family used by CYD HelloRadio if compatible with the current Arduino core. If the current library release removed internal DAC support, pin a compatible release or implement the legacy internal DAC I2S path explicitly rather than changing hardware.

- [ ] **Step 4: Implement hardware initialization**

Document explicitly in code:

```cpp
// CYD ESP32-2432S028R: GPIO26 feeds onboard FM8002A amplifier.
// Speaker is connected to SPEAK connector.
```

Initialize internal DAC left channel and default volume 10/21 or an equivalent normalized scale.

- [ ] **Step 5: Run test + build**

Run: `python tests/test_audio_hardware_path.py`
Expected: PASS.

Run: `platformio run -e cyd`
Expected: SUCCESS.

- [ ] **Step 6: Hardware tone/MP3 smoke test**

Add temporary UART command `audiotest` that plays a short known MP3 from SD or a generated PCM/tone path if supported. Acceptance: audible clean sound from SPEAK connector without external DAC hardware.

- [ ] **Step 7: Commit**

```bash
git add src/audio platformio.ini tests/test_audio_hardware_path.py
git commit -m "feat: drive CYD onboard FM8002A audio"
```

---

### Task 3: Discover SD media sets with optional audio

**Files:**
- Create: `src/media/media_item.h`
- Create: `src/media/sd_library.cpp`
- Create: `src/media/sd_library.h`
- Modify: `src/main.cpp`
- Test: `tests/test_sd_media_discovery.py`

**Interfaces:**
- Produces:

```cpp
struct MediaItem {
    String base;
    String title;
    bool hasVideo;
    bool hasIndex;
    bool hasAudio;
};
```

- [ ] **Step 1: Write failing test**

Test source text for `.mjpeg`, `.idx`, `.mp3`, and `hasAudio` discovery logic.

- [ ] **Step 2: Run and verify failure**

Run: `python tests/test_sd_media_discovery.py`
Expected: FAIL.

- [ ] **Step 3: Implement media grouping**

Scan `/videos`, group files by basename, only expose entries with `.mjpeg + .idx`. Set `hasAudio=true` when matching `.mp3` exists.

- [ ] **Step 4: Update SD browser**

Show audio icon/text when `hasAudio`; show `VIDEO ONLY` otherwise.

- [ ] **Step 5: Run tests + build**

Run: `python tests/test_sd_media_discovery.py`
Expected: PASS.

Run: `platformio run -e cyd`
Expected: SUCCESS.

- [ ] **Step 6: Commit**

```bash
git add src/media src/main.cpp tests/test_sd_media_discovery.py
git commit -m "feat: group SD video index and audio tracks"
```

---

### Task 4: Synchronize indexed MJPEG with MP3 playback

**Files:**
- Create: `src/media/sd_player.h`
- Create: `src/media/sd_player.cpp`
- Modify: `src/main.cpp`
- Test: `tests/test_sd_av_sync_policy.py`

**Interfaces:**
- Produces:
  - `bool sdPlayerStart(const MediaItem&)`
  - `void sdPlayerLoop()`
  - `void sdPlayerPause()`
  - `void sdPlayerSeekMs(int64_t targetMs)`
  - `void sdPlayerStop()`
  - `SdPlayerStatus sdPlayerStatus()`

- [ ] **Step 1: Write failing policy test**

```python
from pathlib import Path
CPP = Path('src/media/sd_player.cpp').read_text(encoding='utf-8')

def test_video_can_drop_when_late_but_audio_is_continuous():
    assert 'late' in CPP.lower() or 'behind' in CPP.lower()
    assert 'drop' in CPP.lower()
    assert 'audioLoop' in CPP
```

- [ ] **Step 2: Run and verify failure**

Run: `python tests/test_sd_av_sync_policy.py`
Expected: FAIL.

- [ ] **Step 3: Implement master playback clock**

At start store `playStartMs`. If audio position is exposed and stable, use it; otherwise use `millis() - playStartMs - pausedDuration`. Target video frame is `floor(clockMs * fps / 1000)`. Read index offset for target frame. When one or more frames are late, skip directly to the current target frame rather than incrementing blindly.

- [ ] **Step 4: Service audio on every loop**

Call `audioLoop()` at high frequency and never put long delays in SD_PLAYER. JPEG decode happens only when a frame is due.

- [ ] **Step 5: Implement pause and seek**

Pause freezes the master clock and audio. Seek clamps 0..duration, seeks video by `.idx`, and restarts/repositions audio. If the selected audio library cannot sample-seek MP3 files directly, close/reopen and discard/decode until target or use library-supported `setAudioPlayPosition()` equivalent.

- [ ] **Step 6: Run tests + build**

Run: `python tests/test_sd_av_sync_policy.py`
Expected: PASS.

Run: `platformio run -e cyd`
Expected: SUCCESS.

- [ ] **Step 7: Hardware acceptance**

Use `VTV1_40M59_CYD` with matching MP3. Verify:
- audio is audible through onboard speaker,
- video colors remain correct,
- pause stops both,
- +1 minute and -1 minute recover within 2 seconds,
- after 5 minutes lips/speech are subjectively within about 250 ms.

- [ ] **Step 8: Commit**

```bash
git add src/media src/main.cpp tests/test_sd_av_sync_policy.py
git commit -m "feat: synchronize SD MJPEG and MP3 playback"
```

---

### Task 5: Finish SD player controls and diagnostics

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/media/sd_player.*`
- Test: `tests/test_sd_player_controls.py`

**Interfaces:**
- Player controls: BACK, -1m, pause/play, +1m, VOL -, VOL +.

- [ ] **Step 1: Write failing control test**

Assert source contains actions for back/pause/seek/volume and that touch handling uses calibrated screen coordinates.

- [ ] **Step 2: Run and verify failure**

Run: `python tests/test_sd_player_controls.py`
Expected: FAIL until all controls exist.

- [ ] **Step 3: Implement controls and status**

OSD displays `elapsed / total`, volume numeric value, and `AUDIO` or `NO AUDIO`.

- [ ] **Step 4: Add UART diagnostics**

Commands:
- `audioinfo`
- `vol <0-21>` or normalized equivalent
- `sdstatus`

- [ ] **Step 5: Run full build and hardware regression**

Run: `platformio run -e cyd`
Expected: SUCCESS.

Verify touch calibration remains aligned on all SD screens.

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp src/media tests/test_sd_player_controls.py
git commit -m "feat: complete SD player controls and diagnostics"
```
