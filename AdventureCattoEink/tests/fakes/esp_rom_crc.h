#pragma once

#include <stddef.h>
#include <stdint.h>

inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t* bytes, size_t size) {
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}
