#include "../PetNeeds.h"
#include "../CareNotificationText.h"
#include "test_support.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <vector>

using TestSupport::require;

// Hardware boundaries are fakes; the generated include below contains the actual
// firmware notification functions, entry/resume routing, word wrap, and bold renderer.
constexpr int SCREEN_W = 250, SCREEN_H = 122, EPD_W = 250, EPD_H = 122;
constexpr uint8_t BLACK = 0, WHITE = 1;
constexpr unsigned long INACTIVITY_TIMEOUT_MS = 30000, DIRECTION_REARM_MS = 70;
constexpr uint32_t SLEEP_WAKE_WINDOW_SEC = 1800;
enum AppMode { APP_HOME, APP_TAMAGOTCHI, APP_EREADER };
enum PetMode { PET_IDLE, PET_ACTION, PET_RAN_AWAY, PET_INTRO, PET_SLEEP_SELECT,
               PET_SLEEPING, PET_COOLDOWN_MSG, PET_CARE_NOTICE };
enum InputEvent { INPUT_NONE, INPUT_MAIN, INPUT_UP, INPUT_DOWN };
constexpr int ACTIVE_NONE = 0;
AppMode appMode;
PetMode petMode;
PetCare::State careState;
PetCare::Notice currentCareNotice;
struct { PetCare::State care; } rtcPetState;
bool careNoticeAwaitRelease, inputCaptureLocked, discardWakeInput;
bool forceFullRefreshNextFrame, hasSeenDeliveryIntro, sleepWakeButtonShown;
bool released, homePending, runaway, expireDuringDrain;
unsigned long fakeMillis, careNoticeReleasedSinceMs, lastActivityMs, lastFrameMs;
uint32_t sleepStartEpoch, sleepDurationSec, fakeEpoch;
int activeAction, currentFrame, refreshCount, drawCount, sleepCount, drainCount;
const char* currentActivityMessage;
struct Animation {} sleepingAnimation, idleAnimation;
const Animation* currentAnimation;
std::deque<InputEvent> inputs;
uint8_t pixels[SCREEN_H][SCREEN_W];
std::string bodyDrawn;

unsigned long millis() { return fakeMillis; }
uint32_t fakeTime(void*) { return fakeEpoch; }
bool allButtonsReleased() { return released; }
void clearAllPendingInput() { inputs.clear(); homePending = false; }
void resetMainInputState() { clearAllPendingInput(); }
InputEvent popInputEvent() {
  if (inputs.empty()) return INPUT_NONE;
  auto event = inputs.front(); inputs.pop_front(); return event;
}
void EPD_Init() {}
void clearImageBuffer() { std::memset(pixels, 255, sizeof(pixels)); bodyDrawn.clear(); }
void fullRefresh() { ++refreshCount; }
void EPD_DrawPoint(uint16_t x, uint16_t y, uint8_t color) {
  require(x < SCREEN_W && y < SCREEN_H, "renderer wrote outside the display");
  pixels[y][x] = color == BLACK ? 0 : 255;
}
void EPD_ShowString(int x, int y, const char* text, uint8_t color, int size);
void drawAnimationFrame(const Animation*, const uint8_t*) {
  ++drawCount;
  if (forceFullRefreshNextFrame) { fullRefresh(); forceFullRefreshNextFrame = false; }
}
const uint8_t* getAnimationFrame(const Animation*, int) { return nullptr; }
void startIdle() { petMode = PET_IDLE; drawAnimationFrame(&idleAnimation, nullptr); }
void startPetAgeTimerIfNeeded() {}
void savePetStateCheckpoint() {}
void tracePetEvent(const char*, const char* = "") {}
void startDeliveryIntro() { petMode = PET_INTRO; inputCaptureLocked = true; }
bool shouldShowRunaway() { return runaway; }
void showRunawayScreen() { petMode = PET_RAN_AWAY; inputCaptureLocked = false; }
void drainNeedsOverTime() {
  ++drainCount;
  if (expireDuringDrain) careState.categories[currentCareNotice.category].qualified = PetCare::Normal;
  if (runaway) showRunawayScreen();
}
void wakeCatFromSleep(bool) { sleepDurationSec = 0; startIdle(); }
void drawSleepDialog() { ++drawCount; }
void drawCooldownDialog() { ++drawCount; }
void enterDeepSleep() { ++sleepCount; rtcPetState.care = careState; }
bool canMenuButtonEnterHome();
void updateMenuButtonInput() {
  if (homePending && canMenuButtonEnterHome()) {
    appMode = APP_HOME; petMode = PET_IDLE; inputCaptureLocked = false;
    clearAllPendingInput();
  }
}
bool showNextCareNotice();
void drawCareNoticeScreen();
void enterTamagotchiInitialScreen();
void acknowledgeCareNotice();
void loopCareNotice();
void redrawTamagotchiAfterSerialPortal();

