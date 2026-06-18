#pragma once

#include "animations.h"

#define sprite_0_width sadPetSprite0Width
#define sprite_0_height sadPetSprite0Height
#define sprite_0 sadPetSprite0
#define sprite_1_width sadPetSprite1Width
#define sprite_1_height sadPetSprite1Height
#define sprite_1 sadPetSprite1
#include "sad_pet_images_raw.h"
#undef sprite_0_width
#undef sprite_0_height
#undef sprite_0
#undef sprite_1_width
#undef sprite_1_height
#undef sprite_1

// ---------------- SAD PETS ANIMATION ----------------
// 96x64, SSD1306 vertical pages, LSB = top
// bit 1 = black, bit 0 = white

#define SAD_PET_FRAME_COUNT 2

const uint8_t* const sadPetFrames[] PROGMEM = {
  sadPetSprite0,
  sadPetSprite1
};

const Animation sadPetAnimation = {
  "Sad Pets",
  sadPetFrames,
  SAD_PET_FRAME_COUNT,
  sadPetSprite0Width,
  sadPetSprite0Height,
  SPRITE_FORMAT_VERTICAL_LSB,
  false,
  77,
  34,
  500
};
