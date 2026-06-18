#pragma once

#include "animations.h"

#define sprite_0_width sadPlaySprite0Width
#define sprite_0_height sadPlaySprite0Height
#define sprite_0 sadPlaySprite0
#define sprite_1_width sadPlaySprite1Width
#define sprite_1_height sadPlaySprite1Height
#define sprite_1 sadPlaySprite1
#include "sad_play_images_raw.h"
#undef sprite_0_width
#undef sprite_0_height
#undef sprite_0
#undef sprite_1_width
#undef sprite_1_height
#undef sprite_1

// ---------------- SAD PLAY ANIMATION ----------------
// 96x64, SSD1306 vertical pages, LSB = top
// bit 1 = black, bit 0 = white

#define SAD_PLAY_FRAME_COUNT 2

const uint8_t* const sadPlayFrames[] PROGMEM = {
  sadPlaySprite0,
  sadPlaySprite1
};

const Animation sadPlayAnimation = {
  "Sad Play",
  sadPlayFrames,
  SAD_PLAY_FRAME_COUNT,
  sadPlaySprite0Width,
  sadPlaySprite0Height,
  SPRITE_FORMAT_VERTICAL_LSB,
  false,
  77,
  34,
  500
};