#define time fakeTime
#include "care_firmware_under_test.h"
#undef time

void EPD_ShowString(int x, int y, const char* text, uint8_t color, int size) {
  require(size == 12, "body must use 12px text");
  require(x == 6 && y >= 30 && y + 12 <= SCREEN_H - 6, "body layout bounds");
  require(x + std::strlen(text) * 6 <= SCREEN_W - 6, "body line exceeds right margin");
  if (!bodyDrawn.empty()) bodyDrawn += ' ';
  bodyDrawn += text;
  for (const char* letter = text; *letter; ++letter, x += 6) {
    require(*letter >= ' ' && *letter <= '~', "non-ASCII body text");
    const uint8_t* glyph = ascii_1206[*letter - ' '];
    for (uint16_t row = 0; row < 12; ++row) {
      for (uint16_t col = 0; col < 6; ++col) {
        EPD_DrawPoint(static_cast<uint16_t>(x + col), static_cast<uint16_t>(y + row),
                      glyph[row] & (1u << col) ? color : WHITE);
      }
    }
  }
}

void reset() {
  appMode = APP_TAMAGOTCHI; petMode = PET_IDLE;
  careState = {}; careState.initialized = true; currentCareNotice = {};
  rtcPetState = {};
  careNoticeAwaitRelease = inputCaptureLocked = discardWakeInput = false;
  forceFullRefreshNextFrame = sleepWakeButtonShown = false;
  hasSeenDeliveryIntro = released = true;
  homePending = runaway = expireDuringDrain = false;
  fakeMillis = 1000; careNoticeReleasedSinceMs = lastActivityMs = lastFrameMs = 0;
  sleepStartEpoch = sleepDurationSec = 0; fakeEpoch = 1789405993;
  activeAction = ACTIVE_NONE; currentFrame = 0;
  refreshCount = drawCount = sleepCount = drainCount = 0;
  currentActivityMessage = ""; currentAnimation = &idleAnimation;
  clearAllPendingInput(); clearImageBuffer();
}

void pending(PetCare::Category category) { careState.categories[category].qualified = PetCare::Happy; }
void arm() {
  released = true;
  loopCareNotice();
  fakeMillis += DIRECTION_REARM_MS;
  loopCareNotice();
  require(!inputCaptureLocked && !careNoticeAwaitRelease, "release did not re-arm Enter");
}
void pressEnter() { released = false; inputs.push_back(INPUT_MAIN); loopCareNotice(); }

void testInputAndDeferral() {
  reset(); pending(PetCare::Overall); pending(PetCare::Pee);
  released = false; inputs.push_back(INPUT_MAIN); homePending = true;
  enterTamagotchiInitialScreen();
  require(petMode == PET_CARE_NOTICE && refreshCount == 1, "entry must show full-refresh notice");
  inputs.push_back(INPUT_MAIN); homePending = true;
  loopCareNotice();
  require(careState.categories[0].applied == PetCare::Normal && appMode == APP_TAMAGOTCHI,
          "opening/held/wake press dismissed a notice");
  arm();
  inputs.push_back(INPUT_UP); loopCareNotice();
  inputs.push_back(INPUT_DOWN); loopCareNotice();
  released = false; loopCareNotice();
  require(careState.categories[0].applied == PetCare::Normal, "disabled buttons applied a notice");
  pressEnter();
  require(careState.categories[0].applied == PetCare::Happy && currentCareNotice.category == PetCare::Pee,
          "Enter did not apply only the visible notice");
  inputs.push_back(INPUT_MAIN); loopCareNotice();
  require(careState.categories[PetCare::Pee].applied == PetCare::Normal, "duplicate Enter skipped next notice");
  arm(); homePending = true; loopCareNotice();
  require(appMode == APP_HOME && careState.categories[PetCare::Pee].applied == PetCare::Normal,
          "Home must defer without applying");
  appMode = APP_TAMAGOTCHI; enterTamagotchiInitialScreen();
  require(currentCareNotice.category == PetCare::Pee, "deferred notice lost on re-entry");
  arm(); pressEnter();
  require(petMode == PET_IDLE && discardWakeInput && inputs.empty(), "final acknowledgement leaked into an action");
  require(drawCount == 1 && refreshCount == 4, "idle return must use full refresh");
}

