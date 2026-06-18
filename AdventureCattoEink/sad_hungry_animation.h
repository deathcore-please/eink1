#pragma once

#include "animations.h"

#define sprite_0_width sadHungrySprite0Width
#define sprite_0_height sadHungrySprite0Height
#define sprite_0 sadHungrySprite0
#define sprite_1_width sadHungrySprite1Width
#define sprite_1_height sadHungrySprite1Height
#define sprite_1 sadHungrySprite1
#include "sad_hungry_images_raw.h"
#undef sprite_0_width
#undef sprite_0_height
#undef sprite_0
#undef sprite_1_width
#undef sprite_1_height
#undef sprite_1

// ---------------- SAD HUNGRY ANIMATION ----------------
// 96x64, SSD1306 vertical pages, LSB = top
// bit 1 = black, bit 0 = white

#define SAD_HUNGRY_FRAME_COUNT 2

const uint8_t* const sadHungryFrames[] PROGMEM = {
  sadHungrySprite0,
  sadHungrySprite1
};

const Animation sadHungryAnimation = {
  "Sad Hungry",
  sadHungryFrames,
  SAD_HUNGRY_FRAME_COUNT,
  sadHungrySprite0Width,
  sadHungrySprite0Height,
  SPRITE_FORMAT_VERTICAL_LSB,
  false,
  77,
  34,
  500
};
