#pragma once

#include "animations.h"

#define sprite_0_width sadPeeSprite0Width
#define sprite_0_height sadPeeSprite0Height
#define sprite_0 sadPeeSprite0
#define sprite_1_width sadPeeSprite1Width
#define sprite_1_height sadPeeSprite1Height
#define sprite_1 sadPeeSprite1
#include "sad_pee_images_raw.h"
#undef sprite_0_width
#undef sprite_0_height
#undef sprite_0
#undef sprite_1_width
#undef sprite_1_height
#undef sprite_1

// ---------------- SAD PEE ANIMATION ----------------
// 96x64, SSD1306 vertical pages, LSB = top
// bit 1 = black, bit 0 = white

#define SAD_PEE_FRAME_COUNT 2

const uint8_t* const sadPeeFrames[] PROGMEM = {
  sadPeeSprite0,
  sadPeeSprite1
};

const Animation sadPeeAnimation = {
  "Sad Pee",
  sadPeeFrames,
  SAD_PEE_FRAME_COUNT,
  sadPeeSprite0Width,
  sadPeeSprite0Height,
  SPRITE_FORMAT_VERTICAL_LSB,
  false,
  77,
  34,
  500
};
