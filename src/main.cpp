#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <JPEGDEC.h>
#include "display/display.h"
#include "display/ui_draw.h"
#include "config.h"

#include <vector>
#include <functional>

JPEGDEC jpeg;
SPIClass sdSPI(VSPI);

static constexpr uint16_t DISCOVERY_SERVER_PORT = 4210;
static constexpr uint16_t DISCOVERY_LOCAL_PORT  = 4211;
static constexpr int YT_ROWS = 3;
static constexpr int YT_ROW_H = 66;

struct VideoIndex {
  float fps = 30.0f;
  uint32_t frames = 0;
  File idxFile;
  bool valid = false;
};

enum class Screen {
  HOME,
  SD_BROWSER,
  SD_PLAYER,
  YT_BROWSER,
  YT_KEYBOARD,
  TV_BROWSER,
  YT_PLAYER
};

struct PlayerState {
  Screen screen = Screen::HOME;
  String basePath;
  File vfile;
  VideoIndex idx;
  uint32_t frame = 0;
  bool playing = false;
  bool hasAudio = false;
  uint32_t startMs = 0;
  uint32_t pausedAtMs = 0;
  bool osdVisible = true;
  uint32_t osdShownMs = 0;
  uint8_t volume = DEFAULT_VOLUME;
  uint8_t brightness = DEFAULT_BRIGHT;
  bool dirty = true;
  int browserPage = 0;
} st;

enum class UiLanguage : uint8_t { VI = 0, EN = 1 };
static UiLanguage uiLanguage = UiLanguage::VI;

static const char *tr(const char *vi, const char *en) {
  return uiLanguage == UiLanguage::VI ? vi : en;
}

static void saveLanguage() {
  Preferences prefs;
  if (!prefs.begin("cydlang", false)) return;
  prefs.putUChar("lang", uiLanguage == UiLanguage::EN ? 1 : 0);
  prefs.end();
}

static void loadLanguage() {
  Preferences prefs;
  if (!prefs.begin("cydlang", true)) { uiLanguage = UiLanguage::VI; return; }
  uiLanguage = prefs.getUChar("lang", 0) == 1 ? UiLanguage::EN : UiLanguage::VI;
  prefs.end();
}

static void setLanguage(UiLanguage lang) {
  if (uiLanguage == lang) return;
  uiLanguage = lang;
  saveLanguage();
  st.dirty = true;
}

struct YTItem {
  String id;
  String url;
  String title;
  String channel;
  uint32_t duration = 0;
};

struct TVItem {
  String id;
  String name;
};

static bool sdReady = false;
static uint8_t *frameBuf = nullptr;
static size_t frameBufSize = 0;
static std::vector<String> videoList;
static std::vector<YTItem> ytItems;
static std::vector<TVItem> tvItems;
static int ytScroll = 0;
static int tvScroll = 0;
static String ytQuery = "Tran Dang Khoa";
static String ytServer;
static String ytMessage;
static String ytPlayingTitle;
static String remoteSourceLabel = "YouTube";
static Screen remoteReturnScreen = Screen::YT_BROWSER;
static String tvMessage;
static bool ytFeedLoading = false;
static bool ytStreamActive = false;
static HTTPClient ytStreamHttp;
static WiFiClient *ytStreamClient = nullptr;
static WiFiUDP discoveryUdp;

// Persistent MJPEG parser state. Must survive short Wi-Fi gaps between loop() calls.
static bool ytParserInJpeg = false;
static int ytParserPrev = -1;
static size_t ytParserN = 0;
static uint32_t ytParserLastByteMs = 0;

static constexpr int YT_FRAME_W = 320;
static constexpr int YT_FRAME_H = 180;
static constexpr int YT_VIDEO_Y = 30;
static constexpr int YT_VIDEO_H = 180;
static constexpr int YT_OSD_TOP_H = 28;
static constexpr int YT_OSD_BOTTOM_Y = 216;
static constexpr int YT_FRAME_PARTS = 2;
static constexpr int YT_FRAME_PART_H = 90;
static constexpr size_t YT_JPEG_INITIAL_BUF = 16 * 1024;
static uint16_t *ytFramePart[YT_FRAME_PARTS] = {nullptr, nullptr};
static bool ytFrameBufferEnabled = false;
static bool ytOsdDirty = true;

// Runtime diagnostics. touchDebugUntil enables bounded raw/calibrated touch logging.
static uint32_t touchDebugUntil = 0;
static uint32_t touchLastLogMs = 0;

static constexpr uint32_t TOUCH_CAL_MAGIC = 0x43594454u; // "CYDT"
struct TouchCalibration {
  uint32_t magic = 0;
  uint16_t rawLeft = TOUCH_Y_MIN;
  uint16_t rawRight = TOUCH_Y_MAX;
  uint16_t rawTop = TOUCH_X_MAX;
  uint16_t rawBottom = TOUCH_X_MIN;
};
static TouchCalibration touchCal;
static bool touchCalibrated = false;

// JPEG byte-order diagnostic matrix:
// A=BIG/swap0, B=BIG/swap1, C=LITTLE/swap0, D=LITTLE/swap1.
// D is the default because JPEGDEC native RGB565 + legacy display stack byte swap is the
// normal path for a 16-bit SPI panel. It can be changed at runtime via UART.
static char jpegColorMode = 'D';
static int jpegPixelType = RGB565_LITTLE_ENDIAN;
static bool jpegSwapBytes = true;
static bool jpegSwapRB = true;  // software R<->B correction for decoded JPEG RGB565

// YouTube stream runtime metrics.
static uint32_t ytStatsStartMs = 0;
static uint32_t ytFramesRx = 0, ytFramesDecoded = 0, ytFramesDropped = 0;
static uint64_t ytJpegBytes = 0, ytDecodeUs = 0, ytRenderUs = 0;
static bool ytDecodeInProgress = false;

static bool ensureBuf(size_t n) {
  if (n <= frameBufSize) return true;
  uint8_t *nb = (uint8_t *)realloc(frameBuf, n);
  if (!nb) return false;
  frameBuf = nb;
  frameBufSize = n;
  return true;
}

static void setBrightness(uint8_t v) {
  st.brightness = v;
  ledcWrite(BL_CHANNEL, v);
}

static void showOsd() {
  st.osdVisible = true;
  st.osdShownMs = millis();
}

