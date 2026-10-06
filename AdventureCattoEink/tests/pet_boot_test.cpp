#include "../PetStateStore.h"
#include "../PetTrace.h"
#include "test_support.h"
#include <Preferences.h>
#include <esp_app_desc.h>
#include <cstring>
#include <ctime>

using TestSupport::require;

using esp_sleep_wakeup_cause_t = int;
constexpr int ESP_SLEEP_WAKEUP_UNDEFINED = 0, ESP_SLEEP_WAKEUP_EXT1 = 3;
constexpr int INPUT = 0, OUTPUT = 1, HIGH = 1, FALLING = 2;
constexpr int EPD_POWER = 7, HOME_KEY = 5, EXIT_KEY = 1, PRV_KEY = 6, NEXT_KEY = 4, OK_KEY = 2;
constexpr int MENU_BUTTON_KEY = HOME_KEY;
enum AppMode { APP_HOME, APP_TAMAGOTCHI, APP_EREADER };
enum HomeOption { HOME_OPTION_TAMAGOTCHI, HOME_OPTION_EREADER };
enum PetMode { PET_IDLE, PET_ACTION, PET_RAN_AWAY, PET_INTRO, PET_SLEEP_SELECT,
               PET_SLEEPING, PET_COOLDOWN_MSG, PET_CARE_NOTICE };
enum SelectedAction { ACTION_PEE, ACTION_FOOD, ACTION_PLAY, ACTION_PET, ACTION_SLEEP };
enum SadNeed { SAD_NEED_NONE, SAD_NEED_PEE, SAD_NEED_FOOD, SAD_NEED_PLAY, SAD_NEED_PETS };
enum ActiveAction { ACTIVE_NONE, ACTIVE_PEE, ACTIVE_FOOD, ACTIVE_PLAY, ACTIVE_PET };
enum PetStateRestoreSource { PET_STATE_FRESH, PET_STATE_RTC, PET_STATE_CHECKPOINT };

AppMode appMode;
HomeOption homeSelection;
PetMode petMode;
SelectedAction selectedAction;
SadNeed activeSadNeed;
ActiveAction activeAction;
PetCare::State careState;
PetStateStore::Snapshot rtcPetState;
bool careNoticeAwaitRelease, petAgeStarted, hasSeenDeliveryIntro, pendingRunaway;
bool petCheckpointDirty, discardWakeInput, forceFullRefreshNextFrame, serialPortalDisplayActive;
bool sleepWakeButtonShown;
bool inputCaptureLocked;
unsigned long inputLockoutUntil;
const char* currentActivityMessage;
constexpr int SCREEN_W = 250, ACTIVITY_TEXT_SIZE = 12, RUNAWAY_TEXT_MARGIN = 6;
constexpr const char* RUNAWAY_MESSAGE = "runaway";
uint8_t peeValue, foodValue, playValue, loveValue, happinessValue;
uint8_t happinessCrisisPenalty, rtcRunawayDeathLogged;
uint32_t petBirthEpoch, lastNeedDrainEpoch;
uint32_t peeDrainCarry, foodDrainCarry, playDrainCarry, loveDrainCarry, happinessCrisisCarry;
uint32_t peeCooldownUntilEpoch, foodCooldownUntilEpoch, playCooldownUntilEpoch;
uint32_t sleepStartEpoch, sleepDurationSec, sleepCooldownUntilEpoch;
unsigned long lastPetCheckpointMs, lastActivityMs, lastFrameMs, lastNeedDrainMs, sleepOkIgnoredUntilMs;
const char* runawayDiagnosticReason;
int lastWakeCause, fakeWakeCause;
unsigned long fakeMillis, clockBaseMillis;
time_t fakeEpoch;
unsigned homeDraws, tamagotchiEntries, readerEntries, runawayDraws;
PetTrace::Frame tracedBefore, tracedAfter;
PetTrace::Frame tracedDisplay;
unsigned traceDisplayCalls, oldDeathLogCalls;

