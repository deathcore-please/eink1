#include "../PetCare.h"
#include "../PetNeeds.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

namespace C = PetCare;
namespace N = PetNeeds;
using TestSupport::require;
using TestSupport::sameCare;
using TestSupport::sameState;

namespace {

constexpr uint32_t Day = 86400;
constexpr uint32_t Epoch = 1789405993;
using Values = std::array<uint8_t, C::Count>;
constexpr Values Neutral = {{40, 50, 50, 40}};
constexpr Values High = {{100, 100, 100, 100}};
constexpr C::Rule ExpectedRules[C::Count] = {
  {50, true, 30, 2, 30, 2 * Day, 70, 130},
  {70, false, 30, 2, 40, Day, 60, 140},
  {80, false, 20, 2, 40, Day, 50, 150},
  {50, false, 30, 3, 40, Day, 70, 130}
};

static_assert(C::DaySeconds == Day, "Care uses elapsed 24-hour days");
static_assert(N::DrainRateScale == 10000, "Need carries use hundredths of a percent");
static_assert(N::CrisisRateScale == 100, "Crisis carries use percent units");
static_assert(std::is_trivial<C::CategoryState>::value, "RTC categories must be trivial");
static_assert(std::is_standard_layout<C::CategoryState>::value, "RTC category layout");
static_assert(std::is_trivial<C::State>::value, "RTC care must have no constructors");
static_assert(std::is_standard_layout<C::State>::value, "RTC care layout");
static_assert(std::is_trivial<N::State>::value, "RTC needs must have no constructors");
static_assert(std::is_standard_layout<N::State>::value, "RTC needs layout");
static_assert(std::is_trivially_copyable<N::State>::value, "RTC needs can be copied");

std::string where(C::Category category, const char* detail) {
  return "category " + std::to_string(category) + ": " + detail;
}

void observe(C::State& state, const Values& values, uint32_t now) {
  C::observe(state, values.data(), now);
}

// Standalone callers must service deadlines before observing a later value change.
// PetNeeds::advance owns this chronological scheduling in the integrated model.
void hold(C::State& state, const Values& values, uint32_t from, uint32_t to) {
  require(to >= from, "invalid standalone hold interval");
  while (from < to) {
    const uint64_t deadline = C::nextDeadline(state, from);
    const uint32_t next = deadline <= to ? static_cast<uint32_t>(deadline) : to;
    require(next > from, "standalone deadline did not move forward");
    observe(state, values, next);
    from = next;
  }
}

C::Notice notice(const C::State& state, C::Category category, C::Mode from, C::Mode to) {
  C::Notice result = {};
  require(C::nextNotice(state, result), where(category, "expected a pending notice"));
  require(result.category == category && result.from == from && result.to == to,
          where(category, "wrong notice category or transition"));
  return result;
}

void ack(C::State& state, C::Category category, C::Mode from, C::Mode to) {
  require(C::acknowledge(state, notice(state, category, from, to)), "valid acknowledgement rejected");
}

void noNotice(const C::State& state) {
  C::Notice result = {};
  require(!C::nextNotice(state, result), "unexpected pending notice");
}

void ackAll(C::State& state) {
  C::Notice next = {};
  unsigned count = 0;
  while (C::nextNotice(state, next)) {
    require(++count <= 2 * C::Count, "notice queue did not settle");
    require(C::acknowledge(state, next), "current notice could not be acknowledged");
  }
}

struct Scenario {
  C::State state = {};
  Values values = Neutral;
  uint32_t now = Epoch;
};

Scenario sadCategory(C::Category category, uint32_t start = Epoch) {
  Scenario scenario;
  scenario.now = start;
  observe(scenario.state, scenario.values, scenario.now);
  const auto& rule = ExpectedRules[category];
  for (unsigned dip = 0; dip < rule.dipsRequired; ++dip) {
    scenario.values[category] = static_cast<uint8_t>(rule.lowThreshold - 1);
    observe(scenario.state, scenario.values, ++scenario.now);
    if (dip + 1 < rule.dipsRequired) {
      scenario.values[category] = rule.lowThreshold;
      observe(scenario.state, scenario.values, ++scenario.now);
    }
  }
  require(scenario.state.categories[category].qualified == C::Sad,
          where(category, "crossing fixture did not become sad"));
  return scenario;
}

C::State allSad(uint32_t now) {
  C::State state = {};
  observe(state, Neutral, now - 6);
  for (uint32_t dip = 0; dip < 3; ++dip) {
    observe(state, {{0, 0, 0, 0}}, now - 5 + dip * 2);
    if (dip != 2) {
      observe(state, Neutral, now - 4 + dip * 2);
    }
  }
  ackAll(state);
  for (const auto& category : state.categories) {
    require(category.qualified == C::Sad && category.applied == C::Sad, "all-sad fixture");
  }
  return state;
}

N::State needs(uint8_t pee = 100, uint8_t food = 100, uint8_t play = 100, uint8_t pets = 100) {
  N::State state = {};
  const uint8_t values[N::Count] = {pee, food, play, pets};
  for (int i = 0; i < N::Count; ++i) {
    state.needs[i].value = values[i];
  }
  state.started = true;
  N::refreshHappiness(state);
  return state;
}

void freeze(N::State& state) {
  for (auto& need : state.needs) {
    need.cooldownUntilEpoch = UINT32_MAX;
  }
}

uint32_t denominator(int need) {
  return N::FullDrainSeconds[need] * N::DrainRateScale;
}

uint32_t expectedRate(C::Category category, C::Mode mode) {
  return mode == C::Happy ? ExpectedRules[category].happyRatePercent
       : mode == C::Sad ? ExpectedRules[category].sadRatePercent : 100;
}

C::Category careCategory(int need) {
  constexpr C::Category map[N::Count] = {C::Pee, C::Food, C::Play, C::Count};
  return map[need];
}

void testRuleContract() {
  require(C::Overall == 0 && C::Pee == 1 && C::Play == 2 && C::Food == 3,
          "observation order is Overall, Pee, Play, Food");
  for (int i = 0; i < C::Count; ++i) {
    const auto& actual = C::Rules[i];
    const auto& expected = ExpectedRules[i];
    require(actual.happyThreshold == expected.happyThreshold &&
            actual.happyInclusive == expected.happyInclusive &&
            actual.lowThreshold == expected.lowThreshold &&
            actual.dipsRequired == expected.dipsRequired &&
            actual.recoveryThreshold == expected.recoveryThreshold &&
            actual.recoverySeconds == expected.recoverySeconds &&
            actual.happyRatePercent == expected.happyRatePercent &&
            actual.sadRatePercent == expected.sadRatePercent,
            where(static_cast<C::Category>(i), "approved rule changed"));
  }
  C::State empty = {};
  noNotice(empty);
  require(C::nextDeadline(empty, Epoch) == UINT64_MAX, "uninitialized care has no deadline");
  require(C::ratePercent(empty, C::Count) == 100, "no local modifier for the sentinel category");
}

void testHappyThresholds() {
  for (uint32_t start : {0u, Epoch}) {
    for (int i = 0; i < C::Count; ++i) {
      const auto category = static_cast<C::Category>(i);
      const auto& rule = ExpectedRules[i];
      for (unsigned value = 0; value <= 100; ++value) {
        C::State state = {};
        Values values = Neutral;
        values[i] = static_cast<uint8_t>(value);
        const bool high = rule.happyInclusive ? value >= rule.happyThreshold : value > rule.happyThreshold;
        observe(state, values, start);
        require(state.categories[i].high == high, where(category, "happy threshold exactness"));
        require(state.categories[i].dipCount == 0, "an initially low value is not a crossing");
        require(C::nextDeadline(state, start) == (high ? uint64_t{start} + Day : UINT64_MAX),
                where(category, "initial happy deadline"));
        hold(state, values, start, start + Day - 1);
        require(state.categories[i].qualified == C::Normal, "happy qualified a second early");
        hold(state, values, start + Day - 1, start + Day);
        require(state.categories[i].qualified == (high ? C::Happy : C::Normal),
                where(category, "happy must qualify at exactly 24 hours"));
        require(C::ratePercent(state, category) == 100, "qualification applied without acknowledgement");
        require(C::nextDeadline(state, start + Day) == UINT64_MAX, "completed happy timer repeated");
      }
    }
  }
}

void testHappyContinuityAndExpiry() {
  for (int i = 0; i < C::Count; ++i) {
    const auto category = static_cast<C::Category>(i);
    const auto& rule = ExpectedRules[i];
    C::State state = {};
    Values values = Neutral;
    values[i] = 100;
    observe(state, values, Epoch);
    const uint8_t boundary = static_cast<uint8_t>(rule.happyThreshold - (rule.happyInclusive ? 1 : 0));
    hold(state, values, Epoch, Epoch + Day - 2);
    values[i] = boundary;
    observe(state, values, Epoch + Day - 1);
    require(!state.categories[i].high && state.categories[i].happySince == 0,
            "dropping out of the high range must reset the streak");
    values[i] = 100;
    const uint32_t restart = Epoch + Day;
    observe(state, values, restart);
    require(C::nextDeadline(state, restart) == uint64_t{restart} + Day, "happy timer was not restarted");
    hold(state, values, restart, restart + Day - 1);
    noNotice(state);
    hold(state, values, restart + Day - 1, restart + Day);
    ack(state, category, C::Normal, C::Happy);
    hold(state, values, restart + Day, restart + 5 * Day);
    require(state.categories[i].happySince == restart, "repeated high days restarted the streak");
    require(C::ratePercent(state, category) == rule.happyRatePercent, "happy days stacked rates");
    noNotice(state);
    values[i] = boundary;
    observe(state, values, restart + 5 * Day + 1);
    require(state.categories[i].qualified == C::Normal, "happy did not expire at the threshold");
    notice(state, category, C::Happy, C::Normal);
    require(C::ratePercent(state, category) == rule.happyRatePercent, "expiry bypassed exit acknowledgement");
    ack(state, category, C::Happy, C::Normal);
    require(C::ratePercent(state, category) == 100, "happy exit did not restore the base rate");
    noNotice(state);
  }
}

void testDipThresholdsAndCrossings() {
  for (int i = 0; i < C::Count; ++i) {
    const auto category = static_cast<C::Category>(i);
    const auto& rule = ExpectedRules[i];
    for (unsigned value = 0; value <= 100; ++value) {
      C::State state = {};
      Values values = Neutral;
      observe(state, values, Epoch);
      values[i] = static_cast<uint8_t>(value);
      observe(state, values, Epoch + 1);
      require(state.categories[i].low == (value < rule.lowThreshold),
              where(category, "low threshold must be strict"));
      require(state.categories[i].dipCount == (value < rule.lowThreshold ? 1 : 0),
              where(category, "only downward crossings count"));
    }

    C::State state = {};
    Values values = Neutral;
    values[i] = static_cast<uint8_t>(rule.lowThreshold - 1);
    observe(state, values, 0);
    hold(state, values, 0, 3 * Day);
    require(state.categories[i].dipCount == 0 && state.categories[i].qualified == C::Normal,
            "initial low values or repeated low observations created dips");
    uint32_t now = 3 * Day;
    for (unsigned dip = 1; dip <= rule.dipsRequired; ++dip) {
      values[i] = rule.lowThreshold;
      observe(state, values, ++now);
      require(!state.categories[i].low, "exact low threshold must re-arm a crossing");
      values[i] = static_cast<uint8_t>(rule.lowThreshold - 1);
      observe(state, values, ++now);
      require(state.categories[i].dipCount == dip, "crossing count mismatch");
      for (unsigned repeat = 0; repeat < 5; ++repeat) {
        observe(state, values, ++now);
        require(state.categories[i].dipCount == dip, "remaining low counted extra dips");
      }
      require(state.categories[i].qualified == (dip == rule.dipsRequired ? C::Sad : C::Normal),
              where(category, "wrong two/three-dip requirement"));
    }
    ack(state, category, C::Normal, C::Sad);
    for (unsigned extra = 0; extra < 12; ++extra) {
      values[i] = rule.lowThreshold;
      observe(state, values, ++now);
      values[i] = static_cast<uint8_t>(rule.lowThreshold - 1);
      observe(state, values, ++now);
      require(state.categories[i].dipCount == rule.dipsRequired, "rolling dip storage exceeded its capacity");
      require(C::ratePercent(state, category) == rule.sadRatePercent, "sad crossings stacked modifiers");
      noNotice(state);
    }
    hold(state, values, now, now + 2 * Day);
    require(state.categories[i].dipCount == 0, "old dips were not pruned");
    require(state.categories[i].qualified == C::Sad, "sad mode expired without recovery");
  }
}

void testRollingDipWindows() {
  for (int i = 0; i < C::Count; ++i) {
    const auto category = static_cast<C::Category>(i);
    const auto& rule = ExpectedRules[i];
    for (uint32_t gap : {Day - 1, Day, Day + 1}) {
      C::State state = {};
      Values values = Neutral;
      const uint32_t first = Day - 10;
      observe(state, values, first - 1);
      values[i] = static_cast<uint8_t>(rule.lowThreshold - 1);
      observe(state, values, first);
      values[i] = rule.lowThreshold;
      observe(state, values, first + 1);
      if (rule.dipsRequired == 3) {
        values[i] = static_cast<uint8_t>(rule.lowThreshold - 1);
        observe(state, values, first + 10);
        values[i] = rule.lowThreshold;
        observe(state, values, first + 11);
      }
      values[i] = static_cast<uint8_t>(rule.lowThreshold - 1);
      observe(state, values, first + gap);
      const bool inside = gap < Day;
      require(state.categories[i].qualified == (inside ? C::Sad : C::Normal),
              where(category, "rolling 24h boundary must exclude a dip exactly 24h old"));
      require(state.categories[i].dipCount == rule.dipsRequired - (inside ? 0 : 1),
              "rolling-window count mismatch");
      for (unsigned d = state.categories[i].dipCount; d < 3; ++d) {
        require(state.categories[i].dips[d] == 0, "pruned dip slots retain stale timestamps");
      }
    }
    auto midnight = sadCategory(category, Day - 2);
    require(midnight.state.categories[i].qualified == C::Sad,
            "crossings straddling midnight must share a rolling window");
  }
}

void testRecoveryThresholdsAndContinuity() {
  for (int i = 0; i < C::Count; ++i) {
    const auto category = static_cast<C::Category>(i);
    const auto& rule = ExpectedRules[i];
    auto scenario = sadCategory(category);
    ack(scenario.state, category, C::Normal, C::Sad);
    scenario.values[i] = rule.recoveryThreshold;
    observe(scenario.state, scenario.values, ++scenario.now);
    const uint32_t thresholdEnd = scenario.now + rule.recoverySeconds + Day;
    hold(scenario.state, scenario.values, scenario.now, thresholdEnd);
    scenario.now = thresholdEnd;
    require(scenario.state.categories[i].qualified == C::Sad &&
            !scenario.state.categories[i].recovering, "recovery threshold must be strict");
    require(C::nextDeadline(scenario.state, scenario.now) == UINT64_MAX,
            "non-recovering sad mode must not schedule recovery");

    scenario.values[i] = static_cast<uint8_t>(rule.recoveryThreshold + 1);
    observe(scenario.state, scenario.values, ++scenario.now);
    const uint32_t interrupted = scenario.now + rule.recoverySeconds - 1;
    hold(scenario.state, scenario.values, scenario.now, interrupted);
    require(scenario.state.categories[i].qualified == C::Sad, "recovery completed early");
    scenario.values[i] = rule.recoveryThreshold;
    observe(scenario.state, scenario.values, interrupted);
    require(!scenario.state.categories[i].recovering && scenario.state.categories[i].recoverySince == 0,
            "one threshold observation must reset recovery");
    scenario.values[i] = static_cast<uint8_t>(rule.recoveryThreshold + 1);
    const uint32_t restart = interrupted + 1;
    observe(scenario.state, scenario.values, restart);
    require(C::nextDeadline(scenario.state, restart) == uint64_t{restart} + rule.recoverySeconds,
            "recovery restart deadline");
    hold(scenario.state, scenario.values, restart, restart + rule.recoverySeconds - 1);
    require(scenario.state.categories[i].qualified == C::Sad, "interrupted recovery time was reused");
    hold(scenario.state, scenario.values, restart + rule.recoverySeconds - 1, restart + rule.recoverySeconds);
    require(scenario.state.categories[i].qualified == C::Normal,
            where(category, "recovery must finish at exactly 48h overall / 24h otherwise"));
    require(!scenario.state.categories[i].recovering && scenario.state.categories[i].recoverySince == 0,
            "completed recovery timer was not cleared");
    require(C::ratePercent(scenario.state, category) == rule.sadRatePercent,
            "recovery applied without acknowledging its exit");
    ack(scenario.state, category, C::Sad, C::Normal);
    require(C::ratePercent(scenario.state, category) == 100, "sad exit did not restore base rate");
    noNotice(scenario.state);
  }
}

void testFreshHappyAfterRecovery() {
  for (int i = 0; i < C::Count; ++i) {
    const auto category = static_cast<C::Category>(i);
    for (bool delayedExit : {false, true}) {
      auto scenario = sadCategory(category);
      ack(scenario.state, category, C::Normal, C::Sad);
      scenario.values[i] = 100;
      observe(scenario.state, scenario.values, ++scenario.now);
      const uint32_t recovered = scenario.now + ExpectedRules[i].recoverySeconds;
      hold(scenario.state, scenario.values, scenario.now, recovered);
      const auto& state = scenario.state.categories[i];
      require(state.qualified == C::Normal && state.high && state.happySince == recovered,
              "recovery time must not double as happy time");
      if (!delayedExit) {
        ack(scenario.state, category, C::Sad, C::Normal);
      }
      hold(scenario.state, scenario.values, recovered, recovered + Day - 1);
      require(state.qualified == C::Normal, "fresh happy streak qualified early");
      hold(scenario.state, scenario.values, recovered + Day - 1, recovered + Day);
      require(state.qualified == C::Happy, "fresh happy streak did not qualify after one complete day");
      if (delayedExit) {
        require(C::ratePercent(scenario.state, category) == ExpectedRules[i].sadRatePercent,
                "unacknowledged sad modifier changed during fresh happy streak");
        ack(scenario.state, category, C::Sad, C::Normal);
      }
      require(C::ratePercent(scenario.state, category) == 100, "exit and entry must be separate acknowledgements");
      ack(scenario.state, category, C::Normal, C::Happy);
      require(C::ratePercent(scenario.state, category) == ExpectedRules[i].happyRatePercent,
              "fresh happy rate after sad recovery");
      noNotice(scenario.state);
    }
  }
}

void testNoticeOrderingAndStaleness() {
  C::State state = {};
  observe(state, High, Epoch);
  hold(state, High, Epoch, Epoch + Day);
  const auto first = notice(state, C::Overall, C::Normal, C::Happy);
  const auto before = state;
  for (C::Notice invalid : {C::Notice{C::Pee, C::Normal, C::Happy},
                           C::Notice{C::Overall, C::Sad, C::Happy},
                           C::Notice{C::Overall, C::Normal, C::Sad},
                           C::Notice{C::Count, C::Normal, C::Happy}}) {
    require(!C::acknowledge(state, invalid), "invalid/out-of-order notice was accepted");
    sameCare(state, before, "rejected acknowledgement must be a no-op");
  }
  for (int i = 0; i < C::Count; ++i) {
    ack(state, static_cast<C::Category>(i), C::Normal, C::Happy);
    require(!C::acknowledge(state, first), "duplicate acknowledgement was accepted");
  }
  noNotice(state);
  observe(state, Neutral, Epoch + Day + 1);
  for (int i = 0; i < C::Count; ++i) {
    ack(state, static_cast<C::Category>(i), C::Happy, C::Normal);
  }
  noNotice(state);

  state = {};
  observe(state, High, Epoch);
  hold(state, High, Epoch, Epoch + Day);
  const auto expired = notice(state, C::Overall, C::Normal, C::Happy);
  observe(state, Neutral, Epoch + Day + 1);
  noNotice(state);
  require(!C::acknowledge(state, expired), "expired unacknowledged happy entry was replayed");

  state = {};
  observe(state, High, Epoch);
  hold(state, High, Epoch, Epoch + Day);
  ackAll(state);
  observe(state, Neutral, Epoch + Day + 1);
  const auto staleExit = notice(state, C::Overall, C::Happy, C::Normal);
  observe(state, High, Epoch + Day + 2);
  hold(state, High, Epoch + Day + 2, Epoch + 2 * Day + 2);
  noNotice(state);
  require(!C::acknowledge(state, staleExit), "stale happy exit survived requalification");
}

void testHappyToSadNeedsTwoAcknowledgements() {
  for (int i = 0; i < C::Count; ++i) {
    const auto category = static_cast<C::Category>(i);
    const auto& rule = ExpectedRules[i];
    Values values = Neutral;
    values[i] = 100;
    C::State state = {};
    observe(state, values, Epoch);
    hold(state, values, Epoch, Epoch + Day);
    ack(state, category, C::Normal, C::Happy);
    uint32_t now = Epoch + Day;
    for (unsigned dip = 0; dip < rule.dipsRequired; ++dip) {
      values[i] = static_cast<uint8_t>(rule.lowThreshold - 1);
      observe(state, values, ++now);
      if (dip + 1 < rule.dipsRequired) {
        values[i] = rule.lowThreshold;
        observe(state, values, ++now);
      }
    }
    require(state.categories[i].qualified == C::Sad, "sad must override the previous happy qualification");
    require(C::ratePercent(state, category) == rule.happyRatePercent, "happy modifier changed before exit acknowledgement");
    require(!C::acknowledge(state, {category, C::Happy, C::Sad}), "direct happy-to-sad acknowledgement accepted");
    ack(state, category, C::Happy, C::Normal);
    require(C::ratePercent(state, category) == 100, "happy exit did not remove only the happy modifier");
    ack(state, category, C::Normal, C::Sad);
    require(C::ratePercent(state, category) == rule.sadRatePercent, "sad entry stacked with happy");
    noNotice(state);
  }
}

// These arithmetic fixtures seed applied RTC modes independently of qualification.
// Transition qualification itself is tested above through real observations.
void seedModes(N::State& state, unsigned combination) {
  state.care.initialized = true;
  for (int i = 0; i < C::Count; ++i) {
    const auto mode = static_cast<C::Mode>(combination % 3);
    combination /= 3;
    state.care.categories[i].applied = mode;
    state.care.categories[i].qualified = mode;
  }
}

void testMultipliedNeedRates() {
  for (unsigned combination = 0; combination < 81; ++combination) {
    for (uint32_t elapsed : {1u, 143u, 144u, 215u, 216u, 3599u, 7200u}) {
      auto state = needs(80, 80, 80, 80);
      seedModes(state, combination);
      const auto global = expectedRate(C::Overall, state.care.categories[C::Overall].applied);
      uint32_t rates[N::Count] = {};
      for (int i = 0; i < N::Count; ++i) {
        const auto category = careCategory(i);
        const uint32_t local = category == C::Count ? 100 : expectedRate(category, state.care.categories[category].applied);
        rates[i] = global * local;
        require(N::needRatePercentSquared(state, static_cast<N::Need>(i)) == rates[i],
                "need rates must multiply global and matching local percentages; pets are global-only");
        state.needs[i].drainCarry = denominator(i) - 17;
      }
      require(N::needRatePercentSquared(state, N::None) == N::DrainRateScale &&
              N::needRatePercentSquared(state, N::Count) == N::DrainRateScale, "invalid need rate sentinel");
      const auto before = state;
      N::advance(state, Epoch, Epoch + elapsed);
      for (int i = 0; i < N::Count; ++i) {
        const uint64_t units = uint64_t{before.needs[i].drainCarry} + uint64_t{elapsed} * 100 * rates[i];
        const auto ticks = units / denominator(i);
        const auto expectedValue = ticks >= 80 ? 0 : 80 - ticks;
        require(state.needs[i].value == expectedValue, "multiplied-rate drain value disagrees with integer oracle");
        require(state.needs[i].drainCarry == (expectedValue ? units % denominator(i) : 0),
                "multiplied-rate carry disagrees with integer oracle");
      }
    }
  }
  auto pending = needs();
  for (auto& category : pending.care.categories) {
    category.qualified = C::Sad;
  }
  for (int i = 0; i < N::Count; ++i) {
    require(N::needRatePercentSquared(pending, static_cast<N::Need>(i)) == 10000,
            "unacknowledged qualifications changed need rates");
  }
}

void testCrisisScaling() {
  constexpr uint32_t divisor = 2700 * 100;
  for (unsigned combination = 0; combination < 81; ++combination) {
    for (unsigned criticalMask : {1u, 2u, 3u}) {
      auto initial = needs(criticalMask & 1 ? 0 : 100, criticalMask & 2 ? 0 : 100);
      freeze(initial);
      seedModes(initial, combination);
      const uint32_t count = criticalMask == 3 ? 2 : 1;
      const uint32_t rate = count * expectedRate(C::Overall, initial.care.categories[C::Overall].applied);
      const uint32_t point = (divisor + rate - 1) / rate;
      for (uint32_t elapsed : {point - 1, point, point + 1, 12345u}) {
        auto state = initial;
        N::advance(state, Epoch, Epoch + elapsed);
        const uint64_t units = uint64_t{elapsed} * rate;
        require(state.crisisPenalty == units / divisor, "crisis rate must use global modifier once per empty critical need");
        require(state.crisisCarry == units % divisor, "scaled crisis fractional carry");
        const uint8_t weighted = N::weightedHappiness(state);
        require(state.happiness == (weighted > state.crisisPenalty ? weighted - state.crisisPenalty : 0),
                "scaled crisis happiness clamp");
      }
      auto paused = initial;
      paused.crisisPenalty = 7;
      paused.crisisCarry = divisor - 73;
      N::refreshHappiness(paused);
      N::advance(paused, Epoch, Epoch + 1777, Epoch + 777);
      const uint64_t units = uint64_t{divisor - 73} + 1000u * rate;
      require(paused.crisisPenalty == 7 + units / divisor && paused.crisisCarry == units % divisor,
              "sleep must exclude only paused crisis time, preserving scaled carry");
    }
  }
  auto state = needs(100, 100, 0, 0);
  seedModes(state, 80);
  freeze(state);
  N::advance(state, Epoch, Epoch + Day);
  require(state.crisisCarry == 0 && state.crisisPenalty == 0, "empty play/pets must never accrue crisis");
}

void changeApplied(C::State& state, C::Mode target) {
  state.initialized = true;
  for (auto& category : state.categories) {
    category.qualified = target;
  }
  ackAll(state);
}

void testCarryContinuityAcrossAcknowledgements() {
  auto state = needs();
  for (int i = 0; i < N::Count; ++i) {
    state.needs[i].drainCarry = denominator(i) / 3 + 19;
  }
  uint64_t accumulated[N::Count] = {};
  for (int i = 0; i < N::Count; ++i) {
    accumulated[i] = state.needs[i].drainCarry;
  }
  uint32_t now = Epoch;
  for (C::Mode mode : {C::Normal, C::Happy, C::Sad, C::Normal, C::Sad, C::Happy}) {
    const auto before = state;
    changeApplied(state.care, mode);
    auto onlyCareChanged = state;
    onlyCareChanged.care = before.care;
    sameState(onlyCareChanged, before, "acknowledgement must not renormalize carries or change needs");
    const uint32_t elapsed = 137;
    for (int i = 0; i < N::Count; ++i) {
      const auto category = careCategory(i);
      const uint32_t local = category == C::Count ? 100 : expectedRate(category, mode);
      accumulated[i] += uint64_t{elapsed} * 100 * expectedRate(C::Overall, mode) * local;
    }
    N::advance(state, now, now + elapsed);
    now += elapsed;
    for (int i = 0; i < N::Count; ++i) {
      require(state.needs[i].value == 100 - accumulated[i] / denominator(i), "rate-change drain continuity");
      require(state.needs[i].drainCarry == accumulated[i] % denominator(i), "rate-change fractional carry continuity");
    }
  }

  state = needs(100, 0);
  freeze(state);
  state.crisisCarry = 269999;
  uint64_t crisisUnits = state.crisisCarry;
  now = Epoch;
  for (C::Mode mode : {C::Normal, C::Happy, C::Sad, C::Normal, C::Happy, C::Sad}) {
    const auto carry = state.crisisCarry;
    const auto penalty = state.crisisPenalty;
    changeApplied(state.care, mode);
    require(state.crisisCarry == carry && state.crisisPenalty == penalty, "acknowledgement reset crisis history");
    crisisUnits += 777u * expectedRate(C::Overall, mode);
    N::advance(state, now, now + 777);
    now += 777;
    require(state.crisisPenalty == crisisUnits / 270000 && state.crisisCarry == crisisUnits % 270000,
            "crisis carry changed units across acknowledged rate changes");
  }
  const auto beforeRefill = state;
  N::refill(state, N::Play, now + 1000, now);
  require(state.crisisCarry == beforeRefill.crisisCarry && state.crisisPenalty == beforeRefill.crisisPenalty,
          "noncritical activity erased scaled crisis carry");
  N::refill(state, N::Food, now + 1000, now);
  require(state.crisisCarry == 0 && state.crisisPenalty == 0, "critical recovery must clear scaled crisis carry");
}

N::Need steppedAdvance(N::State& state, uint32_t from, uint32_t to, uint32_t pauseEnd, uint32_t step) {
  N::Need first = N::None;
  while (from < to) {
    const uint32_t next = from + std::min(step, to - from);
    const auto low = N::advance(state, from, next, pauseEnd);
    if (first == N::None) {
      first = low;
    }
    from = next;
  }
  return first;
}

void testChronologicalDeadlinesAndSleepingCare() {
  auto bulk = needs();
  freeze(bulk);
  bulk.care = allSad(Epoch);
  auto stepped = bulk;
  N::advance(bulk, Epoch, Epoch + 4 * Day, Epoch + 4 * Day);
  steppedAdvance(stepped, Epoch, Epoch + 4 * Day, Epoch + 4 * Day, 997);
  sameState(bulk, stepped, "bulk sleep must process recovery and then a fresh happy day");
  for (int i = 0; i < C::Count; ++i) {
    const auto& category = bulk.care.categories[i];
    require(category.qualified == C::Happy && category.applied == C::Sad,
            "care tracks wall time during scheduled sleep without applying unacknowledged rates");
    require(category.happySince == Epoch + ExpectedRules[i].recoverySeconds,
            "bulk advance skipped the exact recovery deadline");
  }
  ackAll(bulk.care);
  for (int i = 0; i < C::Count; ++i) {
    require(bulk.care.categories[i].applied == C::Happy, "ordered sad exits and happy entries did not settle");
  }

  for (uint32_t crossing : {Day - 1, Day, Day + 1}) {
    bulk = needs(100, 100, 81, 100);
    freeze(bulk);
    bulk.needs[N::Play].cooldownUntilEpoch = Epoch + crossing - 216;
    stepped = bulk;
    N::advance(bulk, Epoch, Epoch + Day);
    steppedAdvance(stepped, Epoch, Epoch + Day, 0, 113);
    sameState(bulk, stepped, "care deadline / drain tick ordering");
    require(bulk.care.categories[C::Play].qualified == (crossing > Day ? C::Happy : C::Normal),
            "a need outside its high range at the deadline cannot retain happy qualification");
    require(bulk.care.categories[C::Overall].qualified == C::Happy &&
            bulk.care.categories[C::Pee].qualified == C::Happy &&
            bulk.care.categories[C::Food].qualified == C::Happy, "unrelated frozen categories lost their high day");
  }
}

void testBeforeIntroAndObservationMapping() {
  auto state = needs(71, 51, 81, 0);
  require(state.happiness == 58, "mapping fixture weighted happiness");
  state.started = false;
  const auto before = state;
  N::observeCare(state, Epoch);
  require(N::advance(state, Epoch, Epoch + 5 * Day) == N::None, "pre-intro advance returned a crossing");
  sameState(state, before, "before intro no drain and no tracking");
  for (int i = 0; i < N::Count; ++i) {
    N::refill(state, static_cast<N::Need>(i), Epoch + 6 * Day, Epoch + 5 * Day);
    sameCare(state.care, before.care, "pre-intro refill must not start care tracking");
  }
  state = before;
  state.started = true;
  N::observeCare(state, Epoch + 5 * Day);
  for (const auto& category : state.care.categories) {
    require(category.high && category.happySince == Epoch + 5 * Day && category.qualified == C::Normal,
            "intro must start a fresh streak using Overall/Pee/Play/Food observation order");
  }
  const auto observed = state;
  N::advance(state, Epoch + 5 * Day, Epoch + 5 * Day);
  N::advance(state, Epoch + 5 * Day, Epoch);
  N::refill(state, N::None, 0, Epoch + 5 * Day);
  N::refill(state, N::Count, 0, Epoch + 5 * Day);
  sameState(state, observed, "non-forward advances and invalid refills must not track care");

  // Each need must map to its own category despite Food/Play having swapped enum order.
  state = needs(29, 51, 81, 100);
  N::observeCare(state, Epoch);
  require(state.care.categories[C::Pee].low && !state.care.categories[C::Pee].high &&
          state.care.categories[C::Play].high && state.care.categories[C::Food].high,
          "need-to-care observation order is incorrect");
  N::refill(state, N::Pee, Epoch + 1234, Epoch + 321);
  require(state.care.categories[C::Pee].high && !state.care.categories[C::Pee].low &&
          state.care.categories[C::Pee].happySince == Epoch + 321,
          "activity must observe care at the fourth refill argument, not cooldown expiry");
}

struct Event {
  uint32_t time;
  unsigned refillMask;
  std::array<uint32_t, N::Count> cooldowns;
  unsigned acknowledgements;
  uint32_t pauseUntil;
};

void testRandomIdenticalActivitiesAndAcknowledgements() {
  std::mt19937 schedules(0xCA4E2026);
  std::mt19937 sampling(0x57E99026);
  unsigned entries[3] = {};
  unsigned activities = 0;
  unsigned comparisons = 0;
  for (unsigned trial = 0; trial < 96; ++trial) {
    auto bulk = needs();
    freeze(bulk);
    if (trial % 2) {
      bulk.care = allSad(Epoch);
    }
    for (int i = 0; i < N::Count; ++i) {
      bulk.needs[i].drainCarry = schedules() % denominator(i);
    }
    auto stepped = bulk;
    std::vector<Event> events;
    for (uint32_t day = 1; day <= 3; ++day) {
      for (uint32_t offset : {day * Day - 1, day * Day, day * Day + 1}) {
        events.push_back({Epoch + offset, 0, {}, 1u + schedules() % 8, 0});
      }
    }
    uint32_t now = events.back().time;
    // End the protected introduction with every bar refilled and draining again.
    events.push_back({++now, 15, {}, 8, 0});
    for (unsigned event = 0; event < 100; ++event) {
      now += 1u + schedules() % (8 * 3600);
      Event next = {now, schedules() % 16, {}, schedules() % 9, 0};
      for (int i = 0; i < N::Count; ++i) {
        next.cooldowns[i] = i == N::Pets ? 0 : now + schedules() % 5401;
      }
      if (event % 11 == 0) {
        next.pauseUntil = now + schedules() % 18001;
      }
      events.push_back(next);
    }
    events.push_back({now + 4 * Day, 0, {}, 8, 0});

    uint32_t previous = Epoch;
    uint32_t pauseUntil = 0;
    for (size_t eventIndex = 0; eventIndex < events.size(); ++eventIndex) {
      const auto& event = events[eventIndex];
      const std::string context = "schedule seed CA4E2026 trial " + std::to_string(trial) +
                                 " event " + std::to_string(eventIndex);
      const auto bulkFirst = N::advance(bulk, previous, event.time, pauseUntil);
      const auto steppedFirst = steppedAdvance(stepped, previous, event.time, pauseUntil,
                                               17u + sampling() % 983);
      require(bulkFirst == steppedFirst, context + ": first low need depends on update frequency");
      sameState(bulk, stepped, context + " before activity");
      for (int i = 0; i < N::Count; ++i) {
        if (event.refillMask & (1u << i)) {
          N::refill(bulk, static_cast<N::Need>(i), event.cooldowns[i], event.time);
          N::refill(stepped, static_cast<N::Need>(i), event.cooldowns[i], event.time);
          sameState(bulk, stepped, context + " after refill " + std::to_string(i));
          ++activities;
        }
      }
      for (unsigned ackIndex = 0; ackIndex < event.acknowledgements; ++ackIndex) {
        C::Notice a = {}, b = {};
        const bool pendingA = C::nextNotice(bulk.care, a);
        const bool pendingB = C::nextNotice(stepped.care, b);
        require(pendingA == pendingB, context + ": pending notice differs");
        if (!pendingA) {
          break;
        }
        require(a.category == b.category && a.from == b.from && a.to == b.to, context + ": notice order differs");
        require(C::acknowledge(bulk.care, a) && C::acknowledge(stepped.care, b), context + ": acknowledgement failed");
        ++entries[a.to];
        sameState(bulk, stepped, context + " after acknowledgement");
      }
      previous = event.time;
      pauseUntil = event.pauseUntil;
      ++comparisons;
    }
  }
  require(activities > 10000 && comparisons > 10000, "random activity schedule coverage was reduced");
  require(entries[C::Normal] > 100 && entries[C::Happy] > 100 && entries[C::Sad] > 100,
          "random schedules must exercise acknowledged normal, happy, and sad transitions");
  std::cout << "  schedules: " << comparisons << " intervals, " << activities << " refills; acknowledged normal/happy/sad: "
            << entries[C::Normal] << '/' << entries[C::Happy] << '/' << entries[C::Sad] << '\n';
}

void testRtcPodRoundTrip() {
  auto original = needs(57, 0, 83, 91);
  original.care = allSad(Epoch);
  for (int i = 0; i < N::Count; ++i) {
    original.needs[i].drainCarry = original.needs[i].value ? denominator(i) / 2 + 97 : 0;
    original.needs[i].cooldownUntilEpoch = Epoch + 1234u * static_cast<uint32_t>(i);
  }
  original.crisisPenalty = 7;
  original.crisisCarry = 234567;
  N::refreshHappiness(original);
  N::observeCare(original, Epoch);
  auto& happy = original.care.categories[C::Play];
  happy.qualified = C::Happy;
  happy.applied = C::Normal;
  happy.high = true;
  happy.happySince = Epoch - Day;
  happy.recovering = false;
  happy.recoverySince = 0;

  std::array<unsigned char, sizeof(N::State)> rtc = {};
  std::memcpy(rtc.data(), &original, sizeof(original));
  N::State restored = {};
  std::memcpy(&restored, rtc.data(), sizeof(restored));
  sameState(original, restored, "RTC byte copy compares fields, never padding");
  C::State careRestored = {};
  std::array<unsigned char, sizeof(C::State)> careRtc = {};
  std::memcpy(careRtc.data(), &original.care, sizeof(original.care));
  std::memcpy(&careRestored, careRtc.data(), sizeof(careRestored));
  sameCare(original.care, careRestored, "standalone RTC care copy");

  ackAll(original.care);
  ackAll(restored.care);
  N::advance(original, Epoch, Epoch + 12345, Epoch + 999);
  steppedAdvance(restored, Epoch, Epoch + 12345, Epoch + 999, 37);
  sameState(original, restored, "restored RTC state must continue identically");
}

void testLongGapsAndEpochLimits() {
  const auto started = std::chrono::steady_clock::now();
  for (unsigned combination = 0; combination < 81; ++combination) {
    auto bulk = needs();
    seedModes(bulk, combination);
    for (int i = 0; i < N::Count; ++i) {
      bulk.needs[i].drainCarry = denominator(i) - 1;
    }
    auto stepped = bulk;
    N::advance(bulk, 1, UINT32_MAX);
    steppedAdvance(stepped, 1, UINT32_MAX, 0, 100000007);
    sameState(bulk, stepped, "136-year gap must preserve saturated crisis/care carry");
    require(bulk.happiness == 0 && bulk.crisisPenalty == 100, "long gaps must saturate happiness and penalty");
    require(bulk.crisisCarry < 270000, "long gaps overflowed scaled crisis carry");
    for (const auto& need : bulk.needs) {
      require(need.value == 0 && need.drainCarry == 0, "long gaps must clamp each empty need and carry");
    }
  }
  auto held = needs();
  freeze(held);
  N::advance(held, 1, UINT32_MAX);
  for (const auto& category : held.care.categories) {
    require(category.qualified == C::Happy && category.happySince == 1,
            "long protected high interval skipped its only qualification deadline");
  }

  C::State nearLimit = {};
  observe(nearLimit, High, UINT32_MAX - 10);
  const uint64_t deadline = uint64_t{UINT32_MAX - 10} + Day;
  require(C::nextDeadline(nearLimit, UINT32_MAX - 10) == deadline,
          "deadline arithmetic must not wrap at the 32-bit epoch limit");
  hold(nearLimit, High, UINT32_MAX - 10, UINT32_MAX);
  for (const auto& category : nearLimit.categories) {
    require(category.qualified == C::Normal, "epoch-end high streak qualified before 24 hours");
  }
  auto endState = needs();
  auto endStepped = endState;
  N::advance(endState, UINT32_MAX - 10000, UINT32_MAX);
  steppedAdvance(endStepped, UINT32_MAX - 10000, UINT32_MAX, 0, 1);
  sameState(endState, endStepped, "near-epoch-limit bulk/one-second updates");
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  require(elapsed < 5000, "long gaps must be bounded by state changes, not elapsed seconds (5-second budget)");
  std::cout << "  bounded long-gap checks: " << elapsed << " ms\n";
}

}  // namespace

