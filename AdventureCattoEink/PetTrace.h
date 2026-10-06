#pragma once

#include <stddef.h>
#include <stdint.h>
#include "PetNeeds.h"

namespace PetTrace {

struct Frame {
  uint32_t epoch;
  uint32_t millis;
  uint64_t uptimeUs;
  uint32_t lastDrainEpoch;
  uint32_t rtcEpoch;
  uint32_t rtcDrainEpoch;
  uint32_t birthEpoch;
  uint32_t sleepStartEpoch;
  uint32_t sleepDurationSec;
  int32_t wakeCause;
  int32_t resetReason;
  int32_t appMode;
  int32_t petMode;
  int32_t activeAction;
  bool introSeen;
  bool pendingRunaway;
  bool usbActive;
  PetNeeds::State needs;
};

constexpr size_t BankBytes = 64UL * 1024UL;
constexpr uint64_t SampleIntervalUs = 5ULL * 60ULL * 1000000ULL;

void begin(uint32_t bootId);
bool record(const char* event, const Frame& before, const Frame* after = nullptr,
            const char* detail = "");
bool drain(const Frame& before, const Frame& after);
bool info(size_t& bytes, size_t& entries);
bool read(uint32_t offset, uint8_t* buffer, size_t capacity, size_t& bytesRead, size_t& total);
bool clear();

}  // namespace PetTrace
