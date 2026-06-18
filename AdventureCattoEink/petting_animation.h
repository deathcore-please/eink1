#pragma once

#include "animations.h"
#include "petting_images.h"

// ---------------- PETTING ANIMATION ----------------
// 96x64, SSD1306 vertical pages, LSB = top
// bit 1 = black, bit 0 = white

#define PETTING_FRAME_COUNT 4

const uint8_t* const pettingFrames[] PROGMEM = {
  sprite_0,
  sprite_1,
  sprite_0,
  sprite_1
};

const Animation pettingAnimation = {
  "Petting",
  pettingFrames,
  PETTING_FRAME_COUNT,
  sprite_0_width,
  sprite_0_height,
  SPRITE_FORMAT_VERTICAL_LSB,
  false,
  77,
  34,
  350
};