unsigned long millis() { return fakeMillis; }
int64_t esp_timer_get_time() { return static_cast<int64_t>(fakeMillis) * 1000; }
time_t fakeTime(time_t* result) {
  time_t now = fakeEpoch + static_cast<time_t>((fakeMillis - clockBaseMillis) / 1000);
  if (result) *result = now;
  return now;
}
struct timeval { time_t tv_sec; long tv_usec; };
int settimeofday(const timeval* value, void*) {
  fakeEpoch = value->tv_sec;
  clockBaseMillis = fakeMillis;
  return 0;
}
time_t buildTimeEpoch() { return 1700000000; }
struct SerialFake {
  void begin(int) {}
  void println(const char*) {}
  template<class... Args> void printf(const char*, Args...) {}
} Serial;

void pinMode(int, int) {}
void digitalWrite(int, int) {}
void delay(unsigned long ms) { fakeMillis += ms; }
int esp_sleep_get_wakeup_cause() { return fakeWakeCause; }
uint32_t esp_random() { return 123; }
int esp_reset_reason() { return 5; }
PetTrace::Frame capturePetTraceFrame();
void tracePetEvent(const char* event, const char* = "") {
  if (std::strcmp(event, "runaway_display") == 0) { ++traceDisplayCalls; tracedDisplay = capturePetTraceFrame(); }
}
namespace PetTrace {
void begin(uint32_t) {}
bool drain(const Frame& before, const Frame& after) { tracedBefore = before; tracedAfter = after; return true; }
}
int digitalPinToInterrupt(int pin) { return pin; }
void attachInterrupt(int, void (*)(), int) {}
void handleMenuButtonInterrupt() {}
void attachMainInputInterrupts() {}
void detachMainInputInterrupts() {}
void resetMainInputState() {}
void clearInputQueue() {}
void serialLibraryPortalBegin() {}
void showHomeScreenFull() { ++homeDraws; }
void ereaderEnter() { ++readerEntries; }
void enterTamagotchiInitialScreen() { ++tamagotchiEntries; }
void refreshSadNeedAfterNeedChange(SadNeed) {}
void showRunawayScreen();
void logRunawayDeathIfNeeded(const char*) { if (!rtcRunawayDeathLogged) { ++oldDeathLogCalls; rtcRunawayDeathLogged = 1; } }
void EPD_Init() {}
void clearImageBuffer() {}
void drawWrappedFullScreenMessage(const char*, int, int, int, int) {}
void fullRefresh() { ++runawayDraws; }

void resetPetAgeTimer();
PetNeeds::State captureNeedState();
void applyNeedState(const PetNeeds::State& state);
bool hasRunawayCriticalNeed();
void maybeTriggerRunaway();
void updateHappiness();
void initDefaultPetState();
void refreshRtcEpochFromClock();
void savePetStateToRtc();
bool restorePetStateFromRtc();
void savePetStateCheckpoint();
void servicePetStateCheckpoint();
PetStateRestoreSource restorePetStateForBoot(esp_sleep_wakeup_cause_t wakeCause);
void initNeedDrainClockIfNeeded();
void resetNeedDrainClock();
void drainNeedsOverTime();
void applyNeedsSinceSleep();
void initDeviceTime(bool restored);

#define time fakeTime
#include "pet_boot_firmware_under_test.h"
#undef time

constexpr uint32_t Start = 1790112333;

