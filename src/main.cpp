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

struct YTItem {
  String id;
  String url;
  String title;
  String channel;
  uint32_t duration = 0;
};

static bool sdReady = false;
static uint8_t *frameBuf = nullptr;
static size_t frameBufSize = 0;
static std::vector<String> videoList;
static std::vector<YTItem> ytItems;
static int ytScroll = 0;
static String ytQuery = "music technology robots";
static String ytServer;
static String ytMessage;
static String ytPlayingTitle;
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

// Runtime diagnostics. touchDebugUntil enables bounded raw/calibrated touch logging.
static uint32_t touchDebugUntil = 0;
static uint32_t touchLastLogMs = 0;

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

static String shorten(const String &s, size_t n) {
  if (s.length() <= n) return s;
  return s.substring(0, n > 3 ? n - 3 : n) + "...";
}

static int jpegDraw(JPEGDRAW *p) {
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

static bool readTouch(int &x, int &y) {
  // IRQ is active LOW on XPT2046. It is a real hardware indication and avoids
  // mistaking idle ADC values for touches.
  if (digitalRead(TOUCH_IRQ) != LOW) return false;

  // Median-of-3 style averaging reduces resistive touch jitter.
  uint32_t sx = 0, sy = 0;
  for (int i = 0; i < 3; ++i) {
    sy += touchRead12(0x90); // raw Y
    sx += touchRead12(0xD0); // raw X
  }
  int rawX = (int)(sx / 3);
  int rawY = (int)(sy / 3);
  if (rawX < 50 || rawY < 50) return false;

  x = map(rawY, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, ui().width());
  y = map(rawX, TOUCH_X_MIN, TOUCH_X_MAX, ui().height(), 0);
  x = constrain(x, 0, ui().width() - 1);
  y = constrain(y, 0, ui().height() - 1);
  if ((int32_t)(touchDebugUntil - millis()) > 0 && millis() - touchLastLogMs >= 80) {
    touchLastLogMs = millis();
    Serial.printf("[TOUCH] rawX=%d rawY=%d x=%d y=%d irq=%d\n", rawX, rawY, x, y, digitalRead(TOUCH_IRQ));
  }
  return true;
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
  ui().text("BACK", 34, barY + 30, 2);
  ui().text("<< 1m", W / 2 - 70, barY + 30, 2);
  ui().text(st.playing ? "| |" : ">", W / 2, barY + 30, 4);
  ui().text("1m >>", W / 2 + 70, barY + 30, 2);
  ui().text("VOL " + String(st.volume), W - 34, barY + 30, 2);
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
  drawHeader("SD TV", true, sdReady ? "SD OK" : "NO SD");
  if (!sdReady) {
    ui().setDatum(MC_DATUM);
    ui().setTextColor(TFT_RED, TFT_BLACK);
    ui().text("Khong co the SD", 160, 105, 4);
    ui().setTextColor(TFT_DARKGREY, TFT_BLACK);
    ui().text("YouTube TV van dung duoc", 160, 140, 2);
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
    ui().text("Chua co video", 160, 110, 4);
    ui().setTextColor(TFT_DARKGREY, TFT_BLACK);
    ui().text("Copy .mjpeg/.mp3/.idx vao /videos", 160, 145, 2);
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
    ui().text("CHON WIFI TREN DIEN THOAI", 8, 8, 2);
    ui().setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    ui().text("Ket noi AP: CYD-MiniTV-Setup", 8, 28, 2);
    ui().text("Cac WiFi gan day:", 8, 50, 2);
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
      ui().text("Khong quet thay SSID", 12, 80, 2);
    }
    ui().setTextColor(TFT_WHITE, TFT_BLACK);
    ui().text("Portal se hien danh sach SSID de bam chon", 8, 218, 1);
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
  drawStatus("QUET LAN", "UDP khong thay - dang quet TCP...");
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
    if (validateServer(ip, 8765, 28)) return true;
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
  drawStatus("TIM SERVER", "UDP broadcast + TCP fallback");
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
  uint16_t cachedPort = 8765;
  if (loadServerCache(cachedIp, cachedPort) && validateServer(cachedIp, cachedPort, 120)) {
    Serial.printf("[SERVER] found by NVS cache: %s\n", ytServer.c_str());
    return true;
  }

  Serial.println("[SERVER] cache failed, trying TCP subnet scan");
  if (scanSubnetForServer()) {
    Serial.printf("[SERVER] found by TCP scan: %s\n", ytServer.c_str());
    return true;
  }

  ytMessage = "Khong tim thay server";
  Serial.println("[SERVER] FAIL: no CYD TV server found on UDP or TCP scan");
  return false;
}

static bool ensureYTReady() {
  if (!ensureWiFi()) return false;
  if (!discoverYTServer()) return false;
  return true;
}

static bool fetchYTList(const String &query, bool searchMode);

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
  drawStatus(searchMode ? "SEARCH" : "YOUTUBE", "Dang tai danh sach...");
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
    ytMessage = err ? "JSON loi" : String((const char *)(doc["error"] | "Search loi"));
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
  ytMessage = ytItems.empty() ? "Khong co ket qua" : "";
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
  drawHeader("YouTube TV", true, "SEARCH");
  if (!ytServer.length()) {
    ui().setDatum(MC_DATUM);
    ui().setTextColor(TFT_YELLOW, TFT_BLACK);
    ui().text("Chua co server", 160, 96, 4);
    ui().setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    ui().text(ytMessage.length() ? ytMessage : "Cham de ket noi", 160, 128, 2);
    ui().drawRounded(80, 160, 160, 42, 8, TFT_CYAN);
    ui().text("KET NOI", 160, 181, 2);
    st.dirty = false;
    return;
  }
  if (ytItems.empty()) {
    ui().setDatum(MC_DATUM);
    ui().setTextColor(TFT_YELLOW, TFT_BLACK);
    ui().text(ytMessage.length() ? ytMessage : "Dang tai...", 160, 110, 4);
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
    String line2 = v.title.length() > 27 ? shorten(v.title.substring(24), 27) : "";
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
      else if (downX > 235) { st.screen = Screen::YT_KEYBOARD; st.dirty = true; }
    } else if (!ytServer.length()) {
      if (downY > 145) { ytServer = ""; if (ensureYTReady()) fetchYTList(ytQuery, false); st.dirty = true; }
    } else {
      int row = (downY - 36) / YT_ROW_H;
      int idx = ytScroll + row;
      if (row >= 0 && row < YT_ROWS && idx < (int)ytItems.size()) {
        // start stream in a separate function below
        ytPlayingTitle = ytItems[idx].title;
        // store selected URL temporarily in query-like global via direct start call
        YTItem chosen = ytItems[idx];
        // close existing stream before opening new one
        if (ytStreamActive) { ytStreamHttp.end(); ytStreamActive = false; }
        drawStatus("DANG MO VIDEO", shorten(chosen.title, 28));
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
// Search keyboard
// -----------------------------------------------------------------------------
static const char *KB[32] = {
  "A","B","C","D","E","F","G","H",
  "I","J","K","L","M","N","O","P",
  "Q","R","S","T","U","V","W","X",
  "Y","Z","<-","SP","GO","CLR","BACK","."
};

static void drawYTKeyboard() {
  ui().fillScreen(TFT_BLACK);
  drawHeader("Search YouTube", true);
  ui().fillRounded(6, 38, 308, 34, 5, ui().color565(24, 27, 32));
  ui().setDatum(ML_DATUM);
  ui().setTextColor(TFT_WHITE, ui().color565(24, 27, 32));
  ui().text(shorten(ytQuery, 36), 12, 55, 2);
  const int keyY = 76;
  const int keyH = 40;
  for (int i = 0; i < 32; ++i) {
    int col = i % 8, row = i / 8;
    int x = col * 40, y = keyY + row * keyH;
    uint16_t bg = (i == 28) ? ui().color565(170, 0, 0) : ui().color565(28, 31, 37);
    ui().fillRounded(x + 2, y + 2, 36, 36, 4, bg);
    ui().setDatum(MC_DATUM);
    ui().setTextColor(TFT_WHITE, bg);
    ui().text(KB[i], x + 20, y + 20, i >= 26 ? 1 : 2);
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
    if (y < 34 && x < 60) { st.screen = Screen::YT_BROWSER; st.dirty = true; }
    else if (y >= 76) {
      int col = constrain(x / 40, 0, 7);
      int row = constrain((y - 76) / 40, 0, 3);
      int i = row * 8 + col;
      if (i >= 0 && i < 26) { if (ytQuery.length() < 48) ytQuery += KB[i]; }
      else if (i == 26) { if (ytQuery.length()) ytQuery.remove(ytQuery.length() - 1); }
      else if (i == 27) { if (ytQuery.length() < 48) ytQuery += ' '; }
      else if (i == 28) {
        if (ytQuery.length()) { st.screen = Screen::YT_BROWSER; fetchYTList(ytQuery, true); st.dirty = true; }
      } else if (i == 29) ytQuery = "";
      else if (i == 30) { st.screen = Screen::YT_BROWSER; st.dirty = true; }
      else if (i == 31) { if (ytQuery.length() < 48) ytQuery += '.'; }
      st.dirty = true;
    }
  }
  wasDown = down;
}

// -----------------------------------------------------------------------------
// YouTube MJPEG stream
// -----------------------------------------------------------------------------
static int readNextYTFrame() {
  if (!ytStreamActive || !ytStreamClient) return -1;
  if (!ensureBuf(64 * 1024)) return -1;

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

static void stopYTStream() {
  if (ytStreamActive) ytStreamHttp.end();
  ytStreamActive = false;
  ytStreamClient = nullptr;
  ytParserInJpeg = false;
  ytParserPrev = -1;
  ytParserN = 0;
  ytParserLastByteMs = 0;
  st.screen = Screen::YT_BROWSER;
  st.dirty = true;
}

static void drawYTOSD() {
  ui().fillRect(0, 0, 320, 28, TFT_BLACK);
  ui().setDatum(ML_DATUM);
  ui().setTextColor(TFT_WHITE, TFT_BLACK);
  ui().text("< BACK", 6, 14, 2);
  ui().setDatum(MR_DATUM);
  ui().text(shorten(ytPlayingTitle, 27), 314, 14, 2);
  ui().fillRect(0, 216, 320, 24, TFT_BLACK);
  ui().setDatum(MC_DATUM);
  ui().setTextColor(TFT_YELLOW, TFT_BLACK);
  ui().text("YouTube via LAN  |  VIDEO ONLY", 160, 228, 2);
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
      Serial.printf("[TOUCH] YT OSD=%d x=%d y=%d\n", st.osdVisible ? 1 : 0, downX, downY);
    }
  }
  wasDown = down;
}

// -----------------------------------------------------------------------------
// Home
// -----------------------------------------------------------------------------
static void drawHome() {
  ui().fillScreen(ui().color565(5, 7, 11));
  ui().setDatum(MC_DATUM);
  ui().setTextColor(TFT_WHITE, ui().color565(5, 7, 11));
  ui().text("CYD MINI TV", 160, 30, 4);
  ui().setTextColor(TFT_DARKGREY, ui().color565(5, 7, 11));
  ui().text("ESP32-2432S028R", 160, 54, 2);
  ui().fillRounded(18, 78, 284, 60, 10, ui().color565(22, 26, 34));
  ui().fillRounded(18, 150, 284, 60, 10, ui().color565(134, 0, 0));
  ui().setTextColor(TFT_CYAN, ui().color565(22, 26, 34));
  ui().text("SD VIDEO", 160, 102, 4);
  ui().setTextColor(TFT_LIGHTGREY, ui().color565(22, 26, 34));
  ui().text(sdReady ? "MJPEG + MP3" : "No SD - van vao duoc", 160, 125, 1);
  ui().setTextColor(TFT_WHITE, ui().color565(134, 0, 0));
  ui().text("YOUTUBE TV", 160, 174, 4);
  ui().text("Swipe feed / Search / Tap to play", 160, 198, 1);
  ui().setTextColor(WiFi.status() == WL_CONNECTED ? TFT_GREEN : TFT_DARKGREY, ui().color565(5, 7, 11));
  ui().text(WiFi.status() == WL_CONNECTED ? ("WiFi " + WiFi.localIP().toString()) : "WiFi setup khi vao YouTube", 160, 228, 1);
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
    if (y >= 70 && y < 145) { st.screen = Screen::SD_BROWSER; st.dirty = true; }
    else if (y >= 145 && y < 220) enterYouTube();
  }
  wasDown = down;
}

void setup() {
  Serial.begin(115200);
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

    case Screen::YT_KEYBOARD:
      if (st.dirty) drawYTKeyboard();
      handleYTKeyboardTouch();
      delay(8);
      break;

    case Screen::YT_PLAYER: {
      handleYTPlayerTouch();
      int len = readNextYTFrame();
      if (len > 0) {
        if (jpeg.openRAM(frameBuf, len, jpegDraw)) {
          prepareJPEGDecode();
          uint32_t d0 = micros();
          ytDecodeInProgress = true;
          int decOk = jpeg.decode(0, 30, 0);
          ytDecodeInProgress = false;
          ytDecodeUs += (uint32_t)(micros() - d0);
          jpeg.close();
          if (decOk) ytFramesDecoded++; else ytFramesDropped++;
        } else {
          ytFramesDropped++;
        }
      } else if (len == 0) {
        ytMessage = "Stream ket thuc";
        stopYTStream();
        break;
      } else {
        // Timeout without a complete frame is not a fatal stream error.
        // Return to the loop quickly so touch/UART remain responsive.
        if (!ytStreamClient || !ytStreamClient->connected()) {
          ytMessage = "Mat stream";
          stopYTStream();
          break;
        }
      }
      if (st.osdVisible && millis() - st.osdShownMs > OSD_TIMEOUT_MS) st.osdVisible = false;
      if (st.osdVisible) drawYTOSD();
      break;
    }
  }
}
