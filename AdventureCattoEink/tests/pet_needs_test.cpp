#include "../PetNeeds.h"
#include "test_support.h"

#include <algorithm>
#include <iostream>
#include <random>

using namespace PetNeeds;

void check(bool condition, const char* message) {
  TestSupport::require(condition, message);
}

State makeState(uint8_t pee = 80, uint8_t food = 80, uint8_t play = 80, uint8_t pets = 80) {
  State state = {{{pee, 0, 0}, {food, 0, 0}, {play, 0, 0}, {pets, 0, 0}}, 0, 0, 0, true, {}};
  refreshHappiness(state);
  return state;
}

void checkSame(const State& a, const State& b) {
  TestSupport::sameState(a, b, "bulk/stepped regression");
}

Need advanceInSteps(State& state, uint32_t from, uint32_t to, uint32_t step, uint32_t pauseEnd = 0) {
  Need firstLow = None;
  while (from < to) {
    uint32_t next = from + std::min(step, to - from);
    uint8_t previousHappiness = state.happiness;
    Need low = advance(state, from, next, pauseEnd);
    check(state.happiness <= previousHappiness, "time passing refilled happiness");
    if (firstLow == None) {
      firstLow = low;
    }
    from = next;
  }
  return firstLow;
}

void testLinearRatesAndReportedCase() {
  State state = makeState(100, 100, 100, 100);
  advance(state, 0, 3600);
  check(state.needs[Pee].value == 84 && state.needs[Play].value == 84, "six-hour drain rate");
  check(state.needs[Food].value == 75 && state.needs[Pets].value == 75, "four-hour drain rate");
  advance(state, 3600, 7200);
  check(state.needs[Pee].value == 67 && state.needs[Food].value == 50, "fractional drain must carry forward");
  advance(state, 7200, 14400);
  check(state.needs[Food].value == 0 && state.needs[Pets].value == 0, "four-hour full drain");
  advance(state, 14400, 21600);
  check(state.needs[Pee].value == 0 && state.needs[Play].value == 0, "six-hour full drain");

  state = makeState(50, 50, 25, 50);
  check(state.happiness == 45, "reported-case initial happiness");
  State continuouslyAwake = state;
  advance(state, 0, 45 * 60);
  advanceInSteps(continuouslyAwake, 0, 45 * 60, 1);
  checkSame(state, continuouslyAwake);
  check(state.happiness == 30 && !hasCriticalNeed(state), "45 percent must not collapse after 45 minutes");
  std::cout << "PASS: linear rates; example with play lowest goes 45% -> 30% after 45 minutes\n";
}

void testCrisisBoundaries() {
  State state = makeState(100, 0, 100, 100);
  for (auto& need : state.needs) {
    need.cooldownUntilEpoch = UINT32_MAX;
  }
  advance(state, 0, 2699);
  check(state.crisisPenalty == 0 && state.happiness == 60, "crisis rate is per point, not per full bar");
  advance(state, 2699, 2700);
  check(state.crisisPenalty == 1 && state.happiness == 59, "one empty critical need costs one point per 45 minutes");

  state = makeState(0, 0, 100, 100);
  for (auto& need : state.needs) {
    need.cooldownUntilEpoch = UINT32_MAX;
  }
  advance(state, 0, 1350);
  check(state.crisisPenalty == 1 && state.happiness == 29, "two empty critical needs double only the small crisis rate");

  state = makeState();
  State awake = state;
  advance(state, 0, 11520);
  advanceInSteps(awake, 0, 11520, 1);
  checkSame(state, awake);
  check(state.needs[Food].value == 0 && state.happiness == 14, "newly empty food must not cause retroactive runaway");
  check(state.crisisPenalty == 0 && state.crisisCarry == 0, "no crisis time before reaching zero");
  advance(state, 11520, 14220);
  check(state.crisisPenalty == 1 && state.happiness == 7, "crisis begins at actual zero crossing");

  state = makeState(100, 100, 0, 0);
  advance(state, 0, 2700);
  check(state.crisisPenalty == 0 && state.happiness > 0, "play and pets cannot create a crisis");
  std::cout << "PASS: crisis units, one/two critical needs, exact zero crossings, play/pets excluded\n";
}