void resetMachine(time_t clock, bool retainRtc = false) {
  if (!retainRtc) rtcPetState = {};
  appMode = APP_TAMAGOTCHI; homeSelection = HOME_OPTION_TAMAGOTCHI;
  petMode = PET_IDLE; activeAction = ACTIVE_NONE;
  selectedAction = ACTION_PEE; activeSadNeed = SAD_NEED_NONE;
  peeValue = foodValue = playValue = loveValue = happinessValue = 100;
  petAgeStarted = hasSeenDeliveryIntro = pendingRunaway = false;
  careNoticeAwaitRelease = petCheckpointDirty = discardWakeInput = false;
  serialPortalDisplayActive = forceFullRefreshNextFrame = sleepWakeButtonShown = false;
  rtcRunawayDeathLogged = happinessCrisisPenalty = 0;
  petBirthEpoch = lastNeedDrainEpoch = 0;
  peeDrainCarry = foodDrainCarry = playDrainCarry = loveDrainCarry = happinessCrisisCarry = 0;
  peeCooldownUntilEpoch = foodCooldownUntilEpoch = playCooldownUntilEpoch = 0;
  sleepStartEpoch = sleepDurationSec = sleepCooldownUntilEpoch = 0;
  lastPetCheckpointMs = lastActivityMs = lastFrameMs = lastNeedDrainMs = sleepOkIgnoredUntilMs = 0;
  careState = {};
  fakeMillis = clockBaseMillis = 0; fakeEpoch = clock;
  fakeWakeCause = ESP_SLEEP_WAKEUP_UNDEFINED;
  homeDraws = tamagotchiEntries = readerEntries = runawayDraws = 0;
  traceDisplayCalls = oldDeathLogCalls = 0;
}

void newPet() {
  FakePetStore::reset();
  fakeAppDescription = {{1}};
  resetMachine(Start);
  initDefaultPetState();
  appMode = APP_TAMAGOTCHI;
  peeValue = 79; foodValue = 65; playValue = 71; loveValue = 83;
  happinessValue = 72;
  hasSeenDeliveryIntro = petAgeStarted = true;
  petBirthEpoch = Start - 17 * PetCare::DaySeconds;
  lastNeedDrainEpoch = Start;
  peeDrainCarry = 1700; foodDrainCarry = 4000; playDrainCarry = 18100; loveDrainCarry = 9000;
  peeCooldownUntilEpoch = Start + 2888;
  foodCooldownUntilEpoch = Start + 2873;
  playCooldownUntilEpoch = Start + 1054;
  careState.initialized = true;
  auto& overall = careState.categories[PetCare::Overall];
  overall.applied = overall.qualified = PetCare::Happy;
  overall.high = true; overall.happySince = Start - PetCare::DaySeconds;
  auto& pee = careState.categories[PetCare::Pee];
  pee.applied = pee.qualified = PetCare::Sad;
  pee.recovering = true; pee.recoverySince = Start - 3000;
  PetNeeds::State state = captureNeedState();
  PetNeeds::refreshHappiness(state);
  applyNeedState(state);
}

void testOrdinaryReset() {
  for (AppMode mode : {APP_HOME, APP_TAMAGOTCHI, APP_EREADER}) {
    for (bool clockSurvives : {false, true}) {
      newPet(); appMode = mode; homeSelection = HOME_OPTION_EREADER; selectedAction = ACTION_PLAY;
      auto before = captureNeedState();
      uint32_t birth = petBirthEpoch;
      savePetStateCheckpoint();
      resetMachine(clockSurvives ? Start : 0);
      setup();
      TestSupport::sameState(captureNeedState(), before, "ordinary restart");
      require(appMode == mode && homeSelection == HOME_OPTION_EREADER && selectedAction == ACTION_PLAY,
              "restart lost the mode or selection");
      require(hasSeenDeliveryIntro && petAgeStarted && petBirthEpoch == birth,
              "restart erased the existing pet life or replayed the intro");
      require(lastNeedDrainEpoch == Start && fakeTime(nullptr) == Start,
              "clock fallback used build time instead of the checkpoint");
      require(discardWakeInput && runawayDraws == 0, "restart injected input or runaway");
    }
  }
}

void testDeepSleepAndCatchup() {
  newPet(); savePetStateCheckpoint();
  foodValue = 92;
  savePetStateToRtc();
  auto expected = captureNeedState();
  PetNeeds::advance(expected, Start, Start + 600);
  resetMachine(Start + 600, true);
  fakeWakeCause = ESP_SLEEP_WAKEUP_EXT1;
  unsigned reads = FakePetStore::reads;
  setup();
  TestSupport::sameState(captureNeedState(), expected, "RTC wake");
  require(FakePetStore::reads == reads, "valid deep sleep RTC state unnecessarily loaded older flash data");
  require(hasSeenDeliveryIntro && lastNeedDrainEpoch == Start + 600 && runawayDraws == 0,
          "normal deep sleep catchup changed");

  newPet(); savePetStateCheckpoint();
  expected = captureNeedState();
  PetNeeds::advance(expected, Start, Start + 1200);
  resetMachine(Start + 1200);
  setup();
  TestSupport::sameState(captureNeedState(), expected, "ordinary reset catchup");
  require(lastNeedDrainEpoch == Start + 1200, "ordinary restart failed to count trustworthy elapsed time");
}

