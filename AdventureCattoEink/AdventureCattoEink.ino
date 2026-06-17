#include <Arduino.h>
#include "EPD.h"
#include <string.h>
#include <pgmspace.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <sys/time.h>
#include <time.h>
#include <stdio.h>

#include "animations.h"
#include "idle_animation.h"
#include "eating_animation.h"
#include "peeing_animation.h"
#include "play_animation.h"
#include "sleeping_animation.h"
#include "sleep_splash_night.h"
#include "sleep_splash_day.h"

extern uint8_t ImageBW[ALLSCREEN_BYTES];

// Board input pins from Elecrow example
#define HOME_KEY 2
#define EXIT_KEY 1
#define PRV_KEY  6
#define NEXT_KEY 4
#define OK_KEY   5

// Button meanings for this version
#define MAIN_TOGGLE_KEY OK_KEY
#define UP_KEY          PRV_KEY
#define DOWN_KEY        NEXT_KEY

// E-paper power pin
#define EPD_POWER 7

// Screen size from CrowPanel driver orientation
#define SCREEN_W 250
#define SCREEN_H 122

// Fixed animation box - DO NOT MOVE
#define ANIMATION_BOX_W 125
#define ANIMATION_BOX_H 83
#define ANIMATION_BOX_X (SCREEN_W - 1 - ANIMATION_BOX_W)
#define ANIMATION_BOX_Y 34

// Refresh settings
#define ERASE_PULSES 2

// Input queue settings
#define INPUT_QUEUE_SIZE 12
#define ISR_DEBOUNCE_US 50000

// Need/meter settings
#define NEED_DRAIN_INTERVAL_MS 5000
#define NEED_DRAIN_AMOUNT 2

// Deep sleep settings
#define SLEEP_ANIMATION_TIMEOUT_MS 15000
#define INACTIVITY_TIMEOUT_MS 30000
#define MAIN_TOGGLE_HOLD_MS 3000
#define RTC_PET_STATE_MAGIC 0xCA770004UL
#define WAKE_BUTTON_PIN_MASK ((1ULL << EXIT_KEY) | (1ULL << HOME_KEY) | (1ULL << NEXT_KEY) | (1ULL << OK_KEY) | (1ULL << PRV_KEY))

// Local hour offset from UTC (seconds). Adjust for your timezone.
#define TIMEZONE_OFFSET_SEC (-5 * 3600)

// HUD positions
#define ACTION_MARKER_X 2
#define ACTION_LABEL_X 8
#define ACTION_BAR_X 40
#define ACTION_BAR_W 78
#define ACTION_BAR_H 4
#define ACTION_TEXT_SIZE 12
#define ACTIVITY_TEXT_SIZE 12
#define ACTIVITY_MESSAGE_X 2
#define ACTIVITY_MESSAGE_Y 20
#define ACTIVITY_MESSAGE_LINE_H 14
#define SELECTION_MARKER_SIZE 3

#define PEE_Y 28
#define FOOD_Y 48
#define PLAY_Y 68

// Home screen layout
#define HOME_MARKER_X 20
#define HOME_LABEL_X 32
#define HOME_TEXT_SIZE 16
#define HOME_TAMAGOTCHI_Y 40
#define HOME_EREADER_Y 70

enum AppMode {
  APP_HOME,
  APP_TAMAGOTCHI,
  APP_EREADER
};

enum HomeOption {
  HOME_OPTION_TAMAGOTCHI,
  HOME_OPTION_EREADER
};

enum PetMode {
  PET_IDLE,
  PET_ACTION
};

enum SelectedAction {
  ACTION_PEE,
  ACTION_FOOD,
  ACTION_PLAY
};

enum ActiveAction {
  ACTIVE_NONE,
  ACTIVE_PEE,
  ACTIVE_FOOD,
  ACTIVE_PLAY
};

enum InputEvent {
  INPUT_NONE,
  INPUT_MAIN,
  INPUT_UP,
  INPUT_DOWN
};

AppMode appMode = APP_TAMAGOTCHI;
HomeOption homeSelection = HOME_OPTION_TAMAGOTCHI;
PetMode petMode = PET_IDLE;
SelectedAction selectedAction = ACTION_PEE;
ActiveAction activeAction = ACTIVE_NONE;

bool mainToggleDown = false;
bool mainToggleHoldHandled = false;
unsigned long mainToggleDownMs = 0;

const Animation* currentAnimation = &idleAnimation;
uint8_t currentFrame = 0;

unsigned long lastFrameMs = 0;
unsigned long lastNeedDrainMs = 0;
unsigned long lastActivityMs = 0;
unsigned long inputLockoutUntil = 0;
bool discardWakeInput = false;
bool forceFullRefreshNextFrame = false;

RTC_DATA_ATTR struct {
  uint32_t magic;
  uint8_t peeValue;
  uint8_t foodValue;
  uint8_t playValue;
  uint8_t selectedAction;
  uint8_t appMode;
  uint8_t homeSelection;
  uint64_t sleepEntryUs;
  uint32_t epochSeconds;
  uint64_t epochSetUs;
} rtcPetState;

// These ones actually change over time
uint8_t peeValue = 100;
uint8_t foodValue = 100;
uint8_t playValue = 100;

