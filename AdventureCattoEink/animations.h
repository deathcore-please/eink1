#pragma once

#include <Arduino.h>
#include <pgmspace.h>

enum SpriteFormat {
  SPRITE_FORMAT_HORIZONTAL_MSB,
  SPRITE_FORMAT_VERTICAL_LSB
};

struct Animation {
  const char* name;
  const uint8_t* const* frames;
  uint8_t frameCount;
  uint16_t width;
  uint16_t height;
  SpriteFormat format;
  bool inverted;

  uint16_t x;
  uint16_t y;

  uint16_t frameHoldMs;
};