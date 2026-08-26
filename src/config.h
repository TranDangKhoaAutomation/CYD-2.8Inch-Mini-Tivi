#pragma once

// ================= Open CYD Player — hardware config =================
// Board: ESP32-2432S028R ("Cheap Yellow Display", 2.8" resistive touch)

// ---- SD card (VSPI) ----
#define SD_CS    5
#define SD_SCK   18
#define SD_MISO  19
#define SD_MOSI  23

// ---- XPT2046 resistive touch (own SPI bus) ----
#define TOUCH_CLK  25
#define TOUCH_CS   33
#define TOUCH_DIN  32   // MOSI
#define TOUCH_DO   39   // MISO
#define TOUCH_IRQ  36

// Raw touch calibration (rough defaults; run on-screen calibration to refine)
#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3700
#define TOUCH_Y_MIN 240
#define TOUCH_Y_MAX 3800

// ---- Backlight (PWM brightness) ----
#define BL_PIN      21
#define BL_CHANNEL  0
#define BL_FREQ     5000
#define BL_RES_BITS 8

// ---- Audio ----
// Option A: external PCM5102A I2S DAC (recommended, clean stereo)
//   BCK -> GPIO 4, LRCK -> GPIO 22, DIN -> GPIO 27
//   (short SCK/FLT/DMP/FMT/BYP bridge pads to GND on the DAC board)
// Option B: onboard amp/speaker via internal DAC on GPIO 26 (mono, lo-fi)
#define USE_EXTERNAL_I2S_DAC 1
#define I2S_BCLK  4
#define I2S_LRC   22
#define I2S_DOUT  27

// ---- Player ----
#define VIDEO_DIR       "/videos"
#define SEEK_STEP_SEC   60          // +/- jump per seek button press
#define DEFAULT_VOLUME  12          // 0..21 (ESP32-audioI2S scale)
#define DEFAULT_BRIGHT  255         // 0..255
#define OSD_TIMEOUT_MS  3000        // on-screen controls auto-hide