#define ACTIVITY_MESSAGE_COUNT 4

const char* const peeActivityMessages[ACTIVITY_MESSAGE_COUNT] = {
  "\"she looks so funny\"",
  "\"what a dumbass cuteass, just like you\"",
  "\"go poke her while she's peeing\"",
  "\"dont' look at her she's embarassed haw\""
};

const char* const playActivityMessages[ACTIVITY_MESSAGE_COUNT] = {
  "\"Not very athletic is she\"",
  "\"Bad Throw\"",
  "\"I think she'll last just one throw\"",
  "\"Play with her more she needs to train\""
};

const char* const eatActivityMessages[ACTIVITY_MESSAGE_COUNT] = {
  "\"Her greed makes me sick\"",
  "\"If gluttony had a form\"",
  "\"Is that healthy for her?\"",
  "\"Bruh she inhaled that\""
};

const char* currentActivityMessage = "";

// Interrupt-driven input queue
volatile InputEvent inputQueue[INPUT_QUEUE_SIZE];
volatile uint8_t inputQueueHead = 0;
volatile uint8_t inputQueueTail = 0;

volatile uint32_t lastMainIsrUs = 0;
volatile uint32_t lastUpIsrUs = 0;
volatile uint32_t lastDownIsrUs = 0;

// Locks interrupt capture while an action animation is running.
// This prevents accidental double-presses from being queued and replayed.
volatile bool inputCaptureLocked = false;

// ---------------- INPUT QUEUE / INTERRUPTS ----------------

void IRAM_ATTR pushInputEventFromISR(InputEvent event) {
  uint8_t nextHead = (inputQueueHead + 1) % INPUT_QUEUE_SIZE;

  // If queue is full, drop the newest event.
  // This prevents corrupting the queue.
  if (nextHead == inputQueueTail) {
    return;
  }

  inputQueue[inputQueueHead] = event;
  inputQueueHead = nextHead;
}

void IRAM_ATTR handleMainToggleInterrupt() {
  if (inputCaptureLocked) {
    return;
  }

  uint32_t now = micros();

  if (now - lastMainIsrUs < ISR_DEBOUNCE_US) {
    return;
  }

  lastMainIsrUs = now;
  pushInputEventFromISR(INPUT_MAIN);
}

void IRAM_ATTR handleUpInterrupt() {
  if (inputCaptureLocked) {
    return;
  }

  uint32_t now = micros();

  if (now - lastUpIsrUs < ISR_DEBOUNCE_US) {
    return;
  }

  lastUpIsrUs = now;
  pushInputEventFromISR(INPUT_UP);
}

void IRAM_ATTR handleDownInterrupt() {
  if (inputCaptureLocked) {
    return;
  }

  uint32_t now = micros();

  if (now - lastDownIsrUs < ISR_DEBOUNCE_US) {
    return;
  }

  lastDownIsrUs = now;
  pushInputEventFromISR(INPUT_DOWN);
}

InputEvent popInputEvent() {
  noInterrupts();

  if (inputQueueHead == inputQueueTail) {
    interrupts();
    return INPUT_NONE;
  }

  InputEvent event = inputQueue[inputQueueTail];
  inputQueueTail = (inputQueueTail + 1) % INPUT_QUEUE_SIZE;

  interrupts();

  return event;
}

void clearInputQueue() {
  noInterrupts();
  inputQueueHead = 0;
  inputQueueTail = 0;
  interrupts();
}

// ---------------- BASIC DRAW HELPERS ----------------

void clearImageBuffer() {
  memset(ImageBW, 0, ALLSCREEN_BYTES);
}

void fullRefresh() {
  EPD_DisplayImage(ImageBW);
  EPD_Update();
  EPD_Sleep();
}

bool getVerticalLsbPixel(const uint8_t* data, int width, int x, int y) {
  int page = y / 8;
  int bitIndex = y % 8;
  int byteIndex = page * width + x;

  uint8_t b = pgm_read_byte(&data[byteIndex]);
  return (b >> bitIndex) & 0x01;
}

void drawVerticalLsbImageCrop(
  const uint8_t* data,
  int srcW,
  int srcH,
  int srcX,
  int srcY,
  int destW,
  int destH
) {
  for (int y = 0; y < destH; y++) {
    for (int x = 0; x < destW; x++) {
      int sampleX = srcX + x;
      int sampleY = srcY + y;

      if (sampleX < 0 || sampleX >= srcW || sampleY < 0 || sampleY >= srcH) {
        EPD_DrawPoint(x, y, WHITE);
        continue;
      }

      bool pixelOn = getVerticalLsbPixel(data, srcW, sampleX, sampleY);
      EPD_DrawPoint(x, y, pixelOn ? BLACK : WHITE);
    }
  }
}

void fillRect(int x, int y, int w, int h, int color) {
  for (int yy = y; yy < y + h; yy++) {
    for (int xx = x; xx < x + w; xx++) {
      if (xx >= 0 && xx < SCREEN_W && yy >= 0 && yy < SCREEN_H) {
        EPD_DrawPoint(xx, yy, color);
      }
    }
  }
}

