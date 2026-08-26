# CYD MiniTV Media Platform Design

## Goal
Build a complete ESP32-2432S028R MiniTV firmware that keeps the now-verified correct display path, supports calibrated touch, plays long SD-card video with onboard amplified audio, and searches/plays YouTube through the existing PC server with synchronized video and audio.

## Hardware baseline
- Board: ESP32-2432S028R / CYD 2.8-inch, 320x240 landscape.
- Display: ILI9341 on HSPI pins MISO=12, MOSI=13, SCLK=14, CS=15, DC=2, BL=21.
- Verified display path: Arduino_GFX + Arduino_ILI9341, 40 MHz SPI, rotation 1, `invertDisplay(true)`.
- JPEG path: JPEGDEC RGB565 big-endian -> `draw16bitBeRGBBitmap()`.
- SD: VSPI CS=5, SCK=18, MISO=19, MOSI=23.
- Touch: XPT2046 IRQ=36, MOSI=32, MISO=39, CLK=25, CS=33.
- Audio: onboard FM8002A power amplifier fed from ESP32 GPIO26 through internal DAC; speaker connects to the board `SPEAK` connector. Use `I2S_DAC_CHANNEL_LEFT_EN` / internal DAC output path rather than an external MAX98357A.

## Architecture

### 1. Display and touch foundation
Replace TFT_eSPI rendering in the main MiniTV firmware with the proven Arduino_GFX display path. Keep all application screens and interaction states, but route drawing through a small display abstraction so video and UI use one consistent color pipeline.

Replace the current hand-bit-banged XPT2046 reader with `XPT2046_Touchscreen`. Run touch in rotation 1 and add a four-point calibration flow. Store calibration bounds and axis orientation in NVS/Preferences. On first boot without valid calibration, show four crosshairs; later boots load calibration automatically. A UART `touchcal` command restarts calibration.

### 2. SD media player
Keep the existing `/videos/<base>.mjpeg + .idx` long-video format. Continue reading `.idx` offsets directly from SD so long files do not allocate the entire index in RAM.

The PC converter will additionally create `/videos/<base>.mp3`, encoded mono at a modest rate suitable for ESP32 decoding. SD playback uses the `.idx` frame clock as the master timeline. Audio starts at video time 0 and runs continuously through the ESP32 audio library to internal DAC GPIO26. Pause pauses both streams. Seek closes/reopens audio at the target time and seeks video via `.idx`. The player exposes play/pause, ±1 minute, back, volume, elapsed/total time, and audio-present state.

### 3. YouTube search and playback
Reuse the existing PC server discovery, `/api/search`, `/api/feed`, `/thumb/<id>.jpg`, and `/api/play` workflow.

Change the server so one selected YouTube item is resolved once and serves two paced outputs derived from the same source:
- `/stream.mjpg`: 320x180, 10 fps, MJPEG.
- `/stream.mp3`: mono MP3, 32 kHz, bitrate selected for stability.

The ESP32 keeps the current on-screen keyboard and result browser. Selecting a result starts both streams. Audio is treated as the timing master; video frames may be dropped when late but audio should not be intentionally dropped. Back stops both network streams. Volume is local on ESP32.

## User interface
Home screen has two large entries: SD VIDEO and YOUTUBE TV. SD browser lists discovered videos. YouTube browser shows thumbnail, title, channel, duration, supports swipe scrolling, and has SEARCH in the header. Search uses the existing on-screen keyboard.

Player overlay shows BACK, title, elapsed/total time when known, pause/play, seek controls, and volume. Touch hitboxes use calibrated screen coordinates only.

## Error handling
- Missing SD: show SD unavailable but leave YouTube usable.
- Missing `.idx`: reject seekable playback and show a clear file error rather than scanning large MJPEG files byte-by-byte.
- Missing `.mp3`: play video silently and show `NO AUDIO`.
- Audio decoder/network failure: continue video if possible and show audio status.
- YouTube server unavailable: show reconnect action and keep SD usable.
- Stream stall: preserve UI responsiveness and allow BACK.
- Invalid touch calibration: discard NVS calibration and re-enter calibration flow.

## Performance constraints
- Display SPI fixed at 40 MHz initially because this exact panel has already been verified at that rate.
- SD default 4 MHz first; increase only after stability testing.
- Video baseline 320x240/12 fps for SD and 320x180/10 fps for YouTube.
- No full-frame framebuffer is required.
- Long video index offsets remain on SD, not heap.
- Audio decode loop must be serviced frequently enough to avoid underrun; video is allowed to drop late frames.

## Validation order
1. Verify Arduino_GFX color baseline with inversion remains correct.
2. Calibrate touch and verify all four corners plus center.
3. Play long VTV1 SD video without audio, verify seek/pause.
4. Play matching MP3 through GPIO26 -> FM8002A speaker and verify volume/pause.
5. Verify SD A/V sync over at least 5 minutes and after ±1 minute seek.
6. Verify YouTube search, thumbnail, selection, MJPEG video.
7. Verify YouTube MP3 audio and A/V sync.
8. Verify BACK, reconnect, server-offline, SD-missing, and long-run stability.