void testHalfHealthCannotCollapseIn45Minutes() {
  unsigned checked = 0;
  for (uint8_t pee = 0; pee <= 100; pee += 5) {
    for (uint8_t food = 0; food <= 100; food += 5) {
      for (uint8_t play = 0; play <= 100; play += 5) {
        for (uint8_t pets = 0; pets <= 100; pets += 5) {
          State state = makeState(pee, food, play, pets);
          if (state.happiness < 40 || state.happiness > 50) {
            continue;
          }
          uint8_t previous = state.happiness;
          // Include a worst-case fractional position just before each next tick.
          for (int i = 0; i < Count; ++i) {
            state.needs[i].drainCarry = state.needs[i].value
              ? FullDrainSeconds[i] * DrainRateScale - 1 : 0;
          }
          advance(state, 0, 2700);
          check(state.happiness >= previous - 19, "near-half happiness fell faster than the configured rates allow");
          check(state.happiness > 0, "near-half happiness collapsed within 45 minutes");
          ++checked;
        }
      }
    }
  }
  check(checked == 45616, "all 45,616 near-half-health combinations must remain covered");
  std::cout << "PASS: " << checked << " near-half-health combinations survive 45 minutes\n";
}

void testCooldownsAndSleep() {
  State state = makeState(100, 100, 100, 100);
  state.needs[Pee].cooldownUntilEpoch = 3600;
  state.needs[Food].cooldownUntilEpoch = 3600;
  state.needs[Play].cooldownUntilEpoch = 1800;
  State awake = state;
  advance(state, 0, 16200);
  advanceInSteps(awake, 0, 16200, 1);
  checkSame(state, awake);
  check(state.needs[Pee].value == 42 && state.needs[Food].value == 13, "expired cooldown must protect its portion of deep sleep");
  check(state.needs[Play].value == 34 && state.happiness == 25, "cooldown expiry catch-up");

  state = makeState();
  state.needs[Food].cooldownUntilEpoch = 3600;
  observeCare(state, 0);
  State before = state;
  advance(state, 0, 3600, 7200);
  checkSame(state, before);
  advance(state, 3600, 7200, 7200);
  checkSame(state, before);
  advance(state, 7200, 9900, 7200);
  advance(before, 7200, 9900);
  checkSame(state, before);

  state = makeState();
  awake = state;
  advance(state, 0, 10000, 7200);
  advanceInSteps(awake, 0, 10000, 1, 7200);
  checkSame(state, awake);
  check(state.needs[Food].value < 80, "drain resumes after scheduled sleep even before hardware wakes");

  state = makeState();
  state.started = false;
  before = state;
  advance(state, 0, 1000000);
  checkSame(state, before);
  state.started = true;
  advance(state, 1000000, 1000144);
  check(state.needs[Food].value == 79, "fresh intro starts at a new drain epoch");

  before = state;
  advance(state, 1000144, 1000144);
  advance(state, 1000144, 999999);
  checkSame(state, before);
  std::cout << "PASS: cooldowns, scheduled sleep, no drain before intro, non-forward clocks\n";
}