void clearLeftHudArea() {
  // Clear only the left-side UI area.
  // This does not touch the sprite box on the right.
  fillRect(0, 0, ANIMATION_BOX_X - 1, SCREEN_H, WHITE);
}

void drawBar(int x, int y, int w, int h, uint8_t value) {
  if (value > 100) value = 100;

  EPD_DrawRectangle(x, y, x + w, y + h, BLACK);

  int innerW = w - 2;
  int fillW = (innerW * value) / 100;

  fillRect(x + 1, y + 1, innerW, h - 1, WHITE);

  if (fillW > 0) {
    fillRect(x + 1, y + 1, fillW, h - 1, BLACK);
  }
}

void drawSelectionMarker(int x, int y, bool selected) {
  if (selected) {
    fillRect(x, y, SELECTION_MARKER_SIZE, SELECTION_MARKER_SIZE, BLACK);
  } else {
    fillRect(x, y, SELECTION_MARKER_SIZE, SELECTION_MARKER_SIZE, WHITE);
    EPD_DrawRectangle(x, y, x + SELECTION_MARKER_SIZE, y + SELECTION_MARKER_SIZE, BLACK);
  }
}

void showTextLine(int x, int y, const char* line) {
  EPD_ShowString(x, y, line, BLACK, ACTIVITY_TEXT_SIZE);
}

void drawWrappedActivityMessage(const char* message) {
  if (message == NULL || message[0] == '\0') {
    return;
  }

  const int charW = ACTIVITY_TEXT_SIZE / 2;
  const int maxChars = (ANIMATION_BOX_X - ACTIVITY_MESSAGE_X - 4) / charW;
  char line[32] = {};
  int lineLen = 0;
  int y = ACTIVITY_MESSAGE_Y;
  const char* p = message;

  while (*p != '\0' && y < SCREEN_H - ACTIVITY_MESSAGE_LINE_H) {
    while (*p == ' ') {
      p++;
    }

    if (*p == '\0') {
      break;
    }

    const char* word = p;
    int wordLen = 0;

    while (p[wordLen] != '\0' && p[wordLen] != ' ') {
      wordLen++;
    }

    int needsSpace = lineLen > 0 ? 1 : 0;
    if (lineLen > 0 && lineLen + needsSpace + wordLen > maxChars) {
      line[lineLen] = '\0';
      showTextLine(ACTIVITY_MESSAGE_X, y, line);
      y += ACTIVITY_MESSAGE_LINE_H;
      lineLen = 0;
      needsSpace = 0;
    }

    if (lineLen > 0 && lineLen < (int)sizeof(line) - 1) {
      line[lineLen++] = ' ';
    }

    for (int i = 0; i < wordLen && lineLen < (int)sizeof(line) - 1; i++) {
      line[lineLen++] = word[i];
    }

    p += wordLen;
  }

  if (lineLen > 0 && y < SCREEN_H) {
    line[lineLen] = '\0';
    showTextLine(ACTIVITY_MESSAGE_X, y, line);
  }
}

void selectActivityMessage(const char* const messages[]) {
  randomSeed((uint32_t)esp_timer_get_time() ^ (uint32_t)micros());
  currentActivityMessage = messages[random(ACTIVITY_MESSAGE_COUNT)];
}

void drawHud() {
  clearLeftHudArea();

  if (petMode == PET_ACTION) {
    drawWrappedActivityMessage(currentActivityMessage);
    return;
  }

  // Pee selector + bar
  drawSelectionMarker(ACTION_MARKER_X, PEE_Y + 4, selectedAction == ACTION_PEE);
  EPD_ShowString(ACTION_LABEL_X, PEE_Y, "PEE", BLACK, ACTION_TEXT_SIZE);
  drawBar(ACTION_BAR_X, PEE_Y + 4, ACTION_BAR_W, ACTION_BAR_H, peeValue);

  // Food selector + bar
  drawSelectionMarker(ACTION_MARKER_X, FOOD_Y + 4, selectedAction == ACTION_FOOD);
  EPD_ShowString(ACTION_LABEL_X, FOOD_Y, "FOOD", BLACK, ACTION_TEXT_SIZE);
  drawBar(ACTION_BAR_X, FOOD_Y + 4, ACTION_BAR_W, ACTION_BAR_H, foodValue);

  // Play selector + bar
  drawSelectionMarker(ACTION_MARKER_X, PLAY_Y + 4, selectedAction == ACTION_PLAY);
  EPD_ShowString(ACTION_LABEL_X, PLAY_Y, "PLAY", BLACK, ACTION_TEXT_SIZE);
  drawBar(ACTION_BAR_X, PLAY_Y + 4, ACTION_BAR_W, ACTION_BAR_H, playValue);
}

void drawHomeContent() {
  drawSelectionMarker(
    HOME_MARKER_X,
    HOME_TAMAGOTCHI_Y + 4,
    homeSelection == HOME_OPTION_TAMAGOTCHI
  );
  EPD_ShowString(HOME_LABEL_X, HOME_TAMAGOTCHI_Y, "TAMAGOTCHI", BLACK, HOME_TEXT_SIZE);

  drawSelectionMarker(
    HOME_MARKER_X,
    HOME_EREADER_Y + 4,
    homeSelection == HOME_OPTION_EREADER
  );
  EPD_ShowString(HOME_LABEL_X, HOME_EREADER_Y, "EREADER", BLACK, HOME_TEXT_SIZE);
}