void testScheduledSleep() {
  newPet(); sleepStartEpoch = Start; sleepDurationSec = 7200;
  sleepCooldownUntilEpoch = Start + 20000;
  auto before = captureNeedState();
  savePetStateCheckpoint();
  uint32_t birth = petBirthEpoch;
  resetMachine(Start + 3600);
  setup();
  require(sleepStartEpoch == Start && sleepDurationSec == 7200 && sleepCooldownUntilEpoch == Start + 20000,
          "restart cancelled scheduled sleep or its cooldown");
  require(peeValue == before.needs[0].value && foodValue == before.needs[1].value &&
          playValue == before.needs[2].value && loveValue == before.needs[3].value,
          "scheduled sleep stopped pausing drain during restart");
  require(petBirthEpoch == birth && hasSeenDeliveryIntro, "sleep restart created a new life");
}

void testFreshFirmwareAndInvalidData() {
  newPet(); savePetStateCheckpoint();
  fakeAppDescription.app_elf_sha256[0] = 2;
  resetMachine(Start);
  setup();
  require(appMode == APP_HOME && homeDraws == 1 && tamagotchiEntries == 0,
          "new firmware did not start at the menu");
  require(!hasSeenDeliveryIntro && !petAgeStarted && peeValue == 80 && happinessValue == 80,
          "new firmware reused an old pet or started its timers");
  resetMachine(0);
  setup();
  require(appMode == APP_HOME && !hasSeenDeliveryIntro && happinessValue == 80,
          "ordinary restart before first intro started draining");

  for (int failure = 0; failure < 5; ++failure) {
    newPet(); savePetStateCheckpoint();
    auto& blob = FakePetStore::blobs.at("pet-life/checkpoint");
    if (failure == 0) blob.back() ^= 1;
    if (failure == 1) blob[0] ^= 1;
    if (failure == 2) blob.pop_back();
    if (failure == 3) FakePetStore::shortReads = true;
    if (failure == 4) FakePetStore::unavailable = true;
    PetStateStore::Snapshot target = {};
    target.peeValue = 44;
    require(!PetStateStore::load(target) && target.peeValue == 44,
            "failed/corrupt checkpoint modified the restoration target");
    resetMachine(Start); setup();
    require(appMode == APP_HOME && peeValue == 80 && !hasSeenDeliveryIntro,
            "invalid checkpoint was interpreted as a live pet");
  }
}

void testRunawayResetAndWriteFailure() {
  newPet(); peeValue = foodValue = playValue = loveValue = happinessValue = 0;
  savePetStateCheckpoint();
  resetMachine(Start); setup();
  require(hasSeenDeliveryIntro && happinessValue == 0 && shouldShowRunaway(),
          "restarting a genuinely ended life incorrectly created a fresh pet");
  resetPetLifeState(); appMode = APP_HOME; savePetStateCheckpoint();
  resetMachine(Start); setup();
  require(!hasSeenDeliveryIntro && !petAgeStarted && happinessValue == 80 && runawayDraws == 0,
          "new life resurrected a pre-reset runaway checkpoint");

  newPet(); savePetStateCheckpoint();
  auto before = captureNeedState();
  FakePetStore::failWrites = true;
  foodValue = 99;
  savePetStateCheckpoint();
  require(petCheckpointDirty, "failed checkpoint write did not request a retry");
  FakePetStore::failWrites = false;
  resetMachine(Start); setup();
  TestSupport::sameState(captureNeedState(), before, "failed write preserved previous snapshot");
}

