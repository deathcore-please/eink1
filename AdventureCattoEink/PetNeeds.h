#pragma once

#include <stdint.h>
#include "PetCare.h"

namespace PetNeeds {

enum Need { None = -1, Pee, Food, Play, Pets, Count };

constexpr uint32_t MaxValue = 100;
constexpr uint8_t SadThreshold = 10;
// Full 100-to-0 drain time, excluding activity cooldowns and scheduled pet sleep.
constexpr uint32_t FullDrainSeconds[Count] = {
  6UL * 3600UL, 4UL * 3600UL, 6UL * 3600UL, 4UL * 3600UL
};
constexpr uint32_t CrisisSecondsPerPointPerNeed = 45UL * 60UL;
// Stable carry units preserve fractional progress when acknowledged rates change.
constexpr uint32_t DrainRateScale = 10000;
constexpr uint32_t CrisisRateScale = 100;

struct NeedState {
  uint8_t value;
  uint32_t drainCarry;
  uint32_t cooldownUntilEpoch;
};

struct State {
  NeedState needs[Count];
  uint8_t happiness;
  uint8_t crisisPenalty;
  uint32_t crisisCarry;
  bool started;
  PetCare::State care;
};

bool hasCriticalNeed(const State& state);
uint8_t weightedHappiness(const State& state);
void refreshHappiness(State& state);
void observeCare(State& state, uint32_t nowEpoch);
void refill(State& state, Need need, uint32_t cooldownUntilEpoch, uint32_t nowEpoch);
uint32_t needRatePercentSquared(const State& state, Need need);

// Advances (fromEpoch, toEpoch], pausing only up to the scheduled sleep end.
// Returns the first need to cross below the sad threshold in this interval.
Need advance(State& state, uint32_t fromEpoch, uint32_t toEpoch,
             uint32_t pausedUntilEpoch = 0);

}  // namespace PetNeeds