void showHomeScreenFull() {
  EPD_Init();
  clearImageBuffer();
  drawHomeContent();
  fullRefresh();
}

void updateHomeSelectionPartial() {
  EPD_Init();

  for (int i = 0; i < ERASE_PULSES; i++) {
    fillRect(0, 0, SCREEN_W, SCREEN_H, WHITE);
    drawHomeContent();
    EPD_DisplayImage(ImageBW);
    EPD_PartUpdate();
  }

  EPD_Sleep();
}

void drawEreaderPlaceholderScreen() {
  EPD_Init();
  clearImageBuffer();
  EPD_ShowString(70, 48, "EREADER", BLACK, HOME_TEXT_SIZE);
  EPD_ShowString(52, 72, "Coming soon", BLACK, ACTIVITY_TEXT_SIZE);
  fullRefresh();
}

// ---------------- SPRITE HELPERS ----------------

const uint8_t* getAnimationFrame(const Animation* anim, uint8_t index) {
  return (const uint8_t*)pgm_read_ptr(&anim->frames[index]);
}

void eraseSpriteArea(const Animation* anim) {
  for (int y = ANIMATION_BOX_Y - 2; y < ANIMATION_BOX_Y + ANIMATION_BOX_H + 2; y++) {
    for (int x = ANIMATION_BOX_X - 2; x < ANIMATION_BOX_X + ANIMATION_BOX_W + 2; x++) {
      if (x >= 0 && x < SCREEN_W && y >= 24 && y < SCREEN_H) {
        EPD_DrawPoint(x, y, WHITE);
      }
    }
  }
}

bool getSpritePixelHorizontalMSB(const uint8_t* sprite, const Animation* anim, int x, int y) {
  int bytesPerRow = anim->width / 8;
  int byteIndex = y * bytesPerRow + (x / 8);
  int bitIndex = 7 - (x % 8);

  uint8_t b = pgm_read_byte(&sprite[byteIndex]);
  bool bitValue = (b >> bitIndex) & 0x01;

  if (anim->inverted) {
    return !bitValue;
  }

  return bitValue;
}

bool getSpritePixelVerticalLSB(const uint8_t* sprite, const Animation* anim, int x, int y) {
  int page = y / 8;
  int bitIndex = y % 8;
  int byteIndex = page * anim->width + x;

  uint8_t b = pgm_read_byte(&sprite[byteIndex]);
  bool bitValue = (b >> bitIndex) & 0x01;

  if (anim->inverted) {
    return !bitValue;
  }

  return bitValue;
}

bool getSpritePixel(const uint8_t* sprite, const Animation* anim, int x, int y) {
  if (anim->format == SPRITE_FORMAT_HORIZONTAL_MSB) {
    return getSpritePixelHorizontalMSB(sprite, anim, x, y);
  }

  if (anim->format == SPRITE_FORMAT_VERTICAL_LSB) {
    return getSpritePixelVerticalLSB(sprite, anim, x, y);
  }

  return false;
}

void drawSprite(const uint8_t* sprite, const Animation* anim) {
  for (int y = 0; y < ANIMATION_BOX_H; y++) {
    int sampleY = (y * anim->height) / ANIMATION_BOX_H;

    for (int x = 0; x < ANIMATION_BOX_W; x++) {
      int sampleX = (x * anim->width) / ANIMATION_BOX_W;
      bool pixelOn = getSpritePixel(sprite, anim, sampleX, sampleY);
      EPD_DrawPoint(ANIMATION_BOX_X + x, ANIMATION_BOX_Y + y, pixelOn ? BLACK : WHITE);
    }
  }
}

void drawAnimationFrame(const Animation* anim, const uint8_t* sprite) {
  if (appMode != APP_TAMAGOTCHI) {
    return;
  }

  EPD_Init();

  if (forceFullRefreshNextFrame) {
    forceFullRefreshNextFrame = false;
    clearImageBuffer();
    drawSprite(sprite, anim);
    drawHud();
    fullRefresh();
    return;
  }

  for (int i = 0; i < ERASE_PULSES; i++) {
    eraseSpriteArea(anim);
    drawSprite(sprite, anim);
    drawHud();

    EPD_DisplayImage(ImageBW);
    EPD_PartUpdate();
  }

  EPD_Sleep();
}

// ---------------- NEED / METER LOGIC ----------------

void applyNeedDrainTicks(uint32_t ticks) {
  if (ticks == 0) {
    return;
  }

  uint32_t totalDrain = ticks * NEED_DRAIN_AMOUNT;

  if (peeValue > totalDrain) {
    peeValue -= totalDrain;
  } else {
    peeValue = 0;
  }

  if (foodValue > totalDrain) {
    foodValue -= totalDrain;
  } else {
    foodValue = 0;
  }

  if (playValue > totalDrain) {
    playValue -= totalDrain;
  } else {
    playValue = 0;
  }
}

