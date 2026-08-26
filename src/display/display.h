#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>

bool displayBegin();
Arduino_GFX *display();
int jpegDrawCallback(JPEGDRAW *draw);
