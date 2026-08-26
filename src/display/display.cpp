#include "display.h"

namespace {
constexpr int TFT_DC_PIN = 2;
constexpr int TFT_CS_PIN = 15;
constexpr int TFT_SCK_PIN = 14;
constexpr int TFT_MOSI_PIN = 13;
constexpr int TFT_MISO_PIN = 12;
constexpr int TFT_BL_PIN = 21;
constexpr int32_t TFT_SPI_HZ = 40000000;

SPIClass tftSpi(HSPI);
Arduino_DataBus *bus = new Arduino_HWSPI(TFT_DC_PIN, TFT_CS_PIN, TFT_SCK_PIN, TFT_MOSI_PIN, TFT_MISO_PIN, &tftSpi, true);
Arduino_GFX *gfx = new Arduino_ILI9341(bus);
}

bool displayBegin() {
    pinMode(TFT_BL_PIN, OUTPUT);
    digitalWrite(TFT_BL_PIN, HIGH);
    if (!gfx->begin(40000000L)) return false;
    gfx->setRotation(1);
    gfx->invertDisplay(true);
    gfx->fillScreen(RGB565_BLACK);
    return true;
}

Arduino_GFX *display() { return gfx; }

int jpegDrawCallback(JPEGDRAW *p) {
    if (!p || !p->pPixels) return 0;
    gfx->draw16bitBeRGBBitmap(p->x, p->y, p->pPixels, p->iWidth, p->iHeight);
    return 1;
}
