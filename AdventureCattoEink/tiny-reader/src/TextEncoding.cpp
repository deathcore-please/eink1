#include "TextEncoding.h"

size_t utf8CharLen(uint8_t b) {
  if ((b & 0x80) == 0) {
    return 1;
  }
  if ((b & 0xE0) == 0xC0) {
    return 2;
  }
  if ((b & 0xF0) == 0xE0) {
    return 3;
  }
  if ((b & 0xF8) == 0xF0) {
    return 4;
  }
  return 1;
}

bool utf8IsContinuation(uint8_t b) {
  return (b & 0xC0) == 0x80;
}

static bool decodeUtf8(const String& raw, size_t index, uint32_t& codePoint, size_t& consumed) {
  if (index >= raw.length()) {
    consumed = 0;
    return false;
  }

  uint8_t b0 = static_cast<uint8_t>(raw[index]);
  if (b0 < 0x80) {
    codePoint = b0;
    consumed = 1;
    return true;
  }

  size_t len = utf8CharLen(b0);
  if (index + len > raw.length()) {
    codePoint = b0;
    consumed = 1;
    return false;
  }

  for (size_t j = 1; j < len; ++j) {
    if (!utf8IsContinuation(static_cast<uint8_t>(raw[index + j]))) {
      codePoint = b0;
      consumed = 1;
      return false;
    }
  }

  if (len == 2) {
    codePoint = ((static_cast<uint32_t>(b0) & 0x1F) << 6) |
                (static_cast<uint8_t>(raw[index + 1]) & 0x3F);
  } else if (len == 3) {
    codePoint = ((static_cast<uint32_t>(b0) & 0x0F) << 12) |
                ((static_cast<uint8_t>(raw[index + 1]) & 0x3F) << 6) |
                (static_cast<uint8_t>(raw[index + 2]) & 0x3F);
  } else {
    codePoint = ((static_cast<uint32_t>(b0) & 0x07) << 18) |
                ((static_cast<uint8_t>(raw[index + 1]) & 0x3F) << 12) |
                ((static_cast<uint8_t>(raw[index + 2]) & 0x3F) << 6) |
                (static_cast<uint8_t>(raw[index + 3]) & 0x3F);
  }

  consumed = len;
  return true;
}

static char mapCodePointToDisplay(uint32_t cp) {
  if (cp == 0xFEFF) {
    return '\0';
  }

  if (cp == 0x2018 || cp == 0x2019 || cp == 0x91 || cp == 0x92) {
    return '\'';
  }
  if (cp == 0x201C || cp == 0x201D || cp == 0x93 || cp == 0x94) {
    return '"';
  }
  if (cp == 0x2013 || cp == 0x2014 || cp == 0x96 || cp == 0x97) {
    return '-';
  }
  if (cp == 0xFF3F) {
    return '_';
  }

  if (cp < 0x80) {
    return static_cast<char>(cp);
  }
  if (cp >= 0xA0 && cp <= 0xFE) {
    return static_cast<char>(cp);
  }

  return '?';
}

String toDisplayText(const String& raw, size_t start, size_t end) {
  if (start >= end || start >= raw.length()) {
    return String();
  }
  if (end > raw.length()) {
    end = raw.length();
  }

  String out;
  out.reserve(end - start);

  for (size_t i = start; i < end;) {
    if (static_cast<uint8_t>(raw[i]) == 0xEF && i + 2 < end &&
        static_cast<uint8_t>(raw[i + 1]) == 0xBB &&
        static_cast<uint8_t>(raw[i + 2]) == 0xBF) {
      i += 3;
      continue;
    }

    uint32_t codePoint = 0;
    size_t consumed = 0;
    decodeUtf8(raw, i, codePoint, consumed);
    if (consumed == 0) {
      break;
    }

    char mapped = mapCodePointToDisplay(codePoint);
    if (mapped != '\0') {
      out += mapped;
    }
    i += consumed;
  }

  return out;
}

size_t trailingIncompleteUtf8Bytes(const char* data, size_t len) {
  if (len == 0 || data == nullptr) {
    return 0;
  }

  size_t i = len;
  while (i > 0 && utf8IsContinuation(static_cast<uint8_t>(data[i - 1]))) {
    i--;
  }

  if (i == len) {
    uint8_t lead = static_cast<uint8_t>(data[len - 1]);
    size_t needed = utf8CharLen(lead);
    if (needed > 1) {
      return 1;
    }
    return 0;
  }

  uint8_t lead = static_cast<uint8_t>(data[i]);
  size_t needed = utf8CharLen(lead);
  size_t have = len - i;
  if (needed > 1 && have < needed) {
    return have;
  }

  return 0;
}
