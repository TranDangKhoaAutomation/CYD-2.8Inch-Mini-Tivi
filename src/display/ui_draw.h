#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

enum UiDatum : uint8_t {
    TL_DATUM, TR_DATUM, ML_DATUM, MC_DATUM, MR_DATUM, BR_DATUM
};

constexpr uint16_t TFT_BLACK     = 0x0000;
constexpr uint16_t TFT_BLUE      = 0x001F;
constexpr uint16_t TFT_RED       = 0xF800;
constexpr uint16_t TFT_GREEN     = 0x07E0;
constexpr uint16_t TFT_CYAN      = 0x07FF;
constexpr uint16_t TFT_YELLOW    = 0xFFE0;
constexpr uint16_t TFT_WHITE     = 0xFFFF;
constexpr uint16_t TFT_DARKGREY  = 0x7BEF;
constexpr uint16_t TFT_LIGHTGREY = 0xC618;

class UiDraw {
public:
    explicit UiDraw(Arduino_GFX *gfx = nullptr) : gfx_(gfx) {}
    void bind(Arduino_GFX *gfx) { gfx_ = gfx; }
    int16_t width() const { return gfx_ ? gfx_->width() : 320; }
    int16_t height() const { return gfx_ ? gfx_->height() : 240; }
    uint16_t color565(uint8_t r, uint8_t g, uint8_t b) const;
    void fillScreen(uint16_t color);
    void fillRect(int16_t x,int16_t y,int16_t w,int16_t h,uint16_t color);
    void drawRect(int16_t x,int16_t y,int16_t w,int16_t h,uint16_t color);
    void drawFastHLine(int16_t x,int16_t y,int16_t w,uint16_t color);
    void fillRounded(int16_t x,int16_t y,int16_t w,int16_t h,int16_t radius,uint16_t color);
    void drawRounded(int16_t x,int16_t y,int16_t w,int16_t h,int16_t radius,uint16_t color);
    void setDatum(UiDatum datum) { datum_ = datum; }
    void setTextColor(uint16_t fg, uint16_t bg);
    void text(const String &s, int16_t x, int16_t y, uint8_t size = 1);
private:
    Arduino_GFX *gfx_ = nullptr;
    UiDatum datum_ = TL_DATUM;
    uint16_t fg_ = TFT_WHITE;
    uint16_t bg_ = TFT_BLACK;
};

UiDraw &ui();