void drainNeedsOverTime() {
  unsigned long now = millis();

  if (now - lastNeedDrainMs < NEED_DRAIN_INTERVAL_MS) {
    return;
  }

  lastNeedDrainMs = now;

  if (peeValue > NEED_DRAIN_AMOUNT) {
    peeValue -= NEED_DRAIN_AMOUNT;
  } else {
    peeValue = 0;
  }

  if (foodValue > NEED_DRAIN_AMOUNT) {
    foodValue -= NEED_DRAIN_AMOUNT;
  } else {
    foodValue = 0;
  }

  if (playValue > NEED_DRAIN_AMOUNT) {
    playValue -= NEED_DRAIN_AMOUNT;
  } else {
    playValue = 0;
  }
}

// ---------------- PET STATE / SLEEP ----------------

time_t buildTimeEpoch() {
  char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  struct tm tmInfo = {};
  char monthName[4] = {};
  int day = 0;
  int year = 0;

  sscanf(__DATE__, "%s %d %d", monthName, &day, &year);
  tmInfo.tm_mday = day;
  tmInfo.tm_year = year - 1900;
  tmInfo.tm_mon = (strstr(months, monthName) - months) / 3;
  sscanf(__TIME__, "%d:%d:%d", &tmInfo.tm_hour, &tmInfo.tm_min, &tmInfo.tm_sec);

  return mktime(&tmInfo);
}

void refreshRtcEpochFromClock() {
  rtcPetState.epochSeconds = (uint32_t)time(NULL);
  rtcPetState.epochSetUs = esp_timer_get_time();
}

void initDeviceTime(bool restoreFromRtc) {
  if (restoreFromRtc && rtcPetState.epochSeconds > 100000) {
    struct timeval tv;
    tv.tv_sec = (time_t)rtcPetState.epochSeconds;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
    rtcPetState.epochSetUs = esp_timer_get_time();
    return;
  }

  time_t bootTime = buildTimeEpoch();
  struct timeval tv;
  tv.tv_sec = bootTime;
  tv.tv_usec = 0;
  settimeofday(&tv, NULL);
  refreshRtcEpochFromClock();
}

int getLocalHour() {
  time_t now = time(NULL) + TIMEZONE_OFFSET_SEC;
  struct tm tmInfo;
  gmtime_r(&now, &tmInfo);
  return tmInfo.tm_hour;
}

bool isNightTime() {
  int hour = getLocalHour();
  return hour >= 19 || hour < 7;
}

void showModeSelectSplashScreen() {
  Serial.println("Mode select splash");

  EPD_Init();
  clearImageBuffer();
  EPD_ShowString(88, 52, "SPLASH", BLACK, 16);
  // #region agent log
  uint32_t nonzeroBytes = 0;
  for (uint32_t i = 0; i < ALLSCREEN_BYTES; i++) {
    if (ImageBW[i] != 0) {
      nonzeroBytes++;
    }
  }
  Serial.printf(
    "{\"sessionId\":\"439210\",\"hypothesisId\":\"A\",\"location\":"
    "\"showModeSelectSplashScreen\",\"message\":\"splash buffer before refresh\","
    "\"data\":{\"nonzeroBytes\":%lu},\"timestamp\":%lu}\n",
    nonzeroBytes, millis()
  );
  // #endregion
  fullRefresh();
}

void showSleepSplashScreen() {
  const uint8_t* image = daySleepSplash;
  int imageW = DAY_SLEEP_SPLASH_W;
  int imageH = DAY_SLEEP_SPLASH_H;

  if (isNightTime()) {
    image = nightSleepSplash;
    imageW = NIGHT_SLEEP_SPLASH_W;
    imageH = NIGHT_SLEEP_SPLASH_H;
  }

  int srcX = (imageW - SCREEN_W) / 2;
  int srcY = (imageH - SCREEN_H) / 2;
  if (srcX < 0) {
    srcX = 0;
  }
  if (srcY < 0) {
    srcY = 0;
  }

  Serial.print("Sleep splash: ");
  Serial.println(isNightTime() ? "night" : "day");

  EPD_Init();
  EPD_ALL_Fill(WHITE);
  drawVerticalLsbImageCrop(image, imageW, imageH, srcX, srcY, SCREEN_W, SCREEN_H);
  fullRefresh();
}

void initDefaultPetState() {
  peeValue = 100;
  foodValue = 100;
  playValue = 100;
  selectedAction = ACTION_PEE;
  appMode = APP_TAMAGOTCHI;
  homeSelection = HOME_OPTION_TAMAGOTCHI;
}

void savePetStateToRtc() {
  refreshRtcEpochFromClock();

  rtcPetState.magic = RTC_PET_STATE_MAGIC;
  rtcPetState.peeValue = peeValue;
  rtcPetState.foodValue = foodValue;
  rtcPetState.playValue = playValue;
  rtcPetState.selectedAction = selectedAction;
  rtcPetState.appMode = (uint8_t)appMode;
  rtcPetState.homeSelection = (uint8_t)homeSelection;
  rtcPetState.sleepEntryUs = esp_timer_get_time();
}