int main() {
  TestSupport::Runner runner;
  runner.run("approved rule constants and category order", testRuleContract);
  runner.run("all 0..100 high thresholds; exact 24h; epoch zero", testHappyThresholds);
  runner.run("continuous high streaks, expiry, no daily stacking", testHappyContinuityAndExpiry);
  runner.run("strict low thresholds, crossings only, no sad stacking", testDipThresholdsAndCrossings);
  runner.run("rolling 24h two/three-dip windows and midnight", testRollingDipWindows);
  runner.run("strict recovery thresholds; interrupted 24h/48h recovery", testRecoveryThresholdsAndContinuity);
  runner.run("fresh happy day after recovery, immediate/delayed exit acknowledgement", testFreshHappyAfterRecovery);
  runner.run("notice priority, stale/invalid/duplicate acknowledgements", testNoticeOrderingAndStaleness);
  runner.run("happy-to-sad requires exit then entry acknowledgement", testHappyToSadNeedsTwoAcknowledgements);
  runner.run("81 modifier combinations, multiplication, pets global-only", testMultipliedNeedRates);
  runner.run("crisis scaling, exact points, local modifiers excluded, sleep", testCrisisScaling);
  runner.run("stable fractional carries across acknowledged rate changes", testCarryContinuityAcrossAcknowledgements);
  runner.run("chronological recovery/high deadlines and sleeping care", testChronologicalDeadlinesAndSleepingCare);
  runner.run("before-intro no tracking, enum mapping, refill observation epoch", testBeforeIntroAndObservationMapping);
  runner.run("random schedules: identical activities and acknowledgement times", testRandomIdenticalActivitiesAndAcknowledgements);
  runner.run("RTC POD copy round-trip and continued evolution", testRtcPodRoundTrip);
  runner.run("bounded long gaps and 32-bit epoch limits", testLongGapsAndEpochLimits);
  return runner.finish();
}
