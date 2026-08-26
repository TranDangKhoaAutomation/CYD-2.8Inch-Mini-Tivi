#include "ui_draw.h"
#include "display.h"

namespace { UiDraw instance; }

UiDraw &ui() {
    instance.bind(display());
    return instance;
}

uint16_t UiDraw::color565(uint8_t r, uint8_t g, uint8_t b) const {
    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3));
}

void UiDraw::fillScreen(uint16_t color) { if (gfx_) gfx_->fillScreen(color); }
void UiDraw::fillRect(int16_t x,int16_t y,int16_t w,int16_t h,uint16_t color) { if (gfx_) gfx_->fillRect(x,y,w,h,color); }
void UiDraw::drawRect(int16_t x,int16_t y,int16_t w,int16_t h,uint16_t color) { if (gfx_) gfx_->drawRect(x,y,w,h,color); }
void UiDraw::drawFastHLine(int16_t x,int16_t y,int16_t w,uint16_t color) { if (gfx_) gfx_->drawFastHLine(x,y,w,color); }
void UiDraw::fillRounded(int16_t x,int16_t y,int16_t w,int16_t h,int16_t radius,uint16_t color) { if (gfx_) gfx_->fillRoundRect(x,y,w,h,radius,color); }
void UiDraw::drawRounded(int16_t x,int16_t y,int16_t w,int16_t h,int16_t radius,uint16_t color) { if (gfx_) gfx_->drawRoundRect(x,y,w,h,radius,color); }

void UiDraw::setTextColor(uint16_t fg, uint16_t bg) {
    fg_ = fg; bg_ = bg;
    if (gfx_) gfx_->setTextColor(fg, bg);
}

void UiDraw::text(const String &s, int16_t x, int16_t y, uint8_t size) {
    if (!gfx_) return;
    if (size < 1) size = 1;
    const int16_t w = (int16_t)s.length() * 6 * size;
    const int16_t h = 8 * size;
    int16_t px=x, py=y;
    switch (datum_) {
        case TR_DATUM: px -= w; break;
        case ML_DATUM: py -= h/2; break;
        case MC_DATUM: px -= w/2; py -= h/2; break;
        case MR_DATUM: px -= w; py -= h/2; break;
        case BR_DATUM: px -= w; py -= h; break;
        case TL_DATUM: default: break;
    }
    gfx_->setTextSize(size);
    gfx_->setTextColor(fg_, bg_);
    gfx_->setCursor(px, py);
    gfx_->print(s);
}