bool restorePetStateFromRtc() {
  if (rtcPetState.magic != RTC_PET_STATE_MAGIC) {
    return false;
  }

  peeValue = rtcPetState.peeValue;
  foodValue = rtcPetState.foodValue;
  playValue = rtcPetState.playValue;
  selectedAction = (SelectedAction)rtcPetState.selectedAction;
  appMode = (AppMode)rtcPetState.appMode;
  homeSelection = (HomeOption)rtcPetState.homeSelection;

  if (appMode > APP_EREADER) {
    appMode = APP_TAMAGOTCHI;
  }

  if (homeSelection > HOME_OPTION_EREADER) {
    homeSelection = HOME_OPTION_TAMAGOTCHI;
  }

  return true;
}

void applyNeedsSinceSleep() {
  uint64_t nowUs = esp_timer_get_time();

  if (nowUs < rtcPetState.sleepEntryUs) {
    Serial.println("Skipping sleep need drain: timer reset during deep sleep");
    return;
  }

  uint64_t elapsedUs = nowUs - rtcPetState.sleepEntryUs;
  uint32_t ticks = elapsedUs / ((uint64_t)NEED_DRAIN_INTERVAL_MS * 1000ULL);
  applyNeedDrainTicks(ticks);
}

bool allButtonsReleased() {
  return digitalRead(HOME_KEY) == HIGH &&
         digitalRead(EXIT_KEY) == HIGH &&
         digitalRead(PRV_KEY) == HIGH &&
         digitalRead(NEXT_KEY) == HIGH &&
         digitalRead(OK_KEY) == HIGH;
}

void prepareWirelessForSleep() {
  // Future: WiFi.disconnect(true), WiFi.mode(WIFI_OFF), btStop()
}

void enterDeepSleep() {
  Serial.println("Entering deep sleep");

  savePetStateToRtc();

  if (appMode == APP_HOME) {
    showModeSelectSplashScreen();
  } else {
    showSleepSplashScreen();
  }

  digitalWrite(EPD_POWER, LOW);
  prepareWirelessForSleep();

  esp_sleep_enable_ext1_wakeup(WAKE_BUTTON_PIN_MASK, ESP_EXT1_WAKEUP_ANY_LOW);

  Serial.flush();
  esp_deep_sleep_start();
}

// ---------------- ANIMATION CONTROL ----------------

void enterHomeScreen() {
  Serial.println("Entering home screen");

  appMode = APP_HOME;
  petMode = PET_IDLE;
  activeAction = ACTIVE_NONE;
  currentActivityMessage = "";
  clearInputQueue();
  lastActivityMs = millis();
  showHomeScreenFull();
}

void enterTamagotchi() {
  Serial.println("Entering tamagotchi");

  appMode = APP_TAMAGOTCHI;
  forceFullRefreshNextFrame = true;
  clearInputQueue();
  startIdle();
  lastActivityMs = millis();
}

void enterEreaderMode() {
  Serial.println("Entering ereader placeholder");

  appMode = APP_EREADER;
  clearInputQueue();
  lastActivityMs = millis();
  drawEreaderPlaceholderScreen();
}

void startIdle() {
  currentAnimation = &idleAnimation;
  currentFrame = 0;
  petMode = PET_IDLE;
  activeAction = ACTIVE_NONE;
  lastFrameMs = millis();
  lastNeedDrainMs = millis();

  Serial.println("Idle animation started");

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );
}

void startDrowsySleeping() {
  if (currentAnimation == &sleepingAnimation) {
    return;
  }

  currentAnimation = &sleepingAnimation;
  currentFrame = 0;
  lastFrameMs = millis();

  Serial.println("Drowsy sleeping animation started");

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );
}

void returnToIdleFromDrowsy() {
  if (currentAnimation != &sleepingAnimation) {
    return;
  }

  currentAnimation = &idleAnimation;
  currentFrame = 0;
  lastFrameMs = millis();

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );
}

void startEating() {
  currentAnimation = &eatingAnimation;
  currentFrame = 0;
  petMode = PET_ACTION;
  activeAction = ACTIVE_FOOD;
  selectActivityMessage(eatActivityMessages);
  lastFrameMs = millis();

  Serial.println("Eating animation started");

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );
}

void startPlaying() {
  currentAnimation = &playAnimation;
  currentFrame = 0;
  petMode = PET_ACTION;
  activeAction = ACTIVE_PLAY;
  selectActivityMessage(playActivityMessages);
  lastFrameMs = millis();

  Serial.println("Play animation started");

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );
}

void startPeeing() {
  currentAnimation = &peeingAnimation;
  currentFrame = 0;
  petMode = PET_ACTION;
  activeAction = ACTIVE_PEE;
  selectActivityMessage(peeActivityMessages);
  lastFrameMs = millis();

  Serial.println("Peeing animation started");

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );
}

void startSelectedAction() {
  // The moment we commit to an action, stop collecting inputs.
  // This prevents accidental double-presses from being queued and replayed
  // after the action animation finishes.
  inputCaptureLocked = true;
  clearInputQueue();

  if (selectedAction == ACTION_PEE) {
    startPeeing();
  } else if (selectedAction == ACTION_FOOD) {
    startEating();
  } else {
    startPlaying();
  }
}