void testStaleAndRunaway() {
  reset(); pending(PetCare::Overall); enterTamagotchiInitialScreen(); arm();
  expireDuringDrain = true; pressEnter();
  require(careState.categories[0].applied == PetCare::Normal && petMode == PET_IDLE,
          "stale notice was applied without revalidation");
  reset(); pending(PetCare::Overall); runaway = true; enterTamagotchiInitialScreen();
  require(petMode == PET_RAN_AWAY && refreshCount == 0, "notice took precedence over runaway at entry");
  reset(); pending(PetCare::Overall); enterTamagotchiInitialScreen(); arm();
  runaway = true; pressEnter();
  require(petMode == PET_RAN_AWAY && careState.categories[0].applied == PetCare::Normal,
          "notice acknowledgement hid runaway");
}

void testSleepAndPortalResume() {
  reset(); pending(PetCare::Overall);
  sleepStartEpoch = fakeEpoch - 100; sleepDurationSec = 7200;
  enterTamagotchiInitialScreen(); arm(); pressEnter();
  require(petMode == PET_SLEEPING && sleepDurationSec == 7200 && discardWakeInput,
          "notification cancelled scheduled sleep or leaked wake input");
  reset(); pending(PetCare::Pee); enterTamagotchiInitialScreen(); arm();
  fakeMillis += INACTIVITY_TIMEOUT_MS; loopCareNotice();
  require(sleepCount == 1, "notification must allow normal inactivity sleep");
  TestSupport::sameCare(rtcPetState.care, careState, "deferred notice retained for sleep");
  careState = rtcPetState.care; petMode = PET_IDLE; released = false;
  enterTamagotchiInitialScreen(); inputs.push_back(INPUT_MAIN); homePending = true; loopCareNotice();
  require(petMode == PET_CARE_NOTICE && careState.categories[PetCare::Pee].applied == PetCare::Normal,
          "wake press acknowledged or dismissed retained notice");

  redrawTamagotchiAfterSerialPortal();
  require(petMode == PET_CARE_NOTICE && careNoticeAwaitRelease && inputCaptureLocked,
          "USB disconnect must restore notice with fresh-input guard");
  careState.categories[PetCare::Pee].qualified = PetCare::Normal;
  redrawTamagotchiAfterSerialPortal();
  require(petMode == PET_IDLE && !inputCaptureLocked, "expired USB-deferred notice left controls locked");
  reset(); pending(PetCare::Food); redrawTamagotchiAfterSerialPortal();
  require(petMode == PET_IDLE, "USB disconnect injected a newly earned notice into idle");
  reset(); hasSeenDeliveryIntro = false; enterTamagotchiInitialScreen();
  require(petMode == PET_INTRO, "first intro routing regressed");
}

void testRenderAllMessages() {
  constexpr int width = 4 * (SCREEN_W + 8), height = 4 * (SCREEN_H + 8);
  std::vector<uint8_t> sheet(width * height * 3, 255);
  for (int category = 0; category < PetCare::Count; ++category) {
    const PetCare::Mode from[] = {PetCare::Normal, PetCare::Happy, PetCare::Normal, PetCare::Sad};
    const PetCare::Mode to[] = {PetCare::Happy, PetCare::Normal, PetCare::Sad, PetCare::Normal};
    for (int transition = 0; transition < 4; ++transition) {
      reset();
      currentCareNotice = {static_cast<PetCare::Category>(category), from[transition], to[transition]};
      drawCareNoticeScreen();
      require(bodyDrawn == CareNotificationText::message(currentCareNotice), "message was truncated or altered");
      int ink = 0;
      for (int y = 0; y < SCREEN_H; ++y) {
        for (int x = 0; x < SCREEN_W; ++x) {
          if (pixels[y][x] == 0) { ++ink; require(x >= 6 && x < SCREEN_W - 6, "ink outside margins"); }
          int px = category * (SCREEN_W + 8) + x;
          int py = transition * (SCREEN_H + 8) + y;
          for (int rgb = 0; rgb < 3; ++rgb) sheet[(py * width + px) * 3 + rgb] = pixels[y][x];
        }
      }
      require(ink > 500 && refreshCount == 1, "notification is blank or missing refresh");
    }
  }
  // A PPM contact sheet uses the real 6x12 and 8x16 glyphs, at native resolution.
  std::ofstream output("care-notifications.ppm", std::ios::binary);
  output << "P6\n" << width << ' ' << height << "\n255\n";
  output.write(reinterpret_cast<const char*>(sheet.data()), static_cast<std::streamsize>(sheet.size()));
  require(output.good(), "contact sheet could not be written");
}

int main() {
  TestSupport::Runner runner;
  runner.run("firmware Enter, held inputs, Home deferral and full refresh", testInputAndDeferral);
  runner.run("firmware stale acknowledgement and runaway priority", testStaleAndRunaway);
  runner.run("firmware sleep, wake, USB restoration and intro routing", testSleepAndPortalResume);
  runner.run("all 16 actual-font screens: exact text, margins and nonblank pixels", testRenderAllMessages);
  return runner.finish();
}
