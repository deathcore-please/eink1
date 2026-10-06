#include "PetNeeds.h"

namespace PetNeeds {

bool hasCriticalNeed(const State& state) {
  return state.needs[Food].value == 0 || state.needs[Pee].value == 0;
}

uint8_t weightedHappiness(const State& state) {
  uint16_t sum = 4u * state.needs[Food].value + 3u * state.needs[Pee].value +
                 2u * state.needs[Play].value + state.needs[Pets].value;
  return static_cast<uint8_t>((sum + 5) / 10);
}

void refreshHappiness(State& state) {
  if (!hasCriticalNeed(state)) {
    state.crisisPenalty = 0;
    state.crisisCarry = 0;
  }
  uint8_t weighted = weightedHappiness(state);
  state.happiness = weighted > state.crisisPenalty
                      ? static_cast<uint8_t>(weighted - state.crisisPenalty) : 0;
}

void observeCare(State& state, uint32_t nowEpoch) {
  if (!state.started) {
    return;
  }
  const uint8_t values[PetCare::Count] = {
    state.happiness, state.needs[Pee].value,
    state.needs[Play].value, state.needs[Food].value
  };
  PetCare::observe(state.care, values, nowEpoch);
}

void refill(State& state, Need need, uint32_t cooldownUntilEpoch, uint32_t nowEpoch) {
  if (need < Pee || need >= Count) {
    return;
  }
  state.needs[need] = {static_cast<uint8_t>(MaxValue), 0, cooldownUntilEpoch};
  refreshHappiness(state);
  observeCare(state, nowEpoch);
}

uint32_t needRatePercentSquared(const State& state, Need need) {
  const PetCare::Category categories[Count] = {
    PetCare::Pee, PetCare::Food, PetCare::Play, PetCare::Count
  };
  if (need < Pee || need >= Count) {
    return DrainRateScale;
  }
  return static_cast<uint32_t>(PetCare::ratePercent(state.care, PetCare::Overall)) *
         PetCare::ratePercent(state.care, categories[need]);
}

static uint32_t later(uint32_t a, uint32_t b) { return a > b ? a : b; }

static uint64_t secondsToPoint(uint32_t denominator, uint32_t carry, uint32_t rate) {
  return (static_cast<uint64_t>(denominator) - carry + rate - 1) / rate;
}

Need advance(State& state, uint32_t fromEpoch, uint32_t toEpoch,
             uint32_t pausedUntilEpoch) {
  if (!state.started || toEpoch <= fromEpoch) {
    return None;
  }
  refreshHappiness(state);
  observeCare(state, fromEpoch);
  Need firstLow = None;
  uint32_t rates[Count];
  for (int i = 0; i < Count; ++i) {
    rates[i] = MaxValue * needRatePercentSquared(state, static_cast<Need>(i));
  }
  const uint32_t crisisDenominator = CrisisSecondsPerPointPerNeed * CrisisRateScale;
  const uint32_t overallRate = PetCare::ratePercent(state.care, PetCare::Overall);

  // Jump between percentage changes and care deadlines, never elapsed seconds.
  // Applied rates cannot change here: only a separate acknowledgement changes them.
  while (fromEpoch < toEpoch) {
    uint64_t next = toEpoch;
    uint64_t deadline = PetCare::nextDeadline(state.care, fromEpoch);
    if (deadline < next) {
      next = deadline;
    }
    uint32_t activeStart = later(fromEpoch, pausedUntilEpoch);
    for (int i = 0; i < Count; ++i) {
      const NeedState& need = state.needs[i];
      if (need.value == 0) {
        continue;
      }
      uint32_t start = later(activeStart, need.cooldownUntilEpoch);
      uint64_t point = static_cast<uint64_t>(start) + secondsToPoint(
        FullDrainSeconds[i] * DrainRateScale, need.drainCarry, rates[i]);
      if (point < next) {
        next = point;
      }
    }
    uint32_t criticalCount = (state.needs[Food].value == 0 ? 1u : 0u) +
                             (state.needs[Pee].value == 0 ? 1u : 0u);
    uint32_t crisisRate = criticalCount * overallRate;
    if (crisisRate && state.crisisPenalty < MaxValue) {
      uint64_t point = static_cast<uint64_t>(activeStart) +
        secondsToPoint(crisisDenominator, state.crisisCarry, crisisRate);
      if (point < next) {
        next = point;
      }
    }
    uint32_t end = static_cast<uint32_t>(next);

    // Only critical needs already empty at the segment's start accrue crisis time.
    if (crisisRate && end > activeStart) {
      uint64_t units = state.crisisCarry + static_cast<uint64_t>(end - activeStart) * crisisRate;
      uint64_t penalty = state.crisisPenalty + units / crisisDenominator;
      state.crisisPenalty = static_cast<uint8_t>(penalty < MaxValue ? penalty : MaxValue);
      state.crisisCarry = static_cast<uint32_t>(units % crisisDenominator);
    }
    for (int i = 0; i < Count; ++i) {
      NeedState& need = state.needs[i];
      if (!need.value) {
        need.drainCarry = 0;
        continue;
      }
      uint32_t start = later(activeStart, need.cooldownUntilEpoch);
      if (end <= start) {
        continue;
      }
      uint32_t denominator = FullDrainSeconds[i] * DrainRateScale;
      uint64_t units = need.drainCarry + static_cast<uint64_t>(end - start) * rates[i];
      uint64_t ticks = units / denominator;
      uint8_t before = need.value;
      need.value = ticks >= before ? 0 : static_cast<uint8_t>(before - ticks);
      need.drainCarry = need.value ? static_cast<uint32_t>(units % denominator) : 0;
      if (firstLow == None && before >= SadThreshold && need.value < SadThreshold) {
        firstLow = static_cast<Need>(i);
      }
    }
    refreshHappiness(state);
    observeCare(state, end);
    fromEpoch = end;
  }
  return firstLow;
}

}  // namespace PetNeeds
