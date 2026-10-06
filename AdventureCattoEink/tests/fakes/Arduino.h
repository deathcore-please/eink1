#pragma once
#include <stdint.h>
#include <string>

class String : public std::string {
 public:
  using std::string::string;
  using std::string::operator=;
};

inline uint32_t fakeTraceMillis;
inline unsigned long millis() { return fakeTraceMillis; }
