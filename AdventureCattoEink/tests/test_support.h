#pragma once

#include "../PetNeeds.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace TestSupport {

inline void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

inline void sameCare(const PetCare::State& a, const PetCare::State& b,
                     const std::string& context = "care") {
  require(a.initialized == b.initialized, context + ": initialized differs");
  for (int i = 0; i < PetCare::Count; ++i) {
    const auto& x = a.categories[i];
    const auto& y = b.categories[i];
    const std::string at = context + " category " + std::to_string(i) + ": ";
    require(x.qualified == y.qualified, at + "qualified differs");
    require(x.applied == y.applied, at + "applied differs");
    require(x.happySince == y.happySince, at + "happySince differs");
    require(x.recoverySince == y.recoverySince, at + "recoverySince differs");
    require(x.dipCount == y.dipCount, at + "dipCount differs");
    for (int dip = 0; dip < 3; ++dip) {
      require(x.dips[dip] == y.dips[dip], at + "dip " + std::to_string(dip) + " differs");
    }
    require(x.high == y.high, at + "high differs");
    require(x.recovering == y.recovering, at + "recovering differs");
    require(x.low == y.low, at + "low differs");
  }
}

inline void sameState(const PetNeeds::State& a, const PetNeeds::State& b,
                      const std::string& context = "needs") {
  for (int i = 0; i < PetNeeds::Count; ++i) {
    const std::string at = context + " need " + std::to_string(i) + ": ";
    require(a.needs[i].value == b.needs[i].value, at + "value differs");
    require(a.needs[i].drainCarry == b.needs[i].drainCarry, at + "drainCarry differs");
    require(a.needs[i].cooldownUntilEpoch == b.needs[i].cooldownUntilEpoch,
            at + "cooldownUntilEpoch differs");
  }
  require(a.happiness == b.happiness, context + ": happiness differs");
  require(a.crisisPenalty == b.crisisPenalty, context + ": crisisPenalty differs");
  require(a.crisisCarry == b.crisisCarry, context + ": crisisCarry differs");
  require(a.started == b.started, context + ": started differs");
  sameCare(a.care, b.care, context + " care");
}

class Runner {
 public:
  void run(const char* name, void (*test)()) {
    try {
      test();
      ++passed_;
      std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& error) {
      ++failed_;
      std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
    }
  }

  int finish() const {
    std::cout << passed_ << " test groups passed; " << failed_ << " failed.\n";
    return failed_ ? 1 : 0;
  }

 private:
  unsigned passed_ = 0;
  unsigned failed_ = 0;
};

}  // namespace TestSupport
