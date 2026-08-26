# CYD Display + Touch Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the unstable TFT_eSPI display/touch path with the verified Arduino_GFX display pipeline and calibrated XPT2046 touch while preserving MiniTV application state/screens.

**Architecture:** Keep the existing MiniTV state machine, but centralize display setup and drawing behind Arduino_GFX. Use an isolated software-SPI XPT2046 transport with landscape mapping and a four-point calibration stored in Preferences; HSPI is reserved for TFT and VSPI for SD. Do not change SD/YouTube behavior beyond the minimum needed to compile and interact.

**Tech Stack:** PlatformIO, Arduino ESP32 core, Arduino_GFX 1.6.1, JPEGDEC 1.8.2 source/tag, XPT2046_Touchscreen, Preferences/NVS.

**Spec:** `docs/superpowers/specs/2026-08-26-cyd-minitv-media-platform-design.md`

## Global Constraints
- Display must use Arduino_GFX + Arduino_ILI9341.
- Display SPI must start at 40 MHz.
- Landscape rotation must be 1.
- `invertDisplay(true)` is mandatory for this exact panel.
- JPEG output is RGB565 big-endian and is drawn with `draw16bitBeRGBBitmap()`.
- Touch pins are IRQ=36, MOSI=32, MISO=39, CLK=25, CS=33.
- Touch calibration must be stored in NVS and restartable with UART `touchcal`.
- Do not reintroduce TFT_eSPI into the video renderer.

---

### Task 1: Lock display dependencies and configuration

**Files:**
- Modify: `platformio.ini`
- Test: `tests/test_display_stack.py`

**Interfaces:**
- Produces: build-time availability of `Arduino_GFX_Library.h`, `JPEGDEC.h`, and `XPT2046_Touchscreen.h`.

- [ ] **Step 1: Write the failing test**

```python
from pathlib import Path

INI = Path('platformio.ini').read_text(encoding='utf-8')

def test_verified_display_stack_is_selected():
    assert 'GFX Library for Arduino' in INI or 'Arduino_GFX' in INI
    assert 'XPT2046_Touchscreen' in INI
    assert 'TFT_eSPI' not in INI
    assert 'SPI_FREQUENCY=40000000' not in INI
```

- [ ] **Step 2: Run test and verify failure**

Run: `python tests/test_display_stack.py`
Expected: FAIL because MiniTV still declares TFT_eSPI and does not declare the touch library.

- [ ] **Step 3: Update PlatformIO dependencies**

Use exact/pinned sources where possible:

```ini
lib_deps =
    moononournation/GFX Library for Arduino@1.6.1
    https://github.com/bitbank2/JPEGDEC.git#1.8.2
    paulstoffregen/XPT2046_Touchscreen
    tzapu/WiFiManager@^2.0.17
    bblanchon/ArduinoJson@^7.2.1
```

Remove TFT_eSPI-specific `build_flags`. Keep `-O2`, debug level, partitions, and custom upload settings.

- [ ] **Step 4: Run test and PlatformIO build**

Run: `python tests/test_display_stack.py`
Expected: PASS.

Run: `platformio run -e cyd`
Expected: dependencies resolve; compilation may still fail until Task 2 replaces TFT_eSPI symbols.

- [ ] **Step 5: Commit**

```bash
git add platformio.ini tests/test_display_stack.py
git commit -m "build: select verified CYD display stack"
```

---

### Task 2: Introduce verified Arduino_GFX display wrapper

**Files:**
- Create: `src/display/display.h`
- Create: `src/display/display.cpp`
- Modify: `src/main.cpp`
- Test: `tests/test_display_baseline.py`

**Interfaces:**
- Produces:
  - `bool displayBegin()`
  - `Arduino_GFX* display()`
  - `int jpegDrawCallback(JPEGDRAW *draw)`
  - verified rotation/inversion setup.

- [ ] **Step 1: Write failing test**

```python
from pathlib import Path

CPP = Path('src/display/display.cpp').read_text(encoding='utf-8')

def test_display_uses_verified_cyd_baseline():
    assert 'Arduino_HWSPI' in CPP
    assert 'Arduino_ILI9341' in CPP
    assert '40000000' in CPP
    assert 'setRotation(1)' in CPP
    assert 'invertDisplay(true)' in CPP
    assert 'draw16bitBeRGBBitmap' in CPP
```

