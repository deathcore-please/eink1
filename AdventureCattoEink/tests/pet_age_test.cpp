#include "test_support.h"
#include <cstring>

using TestSupport::require;

bool petAgeStarted = false;
uint32_t petBirthEpoch = 0;
struct { uint8_t petAgeStarted; uint32_t petBirthEpoch; } rtcPetState = {};
uint32_t nowEpoch = 1789405993;
bool usedOverride = false, failOpen = false, failWrite = false;
unsigned opens = 0, writes = 0, clockRefreshes = 0;
struct { void println(const char*) {} } Serial;

class Preferences {
 public:
  bool begin(const char* name, bool readOnly) {
    require(std::strcmp(name, "pet-age") == 0 && !readOnly, "age preferences namespace");
    ++opens;
    return !failOpen;
  }
  bool getBool(const char* key, bool defaultValue) {
    require(std::strcmp(key, "start17-used") == 0 && !defaultValue, "one-time age key");
    return usedOverride;
  }
  size_t putBool(const char* key, bool value) {
    require(std::strcmp(key, "start17-used") == 0 && value, "override must only be consumed");
    if (failWrite) return 0;
    ++writes; usedOverride = true; return 1;
  }
  void end() {}
};

void refreshRtcEpochFromClock() { ++clockRefreshes; }
uint32_t fakeTime(void*) { return nowEpoch; }
#define time fakeTime
#include "pet_age_firmware_under_test.h"
#undef time

static_assert(DEFAULT_PET_START_AGE_DAYS == 15, "all later lives start at 15 days");
static_assert(ONE_TIME_PET_START_AGE_DAYS == 17, "this one-time start is 17 days");
constexpr uint32_t Day = 86400;

void testOneTimeAndRunaway() {
  usedOverride = false; opens = writes = 0;
  resetPetAgeTimer();
  require(!usedOverride && opens == 0, "reset/pre-intro must not consume override");
  startPetAgeTimerIfNeeded();
  require(petAgeStarted && nowEpoch - petBirthEpoch == 17 * Day, "first intro must start at 17 days");
  require(usedOverride && writes == 1, "first start must persist override consumption");
  const auto firstBirth = petBirthEpoch;
  nowEpoch += Day;
  startPetAgeTimerIfNeeded();
  require(petBirthEpoch == firstBirth && nowEpoch - petBirthEpoch == 18 * Day && opens == 1,
          "normal re-entry must keep advancing, without another NVS access");
  petAgeStarted = rtcPetState.petAgeStarted != 0;
  petBirthEpoch = rtcPetState.petBirthEpoch;
  startPetAgeTimerIfNeeded();
  require(petBirthEpoch == firstBirth && opens == 1, "RTC wake must preserve age");
  for (unsigned life = 0; life < 5; ++life) {
    resetPetAgeTimer();
    require(!petAgeStarted && petBirthEpoch == 0, "runaway reset must clear the age timer");
    startPetAgeTimerIfNeeded();
    require(nowEpoch - petBirthEpoch == 15 * Day && writes == 1, "later lives must always start at 15");
    nowEpoch += 100;
  }
  // Simulate losing volatile and RTC state; NVS survives power loss/normal upload.
  petAgeStarted = false; petBirthEpoch = 0; rtcPetState = {};
  startPetAgeTimerIfNeeded();
  require(nowEpoch - petBirthEpoch == 15 * Day && writes == 1, "cold reboot must not regrant override");
}

void testStorageFailure() {
  usedOverride = false; failOpen = true;
  resetPetAgeTimer(); startPetAgeTimerIfNeeded();
  require(nowEpoch - petBirthEpoch == 15 * Day && !usedOverride, "NVS open failure must use normal age");
  failOpen = false; failWrite = true;
  resetPetAgeTimer(); startPetAgeTimerIfNeeded();
  require(nowEpoch - petBirthEpoch == 15 * Day && !usedOverride, "unpersisted override must not be applied");
  failWrite = false;
}

int main() {
  TestSupport::Runner runner;
  runner.run("one-time 17-day start, runaway 15-day starts, RTC wake and cold reboot", testOneTimeAndRunaway);
  runner.run("starting-age storage failure uses normal age", testStorageFailure);
  return runner.finish();
}
