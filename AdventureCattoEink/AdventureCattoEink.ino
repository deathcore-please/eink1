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
#define ANIMATION_BOX_W 96
#define ANIMATION_BOX_H 64
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
#define RTC_PET_STATE_MAGIC 0xCA770002UL
#define WAKE_BUTTON_PIN_MASK ((1ULL << EXIT_KEY) | (1ULL << HOME_KEY) | (1ULL << NEXT_KEY) | (1ULL << OK_KEY) | (1ULL << PRV_KEY))

// Local hour offset from UTC (seconds). Adjust for your timezone.
#define TIMEZONE_OFFSET_SEC (-5 * 3600)

// HUD positions
#define HAPPINESS_X 5
#define HAPPINESS_Y 5
#define HAPPINESS_W 100
#define HAPPINESS_H 8

#define ACTION_LABEL_X 18
#define ACTION_BAR_X 62
#define ACTION_BAR_W 78
#define ACTION_BAR_H 8

#define PEE_Y 48
#define FOOD_Y 72

enum PetMode {
  PET_IDLE,
  PET_ACTION
};

enum SelectedAction {
  ACTION_PEE,
  ACTION_FOOD
};

enum ActiveAction {
  ACTIVE_NONE,
  ACTIVE_PEE,
  ACTIVE_FOOD
};

enum InputEvent {
  INPUT_NONE,
  INPUT_MAIN,
  INPUT_UP,
  INPUT_DOWN
};

PetMode petMode = PET_IDLE;
SelectedAction selectedAction = ACTION_PEE;
ActiveAction activeAction = ACTIVE_NONE;

const Animation* currentAnimation = &idleAnimation;
uint8_t currentFrame = 0;

unsigned long lastFrameMs = 0;
unsigned long lastNeedDrainMs = 0;
unsigned long lastActivityMs = 0;
unsigned long inputLockoutUntil = 0;
bool discardWakeInput = false;

RTC_DATA_ATTR struct {
  uint32_t magic;
  uint8_t peeValue;
  uint8_t foodValue;
  uint8_t happinessValue;
  uint8_t selectedAction;
  uint64_t sleepEntryUs;
  uint32_t epochSeconds;
  uint64_t epochSetUs;
} rtcPetState;

// Hardcoded happiness for now
uint8_t happinessValue = 75;

// These ones actually change over time
uint8_t peeValue = 100;
uint8_t foodValue = 100;

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
  // This does not touch the 96x64 sprite box on the right.
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
    fillRect(x, y, 6, 6, BLACK);
  } else {
    fillRect(x, y, 6, 6, WHITE);
    EPD_DrawRectangle(x, y, x + 6, y + 6, BLACK);
  }
}

void drawHud() {
  clearLeftHudArea();

  // Top-left happiness bar, no text for now
  drawBar(HAPPINESS_X, HAPPINESS_Y, HAPPINESS_W, HAPPINESS_H, happinessValue);

  // Pee selector + bar
  drawSelectionMarker(6, PEE_Y + 4, selectedAction == ACTION_PEE);
  EPD_ShowString(ACTION_LABEL_X, PEE_Y, "Pee", BLACK, 16);
  drawBar(ACTION_BAR_X, PEE_Y + 4, ACTION_BAR_W, ACTION_BAR_H, peeValue);

  // Food selector + bar
  drawSelectionMarker(6, FOOD_Y + 4, selectedAction == ACTION_FOOD);
  EPD_ShowString(ACTION_LABEL_X, FOOD_Y, "Food", BLACK, 16);
  drawBar(ACTION_BAR_X, FOOD_Y + 4, ACTION_BAR_W, ACTION_BAR_H, foodValue);
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
  for (int y = 0; y < anim->height; y++) {
    for (int x = 0; x < anim->width; x++) {
      bool pixelOn = getSpritePixel(sprite, anim, x, y);
      EPD_DrawPoint(ANIMATION_BOX_X + x, ANIMATION_BOX_Y + y, pixelOn ? BLACK : WHITE);
    }
  }
}

void drawAnimationFrame(const Animation* anim, const uint8_t* sprite) {
  EPD_Init();

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
  happinessValue = 75;
  peeValue = 100;
  foodValue = 100;
  selectedAction = ACTION_PEE;
}

void savePetStateToRtc() {
  refreshRtcEpochFromClock();

  rtcPetState.magic = RTC_PET_STATE_MAGIC;
  rtcPetState.peeValue = peeValue;
  rtcPetState.foodValue = foodValue;
  rtcPetState.happinessValue = happinessValue;
  rtcPetState.selectedAction = selectedAction;
  rtcPetState.sleepEntryUs = esp_timer_get_time();
}

bool restorePetStateFromRtc() {
  if (rtcPetState.magic != RTC_PET_STATE_MAGIC) {
    return false;
  }

  peeValue = rtcPetState.peeValue;
  foodValue = rtcPetState.foodValue;
  happinessValue = rtcPetState.happinessValue;
  selectedAction = (SelectedAction)rtcPetState.selectedAction;
  return true;
}

void applyNeedsSinceSleep() {
  uint64_t elapsedUs = esp_timer_get_time() - rtcPetState.sleepEntryUs;
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
  showSleepSplashScreen();

  digitalWrite(EPD_POWER, LOW);
  prepareWirelessForSleep();

  esp_sleep_enable_ext1_wakeup(WAKE_BUTTON_PIN_MASK, ESP_EXT1_WAKEUP_ANY_LOW);

  Serial.flush();
  esp_deep_sleep_start();
}

// ---------------- ANIMATION CONTROL ----------------

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
  lastFrameMs = millis();

  Serial.println("Eating animation started");

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
  } else {
    startEating();
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

  currentAnimation = &idleAnimation;
  currentFrame = 0;
  petMode = PET_IDLE;
  activeAction = ACTIVE_NONE;
  lastFrameMs = millis();
  lastNeedDrainMs = millis();

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );

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
  if (selectedAction == ACTION_FOOD) {
    selectedAction = ACTION_PEE;
  }

  Serial.println("Selected: Pee");

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );
}

void moveSelectionDown() {
  if (selectedAction == ACTION_PEE) {
    selectedAction = ACTION_FOOD;
  }

  Serial.println("Selected: Food");

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );
}

// ---------------- INPUT HANDLING ----------------

void processInputEvent(InputEvent event) {
  if (event == INPUT_NONE) {
    return;
  }

  lastActivityMs = millis();

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

    if (event == INPUT_MAIN) {
      startSelectedAction();
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
  Serial.println("Main toggle: select action");
  Serial.println("Up/down: select pee or food");
  Serial.println("Inputs are queued immediately on button press");
  Serial.println("Input capture locks during pee/eat to prevent duplicate actions");

  lastActivityMs = millis();

  bool restoredFromRtc = false;
  if (wakeCause == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.println("Woke from deep sleep");
    discardWakeInput = true;
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

  startIdle();
}

void loop() {
  unsigned long now = millis();

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

  // Short lockout after returning from pee/eat.
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