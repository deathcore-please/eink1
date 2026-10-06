#include "PetCare.h"

namespace PetCare {

const Rule Rules[Count] = {
  {50, true,  30, 2, 30, 2 * DaySeconds, 70, 130},
  {70, false, 30, 2, 40, DaySeconds,     60, 140},
  {80, false, 20, 2, 40, DaySeconds,     50, 150},
  {50, false, 30, 3, 40, DaySeconds,     70, 130}
};

static void pruneDips(CategoryState& category, uint32_t now) {
  uint8_t kept = 0;
  for (uint8_t i = 0; i < category.dipCount; ++i) {
    uint32_t dip = category.dips[i];
    if (dip <= now && now - dip < DaySeconds) {
      category.dips[kept++] = dip;
    }
  }
  category.dipCount = kept;
  for (uint8_t i = kept; i < 3; ++i) {
    category.dips[i] = 0;
  }
}

void observe(State& state, const uint8_t values[Count], uint32_t now) {
  for (uint8_t i = 0; i < Count; ++i) {
    CategoryState& category = state.categories[i];
    const Rule& rule = Rules[i];
    bool high = rule.happyInclusive ? values[i] >= rule.happyThreshold
                                    : values[i] > rule.happyThreshold;
    bool low = values[i] < rule.lowThreshold;
    pruneDips(category, now);

    // A new life already below a threshold is not a downward crossing.
    if (state.initialized && low && !category.low) {
      if (category.dipCount == rule.dipsRequired) {
        for (uint8_t n = 1; n < category.dipCount; ++n) {
          category.dips[n - 1] = category.dips[n];
        }
        --category.dipCount;
      }
      category.dips[category.dipCount++] = now;
      if (category.dipCount >= rule.dipsRequired) {
        category.qualified = Sad;
      }
    }
    category.low = low;

    if (category.qualified == Sad) {
      category.high = false;
      category.happySince = 0;
      bool recovering = values[i] > rule.recoveryThreshold;
      if (recovering && !category.recovering) {
        category.recoverySince = now;
      }
      category.recovering = recovering;
      if (!recovering) {
        category.recoverySince = 0;
      } else if (now >= category.recoverySince &&
                 now - category.recoverySince >= rule.recoverySeconds) {
        category.qualified = Normal;
        category.recovering = false;
        category.recoverySince = 0;
      }
      if (category.qualified == Sad) {
        continue;
      }
      // Recovery time never doubles as a happy streak.
    }

    if (high && !category.high) {
      category.happySince = now;
    }
    category.high = high;
    if (!high) {
      category.happySince = 0;
      category.qualified = Normal;
    } else if (now >= category.happySince && now - category.happySince >= DaySeconds) {
      category.qualified = Happy;
    }
  }
  state.initialized = true;
}

uint64_t nextDeadline(const State& state, uint32_t now) {
  uint64_t next = UINT64_MAX;
  if (!state.initialized) {
    return next;
  }
  for (uint8_t i = 0; i < Count; ++i) {
    const CategoryState& category = state.categories[i];
    uint64_t deadline = UINT64_MAX;
    if (category.qualified == Normal && category.high) {
      deadline = static_cast<uint64_t>(category.happySince) + DaySeconds;
    } else if (category.qualified == Sad && category.recovering) {
      deadline = static_cast<uint64_t>(category.recoverySince) + Rules[i].recoverySeconds;
    }
    if (deadline > now && deadline < next) {
      next = deadline;
    }
  }
  return next;
}

bool nextNotice(const State& state, Notice& notice) {
  if (!state.initialized) {
    return false;
  }
  for (uint8_t i = 0; i < Count; ++i) {
    const CategoryState& category = state.categories[i];
    if (category.applied != category.qualified) {
      notice = {static_cast<Category>(i), category.applied,
                category.applied == Normal ? category.qualified : Normal};
      return true;
    }
  }
  return false;
}

bool acknowledge(State& state, const Notice& notice) {
  Notice current = {};
  if (!nextNotice(state, current) || current.category != notice.category ||
      current.from != notice.from || current.to != notice.to) {
    return false;
  }
  state.categories[notice.category].applied = notice.to;
  return true;
}

uint8_t ratePercent(const State& state, Category category) {
  if (category >= Count) {
    return 100;
  }
  Mode mode = state.categories[category].applied;
  return mode == Happy ? Rules[category].happyRatePercent
       : mode == Sad ? Rules[category].sadRatePercent : 100;
}

}  // namespace PetCare