void finishActionAndReturnToIdle() {
  Serial.println("Action finished, returning to idle");

  if (activeAction == ACTIVE_PEE) {
    peeValue = 100;
  }

  if (activeAction == ACTIVE_FOOD) {
    foodValue = 100;
  }

  if (activeAction == ACTIVE_PLAY) {
    playValue = 100;
  }

  currentAnimation = &idleAnimation;
  currentFrame = 0;
  petMode = PET_IDLE;
  activeAction = ACTIVE_NONE;
  currentActivityMessage = "";

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );

  lastFrameMs = millis();
  lastNeedDrainMs = millis();
  lastActivityMs = millis();

  // Clear anything pressed during the action, then allow fresh inputs again.
  clearInputQueue();

  inputLockoutUntil = millis() + 100;
  inputCaptureLocked = false;
}

void advanceIdleAnimation() {
  currentFrame++;

  if (currentFrame >= currentAnimation->frameCount) {
    currentFrame = 0;
  }

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );
}

void advanceActionAnimation() {
  currentFrame++;

  if (currentFrame >= currentAnimation->frameCount) {
    finishActionAndReturnToIdle();
    return;
  }

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );
}

void moveSelectionUp() {
  if (selectedAction == ACTION_PLAY) {
    selectedAction = ACTION_FOOD;
  } else if (selectedAction == ACTION_FOOD) {
    selectedAction = ACTION_PEE;
  }

  Serial.print("Selected: ");
  Serial.println(selectedAction == ACTION_PEE ? "Pee" : "Food");

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );

  lastActivityMs = millis();
}

void moveSelectionDown() {
  if (selectedAction == ACTION_PEE) {
    selectedAction = ACTION_FOOD;
  } else if (selectedAction == ACTION_FOOD) {
    selectedAction = ACTION_PLAY;
  }

  Serial.print("Selected: ");
  if (selectedAction == ACTION_PEE) {
    Serial.println("Pee");
  } else if (selectedAction == ACTION_FOOD) {
    Serial.println("Food");
  } else {
    Serial.println("Play");
  }

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );

  lastActivityMs = millis();
}

void moveHomeSelectionUp() {
  if (homeSelection == HOME_OPTION_EREADER) {
    homeSelection = HOME_OPTION_TAMAGOTCHI;
  }

  Serial.println("Home selection: Tamagotchi");
  updateHomeSelectionPartial();
  lastActivityMs = millis();
}

void moveHomeSelectionDown() {
  if (homeSelection == HOME_OPTION_TAMAGOTCHI) {
    homeSelection = HOME_OPTION_EREADER;
  }

  Serial.println("Home selection: Ereader");
  updateHomeSelectionPartial();
  lastActivityMs = millis();
}

void onMainToggleHold() {
  lastActivityMs = millis();

  if (appMode == APP_TAMAGOTCHI) {
    enterHomeScreen();
    return;
  }

  if (appMode == APP_EREADER) {
    enterHomeScreen();
  }
}

void onMainToggleTap() {
  lastActivityMs = millis();

  if (appMode == APP_HOME) {
    if (homeSelection == HOME_OPTION_TAMAGOTCHI) {
      enterTamagotchi();
    } else {
      enterEreaderMode();
    }
    return;
  }

  if (appMode == APP_TAMAGOTCHI && petMode == PET_IDLE) {
    if (currentAnimation == &sleepingAnimation) {
      returnToIdleFromDrowsy();
    }

    startSelectedAction();
  }
}

bool isMainTogglePressed() {
  return digitalRead(MAIN_TOGGLE_KEY) == LOW;
}

void updateMainToggleInput() {
  bool pressed = isMainTogglePressed();
  unsigned long now = millis();

  if (pressed && !mainToggleDown) {
    mainToggleDown = true;
    mainToggleDownMs = now;
    mainToggleHoldHandled = false;
  }

  if (pressed && mainToggleDown && !mainToggleHoldHandled) {
    if (now - mainToggleDownMs >= MAIN_TOGGLE_HOLD_MS) {
      mainToggleHoldHandled = true;
      onMainToggleHold();
    }
  }

  if (!pressed && mainToggleDown) {
    if (!mainToggleHoldHandled) {
      onMainToggleTap();
    }

    mainToggleDown = false;
    mainToggleHoldHandled = false;
  }
}

// ---------------- INPUT HANDLING ----------------

void processInputEvent(InputEvent event) {
  if (event == INPUT_NONE) {
    return;
  }

  lastActivityMs = millis();

  if (event == INPUT_MAIN) {
    // Main toggle is handled via hold/tap polling.
    return;
  }

  if (appMode == APP_HOME) {
    if (event == INPUT_UP) {
      moveHomeSelectionUp();
      return;
    }

    if (event == INPUT_DOWN) {
      moveHomeSelectionDown();
      return;
    }

    return;
  }

  if (appMode == APP_EREADER) {
    return;
  }

  // While action is playing, events should not be processed.
  if (petMode == PET_ACTION) {
    return;
  }

  // Idle mode input handling
  if (petMode == PET_IDLE) {
    if (currentAnimation == &sleepingAnimation) {
      returnToIdleFromDrowsy();
    }

    if (event == INPUT_UP) {
      moveSelectionUp();
      return;
    }

    if (event == INPUT_DOWN) {
      moveSelectionDown();
      return;
    }
  }
}

// ---------------- SETUP / LOOP ----------------