static String fmtTime(uint32_t sec) {
  char b[16];
  snprintf(b, sizeof(b), "%lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
  return String(b);
}

static size_t utf8Step(const String &s, size_t i) {
  if (i >= s.length()) return s.length();
  uint8_t c = (uint8_t)s[i];
  size_t step = 1;
  if ((c & 0xE0) == 0xC0) step = 2;
  else if ((c & 0xF0) == 0xE0) step = 3;
  else if ((c & 0xF8) == 0xF0) step = 4;
  return min(s.length(), i + step);
}

static size_t utf8Count(const String &s) {
  size_t count = 0;
  for (size_t i = 0; i < s.length(); i = utf8Step(s, i)) ++count;
  return count;
}

static size_t utf8ByteIndex(const String &s, size_t cpIndex) {
  size_t i = 0, cp = 0;
  while (i < s.length() && cp < cpIndex) { i = utf8Step(s, i); ++cp; }
  return i;
}

static String shorten(const String &s, size_t n) {
  const size_t count = utf8Count(s);
  if (count <= n) return s;
  const size_t keep = n > 3 ? n - 3 : n;
  return s.substring(0, utf8ByteIndex(s, keep)) + "...";
}

static int copyYTDecodedBlock(JPEGDRAW *p) {
  if (!ytFrameBufferEnabled || !p || !p->pPixels) return 0;
  if (p->x < 0 || p->y < 0 || p->x >= YT_FRAME_W || p->y >= YT_FRAME_H) return 1;
  const int copyW = min((int)p->iWidth, YT_FRAME_W - p->x);
  const int copyH = min((int)p->iHeight, YT_FRAME_H - p->y);
  const uint8_t *src = reinterpret_cast<const uint8_t *>(p->pPixels);

  for (int row = 0; row < copyH; ++row) {
    const int globalY = p->y + row;
    const int part = globalY / YT_FRAME_PART_H;
    const int localY = globalY % YT_FRAME_PART_H;
    if (part < 0 || part >= YT_FRAME_PARTS || !ytFramePart[part]) continue;
    uint8_t *dstBase = reinterpret_cast<uint8_t *>(ytFramePart[part]);
    uint8_t *dst = dstBase + (localY * YT_FRAME_W + p->x) * sizeof(uint16_t);
    memcpy(dst, src + row * p->iWidth * sizeof(uint16_t), copyW * sizeof(uint16_t));
  }
  return 1;
}

static void freeYTFrameBuffer() {
  for (int i = 0; i < YT_FRAME_PARTS; ++i) {
    if (ytFramePart[i]) {
      free(ytFramePart[i]);
      ytFramePart[i] = nullptr;
    }
  }
  ytFrameBufferEnabled = false;
}

static bool allocateYTFrameBuffer() {
  if (ytFrameBufferEnabled) return true;

  if (frameBuf && frameBufSize > YT_JPEG_INITIAL_BUF) {
    uint8_t *smaller = (uint8_t *)realloc(frameBuf, YT_JPEG_INITIAL_BUF);
    if (smaller) {
      frameBuf = smaller;
      frameBufSize = YT_JPEG_INITIAL_BUF;
    }
  } else if (!frameBuf) {
    ensureBuf(YT_JPEG_INITIAL_BUF);
  }

  const size_t partBytes = (size_t)YT_FRAME_W * YT_FRAME_PART_H * sizeof(uint16_t);
  Serial.printf("[YTBUF] split alloc free=%u largest=%u part=%u x%d\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap(), (unsigned)partBytes, YT_FRAME_PARTS);
  for (int i = 0; i < YT_FRAME_PARTS; ++i) {
    if (!ytFramePart[i]) ytFramePart[i] = (uint16_t *)malloc(partBytes);
    if (!ytFramePart[i]) {
      Serial.printf("[YTBUF] part %d FAILED free=%u largest=%u\n",
                    i, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      freeYTFrameBuffer();
      Serial.println("[YTBUF] framebuffer=FALLBACK-DIRECT");
      return false;
    }
    memset(ytFramePart[i], 0, partBytes);
    Serial.printf("[YTBUF] part %d OK free=%u largest=%u\n",
                  i, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
  ytFrameBufferEnabled = true;
  Serial.printf("[YTBUF] framebuffer=SPLIT-ON free=%u largest=%u\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return true;
}

static void presentYTFrame() {
  if (!ytFrameBufferEnabled || !display()) return;
  const uint32_t t0 = micros();
  for (int i = 0; i < YT_FRAME_PARTS; ++i) {
    if (!ytFramePart[i]) return;
    display()->draw16bitBeRGBBitmap(0, YT_VIDEO_Y + i * YT_FRAME_PART_H,
                                    ytFramePart[i], YT_FRAME_W, YT_FRAME_PART_H);
  }
  ytRenderUs += (uint32_t)(micros() - t0);
}

static int jpegDraw(JPEGDRAW *p) {
  if (ytDecodeInProgress && ytFrameBufferEnabled) return copyYTDecodedBlock(p);
  uint32_t t0 = ytDecodeInProgress ? micros() : 0;
  int ok = jpegDrawCallback(p);
  if (ytDecodeInProgress) ytRenderUs += (uint32_t)(micros() - t0);
  return ok;
}

static void applyJPEGColorMode(char mode) {
  (void)mode;
  jpegColorMode = 'A';
  jpegPixelType = RGB565_BIG_ENDIAN;
  jpegSwapBytes = false;
  jpegSwapRB = false;
  Serial.println("[COLOR] verified path: RGB565_BIG_ENDIAN -> Arduino_GFX BE bitmap");
}

static inline void prepareJPEGDecode() {
  jpeg.setPixelType(RGB565_BIG_ENDIAN);
}


static char runJPEGColorAutoTest() {
  applyJPEGColorMode('A');
  Serial.println("[COLORAUTO] fixed verified path: BIG_ENDIAN + Arduino_GFX + inversion");
  st.dirty = true;
  return 'A';
}

static uint16_t touchRead12(uint8_t cmd) {
  uint16_t raw = 0;
  digitalWrite(TOUCH_CS, LOW);
  for (int i = 7; i >= 0; --i) {
    digitalWrite(TOUCH_DIN, (cmd >> i) & 1);
    digitalWrite(TOUCH_CLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(TOUCH_CLK, LOW);
    delayMicroseconds(1);
  }
  // XPT2046 clocks a 16-bit word: 1 null bit + 12 ADC bits + 3 trailing bits.
  for (int i = 15; i >= 0; --i) {
    digitalWrite(TOUCH_CLK, HIGH);
    delayMicroseconds(1);
    raw |= (uint16_t)digitalRead(TOUCH_DO) << i;
    digitalWrite(TOUCH_CLK, LOW);
    delayMicroseconds(1);
  }
  digitalWrite(TOUCH_CS, HIGH);
  return (raw >> 3) & 0x0FFF;
}

static bool readTouchRaw(int &rawX, int &rawY) {
  if (digitalRead(TOUCH_IRQ) != LOW) return false;
  uint32_t sx = 0, sy = 0;
  constexpr int samples = 5;
  for (int i = 0; i < samples; ++i) {
    sy += touchRead12(0x90);
    sx += touchRead12(0xD0);
  }
  rawX = (int)(sx / samples);
  rawY = (int)(sy / samples);
  return rawX >= 50 && rawY >= 50 && rawX <= 4095 && rawY <= 4095;
}

static bool touchCalibrationSane(const TouchCalibration &c) {
  if (c.magic != TOUCH_CAL_MAGIC) return false;
  if (c.rawLeft < 40 || c.rawLeft > 4055 || c.rawRight < 40 || c.rawRight > 4055 ||
      c.rawTop < 40 || c.rawTop > 4055 || c.rawBottom < 40 || c.rawBottom > 4055) return false;
  if (abs((int)c.rawRight - (int)c.rawLeft) < 1000) return false;
  if (abs((int)c.rawBottom - (int)c.rawTop) < 1000) return false;
  return true;
}

static bool loadTouchCalibration() {
  Preferences prefs;
  if (!prefs.begin("cydtouch", true)) return false;
  TouchCalibration c;
  c.magic = prefs.getUInt("magic", 0);
  c.rawLeft = prefs.getUShort("left", 0);
  c.rawRight = prefs.getUShort("right", 0);
  c.rawTop = prefs.getUShort("top", 0);
  c.rawBottom = prefs.getUShort("bottom", 0);
  prefs.end();
  if (!touchCalibrationSane(c)) return false;
  touchCal = c;
  touchCalibrated = true;
  Serial.printf("[TOUCHCAL] loaded L=%u R=%u T=%u B=%u\n",
                touchCal.rawLeft, touchCal.rawRight, touchCal.rawTop, touchCal.rawBottom);
  return true;
}

static void saveTouchCalibration() {
  Preferences prefs;
  if (!prefs.begin("cydtouch", false)) return;
  prefs.putUInt("magic", TOUCH_CAL_MAGIC);
  prefs.putUShort("left", touchCal.rawLeft);
  prefs.putUShort("right", touchCal.rawRight);
  prefs.putUShort("top", touchCal.rawTop);
  prefs.putUShort("bottom", touchCal.rawBottom);
  prefs.end();
}

static void clearTouchCalibration() {
  Preferences prefs;
  if (prefs.begin("cydtouch", false)) {
    prefs.clear();
    prefs.end();
  }
  touchCalibrated = false;
}

static void mapTouchRawToScreen(int rawX, int rawY, int &x, int &y) {
  x = (int)map(rawY, touchCal.rawLeft, touchCal.rawRight, 20, 299);
  y = (int)map(rawX, touchCal.rawTop, touchCal.rawBottom, 20, 219);
  x = constrain(x, 0, ui().width() - 1);
  y = constrain(y, 0, ui().height() - 1);
}

static bool readTouch(int &x, int &y) {
  int rawX = 0, rawY = 0;
  if (!readTouchRaw(rawX, rawY)) return false;
  if (!touchCalibrated) {
    x = map(rawY, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, ui().width() - 1);
    y = map(rawX, TOUCH_X_MIN, TOUCH_X_MAX, ui().height() - 1, 0);
  } else {
    mapTouchRawToScreen(rawX, rawY, x, y);
  }
  x = constrain(x, 0, ui().width() - 1);
  y = constrain(y, 0, ui().height() - 1);
  if ((int32_t)(touchDebugUntil - millis()) > 0 && millis() - touchLastLogMs >= 80) {
    touchLastLogMs = millis();
    Serial.printf("[TOUCH] rawX=%d rawY=%d x=%d y=%d irq=%d cal=%d\n",
                  rawX, rawY, x, y, digitalRead(TOUCH_IRQ), touchCalibrated ? 1 : 0);
  }
  return true;
}

static void drawTouchCalibrationTarget(int x, int y, int index) {
  ui().fillScreen(TFT_BLACK);
  ui().setDatum(MC_DATUM);
  ui().setTextColor(TFT_WHITE, TFT_BLACK);
  ui().text(tr("HIỆU CHUẨN CẢM ỨNG", "TOUCH CALIBRATION"), 160, 82, 2);
  ui().setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  ui().text(String(tr("Chạm đúng dấu +  ", "Touch the +  ")) + String(index + 1) + "/4", 160, 105, 1);
  ui().fillRect(x - 14, y - 1, 29, 3, TFT_YELLOW);
  ui().fillRect(x - 1, y - 14, 3, 29, TFT_YELLOW);
  ui().drawRect(x - 6, y - 6, 13, 13, TFT_CYAN);
}

static bool captureTouchCalibrationRaw(int &rawX, int &rawY) {
  while (digitalRead(TOUCH_IRQ) == LOW) delay(10);
  while (digitalRead(TOUCH_IRQ) != LOW) delay(5);
  delay(35);
  int xs[9], ys[9];
  int n = 0;
  while (n < 9) {
    int x = 0, y = 0;
    if (readTouchRaw(x, y)) {
      xs[n] = x;
      ys[n] = y;
      ++n;
      delay(8);
    } else if (n < 5) {
      n = 0;
      while (digitalRead(TOUCH_IRQ) != LOW) delay(5);
      delay(25);
    } else {
      break;
    }
  }
  while (digitalRead(TOUCH_IRQ) == LOW) delay(10);
  if (n < 5) return false;
  for (int i = 0; i < n - 1; ++i) {
    for (int j = i + 1; j < n; ++j) {
      if (xs[j] < xs[i]) { int t = xs[i]; xs[i] = xs[j]; xs[j] = t; }
      if (ys[j] < ys[i]) { int t = ys[i]; ys[i] = ys[j]; ys[j] = t; }
    }
  }
  rawX = xs[n / 2];
  rawY = ys[n / 2];
  return true;
}

static void runTouchCalibration() {
  const int tx[4] = {20, 299, 299, 20};
  const int ty[4] = {20, 20, 219, 219};
  int rx[4], ry[4];
  while (true) {
    for (int i = 0; i < 4; ++i) {
      drawTouchCalibrationTarget(tx[i], ty[i], i);
      while (!captureTouchCalibrationRaw(rx[i], ry[i])) delay(50);
      Serial.printf("[TOUCHCAL] p%d target=%d,%d raw=%d,%d\n", i + 1, tx[i], ty[i], rx[i], ry[i]);
      delay(120);
    }
    TouchCalibration c;
    c.magic = TOUCH_CAL_MAGIC;
    c.rawLeft = (uint16_t)((ry[0] + ry[3]) / 2);
    c.rawRight = (uint16_t)((ry[1] + ry[2]) / 2);
    c.rawTop = (uint16_t)((rx[0] + rx[1]) / 2);
    c.rawBottom = (uint16_t)((rx[2] + rx[3]) / 2);
    if (!touchCalibrationSane(c)) {
      Serial.println("[TOUCHCAL] invalid spans, retrying");
      ui().fillScreen(TFT_BLACK);
      ui().setDatum(MC_DATUM);
      ui().setTextColor(TFT_RED, TFT_BLACK);
      ui().text(tr("CHẠM LẠI 4 ĐIỂM", "TOUCH 4 POINTS AGAIN"), 160, 120, 2);
      delay(900);
      continue;
    }
    touchCal = c;
    touchCalibrated = true;

    ui().fillScreen(TFT_BLACK);
    ui().setDatum(MC_DATUM);
    ui().setTextColor(TFT_WHITE, TFT_BLACK);
    ui().text(tr("KIỂM TRA TÂM MÀN HÌNH", "CHECK SCREEN CENTER"), 160, 82, 2);
    ui().fillRect(146, 119, 29, 3, TFT_YELLOW);
    ui().fillRect(159, 106, 3, 29, TFT_YELLOW);
    int crx = 0, cry = 0;
    if (!captureTouchCalibrationRaw(crx, cry)) continue;
    int cx = 0, cy = 0;
    mapTouchRawToScreen(crx, cry, cx, cy);
    Serial.printf("[TOUCHCAL] center raw=%d,%d mapped=%d,%d\n", crx, cry, cx, cy);
    if (abs(cx - 160) > 25 || abs(cy - 120) > 25) {
      Serial.println("[TOUCHCAL] center validation failed, retrying");
      touchCalibrated = false;
      continue;
    }
    saveTouchCalibration();
    Serial.printf("[TOUCHCAL] saved L=%u R=%u T=%u B=%u\n",
                  touchCal.rawLeft, touchCal.rawRight, touchCal.rawTop, touchCal.rawBottom);
    ui().fillScreen(TFT_BLACK);
    ui().setTextColor(TFT_GREEN, TFT_BLACK);
    ui().text(tr("CẢM ỨNG ĐÃ CHUẨN", "TOUCH CALIBRATED"), 160, 120, 2);
    delay(700);
    st.dirty = true;
    return;
  }
}

static void drawHeader(const String &title, bool back, const String &right = "") {
  ui().fillRect(0, 0, ui().width(), 34, ui().color565(14, 16, 20));
  ui().setDatum(ML_DATUM);
  if (back) {
    ui().setTextColor(TFT_CYAN, ui().color565(14, 16, 20));
    ui().text("<", 8, 17, 4);
  }
  ui().setTextColor(TFT_WHITE, ui().color565(14, 16, 20));
  ui().text(title, back ? 38 : 10, 17, 2);
  if (right.length()) {
    ui().setDatum(MR_DATUM);
    ui().setTextColor(TFT_YELLOW, ui().color565(14, 16, 20));
    ui().text(right, ui().width() - 8, 17, 2);
  }
  ui().drawFastHLine(0, 33, ui().width(), ui().color565(40, 44, 52));
}

// -----------------------------------------------------------------------------
// SD playback (kept from Open CYD Player)
// -----------------------------------------------------------------------------
static bool readIndexOffset(VideoIndex &ix, uint32_t frame, uint32_t &offset) {
  if (!ix.valid || !ix.idxFile || frame > ix.frames) return false;
  uint32_t pos = 12u + frame * sizeof(uint32_t);
  if (!ix.idxFile.seek(pos)) return false;
  return ix.idxFile.read((uint8_t *)&offset, sizeof(offset)) == sizeof(offset);
}

static bool loadIndex(const String &base, VideoIndex &ix) {
  ix.valid = false;
  ix.frames = 0;
  if (ix.idxFile) ix.idxFile.close();

  ix.idxFile = SD.open(base + ".idx", FILE_READ);
  if (!ix.idxFile) {
    Serial.printf("[SD][IDX] open failed: %s.idx\n", base.c_str());
    return false;
  }

  char magic[4];
  uint32_t fpsMilli = 0;
  uint32_t frames = 0;
  if (ix.idxFile.read((uint8_t *)magic, 4) != 4 || memcmp(magic, "CYD1", 4) != 0 ||
      ix.idxFile.read((uint8_t *)&fpsMilli, 4) != 4 ||
      ix.idxFile.read((uint8_t *)&frames, 4) != 4) {
    Serial.println("[SD][IDX] invalid/truncated header");
    ix.idxFile.close();
    return false;
  }
  if (fpsMilli < 1000 || fpsMilli > 60000 || frames == 0 || frames > 2000000) {
    Serial.printf("[SD][IDX] invalid header fpsMilli=%lu frames=%lu\n",
                  (unsigned long)fpsMilli, (unsigned long)frames);
    ix.idxFile.close();
    return false;
  }

  uint64_t expectedSize = 12ULL + ((uint64_t)frames + 1ULL) * sizeof(uint32_t);
  if ((uint64_t)ix.idxFile.size() != expectedSize) {
    Serial.printf("[SD][IDX] size mismatch got=%lu expected=%llu\n",
                  (unsigned long)ix.idxFile.size(), (unsigned long long)expectedSize);
    ix.idxFile.close();
    return false;
  }

  ix.fps = fpsMilli / 1000.0f;
  ix.frames = frames;
  ix.valid = true;
  Serial.printf("[SD][IDX] streaming index enabled frames=%lu fps=%.2f idxBytes=%lu heap=%u largest=%u\n",
                (unsigned long)ix.frames, ix.fps, (unsigned long)ix.idxFile.size(),
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return true;
}

static bool validateIndexAgainstMedia(VideoIndex &ix, uint32_t fileSize) {
  if (!ix.valid || !ix.idxFile || ix.frames == 0) return false;
  if (!ix.idxFile.seek(12)) return false;

  uint32_t prev = 0;
  if (ix.idxFile.read((uint8_t *)&prev, sizeof(prev)) != sizeof(prev) || prev != 0) {
    Serial.printf("[SD][IDX] first offset invalid=%lu\n", (unsigned long)prev);
    return false;
  }

  for (uint32_t i = 1; i <= ix.frames; ++i) {
    uint32_t cur = 0;
    if (ix.idxFile.read((uint8_t *)&cur, sizeof(cur)) != sizeof(cur)) {
      Serial.printf("[SD][IDX] truncated at offset #%lu\n", (unsigned long)i);
      return false;
    }
    if (cur <= prev || cur > fileSize) {
      Serial.printf("[SD][IDX] invalid offset i=%lu prev=%lu cur=%lu media=%lu\n",
                    (unsigned long)i, (unsigned long)prev, (unsigned long)cur, (unsigned long)fileSize);
      return false;
    }
    prev = cur;
  }

  if (prev != fileSize) {
    Serial.printf("[SD][IDX] sentinel invalid=%lu media=%lu\n",
                  (unsigned long)prev, (unsigned long)fileSize);
    return false;
  }
  return true;
}

static void invalidateIndex(VideoIndex &ix) {
  if (ix.idxFile) ix.idxFile.close();
  ix.valid = false;
  ix.frames = 0;
}

static void syncClockToFrame() {
  st.startMs = millis() - (uint32_t)(st.frame * 1000.0f / st.idx.fps);
}

static void seekToFrame(uint32_t frame) {
  if (!st.idx.valid) return;
  if (frame >= st.idx.frames) frame = st.idx.frames - 1;
  st.frame = frame;
  uint32_t off = 0;
  if (!readIndexOffset(st.idx, frame, off)) return;
  st.vfile.seek(off);
  syncClockToFrame();
}

static void setPlaying(bool p) {
  if (p == st.playing) return;
  st.playing = p;
  if (p) {
    st.startMs += millis() - st.pausedAtMs;
  } else {
    st.pausedAtMs = millis();
  }
  showOsd();
}

static void stopSDPlayback() {
  if (st.vfile) st.vfile.close();
  invalidateIndex(st.idx);
  st.hasAudio = false;
  st.playing = false;
  st.screen = Screen::SD_BROWSER;
  st.dirty = true;
}

static bool startSDPlayback(const String &base) {
  Serial.printf("[SD][PLAY] base=%s\n", base.c_str());
  if (st.vfile) st.vfile.close();
  invalidateIndex(st.idx);
  st.vfile = SD.open(base + ".mjpeg", FILE_READ);
  if (!st.vfile) { Serial.printf("[SD][PLAY] open failed: %s.mjpeg\n", base.c_str()); return false; }
  bool idxLoaded = loadIndex(base, st.idx);
  if (idxLoaded && !validateIndexAgainstMedia(st.idx, (uint32_t)st.vfile.size())) {
    Serial.printf("[SD][PLAY] idx structure invalid: %s.idx; fallback scan mode\n", base.c_str());
    invalidateIndex(st.idx);
    idxLoaded = false;
  }
  if (!idxLoaded) {
    Serial.printf("[SD][PLAY] idx missing/invalid: %s.idx; fallback scan mode\n", base.c_str());
    st.idx.fps = 30.0f;
    st.idx.frames = 0;
    st.idx.valid = false;
  }
  st.hasAudio = false;  // Wi-Fi-first build: audio disabled for now
  st.basePath = base;
  st.frame = 0;
  st.playing = true;
  st.startMs = millis();
  st.screen = Screen::SD_PLAYER;
  ui().fillScreen(TFT_BLACK);
  showOsd();
  Serial.printf("[SD][PLAY] started size=%u idx=%d fps=%.2f frames=%u\n",
                (unsigned)st.vfile.size(), st.idx.valid ? 1 : 0, st.idx.fps, (unsigned)st.idx.frames);
  return true;
}

static int readNextSDFrame() {
  if (st.idx.valid) {
    if (st.frame >= st.idx.frames) return 0;
    uint32_t off = 0, next = 0;
    if (!readIndexOffset(st.idx, st.frame, off) ||
        !readIndexOffset(st.idx, st.frame + 1, next) || next <= off) {
      Serial.printf("[SD][IDX] read failed frame=%lu\n", (unsigned long)st.frame);
      return -1;
    }
    uint32_t len = next - off;
    if (!ensureBuf(len)) return -1;
    if (!st.vfile.seek(off)) return -1;
    if (st.vfile.read(frameBuf, len) != (int)len) return -1;
    return (int)len;
  }
  if (!ensureBuf(64 * 1024)) return -1;
  int b, prev = -1;
  while ((b = st.vfile.read()) >= 0) {
    if (prev == 0xFF && b == 0xD8) break;
    prev = b;
  }
  if (b < 0) return 0;
  size_t n = 0;
  frameBuf[n++] = 0xFF; frameBuf[n++] = 0xD8;
  prev = -1;
  while ((b = st.vfile.read()) >= 0) {
    if (n >= frameBufSize && !ensureBuf(frameBufSize + 32 * 1024)) return -1;
    frameBuf[n++] = (uint8_t)b;
    if (prev == 0xFF && b == 0xD9) return (int)n;
    prev = b;
  }
  return 0;
}

static void drawSDOsd() {
  const int W = ui().width(), H = ui().height();
  ui().fillRect(0, 0, W, 22, TFT_BLACK);
  ui().setTextColor(TFT_WHITE, TFT_BLACK);
  ui().setDatum(TL_DATUM);
  String name = st.basePath.substring(st.basePath.lastIndexOf('/') + 1);
  ui().text(shorten(name, 22), 4, 4, 2);
  uint32_t curSec = (uint32_t)(st.frame / st.idx.fps);
  uint32_t totSec = st.idx.valid ? (uint32_t)(st.idx.frames / st.idx.fps) : 0;
  ui().setDatum(TR_DATUM);
  ui().text(fmtTime(curSec) + " / " + fmtTime(totSec), W - 4, 4, 2);
  int barY = H - 46;
  ui().fillRect(0, barY, W, 46, TFT_BLACK);
  ui().drawRect(8, barY + 4, W - 16, 8, TFT_DARKGREY);
  if (st.idx.valid && st.idx.frames) {
    int fill = (int)((uint64_t)(W - 18) * st.frame / st.idx.frames);
    ui().fillRect(9, barY + 5, fill, 6, TFT_YELLOW);
  }
  ui().setDatum(MC_DATUM);
  ui().setTextColor(TFT_WHITE, TFT_BLACK);
  ui().text(tr("VỀ", "BACK"), 34, barY + 30, 2);
  ui().text("<< 1m", W / 2 - 70, barY + 30, 2);
  ui().text(st.playing ? "| |" : ">", W / 2, barY + 30, 4);
  ui().text("1m >>", W / 2 + 70, barY + 30, 2);
  ui().text(String(tr("ÂM ", "VOL ")) + String(st.volume), W - 34, barY + 30, 2);
}

static void handleSDPlayerTouch() {
  static bool wasDown = false;
  static int downX = 0, downY = 0;
  static uint32_t downMs = 0;
  static bool dragging = false;
  int x, y;
  bool down = readTouch(x, y);
  const int W = ui().width(), H = ui().height();
  if (down && !wasDown) { downX = x; downY = y; downMs = millis(); dragging = false; }
  if (down && wasDown) {
    int dy = downY - y;
    if (!dragging && abs(dy) > 18 && (downX < 48 || downX > W - 48)) dragging = true;
    if (dragging) {
      if (downX < 48) {
        int nb = constrain(st.brightness + dy / 2, 96, 255);
        setBrightness(nb);
      } else {
        int nv = constrain(st.volume + dy / 24, 0, 21);
        if (nv != st.volume) st.volume = nv;
      }
      downY = y;
      showOsd();
    }
  }
  if (!down && wasDown && !dragging) {
    uint32_t heldMs = millis() - downMs;
    if (heldMs < 600) {
      int barY = H - 46;
      if (downX < 72 && downY < 46) {
        Serial.println("[TOUCH] SD BACK top-left");
        stopSDPlayback();
      } else if (!st.osdVisible) showOsd();
      else if (downY >= barY) {
        if (downY < barY + 16 && st.idx.valid) {
          uint32_t f = (uint64_t)st.idx.frames * constrain(downX - 8, 0, W - 16) / (W - 16);
          seekToFrame(f);
        } else if (downX < 68) stopSDPlayback();
        else if (downX < W / 2 - 35) {
          long f = (long)st.frame - (long)(SEEK_STEP_SEC * st.idx.fps);
          seekToFrame(f < 0 ? 0 : (uint32_t)f);
        } else if (downX < W / 2 + 35) setPlaying(!st.playing);
        else if (downX < W - 68) seekToFrame(st.frame + (uint32_t)(SEEK_STEP_SEC * st.idx.fps));
        showOsd();
      } else if (downY > 22) setPlaying(!st.playing);
    }
  }
  wasDown = down;
}

static void scanVideos() {
  videoList.clear();
  if (!sdReady) return;
  File dir = SD.open(VIDEO_DIR);
  if (!dir) return;
  File f;
  while ((f = dir.openNextFile())) {
    String n = f.name();
    f.close();
    if (n.endsWith(".mjpeg")) {
      String base = n.substring(0, n.length() - 6);
      if (base.startsWith("/")) videoList.push_back(base);
      else videoList.push_back(String(VIDEO_DIR) + "/" + base);
    }
  }
  dir.close();
  Serial.printf("[SD] scan /videos => %u item(s)\n", (unsigned)videoList.size());
  for (size_t i = 0; i < videoList.size(); ++i) Serial.printf("[SD] #%u %s\n", (unsigned)i, videoList[i].c_str());
}

static const int ROWS_PER_PAGE = 6;

static void drawSDBrowser() {
  ui().fillScreen(TFT_BLACK);
  drawHeader(tr("VIDEO THẺ SD", "SD VIDEO"), true, sdReady ? tr("SD OK", "SD OK") : tr("KHÔNG SD", "NO SD"));
  if (!sdReady) {
    ui().setDatum(MC_DATUM);
    ui().setTextColor(TFT_RED, TFT_BLACK);
    ui().text(tr("Không có thẻ SD", "No SD card"), 160, 105, 4);
    ui().setTextColor(TFT_DARKGREY, TFT_BLACK);
    ui().text(tr("YouTube TV vẫn dùng được", "YouTube TV is still available"), 160, 140, 2);
    st.dirty = false;
    return;
  }
  ui().setTextColor(TFT_WHITE, TFT_BLACK);
  ui().setDatum(TL_DATUM);
  int start = st.browserPage * ROWS_PER_PAGE;
  for (int i = 0; i < ROWS_PER_PAGE; i++) {
    int gi = start + i;
    if (gi >= (int)videoList.size()) break;
    int y = 42 + i * 28;
    ui().fillRounded(6, y, ui().width() - 12, 24, 4, ui().color565(24, 24, 24));
    String name = videoList[gi].substring(videoList[gi].lastIndexOf('/') + 1);
    ui().text(shorten(name, 34), 14, y + 4, 2);
  }
  if (videoList.empty()) {
    ui().setDatum(MC_DATUM);
    ui().setTextColor(TFT_YELLOW, TFT_BLACK);
    ui().text(tr("Chưa có video", "No videos yet"), 160, 110, 4);
    ui().setTextColor(TFT_DARKGREY, TFT_BLACK);
    ui().text(tr("Chép .mjpeg/.mp3/.idx vào /videos", "Copy .mjpeg/.mp3/.idx to /videos"), 160, 145, 2);
  }
  int pages = (videoList.size() + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
  if (pages > 1) {
    ui().setDatum(MC_DATUM);
    ui().setTextColor(TFT_CYAN, TFT_BLACK);
    ui().text("<", 20, 228, 4);
    ui().text(">", 300, 228, 4);
    ui().text(String(st.browserPage + 1) + "/" + String(pages), 160, 228, 2);
  }
  st.dirty = false;
}

static void handleSDBrowserTouch() {
  static bool wasDown = false;
  static int lastX = 0, lastY = 0;
  int x = lastX, y = lastY;
  bool down = readTouch(x, y);
  if (down) { lastX = x; lastY = y; }
  if (!down && wasDown) {
    x = lastX; y = lastY;
    Serial.printf("[TOUCH][SD_BROWSER] x=%d y=%d page=%d items=%u\n", x, y, st.browserPage, (unsigned)videoList.size());
    if (y < 34 && x < 60) {
      st.screen = Screen::HOME; st.dirty = true;
    } else if (sdReady) {
      int pages = (videoList.size() + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
      if (y > 214 && pages > 1) {
        if (x < 60 && st.browserPage > 0) { st.browserPage--; st.dirty = true; }
        else if (x > 260 && st.browserPage < pages - 1) { st.browserPage++; st.dirty = true; }
      } else {
        int row = (y - 42) / 28;
        int gi = st.browserPage * ROWS_PER_PAGE + row;
        if (row >= 0 && row < ROWS_PER_PAGE && gi < (int)videoList.size()) {
          if (!startSDPlayback(videoList[gi])) st.dirty = true;
        }
      }
    }
  }
  wasDown = down;
}

// -----------------------------------------------------------------------------
// Wi-Fi + server discovery
// -----------------------------------------------------------------------------
static void drawStatus(const String &line1, const String &line2 = "") {
  ui().fillScreen(TFT_BLACK);
  ui().setDatum(MC_DATUM);
  ui().setTextColor(TFT_CYAN, TFT_BLACK);
  ui().text(line1, 160, 103, 4);
  if (line2.length()) {
    ui().setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    ui().text(line2, 160, 136, 2);
  }
}

static void logNetworkState(const char *tag) {
  Serial.printf("[NET][%s] status=%d ssid='%s' rssi=%d ip=%s mask=%s gw=%s dns=%s\n",
                tag,
                (int)WiFi.status(),
                WiFi.SSID().c_str(),
                WiFi.RSSI(),
                WiFi.localIP().toString().c_str(),
                WiFi.subnetMask().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(),
                WiFi.dnsIP().toString().c_str());
}

static int scanWiFiAndShow(bool showOnScreen) {
  Serial.println("[WIFI] scan start");
  int n = WiFi.scanNetworks(false, true);
  Serial.printf("[WIFI] scan done: %d networks\n", n);
  for (int i = 0; i < n; ++i) {
    Serial.printf("[WIFI] #%02d ssid='%s' rssi=%d ch=%d enc=%d\n",
                  i, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i), (int)WiFi.encryptionType(i));
  }
  if (showOnScreen) {
    ui().fillScreen(TFT_BLACK);
    ui().setDatum(TL_DATUM);
    ui().setTextColor(TFT_CYAN, TFT_BLACK);
    ui().text(tr("CHỌN WIFI TRÊN ĐIỆN THOẠI", "SELECT WIFI ON PHONE"), 8, 8, 2);
    ui().setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    ui().text(tr("Kết nối AP: CYD-MiniTV-Setup", "Connect AP: CYD-MiniTV-Setup"), 8, 28, 2);
    ui().text(tr("Các WiFi gần đây:", "Nearby WiFi networks:"), 8, 50, 2);
    int rows = min(n, 6);
    for (int i = 0; i < rows; ++i) {
      uint16_t c = WiFi.RSSI(i) > -67 ? TFT_GREEN : (WiFi.RSSI(i) > -78 ? TFT_YELLOW : TFT_DARKGREY);
      ui().setTextColor(c, TFT_BLACK);
      String line = String(i + 1) + ". " + WiFi.SSID(i);
      if (line.length() > 32) line = line.substring(0, 31) + "~";
      ui().text(line, 12, 74 + i * 22, 2);
    }
    if (n <= 0) {
      ui().setTextColor(TFT_RED, TFT_BLACK);
      ui().text(tr("Không quét thấy SSID", "No SSID found"), 12, 80, 2);
    }
    ui().setTextColor(TFT_WHITE, TFT_BLACK);
    ui().text(tr("Portal sẽ hiện danh sách SSID để chọn", "Portal will show SSIDs to choose"), 8, 218, 1);
  }
  return n;
}

static bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    logNetworkState("already-connected");
    WiFi.setSleep(false);
    return true;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(120);
  scanWiFiAndShow(true);

  Serial.println("[WIFI] starting WiFiManager portal: CYD-MiniTV-Setup");
  WiFiManager wm;
  wm.setDebugOutput(true);
  wm.setConnectTimeout(15);
  wm.setConfigPortalTimeout(180);
  wm.setMinimumSignalQuality(1);
  wm.setRemoveDuplicateAPs(true);
  wm.setScanDispPerc(true);
  const char *menu[] = {"wifi", "info", "exit"};
  wm.setMenu(menu, 3);
  bool ok = wm.autoConnect("CYD-MiniTV-Setup");
  Serial.printf("[WIFI] WiFiManager returned=%d status=%d\n", ok ? 1 : 0, (int)WiFi.status());
  if (!ok || WiFi.status() != WL_CONNECTED) {
    ytMessage = "WiFi chua ket noi";
    logNetworkState("connect-failed");
    return false;
  }
  WiFi.setSleep(false);
  logNetworkState("connected");
  return true;
}

static IPAddress subnetBroadcast() {
  IPAddress ip = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  IPAddress out;
  for (int i = 0; i < 4; ++i) out[i] = (uint8_t)((ip[i] & mask[i]) | ((~mask[i]) & 0xFF));
  return out;
}

static void saveServerCache(IPAddress ip, uint16_t port) {
  Preferences pref;
  if (!pref.begin("cydtv", false)) return;
  pref.putString("server_ip", ip.toString());
  pref.putUShort("server_port", port);
  pref.end();
}

static bool loadServerCache(IPAddress &ip, uint16_t &port) {
  Preferences pref;
  if (!pref.begin("cydtv", true)) return false;
  String host = pref.getString("server_ip", "");
  port = pref.getUShort("server_port", 8765);
  pref.end();
  if (!host.length() || !ip.fromString(host)) return false;
  Serial.printf("[SERVER] cached NVS %s:%u\n", host.c_str(), port);
  return true;
}

static bool validateServer(IPAddress ip, uint16_t port, uint32_t connectTimeoutMs = 80) {
  WiFiClient c;
  c.setTimeout(300);
  uint32_t t0 = millis();
  if (!c.connect(ip, port, (int32_t)connectTimeoutMs)) return false;
  Serial.printf("[SERVER] TCP open %s:%u in %lums\n", ip.toString().c_str(), port, (unsigned long)(millis() - t0));
  c.printf("GET /status HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", ip.toString().c_str());
  uint32_t until = millis() + 700;
  String resp;
  while ((int32_t)(millis() - until) < 0 && c.connected()) {
    while (c.available()) {
      char ch = (char)c.read();
      if (resp.length() < 900) resp += ch;
    }
    if (resp.indexOf("\r\n\r\n") >= 0 && (resp.indexOf("\"running\"") >= 0 || resp.indexOf("\"selected\"") >= 0)) break;
    delay(2);
  }
  c.stop();
  bool ok = resp.startsWith("HTTP/1.0 200") || resp.startsWith("HTTP/1.1 200");
  ok = ok && (resp.indexOf("\"running\"") >= 0 || resp.indexOf("\"selected\"") >= 0);
  Serial.printf("[SERVER] validate %s:%u => %s bytes=%u\n", ip.toString().c_str(), port, ok ? "OK" : "NOT-CYD-TV", (unsigned)resp.length());
  if (ok) {
    ytServer = "http://" + ip.toString() + ":" + String(port);
    saveServerCache(ip, port);
  }
  return ok;
}

static void sendDiscovery(IPAddress dst) {
  bool begun = discoveryUdp.beginPacket(dst, DISCOVERY_SERVER_PORT);
  if (!begun) {
    Serial.printf("[DISCOVERY] beginPacket failed dst=%s\n", dst.toString().c_str());
    return;
  }
  discoveryUdp.write((const uint8_t *)"CYD_TV_DISCOVER", 15);
  bool ok = discoveryUdp.endPacket();
  Serial.printf("[DISCOVERY] TX dst=%s:%u result=%d\n", dst.toString().c_str(), DISCOVERY_SERVER_PORT, ok ? 1 : 0);
}

static bool waitDiscoveryReply(uint32_t waitMs) {
  uint32_t until = millis() + waitMs;
  while ((int32_t)(millis() - until) < 0) {
    int n = discoveryUdp.parsePacket();
    if (n > 0) {
      IPAddress remote = discoveryUdp.remoteIP();
      uint16_t remotePort = discoveryUdp.remotePort();
      String reply;
      while (discoveryUdp.available()) reply += (char)discoveryUdp.read();
      Serial.printf("[DISCOVERY] RX from %s:%u n=%d payload='%s'\n",
                    remote.toString().c_str(), remotePort, n, reply.c_str());
      if (reply.startsWith("CYD_TV_SERVER|")) {
        int port = reply.substring(14).toInt();
        if (port <= 0) port = 8765;
        if (validateServer(remote, (uint16_t)port, 150)) return true;
      }
    }
    delay(5);
  }
  return false;
}

static bool scanSubnetForServer() {
  IPAddress local = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  if (!(mask[0] == 255 && mask[1] == 255 && mask[2] == 255)) {
    Serial.printf("[SERVER] fallback /24 scan despite mask=%s\n", mask.toString().c_str());
  }
  drawStatus(tr("QUÉT MẠNG LAN", "SCAN LAN"), tr("UDP không thấy - đang quét TCP...", "UDP failed - scanning TCP..."));
  Serial.printf("[SERVER] TCP fallback scan subnet %u.%u.%u.0/24 port 8765\n", local[0], local[1], local[2]);

  IPAddress gw = WiFi.gatewayIP();
  if (gw != local && validateServer(gw, 8765, 50)) return true;

  // Scan the same /24. A short timeout keeps worst case to a few seconds.
  for (int host = 1; host <= 254; ++host) {
    if (host == local[3] || host == gw[3]) continue;
    IPAddress ip(local[0], local[1], local[2], host);
    if ((host & 15) == 0) {
      Serial.printf("[SERVER] scan progress ...%d\n", host);
      ui().fillRect(20, 160, 280, 20, TFT_BLACK);
      ui().setDatum(MC_DATUM);
      ui().setTextColor(TFT_DARKGREY, TFT_BLACK);
      ui().text(String("IP ") + ip.toString(), 160, 170, 2);
    }
    if (validateServer(ip, 8876, 28)) return true;
    delay(1);
  }
  return false;
}

static bool discoverYTServer() {
  if (ytServer.length()) {
    Serial.printf("[SERVER] cached server=%s\n", ytServer.c_str());
    return true;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[SERVER] discovery aborted: WiFi not connected");
    return false;
  }

  logNetworkState("discovery-start");
  drawStatus(tr("TÌM MÁY CHỦ", "FIND SERVER"), tr("Phát UDP + dự phòng TCP", "UDP broadcast + TCP fallback"));
  discoveryUdp.stop();
  if (!discoveryUdp.begin(DISCOVERY_LOCAL_PORT)) {
    Serial.printf("[DISCOVERY] UDP begin failed local port=%u\n", DISCOVERY_LOCAL_PORT);
  } else {
    IPAddress directed = subnetBroadcast();
    Serial.printf("[DISCOVERY] localPort=%u directedBroadcast=%s\n", DISCOVERY_LOCAL_PORT, directed.toString().c_str());
    for (int attempt = 1; attempt <= 3; ++attempt) {
      Serial.printf("[DISCOVERY] attempt=%d\n", attempt);
      sendDiscovery(directed);
      if (directed != IPAddress(255,255,255,255)) sendDiscovery(IPAddress(255,255,255,255));
      if (waitDiscoveryReply(900)) {
        discoveryUdp.stop();
        Serial.printf("[SERVER] found by UDP: %s\n", ytServer.c_str());
        return true;
      }
    }
    discoveryUdp.stop();
  }

  Serial.println("[SERVER] UDP discovery failed, trying last-known NVS cache");
  IPAddress cachedIp;
  uint16_t cachedPort = 8876;
  if (loadServerCache(cachedIp, cachedPort) && validateServer(cachedIp, cachedPort, 120)) {
    Serial.printf("[SERVER] found by NVS cache: %s\n", ytServer.c_str());
    return true;
  }

  Serial.println("[SERVER] cache failed, trying TCP subnet scan");
  if (scanSubnetForServer()) {
    Serial.printf("[SERVER] found by TCP scan: %s\n", ytServer.c_str());
    return true;
  }

  ytMessage = tr("Không tìm thấy máy chủ", "Server not found");
  Serial.println("[SERVER] FAIL: no CYD TV server found on UDP or TCP scan");
  return false;
}

static bool ensureYTReady() {
  if (!ensureWiFi()) return false;
  if (!discoverYTServer()) return false;
  return true;
}

static bool fetchYTList(const String &query, bool searchMode);
static bool startYTDebugStream();

static void rawSDProbe() {
  Serial.printf("[SDPROBE] pins CS=%d SCK=%d MISO=%d MOSI=%d idleMISO=%d\n",
                SD_CS, SD_SCK, SD_MISO, SD_MOSI, digitalRead(SD_MISO));
  SD.end();
  delay(30);
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  sdSPI.beginTransaction(SPISettings(250000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 12; ++i) sdSPI.transfer(0xFF); // >= 80 clocks with CS high
  digitalWrite(SD_CS, LOW);
  sdSPI.transfer(0xFF);
  const uint8_t cmd0[6] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
  for (uint8_t b : cmd0) sdSPI.transfer(b);
  Serial.print("[SDPROBE] CMD0 response bytes:");
  uint8_t first = 0xFF;
  for (int i = 0; i < 24; ++i) {
    uint8_t r = sdSPI.transfer(0xFF);
    if (first == 0xFF && r != 0xFF) first = r;
    Serial.printf(" %02X", r);
  }
  Serial.println();
  digitalWrite(SD_CS, HIGH);
  sdSPI.transfer(0xFF);
  sdSPI.endTransaction();
  Serial.printf("[SDPROBE] firstNonFF=0x%02X (%s) finalMISO=%d\n",
                first, first == 0x01 ? "CARD ENTERED IDLE" : "NO VALID CMD0 RESPONSE", digitalRead(SD_MISO));
}

static void logTFTDiagnostics() {
  Serial.printf("[TFT] Arduino_GFX ILI9341 size=%dx%d pins MOSI=13 MISO=12 SCLK=14 CS=15 DC=2 BL=21 spi=40MHz inversion=ON\n",
                ui().width(), ui().height());
}

static void runTFTColorTest() {
  struct P { uint16_t c; const char *name; uint16_t text; };
  const P p[] = {
    {TFT_RED, "RED", TFT_WHITE}, {TFT_GREEN, "GREEN", TFT_BLACK},
    {TFT_BLUE, "BLUE", TFT_WHITE}, {TFT_WHITE, "WHITE", TFT_BLACK},
    {TFT_BLACK, "BLACK", TFT_WHITE}
  };
  Serial.println("[TFTTEST] begin RED/GREEN/BLUE/WHITE/BLACK");
  for (const auto &v : p) {
    ui().fillScreen(v.c);
    ui().setDatum(MC_DATUM);
    ui().setTextColor(v.text, v.c);
    ui().text(v.name, ui().width()/2, ui().height()/2, 4);
    delay(700);
    Serial.printf("[TFTTEST] %s expected=0x%04X\n", v.name, v.c);
  }
  st.dirty = true;
  Serial.println("[TFTTEST] end");
}

static void handleSerialDebug() {
  static String cmd;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (cmd.length() < 64) cmd += c;
      continue;
    }
    cmd.trim();
    if (!cmd.length()) continue;
    Serial.printf("[UART] cmd='%s'\n", cmd.c_str());
    if (cmd == "net") {
      logNetworkState("uart");
      Serial.printf("[UART] ytServer='%s'\n", ytServer.c_str());
    } else if (cmd == "scan") {
      scanWiFiAndShow(false);
    } else if (cmd == "discover") {
      ytServer = "";
      bool ok = discoverYTServer();
      Serial.printf("[UART] discover result=%d server='%s'\n", ok ? 1 : 0, ytServer.c_str());
      st.dirty = true;
    } else if (cmd == "ready") {
      ytServer = "";
      bool ok = ensureYTReady();
      Serial.printf("[UART] ready result=%d server='%s'\n", ok ? 1 : 0, ytServer.c_str());
      if (ok) {
        bool feedOk = fetchYTList(ytQuery, false);
        Serial.printf("[UART] feed result=%d items=%u\n", feedOk ? 1 : 0, (unsigned)ytItems.size());
      }
      st.dirty = true;
    } else if (cmd == "ytopen") {
      bool ok = startYTDebugStream();
      Serial.printf("[UART] ytopen result=%d server='%s'\n", ok ? 1 : 0, ytServer.c_str());
    } else if (cmd == "sdprobe") {
      rawSDProbe();
    } else if (cmd == "sd") {
      Serial.println("[UART] SD re-init test");
      SD.end();
      delay(50);
      sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
      sdReady = SD.begin(SD_CS, sdSPI, 1000000);
      Serial.printf("[SD] ready=%d type=%u size=%.1fMB used=%.1fMB\n",
                    sdReady ? 1 : 0,
                    sdReady ? (unsigned)SD.cardType() : 0,
                    sdReady ? SD.cardSize()/1048576.0 : 0.0,
                    sdReady ? SD.usedBytes()/1048576.0 : 0.0);
      if (sdReady) scanVideos();
      st.dirty = true;
    } else if (cmd == "tftinfo") {
      logTFTDiagnostics();
    } else if (cmd == "tfttest") {
      runTFTColorTest();
    } else if (cmd == "touch") {
      touchDebugUntil = millis() + 10000;
      Serial.println("[TOUCH] raw + mapped logging enabled for 10 seconds");
    } else if (cmd == "touchcal") {
      Serial.println("[TOUCHCAL] forced recalibration");
      clearTouchCalibration();
      runTouchCalibration();
      st.dirty = true;
    } else if (cmd == "heap") {
      Serial.printf("[HEAP] free=%u largest=%u min=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
    } else if (cmd == "fps") {
      uint32_t ms = ytStatsStartMs ? millis() - ytStatsStartMs : 0;
      float fps = ms ? (ytFramesDecoded * 1000.0f / ms) : 0.0f;
      float avgKb = ytFramesRx ? (ytJpegBytes / 1024.0f / ytFramesRx) : 0.0f;
      float decMs = ytFramesDecoded ? (ytDecodeUs / 1000.0f / ytFramesDecoded) : 0.0f;
      float renMs = ytFramesDecoded ? (ytRenderUs / 1000.0f / ytFramesDecoded) : 0.0f;
      Serial.printf("[FPS] elapsed=%lums rx=%lu decoded=%lu dropped=%lu fps=%.2f avgJPEG=%.2fKB decode=%.2fms render=%.2fms\n",
                    (unsigned long)ms, (unsigned long)ytFramesRx, (unsigned long)ytFramesDecoded,
                    (unsigned long)ytFramesDropped, fps, avgKb, decMs, renMs);
    } else if (cmd == "rbswap on") {
      jpegSwapRB = true;
      Serial.println("[COLOR] software RB swap=ON");
    } else if (cmd == "rbswap off") {
      jpegSwapRB = false;
      Serial.println("[COLOR] software RB swap=OFF");
    } else if (cmd == "colorauto") {
      runJPEGColorAutoTest();
      st.dirty = true;
    } else if (cmd.startsWith("color ") && cmd.length() >= 7) {
      applyJPEGColorMode(cmd[6]);
      st.dirty = true;
    } else if (cmd == "playtest") {
      if (!sdReady) Serial.println("[PLAYTEST] SD not ready");
      else if (!startSDPlayback(String(VIDEO_DIR) + "/COLOR_TEST")) Serial.println("[PLAYTEST] COLOR_TEST failed");
    } else if (cmd == "playreal") {
      if (!sdReady) Serial.println("[PLAYTEST] SD not ready");
      else if (!startSDPlayback(String(VIDEO_DIR) + "/REAL_TEST")) Serial.println("[PLAYTEST] REAL_TEST failed");
    } else if (cmd == "clearwifi") {
      Serial.println("[UART] clearing WiFi credentials and restarting");
      WiFiManager wm;
      wm.resetSettings();
      delay(200);
      ESP.restart();
    } else if (cmd == "help") {
      Serial.println("[UART] commands: help | net | scan | sdprobe | sd | ready | discover | tftinfo | tfttest | touch | heap | fps | rbswap on/off | colorauto | color A/B/C/D | playtest | playreal | clearwifi");
    } else {
      Serial.println("[UART] unknown command; type help");
    }
    cmd = "";
  }
}

static String urlEncode(const String &s) {
  String out;
  char hex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); ++i) {
    uint8_t c = (uint8_t)s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') out += (char)c;
    else if (c == ' ') out += "%20";
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 0xF]; }
  }
  return out;
}

// -----------------------------------------------------------------------------
// YouTube feed/search/browser
// -----------------------------------------------------------------------------
static bool fetchYTList(const String &query, bool searchMode) {
  if (!ensureYTReady()) return false;
  ytFeedLoading = true;
  drawStatus(searchMode ? tr("TÌM KIẾM", "SEARCH") : "YOUTUBE", tr("Đang tải danh sách...", "Loading list..."));
  HTTPClient http;
  String url = ytServer + (searchMode ? "/api/search?q=" : "/api/feed?q=") + urlEncode(query);
  http.setTimeout(20000);
  if (!http.begin(url)) { ytFeedLoading = false; return false; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    ytMessage = "Server HTTP " + String(code);
    http.end(); ytFeedLoading = false; return false;
  }
  String payload = http.getString();
  http.end();
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err || !doc["ok"].as<bool>()) {
    ytMessage = err ? tr("Lỗi JSON", "JSON error") : String((const char *)(doc["error"] | tr("Lỗi tìm kiếm", "Search failed")));
    ytFeedLoading = false; return false;
  }
  ytItems.clear();
  for (JsonObject obj : doc["items"].as<JsonArray>()) {
    YTItem v;
    v.id = String((const char *)(obj["id"] | ""));
    v.url = String((const char *)(obj["url"] | ""));
    v.title = String((const char *)(obj["title"] | "Untitled"));
    v.channel = String((const char *)(obj["channel"] | "YouTube"));
    v.duration = obj["duration"] | 0;
    if (v.url.length()) ytItems.push_back(v);
    if (ytItems.size() >= 15) break;
  }
  ytScroll = 0;
  ytMessage = ytItems.empty() ? tr("Không có kết quả", "No results") : "";
  ytFeedLoading = false;
  st.dirty = true;
  return !ytItems.empty();
}

static bool drawYTThumbnail(const YTItem &v, int x, int y) {
  if (!ytServer.length() || !v.id.length()) return false;
  HTTPClient http;
  http.setTimeout(3500);
  if (!http.begin(ytServer + "/thumb/" + v.id + ".jpg")) return false;
  int code = http.GET();
  int len = http.getSize();
  if (code != HTTP_CODE_OK || len <= 0 || len > 40 * 1024 || !ensureBuf((size_t)len)) { http.end(); return false; }
  WiFiClient *s = http.getStreamPtr();
  int got = s->readBytes(frameBuf, len);
  http.end();
  if (got != len) return false;
  if (!jpeg.openRAM(frameBuf, len, jpegDraw)) return false;
  prepareJPEGDecode();
  jpeg.decode(x, y, 0);
  jpeg.close();
  return true;
}

static void drawYTBrowser() {
  ui().fillScreen(TFT_BLACK);
  String wifi = WiFi.status() == WL_CONNECTED ? "S" : "WIFI";
  drawHeader("YouTube TV", true, tr("TÌM", "SEARCH"));
  if (!ytServer.length()) {
    ui().setDatum(MC_DATUM);
    ui().setTextColor(TFT_YELLOW, TFT_BLACK);
    ui().text(tr("Chưa có máy chủ", "No server"), 160, 96, 4);
    ui().setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    ui().text(ytMessage.length() ? ytMessage : tr("Chạm để kết nối", "Tap to connect"), 160, 128, 2);
    ui().drawRounded(80, 160, 160, 42, 8, TFT_CYAN);
    ui().text(tr("KẾT NỐI", "CONNECT"), 160, 181, 2);
    st.dirty = false;
    return;
  }
  if (ytItems.empty()) {
    ui().setDatum(MC_DATUM);
    ui().setTextColor(TFT_YELLOW, TFT_BLACK);
    ui().text(ytMessage.length() ? ytMessage : tr("Đang tải...", "Loading..."), 160, 110, 4);
    ui().setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    ui().text("Server: " + ytServer.substring(7), 160, 145, 2);
    st.dirty = false;
    return;
  }
  for (int row = 0; row < YT_ROWS; ++row) {
    int idx = ytScroll + row;
    if (idx >= (int)ytItems.size()) break;
    int y = 36 + row * YT_ROW_H;
    const YTItem &v = ytItems[idx];
    ui().fillRect(0, y, 320, YT_ROW_H - 2, ui().color565(9, 10, 13));
    ui().fillRect(4, y + 4, 96, 54, ui().color565(30, 30, 34));
    if (!drawYTThumbnail(v, 4, y + 4)) {
      ui().setDatum(MC_DATUM);
      ui().setTextColor(TFT_DARKGREY, ui().color565(30, 30, 34));
      ui().text("YT", 52, y + 31, 4);
    }
    ui().setDatum(TL_DATUM);
    ui().setTextColor(TFT_WHITE, ui().color565(9, 10, 13));
    ui().text(shorten(v.title, 27), 106, y + 5, 2);
    String line2 = utf8Count(v.title) > 27 ? shorten(v.title.substring(utf8ByteIndex(v.title, 24)), 27) : "";
    if (line2.length()) ui().text(line2, 106, y + 23, 2);
    ui().setTextColor(TFT_DARKGREY, ui().color565(9, 10, 13));
    String meta = shorten(v.channel, 18);
    if (v.duration) meta += "  " + fmtTime(v.duration);
    ui().text(meta, 106, y + 44, 1);
  }
  ui().setDatum(BR_DATUM);
  ui().setTextColor(TFT_DARKGREY, TFT_BLACK);
  ui().text(String(ytScroll + 1) + "-" + String(min(ytScroll + YT_ROWS, (int)ytItems.size())) + "/" + String(ytItems.size()), 316, 238, 1);
  st.dirty = false;
}

static void enterYouTube() {
  st.screen = Screen::YT_BROWSER;
  st.dirty = true;
  if (!ensureYTReady()) return;
  if (ytItems.empty()) fetchYTList(ytQuery, false);
}

static void handleYTBrowserTouch() {
  static bool wasDown = false;
  static int downX = 0, downY = 0, lastX = 0, lastY = 0;
  int x, y;
  bool down = readTouch(x, y);
  if (down && !wasDown) { downX = lastX = x; downY = lastY = y; }
  if (down) { lastX = x; lastY = y; }
  if (!down && wasDown) {
    int dy = lastY - downY;
    int dx = lastX - downX;
    if (abs(dy) > 24 && abs(dy) > abs(dx)) {
      int step = abs(dy) > 90 ? 2 : 1;
      if (dy < 0) ytScroll = min(ytScroll + step, max(0, (int)ytItems.size() - YT_ROWS));
      else ytScroll = max(0, ytScroll - step);
      st.dirty = true;
    } else if (downY < 34) {
      if (downX < 62) { st.screen = Screen::HOME; st.dirty = true; }
      else if (downX > 210) { st.screen = Screen::YT_KEYBOARD; st.dirty = true; }
    } else if (!ytServer.length()) {
      if (downY > 145) { ytServer = ""; if (ensureYTReady()) fetchYTList(ytQuery, false); st.dirty = true; }
    } else {
      int row = (downY - 36) / YT_ROW_H;
      int idx = ytScroll + row;
      if (row >= 0 && row < YT_ROWS && idx < (int)ytItems.size()) {
        // start stream in a separate function below
        ytPlayingTitle = ytItems[idx].title;
        remoteSourceLabel = "YouTube";
        remoteReturnScreen = Screen::YT_BROWSER;
        // store selected URL temporarily in query-like global via direct start call
        YTItem chosen = ytItems[idx];
        // close existing stream before opening new one
        if (ytStreamActive) { ytStreamHttp.end(); ytStreamActive = false; }
        drawStatus(tr("ĐANG MỞ VIDEO", "OPENING VIDEO"), shorten(chosen.title, 28));
        HTTPClient cmd;
        if (cmd.begin(ytServer + "/api/play")) {
          cmd.addHeader("Content-Type", "application/json");
          JsonDocument req;
          req["url"] = chosen.url;
          req["title"] = chosen.title;
          String body; serializeJson(req, body);
          int rc = cmd.POST(body);
          cmd.end();
          if (rc >= 200 && rc < 300) {
            allocateYTFrameBuffer();
            ytStreamHttp.useHTTP10(true);
            ytStreamHttp.setTimeout(10000);
            if (ytStreamHttp.begin(ytServer + "/stream.mjpg")) {
              int sc = ytStreamHttp.GET();
              if (sc == HTTP_CODE_OK) {
                ytStreamClient = ytStreamHttp.getStreamPtr();
                ytStreamActive = true;
                ytParserInJpeg = false;
                ytParserPrev = -1;
                ytParserN = 0;
                ytParserLastByteMs = millis();
                ytStatsStartMs = millis();
                ytFramesRx = ytFramesDecoded = ytFramesDropped = 0;
                ytJpegBytes = ytDecodeUs = ytRenderUs = 0;
                st.screen = Screen::YT_PLAYER;
                st.osdVisible = true;
                st.osdShownMs = millis();
                ytOsdDirty = true;
                ui().fillScreen(TFT_BLACK);
              } else { ytStreamHttp.end(); ytMessage = "Stream HTTP " + String(sc); st.dirty = true; }
            }
          } else { ytMessage = "Play HTTP " + String(rc); st.dirty = true; }
        }
      }
    }
  }
  wasDown = down;
}

// -----------------------------------------------------------------------------
// Live TV browser - VTV Go through the PC server
// -----------------------------------------------------------------------------
static const int TV_ROWS = 6;

static bool fetchTVList() {
  if (!ensureYTReady()) return false;
  drawStatus(tr("TRUYỀN HÌNH", "LIVE TV"), tr("Đang tải danh sách kênh...", "Loading channels..."));
  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(ytServer + "/api/tv/channels")) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    tvMessage = "Server HTTP " + String(code);
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err || !doc["ok"].as<bool>()) {
    tvMessage = err ? tr("Lỗi dữ liệu kênh", "Channel data error")
                    : String((const char *)(doc["error"] | tr("Không tải được kênh", "Could not load channels")));
    return false;
  }
  tvItems.clear();
  for (JsonObject obj : doc["items"].as<JsonArray>()) {
    TVItem item;
    item.id = String((const char *)(obj["id"] | ""));
    item.name = String((const char *)(obj["name"] | "TV"));
    if (item.id.length()) tvItems.push_back(item);
    if (tvItems.size() >= 30) break;
  }
  tvScroll = 0;
  tvMessage = tvItems.empty() ? tr("Không có kênh", "No channels") : "";
  st.dirty = true;
  return !tvItems.empty();
}

static void drawTVBrowser() {
  ui().fillScreen(TFT_BLACK);
  drawHeader(tr("TRUYỀN HÌNH", "LIVE TV"), true, "VTV Go");
  if (!ytServer.length()) {
    ui().setDatum(MC_DATUM);
    ui().setTextColor(TFT_YELLOW, TFT_BLACK);
    ui().text(tr("Chưa có máy chủ", "No server"), 160, 105, 3);
    ui().setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    ui().text(tr("Chạm để kết nối", "Tap to connect"), 160, 140, 2);
    st.dirty = false;
    return;
  }
  if (tvItems.empty()) {
    ui().setDatum(MC_DATUM);
    ui().setTextColor(TFT_YELLOW, TFT_BLACK);
    ui().text(tvMessage.length() ? tvMessage : tr("Đang tải...", "Loading..."), 160, 110, 3);
    st.dirty = false;
    return;
  }
  ui().setDatum(TL_DATUM);
  for (int row = 0; row < TV_ROWS; ++row) {
    int idx = tvScroll + row;
    if (idx >= (int)tvItems.size()) break;
    int y = 42 + row * 29;
    uint16_t card = ui().color565(18, 22, 28);
    ui().fillRounded(8, y, 304, 25, 5, card);
    ui().setTextColor(TFT_WHITE, card);
    ui().text(tvItems[idx].name, 18, y + 4, 2);
    ui().setDatum(MR_DATUM);
    ui().setTextColor(TFT_RED, card);
    ui().text("LIVE", 302, y + 12, 1);
    ui().setDatum(TL_DATUM);
  }
  ui().setDatum(BR_DATUM);
  ui().setTextColor(TFT_DARKGREY, TFT_BLACK);
  ui().text(String(tvScroll + 1) + "-" + String(min(tvScroll + TV_ROWS, (int)tvItems.size())) + "/" + String(tvItems.size()), 316, 238, 1);
  st.dirty = false;
}

static bool startTVChannel(const TVItem &item) {
  if (!ensureYTReady()) return false;
  if (ytStreamActive) { ytStreamHttp.end(); ytStreamActive = false; }
  drawStatus(tr("ĐANG MỞ KÊNH", "OPENING CHANNEL"), item.name);

  HTTPClient cmd;
  cmd.setTimeout(35000);
  if (!cmd.begin(ytServer + "/api/tv/play")) return false;
  cmd.addHeader("Content-Type", "application/json");
  JsonDocument req;
  req["id"] = item.id;
  String body;
  serializeJson(req, body);
  int rc = cmd.POST(body);
  String response = cmd.getString();
  cmd.end();
  if (rc < 200 || rc >= 300) {
    tvMessage = "TV HTTP " + String(rc);
    if (response.length()) {
      JsonDocument errDoc;
      if (!deserializeJson(errDoc, response)) tvMessage = String((const char *)(errDoc["error"] | tvMessage.c_str()));
    }
    st.dirty = true;
    return false;
  }

  allocateYTFrameBuffer();
  ytStreamHttp.useHTTP10(true);
  ytStreamHttp.setTimeout(10000);
  if (!ytStreamHttp.begin(ytServer + "/stream.mjpg")) return false;
  int sc = ytStreamHttp.GET();
  if (sc != HTTP_CODE_OK) {
    ytStreamHttp.end();
    tvMessage = "Stream HTTP " + String(sc);
    st.dirty = true;
    return false;
  }

  ytPlayingTitle = item.name;
  remoteSourceLabel = "VTV Go";
  remoteReturnScreen = Screen::TV_BROWSER;
  ytStreamClient = ytStreamHttp.getStreamPtr();
  ytStreamActive = true;
  ytParserInJpeg = false;
  ytParserPrev = -1;
  ytParserN = 0;
  ytParserLastByteMs = millis();
  ytStatsStartMs = millis();
  ytFramesRx = ytFramesDecoded = ytFramesDropped = 0;
  ytJpegBytes = ytDecodeUs = ytRenderUs = 0;
  st.screen = Screen::YT_PLAYER;
  st.osdVisible = true;
  st.osdShownMs = millis();
  ytOsdDirty = true;
  ui().fillScreen(TFT_BLACK);
  return true;
}

static void enterLiveTV() {
  st.screen = Screen::TV_BROWSER;
  st.dirty = true;
  if (!ensureYTReady()) return;
  if (tvItems.empty()) fetchTVList();
}

static void handleTVBrowserTouch() {
  static bool wasDown = false;
  static int downX = 0, downY = 0, lastX = 0, lastY = 0;
  int x, y;
  bool down = readTouch(x, y);
  if (down && !wasDown) { downX = lastX = x; downY = lastY = y; }
  if (down) { lastX = x; lastY = y; }
  if (!down && wasDown) {
    int dy = lastY - downY;
    int dx = lastX - downX;
    if (abs(dy) > 24 && abs(dy) > abs(dx)) {
      int step = abs(dy) > 90 ? 3 : 1;
      if (dy < 0) tvScroll = min(tvScroll + step, max(0, (int)tvItems.size() - TV_ROWS));
      else tvScroll = max(0, tvScroll - step);
      st.dirty = true;
    } else if (downY < 34 && downX < 70) {
      st.screen = Screen::HOME;
      st.dirty = true;
    } else if (!ytServer.length()) {
      ytServer = "";
      if (ensureYTReady()) fetchTVList();
      st.dirty = true;
    } else {
      int row = (downY - 42) / 29;
      int idx = tvScroll + row;
      if (row >= 0 && row < TV_ROWS && idx < (int)tvItems.size()) {
        startTVChannel(tvItems[idx]);
      }
    }
  }
  wasDown = down;
}

// -----------------------------------------------------------------------------
// Search keyboard - phone-style QWERTY
// -----------------------------------------------------------------------------
static const char KB_ROW1[] = "QWERTYUIOP";
static const char KB_ROW2[] = "ASDFGHJKL";
static const char KB_ROW3[] = "ZXCVBNM";

enum class KeyboardAction : uint8_t { CHAR_KEY, BACKSPACE, SPACE, SEARCH, CLEAR, BACK, DOT };
struct KeyboardKey {
  int16_t x, y, w, h;
  KeyboardAction action;
  char value;
};

static int buildKeyboardKeys(KeyboardKey *out, int maxKeys) {
  int n = 0;
  auto add = [&](int x, int y, int w, int h, KeyboardAction action, char value = 0) {
    if (n < maxKeys) out[n++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, action, value};
  };

  // Row 1: QWERTYUIOP
  for (int i = 0; i < 10; ++i) add(1 + i * 32, 76, 30, 34, KeyboardAction::CHAR_KEY, KB_ROW1[i]);
  // Row 2: ASDFGHJKL, centered like a phone keyboard.
  for (int i = 0; i < 9; ++i) add(8 + i * 34, 112, 32, 34, KeyboardAction::CHAR_KEY, KB_ROW2[i]);
  // Row 3: ZXCVBNM + backspace.
  for (int i = 0; i < 7; ++i) add(9 + i * 36, 148, 34, 34, KeyboardAction::CHAR_KEY, KB_ROW3[i]);
  add(261, 148, 50, 34, KeyboardAction::BACKSPACE);
  // Bottom row: Back / dot / space / clear / search.
  add(4,   186, 50, 48, KeyboardAction::BACK);
  add(56,  186, 34, 48, KeyboardAction::DOT);
  add(92,  186, 104, 48, KeyboardAction::SPACE);
  add(198, 186, 50, 48, KeyboardAction::CLEAR);
  add(250, 186, 66, 48, KeyboardAction::SEARCH);
  return n;
}

static bool hitKeyboardKey(int x, int y, KeyboardKey &hit) {
  KeyboardKey keys[40];
  const int count = buildKeyboardKeys(keys, 40);
  for (int i = 0; i < count; ++i) {
    const KeyboardKey &k = keys[i];
    if (x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h) {
      hit = k;
      return true;
    }
  }
  return false;
}

static String keyboardKeyLabel(const KeyboardKey &k) {
  switch (k.action) {
    case KeyboardAction::CHAR_KEY: return String(k.value);
    case KeyboardAction::BACKSPACE: return "<-";
    case KeyboardAction::SPACE: return tr("CÁCH", "SPACE");
    case KeyboardAction::SEARCH: return tr("TÌM", "SEARCH");
    case KeyboardAction::CLEAR: return tr("XÓA", "CLEAR");
    case KeyboardAction::BACK: return tr("VỀ", "BACK");
    case KeyboardAction::DOT: return ".";
  }
  return "";
}

static void drawYTKeyboard() {
  ui().fillScreen(TFT_BLACK);
  drawHeader(tr("Tìm kiếm video", "Search videos"), true);
  ui().fillRounded(6, 38, 308, 34, 5, ui().color565(24, 27, 32));
  ui().setDatum(ML_DATUM);
  ui().setTextColor(TFT_WHITE, ui().color565(24, 27, 32));
  ui().text(shorten(ytQuery, 36), 12, 55, 2);

  KeyboardKey keys[40];
  const int count = buildKeyboardKeys(keys, 40);
  for (int i = 0; i < count; ++i) {
    const KeyboardKey &k = keys[i];
    uint16_t bg = ui().color565(28, 31, 37);
    if (k.action == KeyboardAction::SEARCH) bg = ui().color565(0, 105, 150);
    else if (k.action == KeyboardAction::BACKSPACE || k.action == KeyboardAction::CLEAR) bg = ui().color565(58, 61, 67);
    ui().fillRounded(k.x + 1, k.y + 1, k.w - 2, k.h - 2, 5, bg);
    ui().setDatum(MC_DATUM);
    ui().setTextColor(TFT_WHITE, bg);
    const uint8_t textSize = k.action == KeyboardAction::CHAR_KEY ? 2 : 1;
    ui().text(keyboardKeyLabel(k), k.x + k.w / 2, k.y + k.h / 2, textSize);
  }
  st.dirty = false;
}

static void handleYTKeyboardTouch() {
  static bool wasDown = false;
  static int lastX = 0, lastY = 0;
  int x = lastX, y = lastY;
  bool down = readTouch(x, y);
  if (down) { lastX = x; lastY = y; }
  if (!down && wasDown) {
    x = lastX; y = lastY;
    if (y < 34 && x < 60) {
      st.screen = Screen::YT_BROWSER;
      st.dirty = true;
    } else {
      KeyboardKey k{};
      if (hitKeyboardKey(x, y, k)) {
        switch (k.action) {
          case KeyboardAction::CHAR_KEY:
            if (ytQuery.length() < 48) ytQuery += k.value;
            break;
          case KeyboardAction::BACKSPACE:
            if (ytQuery.length()) ytQuery.remove(ytQuery.length() - 1);
            break;
          case KeyboardAction::SPACE:
            if (ytQuery.length() < 48) ytQuery += ' ';
            break;
          case KeyboardAction::DOT:
            if (ytQuery.length() < 48) ytQuery += '.';
            break;
          case KeyboardAction::CLEAR:
            ytQuery = "";
            break;
          case KeyboardAction::BACK:
            st.screen = Screen::YT_BROWSER;
            break;
          case KeyboardAction::SEARCH:
            if (ytQuery.length()) {
              st.screen = Screen::YT_BROWSER;
              fetchYTList(ytQuery, true);
            }
            break;
        }
        st.dirty = true;
      }
    }
  }
  wasDown = down;
}

// -----------------------------------------------------------------------------
// YouTube MJPEG stream
// -----------------------------------------------------------------------------
static int readNextYTFrame() {
  if (!ytStreamActive || !ytStreamClient) return -1;
  if (!ensureBuf(YT_JPEG_INITIAL_BUF)) return -1;

  // If a partial JPEG has stalled for too long, discard only that partial frame and
  // resynchronise at the next SOI. This prevents a single dropped packet freezing TV.
  if (ytParserInJpeg && ytParserLastByteMs && millis() - ytParserLastByteMs > 2000) {
    Serial.printf("[YT] stale partial frame dropped (%u bytes)\n", (unsigned)ytParserN);
    ytFramesDropped++;
    ytParserInJpeg = false;
    ytParserPrev = -1;
    ytParserN = 0;
  }

  uint32_t sliceStart = micros();
  size_t processed = 0;
  const size_t BYTE_BUDGET = 24576;
  const uint32_t TIME_BUDGET_US = 12000;

  while (processed < BYTE_BUDGET && (micros() - sliceStart) < TIME_BUDGET_US) {
    int avail = ytStreamClient->available();
    if (avail <= 0) {
      if (!ytStreamClient->connected()) return 0;
      break;
    }
    while (avail-- > 0 && processed < BYTE_BUDGET && (micros() - sliceStart) < TIME_BUDGET_US) {
      int b = ytStreamClient->read();
      if (b < 0) break;
      ++processed;
      ytParserLastByteMs = millis();

      if (!ytParserInJpeg) {
        if (ytParserPrev == 0xFF && b == 0xD8) {
          ytParserInJpeg = true;
          ytParserN = 0;
          frameBuf[ytParserN++] = 0xFF;
          frameBuf[ytParserN++] = 0xD8;
        }
      } else {
        if (ytParserN >= frameBufSize && !ensureBuf(frameBufSize + 32 * 1024)) {
          ytParserInJpeg = false; ytParserN = 0; ytParserPrev = -1;
          return -1;
        }
        frameBuf[ytParserN++] = (uint8_t)b;
        if (ytParserPrev == 0xFF && b == 0xD9) {
          int len = (int)ytParserN;
          ytFramesRx++;
          ytJpegBytes += (uint32_t)len;
          ytParserInJpeg = false;
          ytParserN = 0;
          ytParserPrev = -1;
          return len;
        }
      }
      ytParserPrev = b;
    }
  }
  // No complete frame yet. Parser state is intentionally retained for next loop.
  return -1;
}

static bool startYTDebugStream() {
  if (!ensureYTReady()) return false;
  allocateYTFrameBuffer();
  if (ytStreamActive) {
    ytStreamHttp.end();
    ytStreamActive = false;
  }
  ytStreamHttp.setTimeout(10000);
  if (!ytStreamHttp.begin(ytServer + "/stream.mjpg")) return false;
  int sc = ytStreamHttp.GET();
  if (sc != HTTP_CODE_OK) {
    Serial.printf("[YTDBG] stream HTTP=%d\n", sc);
    ytStreamHttp.end();
    return false;
  }
  ytStreamClient = ytStreamHttp.getStreamPtr();
  ytStreamActive = true;
  ytParserInJpeg = false;
  ytParserPrev = -1;
  ytParserN = 0;
  ytParserLastByteMs = millis();
  ytStatsStartMs = millis();
  ytFramesRx = ytFramesDecoded = ytFramesDropped = 0;
  ytJpegBytes = ytDecodeUs = ytRenderUs = 0;
  st.screen = Screen::YT_PLAYER;
  st.osdVisible = true;
  st.osdShownMs = millis();
  ytOsdDirty = true;
  ui().fillScreen(TFT_BLACK);
  Serial.println("[YTDBG] stream opened");
  return true;
}

static void stopYTStream() {
  if (ytStreamActive) ytStreamHttp.end();
  ytStreamActive = false;
  ytStreamClient = nullptr;
  ytParserInJpeg = false;
  ytParserPrev = -1;
  ytParserN = 0;
  ytParserLastByteMs = 0;
  st.screen = remoteReturnScreen;
  st.dirty = true;
}

static void drawYTOSD() {
  ui().fillRect(0, 0, 320, YT_OSD_TOP_H, TFT_BLACK);
  ui().setDatum(ML_DATUM);
  ui().setTextColor(TFT_WHITE, TFT_BLACK);
  ui().text(tr("< VỀ", "< BACK"), 6, 14, 2);
  ui().setDatum(MR_DATUM);
  ui().setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  const String playingTitle = ytPlayingTitle.length() ? shorten(ytPlayingTitle, 34)
                                                       : tr("Đang phát video", "Playing video");
  ui().text(playingTitle, 314, 14, 1);
  ui().fillRect(0, YT_OSD_BOTTOM_Y, 320, 24, TFT_BLACK);
  ui().setDatum(MC_DATUM);
  ui().setTextColor(TFT_YELLOW, TFT_BLACK);
  ui().text(remoteSourceLabel, 160, 228, 2);
}

static void clearYTOSD() {
  ui().fillRect(0, 0, 320, YT_OSD_TOP_H, TFT_BLACK);
  ui().fillRect(0, YT_OSD_BOTTOM_Y, 320, 24, TFT_BLACK);
}

static void handleYTPlayerTouch() {
  static bool wasDown = false;
  static int downX = 0, downY = 0, lastX = 0, lastY = 0;
  static uint32_t downMs = 0;
  int x, y;
  bool down = readTouch(x, y);
  if (down && !wasDown) {
    downX = lastX = x;
    downY = lastY = y;
    downMs = millis();
  }
  if (down) {
    lastX = x;
    lastY = y;
    // Hold top-left for 700 ms = forced back, independent of OSD state.
    if (downX < 100 && downY < 70 && millis() - downMs > 700) {
      Serial.println("[TOUCH] YT forced BACK (hold top-left)");
      stopYTStream();
      wasDown = false;
      return;
    }
  }
  if (!down && wasDown) {
    int dx = lastX - downX;
    int dy = lastY - downY;
    // Swipe from left edge to right = back.
    if (downX < 55 && dx > 70 && abs(dx) > abs(dy)) {
      Serial.printf("[TOUCH] YT BACK swipe dx=%d dy=%d\n", dx, dy);
      stopYTStream();
    } else if (downX < 110 && downY < 60) {
      // Top-left always backs out; OSD no longer needs to be visible first.
      Serial.printf("[TOUCH] YT BACK tap x=%d y=%d\n", downX, downY);
      stopYTStream();
    } else {
      st.osdVisible = !st.osdVisible;
      st.osdShownMs = millis();
      ytOsdDirty = true;
      Serial.printf("[TOUCH] YT OSD=%d x=%d y=%d\n", st.osdVisible ? 1 : 0, downX, downY);
    }
  }
  wasDown = down;
}

// -----------------------------------------------------------------------------
// Home
// -----------------------------------------------------------------------------
static void drawHome() {
  const uint16_t bg = ui().color565(5, 7, 11);
  ui().fillScreen(bg);

  const uint16_t viBg = uiLanguage == UiLanguage::VI ? ui().color565(0, 105, 150) : ui().color565(28, 31, 37);
  const uint16_t enBg = uiLanguage == UiLanguage::EN ? ui().color565(0, 105, 150) : ui().color565(28, 31, 37);
  ui().fillRounded(184, 3, 76, 22, 5, viBg);
  ui().fillRounded(264, 3, 52, 22, 5, enBg);
  ui().setDatum(MC_DATUM);
  ui().setTextColor(TFT_WHITE, viBg);
  ui().text("TIẾNG VIỆT", 222, 14, 1);
  ui().setTextColor(TFT_WHITE, enBg);
  ui().text("ENGLISH", 290, 14, 1);

  ui().setTextColor(TFT_WHITE, bg);
  ui().text(tr("MINI TIVI", "MINI TV"), 160, 38, 3);
  ui().setTextColor(TFT_LIGHTGREY, bg);
  ui().text(tr("Trần Đăng Khoa", "Tran Dang Khoa"), 160, 59, 2);

  const uint16_t sdCard = ui().color565(22, 26, 34);
  const uint16_t ytCard = ui().color565(134, 0, 0);
  const uint16_t tvCard = ui().color565(16, 63, 48);
  ui().fillRounded(18, 72, 284, 44, 9, sdCard);
  ui().fillRounded(18, 124, 284, 44, 9, ytCard);
  ui().fillRounded(18, 176, 284, 44, 9, tvCard);

  ui().setTextColor(TFT_CYAN, sdCard);
  ui().text(tr("VIDEO THẺ SD", "SD VIDEO"), 160, 94, 2);
  ui().setTextColor(TFT_WHITE, ytCard);
  ui().text("YOUTUBE", 160, 146, 2);
  ui().setTextColor(TFT_WHITE, tvCard);
  ui().text(tr("TRUYỀN HÌNH", "LIVE TV"), 160, 198, 2);

  ui().setTextColor(WiFi.status() == WL_CONNECTED ? TFT_GREEN : TFT_DARKGREY, bg);
  ui().text(WiFi.status() == WL_CONNECTED ? ("WiFi  " + WiFi.localIP().toString())
                                          : tr("WiFi chưa kết nối", "WiFi not connected"), 160, 232, 1);
  st.dirty = false;
}

static void handleHomeTouch() {
  static bool wasDown = false;
  static int lastX = 0, lastY = 0;
  int x = lastX, y = lastY;
  bool down = readTouch(x, y);
  if (down) { lastX = x; lastY = y; }
  if (!down && wasDown) {
    x = lastX; y = lastY;
    Serial.printf("[TOUCH][HOME] x=%d y=%d\n", x, y);
    if (y < 32 && x >= 180) {
      if (x < 262) setLanguage(UiLanguage::VI);
      else setLanguage(UiLanguage::EN);
      st.dirty = true;
    } else if (y >= 68 && y < 120) {
      st.screen = Screen::SD_BROWSER;
      st.dirty = true;
    } else if (y >= 120 && y < 172) {
      enterYouTube();
    } else if (y >= 172 && y < 226) {
      enterLiveTV();
    }
  }
  wasDown = down;
}

void setup() {
  Serial.begin(115200);
  loadLanguage();
  ledcSetup(BL_CHANNEL, BL_FREQ, BL_RES_BITS);
  ledcAttachPin(BL_PIN, BL_CHANNEL);
  setBrightness(DEFAULT_BRIGHT);

  if (!displayBegin()) {
    Serial.println("[TFT] Arduino_GFX init failed");
    while (true) delay(1000);
  }
  delay(20);
  applyJPEGColorMode('A');
  Serial.println("[TFT] Arduino_GFX ILI9341 320x240 rotation=1 inversion=ON spi=40MHz");

  // XPT2046 touch uses software SPI. The CYD routes TFT, touch and SD to
  // three different pin groups but the classic ESP32 only has two user SPI hosts.
  pinMode(TOUCH_CLK, OUTPUT);
  pinMode(TOUCH_DIN, OUTPUT);
  pinMode(TOUCH_DO, INPUT);
  pinMode(TOUCH_CS, OUTPUT);
  pinMode(TOUCH_IRQ, INPUT);
  digitalWrite(TOUCH_CS, HIGH);
  digitalWrite(TOUCH_CLK, LOW);
  if (!loadTouchCalibration()) {
    Serial.println("[TOUCHCAL] no valid calibration; starting 4-point calibration");
    runTouchCalibration();
  }

  // SD owns VSPI exclusively: SCK=18, MISO=19, MOSI=23, CS=5.
  // TFT uses its dedicated HSPI-style pin set through Arduino_GFX; SD remains on VSPI.
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdReady = SD.begin(SD_CS, sdSPI, 4000000);
  if (!sdReady) {
    Serial.println("[SD] init @4MHz failed, retry @1MHz");
    SD.end();
    delay(50);
    sdReady = SD.begin(SD_CS, sdSPI, 1000000);
  }

  if (sdReady) scanVideos();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  Serial.println("[WIFI] boot: trying saved credentials in background");
  WiFi.begin();
  st.screen = Screen::HOME;
  st.dirty = true;
  if (sdReady) {
    Serial.printf("CYD Mini TV boot: SD=OK type=%u total=%.1fMB used=%.1fMB heap=%u\n", (unsigned)SD.cardType(), SD.totalBytes()/1048576.0, SD.usedBytes()/1048576.0, ESP.getFreeHeap());
  } else {
    Serial.printf("CYD Mini TV boot: SD=NONE heap=%u\n", ESP.getFreeHeap());
  }
}

void loop() {
  handleSerialDebug();
  switch (st.screen) {
    case Screen::HOME:
      if (st.dirty) drawHome();
      handleHomeTouch();
      delay(10);
      break;

    case Screen::SD_BROWSER:
      if (st.dirty) drawSDBrowser();
      handleSDBrowserTouch();
      delay(10);
      break;

    case Screen::SD_PLAYER: {
      handleSDPlayerTouch();
      if (st.osdVisible && st.playing && millis() - st.osdShownMs > OSD_TIMEOUT_MS) st.osdVisible = false;
      if (st.playing) {
        uint32_t due = st.startMs + (uint32_t)(st.frame * 1000.0f / st.idx.fps);
        uint32_t now = millis();
        if ((int32_t)(now - due) >= 0) {
          int len = readNextSDFrame();
          if (len <= 0) { stopSDPlayback(); break; }
          if (jpeg.openRAM(frameBuf, len, jpegDraw)) {
            prepareJPEGDecode();
            int32_t behind = (int32_t)(millis() - due);
            if (behind < (int32_t)(1000.0f / st.idx.fps) * 2) jpeg.decode(0, 0, 0);
            jpeg.close();
          }
          st.frame++;
        }
      }
      if (st.osdVisible) drawSDOsd();
      break;
    }

    case Screen::YT_BROWSER:
      if (st.dirty) drawYTBrowser();
      handleYTBrowserTouch();
      delay(8);
      break;

    case Screen::TV_BROWSER:
      if (st.dirty) drawTVBrowser();
      handleTVBrowserTouch();
      delay(8);
      break;

    case Screen::YT_KEYBOARD:
      if (st.dirty) drawYTKeyboard();
      handleYTKeyboardTouch();
      delay(8);
      break;

    case Screen::YT_PLAYER: {
      handleYTPlayerTouch();
      static uint32_t ytAutoStatMs = 0;
      if (millis() - ytAutoStatMs >= 1000) {
        ytAutoStatMs = millis();
        uint32_t ms = ytStatsStartMs ? millis() - ytStatsStartMs : 0;
        float fpsNow = ms ? (ytFramesDecoded * 1000.0f / ms) : 0.0f;
        float decMs = ytFramesDecoded ? (ytDecodeUs / 1000.0f / ytFramesDecoded) : 0.0f;
        float renMs = ytFramesDecoded ? (ytRenderUs / 1000.0f / ytFramesDecoded) : 0.0f;
        Serial.printf("[YTSTAT] t=%lums rx=%lu dec=%lu drop=%lu fps=%.2f decMs=%.1f renMs=%.1f avail=%d conn=%d partial=%u heap=%u\n",
                      (unsigned long)ms, (unsigned long)ytFramesRx, (unsigned long)ytFramesDecoded,
                      (unsigned long)ytFramesDropped, fpsNow, decMs, renMs,
                      ytStreamClient ? ytStreamClient->available() : -1,
                      (ytStreamClient && ytStreamClient->connected()) ? 1 : 0,
                      (unsigned)ytParserN, ESP.getFreeHeap());
      }
      int len = readNextYTFrame();
      if (len > 0) {
        if (jpeg.openRAM(frameBuf, len, jpegDraw)) {
          prepareJPEGDecode();
          uint32_t d0 = micros();
          ytDecodeInProgress = true;
          int decOk = jpeg.decode(0, ytFrameBufferEnabled ? 0 : YT_VIDEO_Y, 0);
          ytDecodeInProgress = false;
          ytDecodeUs += (uint32_t)(micros() - d0);
          jpeg.close();
          if (decOk) {
            if (ytFrameBufferEnabled) presentYTFrame();
            ytFramesDecoded++;
          } else {
            ytFramesDropped++;
          }
        } else {
          ytFramesDropped++;
        }
      } else if (len == 0) {
        ytMessage = tr("Luồng đã kết thúc", "Stream ended");
        stopYTStream();
        break;
      } else {
        // Timeout without a complete frame is not a fatal stream error.
        // Return to the loop quickly so touch/UART remain responsive.
        if (!ytStreamClient || !ytStreamClient->connected()) {
          ytMessage = tr("Mất luồng video", "Stream lost");
          stopYTStream();
          break;
        }
      }
      if (st.osdVisible && millis() - st.osdShownMs > OSD_TIMEOUT_MS) {
        st.osdVisible = false;
        ytOsdDirty = true;
      }
      if (ytOsdDirty) {
        if (st.osdVisible) drawYTOSD(); else clearYTOSD();
        ytOsdDirty = false;
      }
      break;
    }
  }
}
