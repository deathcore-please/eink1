#pragma once

#include <stdint.h>

namespace PetCare {

enum Category : uint8_t { Overall, Pee, Play, Food, Count };
enum Mode : uint8_t { Normal, Happy, Sad };
constexpr uint32_t DaySeconds = 24UL * 3600UL;

struct Rule {
  uint8_t happyThreshold;
  bool happyInclusive;
  uint8_t lowThreshold;
  uint8_t dipsRequired;
  uint8_t recoveryThreshold;
  uint32_t recoverySeconds;
  uint8_t happyRatePercent;
  uint8_t sadRatePercent;
};

extern const Rule Rules[Count];

// Plain retained data: no constructors that could overwrite it on RTC wake.
struct CategoryState {
  Mode qualified;
  Mode applied;
  uint32_t happySince;
  uint32_t recoverySince;
  uint32_t dips[3];
  uint8_t dipCount;
  bool high;
  bool recovering;
  bool low;
};

struct State {
  CategoryState categories[Count];
  bool initialized;
};

struct Notice {
  Category category;
  Mode from;
  Mode to;
};

void observe(State& state, const uint8_t values[Count], uint32_t nowEpoch);
uint64_t nextDeadline(const State& state, uint32_t nowEpoch);
bool nextNotice(const State& state, Notice& notice);
bool acknowledge(State& state, const Notice& notice);
uint8_t ratePercent(const State& state, Category category);

}  // namespace PetCare