void setup() {
  Serial.begin(115200);
  delay(500);

  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();

  pinMode(EPD_POWER, OUTPUT);
  digitalWrite(EPD_POWER, HIGH);

  pinMode(HOME_KEY, INPUT);
  pinMode(EXIT_KEY, INPUT);
  pinMode(PRV_KEY, INPUT);
  pinMode(NEXT_KEY, INPUT);
  pinMode(OK_KEY, INPUT);

  clearInputQueue();

  attachInterrupt(digitalPinToInterrupt(MAIN_TOGGLE_KEY), handleMainToggleInterrupt, FALLING);
  attachInterrupt(digitalPinToInterrupt(UP_KEY), handleUpInterrupt, FALLING);
  attachInterrupt(digitalPinToInterrupt(DOWN_KEY), handleDownInterrupt, FALLING);

  Serial.println("AdventureCattoEink");
  Serial.println("Interrupt input enabled");
  Serial.println("Main toggle tap: select action / enter mode");
  Serial.println("Main toggle hold (3s): exit to home screen");
  Serial.println("Up/down: select pee, food, play, or home menu");
  Serial.println("Inputs are queued immediately on button press");
  Serial.println("Input capture locks during actions to prevent duplicate actions");

  lastActivityMs = millis();

  bool restoredFromRtc = false;
  if (wakeCause == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.println("Woke from deep sleep");
    discardWakeInput = true;
    forceFullRefreshNextFrame = true;
    clearInputQueue();

    if (restorePetStateFromRtc()) {
      applyNeedsSinceSleep();
      restoredFromRtc = true;
    } else {
      initDefaultPetState();
    }
  } else {
    initDefaultPetState();
  }

  initDeviceTime(restoredFromRtc);

  if (appMode == APP_HOME) {
    showHomeScreenFull();
  } else if (appMode == APP_EREADER) {
    drawEreaderPlaceholderScreen();
  } else {
    startIdle();
  }

  lastFrameMs = millis();
  lastNeedDrainMs = millis();
  lastActivityMs = millis();
}

void loop() {
  unsigned long now = millis();

  if (appMode == APP_HOME) {
    drainNeedsOverTime();

    if (discardWakeInput) {
      while (popInputEvent() != INPUT_NONE) {}

      if (allButtonsReleased()) {
        discardWakeInput = false;
        clearInputQueue();
        lastActivityMs = millis();
      }

      delay(5);
      return;
    }

    updateMainToggleInput();

    InputEvent event = popInputEvent();
    if (event != INPUT_NONE) {
      processInputEvent(event);
      delay(5);
      return;
    }

    if (now - lastActivityMs >= INACTIVITY_TIMEOUT_MS) {
      enterDeepSleep();
      return;
    }

    delay(5);
    return;
  }

  if (appMode == APP_EREADER) {
    drainNeedsOverTime();

    if (discardWakeInput) {
      while (popInputEvent() != INPUT_NONE) {}

      if (allButtonsReleased()) {
        discardWakeInput = false;
        clearInputQueue();
        lastActivityMs = millis();
      }

      delay(5);
      return;
    }

    updateMainToggleInput();

    while (popInputEvent() != INPUT_NONE) {}

    delay(5);
    return;
  }

  // While an action animation is playing, do not process inputs.
  // Interrupt capture is also locked, so new button presses are ignored.
  if (petMode == PET_ACTION) {
    if (now - lastFrameMs >= currentAnimation->frameHoldMs) {
      lastFrameMs = now;
      advanceActionAnimation();
    }

    delay(5);
    return;
  }

  // Short lockout after returning from an action.
  if (now < inputLockoutUntil) {
    delay(5);
    return;
  }

  // Discard the button press that woke the device from deep sleep.
  if (discardWakeInput) {
    while (popInputEvent() != INPUT_NONE) {}

    if (petMode == PET_IDLE) {
      drainNeedsOverTime();

      if (now - lastFrameMs >= currentAnimation->frameHoldMs) {
        lastFrameMs = now;
        advanceIdleAnimation();
      }
    }

    if (allButtonsReleased()) {
      discardWakeInput = false;
      clearInputQueue();
      lastActivityMs = millis();
    }

    delay(5);
    return;
  }

  // Process exactly one queued input per loop.
  // This prevents multiple actions from firing all at once.
  InputEvent event = popInputEvent();
  if (event != INPUT_NONE) {
    processInputEvent(event);
    delay(5);
    return;
  }

  updateMainToggleInput();

  if (appMode != APP_TAMAGOTCHI) {
    delay(5);
    return;
  }

  // Idle state: idle loops and needs drain.
  if (petMode == PET_IDLE) {
    drainNeedsOverTime();

    unsigned long inactiveMs = now - lastActivityMs;

    if (inactiveMs >= INACTIVITY_TIMEOUT_MS) {
      enterDeepSleep();
      return;
    }

    if (inactiveMs >= SLEEP_ANIMATION_TIMEOUT_MS) {
      if (currentAnimation != &sleepingAnimation) {
        startDrowsySleeping();
      }
    } else if (currentAnimation == &sleepingAnimation) {
      returnToIdleFromDrowsy();
    }

    if (now - lastFrameMs >= currentAnimation->frameHoldMs) {
      lastFrameMs = now;
      advanceIdleAnimation();
    }
  }

  delay(5);
}