- [ ] **Step 2: Run and verify failure**

Run: `python tests/test_display_baseline.py`
Expected: FAIL because the files do not exist.

- [ ] **Step 3: Implement wrapper**

Core initialization:

```cpp
static Arduino_DataBus *bus = new Arduino_HWSPI(2, 15, 14, 13, 12);
static Arduino_GFX *gfx = new Arduino_ILI9341(bus);

bool displayBegin() {
    pinMode(21, OUTPUT);
    digitalWrite(21, HIGH);
    if (!gfx->begin(40000000L)) return false;
    gfx->setRotation(1);
    gfx->invertDisplay(true);
    gfx->fillScreen(RGB565_BLACK);
    return true;
}

int jpegDrawCallback(JPEGDRAW *p) {
    gfx->draw16bitBeRGBBitmap(p->x, p->y, p->pPixels, p->iWidth, p->iHeight);
    return 1;
}
```

Expose `display()` to existing UI code during migration.

- [ ] **Step 4: Replace TFT_eSPI initialization in `main.cpp`**

Remove `#include <TFT_eSPI.h>` and `TFT_eSPI tft;`. Replace startup with `displayBegin()`. Convert JPEG preparation to `RGB565_BIG_ENDIAN` only.

- [ ] **Step 5: Run test + build**

Run: `python tests/test_display_baseline.py`
Expected: PASS.

Run: `platformio run -e cyd`
Expected: compile errors now limited to remaining TFT_eSPI drawing APIs, to be handled in Task 3.

- [ ] **Step 6: Commit**

```bash
git add src/display src/main.cpp tests/test_display_baseline.py
git commit -m "feat: add verified Arduino_GFX CYD display path"
```

---

### Task 3: Port UI primitives from TFT_eSPI to Arduino_GFX

**Files:**
- Create: `src/display/ui_draw.h`
- Create: `src/display/ui_draw.cpp`
- Modify: `src/main.cpp`
- Test: `tests/test_no_tft_espi.py`

**Interfaces:**
- Produces UI helpers used by all screens:
  - `void uiFill(uint16_t color)`
  - `void uiRect(int x,int y,int w,int h,uint16_t color)`
  - `void uiText(const String&, int x, int y, uint16_t color, uint8_t size=1)`
  - `void uiCenteredText(...)`
  - `uint16_t uiColor(uint8_t r,uint8_t g,uint8_t b)`

- [ ] **Step 1: Write failing test**

```python
from pathlib import Path

SRC = Path('src/main.cpp').read_text(encoding='utf-8')

def test_no_tft_espi_symbols_remain():
    forbidden = ['TFT_eSPI', 'setTextDatum', 'drawRoundRect', 'fillRoundRect']
    for token in forbidden:
        assert token not in SRC
```

- [ ] **Step 2: Run and verify failure**

Run: `python tests/test_no_tft_espi.py`
Expected: FAIL.

- [ ] **Step 3: Implement Arduino_GFX UI helpers**

Use `gfx->fillRect`, `gfx->drawRect`, `gfx->setCursor`, `gfx->setTextColor`, `gfx->setTextSize`, `gfx->print`. Implement centered text by measuring via `getTextBounds()` where available or deterministic fixed-font width for the chosen font size.

- [ ] **Step 4: Port all screens**

Port HOME, SD_BROWSER, SD_PLAYER OSD, YT_BROWSER, YT_KEYBOARD, YT_PLAYER OSD. Preserve hitbox coordinates and screen state transitions.

- [ ] **Step 5: Run test + build**

Run: `python tests/test_no_tft_espi.py`
Expected: PASS.

Run: `platformio run -e cyd`
Expected: SUCCESS.

- [ ] **Step 6: Flash and verify color baseline**

Run: `platformio run -e cyd -t upload --upload-port COM9`
Expected boot log contains display init. Visual acceptance: VTV1 frame colors remain correct with inversion enabled.

- [ ] **Step 7: Commit**

```bash
git add src/display src/main.cpp tests/test_no_tft_espi.py
git commit -m "refactor: port MiniTV UI to Arduino_GFX"
```

---

### Task 4: Replace manual touch bit-bang with XPT2046 library