void testActivitiesAndSadPriority() {
  State state = makeState(50, 0, 20, 20);
  state.crisisPenalty = 3;
  state.crisisCarry = 123 * CrisisRateScale;
  state.needs[Play].drainCarry = 21000 * DrainRateScale;
  refreshHappiness(state);
  uint8_t happinessBefore = state.happiness;
  State before = state;
  refill(state, Play, 1800, 0);
  check(state.needs[Play].value == 100 && state.needs[Play].drainCarry == 0, "refill starts selected bar at exactly 100");
  check(state.happiness == happinessBefore + 16, "play still refills weighted happiness during food crisis");
  check(state.crisisPenalty == 3 && state.crisisCarry == 123 * CrisisRateScale, "noncritical refill does not erase crisis history");
  for (Need need : {Pee, Food, Pets}) {
    check(state.needs[need].value == before.needs[need].value, "activity refilled unrelated need");
    check(state.needs[need].drainCarry == before.needs[need].drainCarry, "activity changed unrelated drain timer");
  }
  happinessBefore = state.happiness;
  refill(state, Pets, 0, 0);
  check(state.happiness == happinessBefore + 8, "petting still refills weighted happiness");
  refill(state, Food, 3600, 0);
  check(state.crisisPenalty == 0 && state.crisisCarry == 0, "critical recovery clears crisis");

  state = makeState(10, 10, 100, 100);
  state.needs[Food].cooldownUntilEpoch = 1000;
  check(advance(state, 0, 2000) == Pee, "sad priority must include cooldown time");
  state = makeState(10, 10, 100, 100);
  state.needs[Pee].drainCarry = 20000 * DrainRateScale;
  check(advance(state, 0, 2000) == Pee, "sad priority must include fractional drain time");
  std::cout << "PASS: isolated refills, happiness recovery, exact sad-animation priority\n";
}

void testUpdateFrequencyAndLongAbsences() {
  std::mt19937 random(128543);
  const uint32_t epoch = 1789405993;
  for (int trial = 0; trial < 500; ++trial) {
    State bulk = makeState();
    for (int i = 0; i < Count; ++i) {
      bulk.needs[i].value = static_cast<uint8_t>(random() % 101);
      bulk.needs[i].drainCarry = bulk.needs[i].value
        ? random() % (FullDrainSeconds[i] * DrainRateScale) : 0;
      bulk.needs[i].cooldownUntilEpoch = i == Pets ? 0 : epoch + random() % 7200;
    }
    bulk.crisisPenalty = static_cast<uint8_t>(random() % 15);
    bulk.crisisCarry = random() % (CrisisSecondsPerPointPerNeed * CrisisRateScale);
    refreshHappiness(bulk);
    State stepped = bulk;
    uint32_t end = epoch + 1 + random() % (48 * 3600);
    uint32_t pauseEnd = trial % 3 == 0 ? epoch + random() % 36000 : 0;
    Need bulkFirst = advance(bulk, epoch, end, pauseEnd);
    Need steppedFirst = advanceInSteps(stepped, epoch, end, 1 + random() % 300, pauseEnd);
    checkSame(bulk, stepped);
    check(bulkFirst == steppedFirst, "sad priority depends on update frequency");
  }

  State state = makeState();
  State stepped = state;
  advance(state, epoch, epoch + 58909);
  advanceInSteps(stepped, epoch, epoch + 58909, 1);
  checkSame(state, stepped);
  check(state.happiness == 0 && hasCriticalNeed(state), "logged overnight absence still permits runaway");

  state = makeState();
  advance(state, 1, UINT32_MAX);
  check(state.happiness == 0 && state.crisisPenalty == 100, "very long absence must saturate without overflow");
  for (const auto& need : state.needs) {
    check(need.value == 0 && need.drainCarry == 0, "empty needs must saturate");
  }
  std::cout << "PASS: 500 bulk/stepped comparisons, logged overnight interval, long-interval overflow\n";
}

int main() {
  TestSupport::Runner runner;
  runner.run("linear rates and reported case", testLinearRatesAndReportedCase);
  runner.run("crisis boundaries", testCrisisBoundaries);
  runner.run("45,616 half-health cases", testHalfHealthCannotCollapseIn45Minutes);
  runner.run("cooldowns and sleep", testCooldownsAndSleep);
  runner.run("activities and sad priority", testActivitiesAndSadPriority);
  runner.run("update frequency and long absences", testUpdateFrequencyAndLongAbsences);
  return runner.finish();
}
