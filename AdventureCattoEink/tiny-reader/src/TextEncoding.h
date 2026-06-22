#pragma once

#include <Arduino.h>

size_t utf8CharLen(uint8_t b);
bool utf8IsContinuation(uint8_t b);

// Decode UTF-8 / smart punctuation into plain ASCII for reader display.
String toDisplayText(const String& raw, size_t start, size_t end);

inline String toDisplayText(const String& raw) {
  return toDisplayText(raw, 0, raw.length());
}

// If buffer ends mid UTF-8 sequence, return how many trailing bytes to drop.
size_t trailingIncompleteUtf8Bytes(const char* data, size_t len);