void testCheckpointRateLimit() {
  newPet(); savePetStateCheckpoint();
  unsigned writes = FakePetStore::writes;
  for (fakeMillis = 1; fakeMillis < PET_CHECKPOINT_INTERVAL_MS; fakeMillis += 13) {
    servicePetStateCheckpoint();
  }
  require(FakePetStore::writes == writes, "idle loop wrote flash on every iteration");
  servicePetStateCheckpoint();
  require(FakePetStore::writes == writes + 1, "periodic checkpoint was never saved");
  FakePetStore::failWrites = true;
  savePetStateCheckpoint();
  FakePetStore::failWrites = false;
  servicePetStateCheckpoint();
  require(FakePetStore::writes == writes + 1, "write failure retried immediately");
  fakeMillis += PET_CHECKPOINT_RETRY_MS;
  servicePetStateCheckpoint();
  require(FakePetStore::writes == writes + 2 && !petCheckpointDirty, "write failure did not retry after backoff");
}

void testLiveTraceCapture() {
  newPet(); savePetStateToRtc();
  peeValue = 67; lastWakeCause = ESP_SLEEP_WAKEUP_EXT1;
  serialPortalDisplayActive = true;
  auto frame = capturePetTraceFrame();
  require(frame.needs.needs[PetNeeds::Pee].value == 67 && frame.rtcEpoch == rtcPetState.epochSeconds &&
          frame.lastDrainEpoch == lastNeedDrainEpoch && frame.rtcDrainEpoch == rtcPetState.lastNeedDrainEpoch,
          "trace captured stale bars instead of live state or lost RTC/live timing distinction");
  require(frame.introSeen && frame.usbActive && frame.birthEpoch == petBirthEpoch &&
          frame.resetReason == 5 && frame.wakeCause == ESP_SLEEP_WAKEUP_EXT1,
          "trace missed lifecycle, USB, age or boot cause");
  auto expected = captureNeedState();
  PetNeeds::advance(expected, Start, Start + 600);
  fakeEpoch = Start + 600;
  drainNeedsOverTime();
  require(tracedBefore.needs.needs[PetNeeds::Pee].value == 67 && tracedBefore.lastDrainEpoch == Start,
          "drain trace did not preserve pre-update live state");
  TestSupport::sameState(tracedAfter.needs, expected, "post-model trace");
  require(tracedAfter.lastDrainEpoch == Start && lastNeedDrainEpoch == Start + 600,
          "diagnostics changed the existing drain commit order");
}

void testStaleRunawayDisplayTrace() {
  newPet();
  petMode = PET_RAN_AWAY;
  rtcRunawayDeathLogged = 1;
  auto before = captureNeedState();
  showRunawayScreen();
  require(traceDisplayCalls == 1 && oldDeathLogCalls == 0 && tracedDisplay.petMode == PET_RAN_AWAY,
          "stale mode or old duplicate guard suppressed the independent trace");
  TestSupport::sameState(tracedDisplay.needs, before, "unexpected display live state");
  TestSupport::sameState(captureNeedState(), before, "diagnostics do not change pet needs");
  showRunawayScreen();
  require(traceDisplayCalls == 2 && oldDeathLogCalls == 0, "a repeat display request was hidden by the old death flag");
}

int main() {
  TestSupport::Runner runner;
  runner.run("ordinary USB/power reset preserves all modes, life, age, bars, carries and care", testOrdinaryReset);
  runner.run("RTC deep sleep priority and identical clock catchup after ordinary restart", testDeepSleepAndCatchup);
  runner.run("scheduled sleep and cooldowns survive restart without premature drain", testScheduledSleep);
  runner.run("new firmware starts fresh; corrupt, truncated and unavailable checkpoints rejected", testFreshFirmwareAndInvalidData);
  runner.run("true runaway and new-life reset persist; failed writes preserve previous record", testRunawayResetAndWriteFailure);
  runner.run("checkpoint write interval and failure retry backoff", testCheckpointRateLimit);
  runner.run("actual firmware trace captures live bars, RTC clocks, lifecycle and both drain states", testLiveTraceCapture);
  runner.run("actual runaway display trace works with stale mode and old logged flag", testStaleRunawayDisplayTrace);
  return runner.finish();
}
