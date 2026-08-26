#include "ui_draw.h"
#include "display.h"
#include "vietnamese_font.h"

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

namespace {
uint32_t nextUtf8CodePoint(const String &s, size_t &i) {
    if (i >= s.length()) return 0;
    const uint8_t c0 = (uint8_t)s[i++];
    if (c0 < 0x80) return c0;
    if ((c0 & 0xE0) == 0xC0 && i < s.length()) {
        const uint8_t c1 = (uint8_t)s[i++];
        if ((c1 & 0xC0) == 0x80) return ((uint32_t)(c0 & 0x1F) << 6) | (c1 & 0x3F);
        return '?';
    }
    if ((c0 & 0xF0) == 0xE0 && i + 1 < s.length()) {
        const uint8_t c1 = (uint8_t)s[i++], c2 = (uint8_t)s[i++];
        if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80)
            return ((uint32_t)(c0 & 0x0F) << 12) | ((uint32_t)(c1 & 0x3F) << 6) | (c2 & 0x3F);
        return '?';
    }
    if ((c0 & 0xF8) == 0xF0 && i + 2 < s.length()) {
        // Four-byte codepoints (emoji etc.) are intentionally replaced with '?'.
        i += 3;
        return '?';
    }
    return '?';
}

struct FontAtlas {
    const uint8_t *bitmap;
    const VietnameseGlyph *glyphs;
    uint16_t count;
    uint8_t height;
};

FontAtlas fontForSize(uint8_t size) {
    switch (size) {
        case 4: return {VI_FONT_4_BITMAP, VI_FONT_4_GLYPHS, VI_FONT_4_GLYPH_COUNT, VI_FONT_4_HEIGHT};
        case 3: return {VI_FONT_3_BITMAP, VI_FONT_3_GLYPHS, VI_FONT_3_GLYPH_COUNT, VI_FONT_3_HEIGHT};
        case 2: return {VI_FONT_2_BITMAP, VI_FONT_2_GLYPHS, VI_FONT_2_GLYPH_COUNT, VI_FONT_2_HEIGHT};
        default: return {VI_FONT_1_BITMAP, VI_FONT_1_GLYPHS, VI_FONT_1_GLYPH_COUNT, VI_FONT_1_HEIGHT};
    }
}

bool findVietnameseGlyph(const FontAtlas &font, uint32_t cp, VietnameseGlyph &out) {
    int lo = 0, hi = (int)font.count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        VietnameseGlyph g;
        memcpy_P(&g, &font.glyphs[mid], sizeof(g));
        if (g.codepoint == cp) { out = g; return true; }
        if (g.codepoint < cp) lo = mid + 1; else hi = mid - 1;
    }
    if (cp != '?') return findVietnameseGlyph(font, '?', out);
    return false;
}

int16_t measureUtf8Text(const String &s, uint8_t size) {
    const FontAtlas font = fontForSize(size);
    int16_t w = 0;
    size_t i = 0;
    while (i < s.length()) {
        VietnameseGlyph g;
        const uint32_t cp = nextUtf8CodePoint(s, i);
        if (findVietnameseGlyph(font, cp, g)) w += g.advance;
    }
    return w;
}

void drawUtf8Text(Arduino_GFX *gfx, const String &s, int16_t x, int16_t y,
                  uint8_t size, uint16_t fg, uint16_t bg) {
    if (!gfx) return;
    const FontAtlas font = fontForSize(size);
    size_t i = 0;
    int16_t cursor = x;
    while (i < s.length()) {
        VietnameseGlyph g;
        const uint32_t cp = nextUtf8CodePoint(s, i);
        if (!findVietnameseGlyph(font, cp, g)) continue;
        gfx->fillRect(cursor, y, g.advance, font.height, bg);
        for (uint8_t row = 0; row < font.height; ++row) {
            int runStart = -1;
            for (uint8_t col = 0; col <= g.width; ++col) {
                bool on = false;
                if (col < g.width) {
                    const uint32_t addr = g.offset + (uint32_t)row * g.rowBytes + (col >> 3);
                    const uint8_t bits = pgm_read_byte(font.bitmap + addr);
                    on = (bits & (0x80 >> (col & 7))) != 0;
                }
                if (on && runStart < 0) runStart = col;
                if ((!on || col == g.width) && runStart >= 0) {
                    gfx->fillRect(cursor + runStart, y + row, col - runStart, 1, fg);
                    runStart = -1;
                }
            }
        }
        cursor += g.advance;
    }
}
} // namespace

void UiDraw::text(const String &s, int16_t x, int16_t y, uint8_t size) {
    if (!gfx_) return;
    if (size < 1) size = 1;
    if (size > 4) size = 4;
    const FontAtlas font = fontForSize(size);
    const int16_t w = measureUtf8Text(s, size);
    const int16_t h = font.height;
    int16_t px=x, py=y;
    switch (datum_) {
        case TR_DATUM: px -= w; break;
        case ML_DATUM: py -= h/2; break;
        case MC_DATUM: px -= w/2; py -= h/2; break;
        case MR_DATUM: px -= w; py -= h/2; break;
        case BR_DATUM: px -= w; py -= h; break;
        case TL_DATUM: default: break;
    }
    drawUtf8Text(gfx_, s, px, py, size, fg_, bg_);
}