**Files:**
- Create: `src/touch/touch.h`
- Create: `src/touch/touch.cpp`
- Modify: `src/main.cpp`
- Test: `tests/test_touch_stack.py`

**Interfaces:**
- Produces:
  - `bool touchBegin()`
  - `bool touchReadRaw(int16_t &x, int16_t &y, int16_t &z)`
  - `bool touchReadScreen(int16_t &x, int16_t &y)`
  - `void touchRequestCalibration()`

- [ ] **Step 1: Write failing test**

```python
from pathlib import Path

CPP = Path('src/touch/touch.cpp').read_text(encoding='utf-8')

def test_touch_uses_xpt2046_library_and_rotation_one():
    assert 'TOUCH_CLK' in CPP and 'TOUCH_MOSI' in CPP and 'TOUCH_MISO' in CPP
    assert 'touchReadScreen' in CPP
    assert 'touchRead12' not in Path('src/main.cpp').read_text(encoding='utf-8')
```

- [ ] **Step 2: Run and verify failure**

Run: `python tests/test_touch_stack.py`
Expected: FAIL.

- [ ] **Step 3: Implement isolated software-SPI reader**

```cpp
// XPT2046 uses dedicated pins and software clocking so it cannot remap HSPI/VSPI.
// touchReadRaw() performs filtered multi-sample 12-bit ADC reads on CLK25/MISO39/MOSI32/CS33.
// touchReadScreen() applies persisted calibration and landscape orientation.
```

Do not create or remap any hardware SPI host for touch. Preserve physical touch pins and keep touch traffic isolated from TFT/SD buses.

- [ ] **Step 4: Remove bit-bang functions from `main.cpp`**

Replace all `readTouch()` calls with `touchReadScreen()`.

- [ ] **Step 5: Run test + build**

Run: `python tests/test_touch_stack.py`
Expected: PASS.

Run: `platformio run -e cyd`
Expected: SUCCESS.

- [ ] **Step 6: Commit**

```bash
git add src/touch src/main.cpp tests/test_touch_stack.py
git commit -m "feat: use XPT2046 library for CYD touch"
```

---

### Task 5: Add four-point calibration stored in NVS

**Files:**
- Modify: `src/touch/touch.h`
- Modify: `src/touch/touch.cpp`
- Modify: `src/main.cpp`
- Test: `tests/test_touch_calibration.py`

**Interfaces:**
- Produces persisted `TouchCalibration` with raw min/max and inversion/swap metadata.

- [ ] **Step 1: Write failing test**

```python
from pathlib import Path

CPP = Path('src/touch/touch.cpp').read_text(encoding='utf-8')

def test_touch_calibration_is_persisted():
    assert 'Preferences' in CPP
    assert 'touchcal' in CPP or 'calibration' in CPP
    assert 'calibrated' in CPP
```

- [ ] **Step 2: Run and verify failure**

Run: `python tests/test_touch_calibration.py`
Expected: FAIL.

- [ ] **Step 3: Implement calibration model**

```cpp
struct TouchCalibration {
    uint16_t xMin, xMax, yMin, yMax;
    bool swapXY;
    bool invertX;
    bool invertY;
    uint32_t magic;
};
```

Store under Preferences namespace `cydtouch`. Reject values outside sane XPT2046 ADC ranges or with spans <1000 counts.

- [ ] **Step 4: Implement four-crosshair flow**

Targets: `(20,20)`, `(299,20)`, `(299,219)`, `(20,219)`. Capture multiple samples at each point, use median/trimmed average, derive bounds/orientation, save to NVS, then draw a center target and require a successful hit within ±20 px before accepting.

- [ ] **Step 5: Add UART command**

`touchcal` clears calibration validity and enters calibration mode on next loop iteration.

- [ ] **Step 6: Run tests + build + hardware test**

Run: `python tests/test_touch_calibration.py`
Expected: PASS.

Run: `platformio run -e cyd`
Expected: SUCCESS.

Hardware acceptance: each corner and center touch reports within ±15 px; HOME/SD/YT hitboxes respond at their visible locations.

- [ ] **Step 7: Commit**

```bash
git add src/touch src/main.cpp tests/test_touch_calibration.py
git commit -m "feat: calibrate CYD resistive touch and persist NVS"
```
