#include <Arduino.h>
#include "EPD.h"
#include <string.h>
#include <pgmspace.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <Preferences.h>

#include "animations.h"
#include "idle_animation.h"
#include "eating_animation.h"
#include "peeing_animation.h"
#include "play_animation.h"
#include "petting_animation.h"
#include "sad_hungry_animation.h"
#include "sad_pee_animation.h"
#include "sad_play_animation.h"
#include "sad_pet_animation.h"
#include "sleeping_animation.h"
#include "deep_sleep_splashes.h"
#include "delivery_intro_animation.h"
#include "menu_cat_icon.h"
#include "menu_reader_icon.h"
#include "EreaderApp.h"
#include "SerialLibraryPortal.h"
#include "library_portal_image.h"
#include "PetNeeds.h"
#include "PetStateStore.h"
#include "PetTrace.h"
#include <esp_system.h>
#include "CareNotificationText.h"

extern uint8_t ImageBW[ALLSCREEN_BYTES];

// Button roles: toggle press = home, top button = OK.
#define HOME_KEY 5
#define EXIT_KEY 1
#define PRV_KEY  6
#define NEXT_KEY 4
#define OK_KEY   2

// Button meanings for this version
#define MAIN_TOGGLE_KEY OK_KEY
#define MENU_BUTTON_KEY HOME_KEY
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
#define DIRECTION_REARM_MS 70

// Need drain rates and happiness weights live in PetNeeds.h / PetNeeds.cpp.
#define DELIVERY_INTRO_REFRESH_PAUSE_MS 2000UL
#define DELIVERY_INTRO_LAST_FRAME_HOLD_MS 4000UL

// Deep sleep settings
#define SLEEP_ANIMATION_TIMEOUT_MS 15000
#define INACTIVITY_TIMEOUT_MS 30000
#define MENU_BUTTON_DEBOUNCE_MS 30
#define RTC_PET_STATE_MAGIC 0xCA770011UL
#define PET_CHECKPOINT_INTERVAL_MS (5UL * 60UL * 1000UL)
#define PET_CHECKPOINT_RETRY_MS 5000UL
#define RTC_SPLASH_ROTATION_MAGIC 0x5A1A5002UL
#define WAKE_BUTTON_PIN_MASK ((1ULL << EXIT_KEY) | (1ULL << HOME_KEY) | (1ULL << NEXT_KEY) | (1ULL << OK_KEY) | (1ULL << PRV_KEY))

// Local hour offset from UTC (seconds). Adjust for your timezone.
#define TIMEZONE_OFFSET_SEC (-5 * 3600)

// HUD positions
#define ACTION_MARKER_X 2
#define ACTION_MARKER_SIZE 6
#define ACTION_LABEL_X 12
#define ACTION_BAR_X 40
#define ACTION_BAR_W 78
#define ACTION_BAR_H 4
#define ACTION_TEXT_SIZE 12
#define ACTIVITY_TEXT_SIZE 12
#define ACTIVITY_MESSAGE_X 2
#define ACTIVITY_MESSAGE_Y 20
#define ACTIVITY_MESSAGE_LINE_H 14

#define HAPPY_Y 8
#define HAPPY_BAR_X ACTION_BAR_X
#define HAPPY_BAR_W ACTION_BAR_W
#define HAPPY_BAR_H 6
#define HAPPY_LABEL "=))"

#define PEE_Y 28
#define FOOD_Y 46
#define PLAY_Y 64
#define LOVE_Y 82
#define SLEEP_Y 104
#define SLEEP_DISABLED_LINE1_Y 98
#define SLEEP_DISABLED_LINE2_Y 110
#define SLEEP_LABEL "SLEEP"
#define SLEEP_DISABLED_LINE1 "she isn't sleepy"
#define SLEEP_DISABLED_LINE2 "one bit"

// User-initiated sleep durations / cooldowns (seconds)
#define SLEEP_SHORT_SEC (2UL * 3600UL)
#define SLEEP_LONG_SEC (10UL * 3600UL)
#define SLEEP_CD_SHORT_SEC (2UL * 3600UL)
#define SLEEP_CD_LONG_SEC (4UL * 3600UL)
#define SLEEP_WAKE_WINDOW_SEC (30UL * 60UL)
#define SLEEP_START_OK_LOCKOUT_MS 2000UL

// Action cooldowns after performing pee/food/play (seconds)
#define PEE_CD_SEC (1UL * 3600UL)
#define FOOD_CD_SEC (1UL * 3600UL)
#define PLAY_CD_SEC (30UL * 60UL)

// Cooldown message dialog layout (centered box)
#define CD_DIALOG_X 25
#define CD_DIALOG_Y 34
#define CD_DIALOG_W 200
#define CD_DIALOG_H 56
#define CD_DIALOG_MARGIN 10

// Sleep screen layout
#define SLEEP_REMAIN_LABEL_Y 30
#define SLEEP_REMAIN_BAR_X 2
#define SLEEP_REMAIN_BAR_Y 46
#define SLEEP_REMAIN_BAR_W 110
#define SLEEP_REMAIN_BAR_H 8
#define SLEEP_WAKE_BTN_X 2
#define SLEEP_WAKE_BTN_Y 64
#define SLEEP_WAKE_BTN_W 96
#define SLEEP_WAKE_BTN_H 18

// Sleep dialog layout
#define SLEEP_DIALOG_X 60
#define SLEEP_DIALOG_Y 30
#define SLEEP_DIALOG_W 130
#define SLEEP_DIALOG_H 62

// Home screen layout
#define HOME_TITLE_TEXT_SIZE 16
#define HOME_OPTION_TEXT_SIZE 16
#define HOME_TITLE_X 37
#define HOME_TITLE_Y 8
#define HOME_TILE_Y 42
#define HOME_TILE_W 110
#define HOME_TILE_H 64
#define HOME_TILE_GAP 10
#define HOME_TAMAGOTCHI_X 10
#define HOME_EREADER_X (HOME_TAMAGOTCHI_X + HOME_TILE_W + HOME_TILE_GAP)

// Top-right age label
#define DEFAULT_PET_START_AGE_DAYS 15UL
#define ONE_TIME_PET_START_AGE_DAYS 17UL
#define AGE_TEXT_SIZE 12
#define AGE_LABEL_MAX_CHARS 22
#define AGE_LABEL_PAD 2
#define AGE_LABEL_W ((AGE_LABEL_MAX_CHARS * (AGE_TEXT_SIZE / 2)) + (AGE_LABEL_PAD * 2))
#define AGE_LABEL_H (AGE_TEXT_SIZE + 2)
#define AGE_LABEL_X (SCREEN_W - AGE_LABEL_W)
#define AGE_LABEL_Y 0

#define RUNAWAY_TEXT_MARGIN 4

static const char RUNAWAY_MESSAGE[] =
  "\"She ran away. Maybe she was asking too much, or maybe you didn't deserve her. "
  "Neverthless, she's not coming back and we can only hope the best for her. "
  "May she lead a happier life, wherever she is. We hope you make better choices "
  "with your next baby, whenever you're ready\"";

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
  PET_ACTION,
  PET_RAN_AWAY,
  PET_INTRO,
  PET_SLEEP_SELECT,
  PET_SLEEPING,
  PET_COOLDOWN_MSG,
  PET_CARE_NOTICE
};

enum SelectedAction {
  ACTION_PEE,
  ACTION_FOOD,
  ACTION_PLAY,
  ACTION_PET,
  ACTION_SLEEP
};

enum ActiveAction {
  ACTIVE_NONE,
  ACTIVE_PEE,
  ACTIVE_FOOD,
  ACTIVE_PLAY,
  ACTIVE_PET
};

enum SadNeed {
  SAD_NEED_NONE,
  SAD_NEED_PEE,
  SAD_NEED_FOOD,
  SAD_NEED_PLAY,
  SAD_NEED_PETS
};

enum InputEvent {
  INPUT_NONE,
  INPUT_MAIN,
  INPUT_UP,
  INPUT_DOWN
};

enum PetStateRestoreSource {
  PET_STATE_FRESH,
  PET_STATE_RTC,
  PET_STATE_CHECKPOINT
};

AppMode appMode = APP_TAMAGOTCHI;
HomeOption homeSelection = HOME_OPTION_TAMAGOTCHI;
PetMode petMode = PET_IDLE;
SelectedAction selectedAction = ACTION_PEE;
ActiveAction activeAction = ACTIVE_NONE;
SadNeed activeSadNeed = SAD_NEED_NONE;

bool menuButtonDown = false;
bool menuButtonRawDown = false;
unsigned long menuButtonLastChangeMs = 0;
bool mainInputInterruptsAttached = false;

const Animation* currentAnimation = &idleAnimation;
uint8_t currentFrame = 0;

unsigned long lastFrameMs = 0;
unsigned long lastNeedDrainMs = 0;
unsigned long lastActivityMs = 0;
unsigned long inputLockoutUntil = 0;
unsigned long upReleasedSinceMs = 0;
unsigned long downReleasedSinceMs = 0;
bool discardWakeInput = false;
bool forceFullRefreshNextFrame = false;
bool eraseFullScreenNextSpriteFrame = false;
bool serialPortalDisplayActive = false;
bool serialPortalPreviousInputCaptureLocked = false;
bool hasSeenDeliveryIntro = false;
bool pendingRunaway = false;
const char* runawayDiagnosticReason = "none";
esp_sleep_wakeup_cause_t lastWakeCause = ESP_SLEEP_WAKEUP_UNDEFINED;
RTC_DATA_ATTR uint8_t rtcRunawayDeathLogged = 0;
bool petCheckpointDirty = false;
unsigned long lastPetCheckpointMs = 0;

void resetNeedDrainClock();
void updateHappiness();
bool hasRunawayCriticalNeed();
bool shouldShowRunaway();
void logRunawayDeathIfNeeded(const char* reason);
void startIdle();
void startPetAgeTimerIfNeeded();
void resetPetAgeTimer();
void savePetStateCheckpoint();
void servicePetStateCheckpoint();
PetStateRestoreSource restorePetStateForBoot(esp_sleep_wakeup_cause_t wakeCause);
PetTrace::Frame capturePetTraceFrame();
void tracePetEvent(const char* event, const char* detail = "");
void showRunawayScreen();
void maybeTriggerRunaway();
void advanceDeliveryIntroAnimation();
void enterHomeScreen();
bool isSleepOnCooldown();
void drawSleepRow();
void drawSleepScreen();
void drawSleepDialog();
void openSleepDialog();
void cancelSleepDialog();
void startSleep(uint32_t durationSec);
void wakeCatFromSleep(bool early);
bool isActionOnCooldown(SelectedAction action);
void openCooldownDialog(SelectedAction action);
void drawCooldownDialog();
void closeCooldownDialog();
bool showNextCareNotice();
void drawCareNoticeScreen();
void acknowledgeCareNotice();
void loopCareNotice();

RTC_DATA_ATTR PetStateStore::Snapshot rtcPetState;

RTC_DATA_ATTR struct {
  uint32_t magic;
  uint8_t order[DEEP_SLEEP_SPLASH_COUNT];
  uint8_t position;
} rtcSplashRotationState;

// These ones actually change over time
uint8_t peeValue = 100;
uint8_t foodValue = 100;
uint8_t playValue = 100;
uint8_t loveValue = 100;
uint8_t happinessValue = 100;
bool petAgeStarted = false;
uint32_t petBirthEpoch = 0;
uint32_t lastNeedDrainEpoch = 0;
uint32_t peeDrainCarry = 0;
uint32_t foodDrainCarry = 0;
uint32_t playDrainCarry = 0;
uint32_t loveDrainCarry = 0;
uint32_t happinessCrisisCarry = 0;
uint8_t happinessCrisisPenalty = 0;
PetCare::State careState = {};
PetCare::Notice currentCareNotice = {};
bool careNoticeAwaitRelease = false;
unsigned long careNoticeReleasedSinceMs = 0;

// User-initiated sleep state
uint32_t sleepStartEpoch = 0;
uint32_t sleepDurationSec = 0;
uint32_t sleepCooldownUntilEpoch = 0;
uint8_t sleepDialogChoice = 0;
unsigned long sleepOkIgnoredUntilMs = 0;
bool sleepWakeButtonShown = false;

// Action cooldown state
uint32_t peeCooldownUntilEpoch = 0;
uint32_t foodCooldownUntilEpoch = 0;
uint32_t playCooldownUntilEpoch = 0;
const char* cooldownDialogMessage = "";

#define PEE_ACTIVITY_MESSAGE_COUNT 4
#define PLAY_ACTIVITY_MESSAGE_COUNT 4
#define PET_ACTIVITY_MESSAGE_COUNT 7
#define EAT_ACTIVITY_MESSAGE_COUNT 4

const char* const peeActivityMessages[PEE_ACTIVITY_MESSAGE_COUNT] = {
  "\"she looks so funny\"",
  "\"what a dumbass cuteass, just like you\"",
  "\"go poke her while she's peeing\"",
  "\"dont' look at her she's embarassed haw\""
};

const char* const playActivityMessages[PLAY_ACTIVITY_MESSAGE_COUNT] = {
  "\"Not very athletic is she\"",
  "\"Bad Throw\"",
  "\"I think she'll last just one throw\"",
  "\"Play with her more she needs to train\""
};

const char* const petActivityMessages[PET_ACTIVITY_MESSAGE_COUNT] = {
  "\"she pretends not to like it\"",
  "\"tiny head, big thoughts\"",
  "\"she is accepting affection\"",
  "\"critical softness detected\"",
  "\"Keep her safe unless you want me to steal her\"",
  "\"Yeah you like it don't you\"",
  "\"This needs a new animation\""
};

const char* const eatActivityMessages[EAT_ACTIVITY_MESSAGE_COUNT] = {
  "\"Her greed makes me sick\"",
  "\"If gluttony had a form\"",
  "\"Is that healthy for her?\"",
  "\"Bruh she inhaled that\""
};

#define FOOD_COOLDOWN_MESSAGE_COUNT 4
#define PEE_COOLDOWN_MESSAGE_COUNT 2
#define PLAY_COOLDOWN_MESSAGE_COUNT 3

const char* const foodCooldownMessages[FOOD_COOLDOWN_MESSAGE_COUNT] = {
  "\"She's round rn\"",
  "\"She chonky\"",
  "\"The glutton is satiated\"",
  "\"She's full\""
};

const char* const peeCooldownMessages[PEE_COOLDOWN_MESSAGE_COUNT] = {
  "\"She literally just peed\"",
  "\"Her bowels are empty\""
};

const char* const playCooldownMessages[PLAY_COOLDOWN_MESSAGE_COUNT] = {
  "\"She no athlete bro\"",
  "\"ded\"",
  "\"no stamina left\""
};

const char* currentActivityMessage = "";

void refreshRtcEpochFromClock();

// Interrupt-driven input queue
volatile InputEvent inputQueue[INPUT_QUEUE_SIZE];
volatile uint8_t inputQueueHead = 0;
volatile uint8_t inputQueueTail = 0;

volatile uint32_t lastMainIsrUs = 0;
volatile uint32_t lastUpIsrUs = 0;
volatile uint32_t lastDownIsrUs = 0;
volatile uint32_t lastMenuIsrUs = 0;
volatile bool menuButtonPressPending = false;
volatile bool upInputArmed = true;
volatile bool downInputArmed = true;

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

  if (!upInputArmed) {
    return;
  }

  uint32_t now = micros();

  if (now - lastUpIsrUs < ISR_DEBOUNCE_US) {
    return;
  }

  lastUpIsrUs = now;
  upInputArmed = false;
  pushInputEventFromISR(INPUT_UP);
}

void IRAM_ATTR handleDownInterrupt() {
  if (inputCaptureLocked) {
    return;
  }

  if (!downInputArmed) {
    return;
  }

  uint32_t now = micros();

  if (now - lastDownIsrUs < ISR_DEBOUNCE_US) {
    return;
  }

  lastDownIsrUs = now;
  downInputArmed = false;
  pushInputEventFromISR(INPUT_DOWN);
}

void IRAM_ATTR handleMenuButtonInterrupt() {
  uint32_t now = micros();

  if (now - lastMenuIsrUs < ISR_DEBOUNCE_US) {
    return;
  }

  lastMenuIsrUs = now;
  menuButtonPressPending = true;
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

void clearAllPendingInput() {
  clearInputQueue();
  noInterrupts();
  menuButtonPressPending = false;
  interrupts();
}

void resetMainInputState() {
  unsigned long nowMs = millis();
  uint32_t nowUs = micros();

  noInterrupts();
  inputQueueHead = 0;
  inputQueueTail = 0;
  menuButtonPressPending = false;
  upInputArmed = true;
  downInputArmed = true;
  lastMainIsrUs = nowUs;
  lastUpIsrUs = nowUs;
  lastDownIsrUs = nowUs;
  interrupts();

  upReleasedSinceMs = digitalRead(UP_KEY) == HIGH ? nowMs : 0;
  downReleasedSinceMs = digitalRead(DOWN_KEY) == HIGH ? nowMs : 0;
  menuButtonRawDown = isMenuButtonPressed();
  menuButtonDown = menuButtonRawDown;
  menuButtonLastChangeMs = nowMs;
}

void attachMainInputInterrupts() {
  if (mainInputInterruptsAttached) {
    return;
  }

  attachInterrupt(digitalPinToInterrupt(MAIN_TOGGLE_KEY), handleMainToggleInterrupt, FALLING);
  attachInterrupt(digitalPinToInterrupt(UP_KEY), handleUpInterrupt, FALLING);
  attachInterrupt(digitalPinToInterrupt(DOWN_KEY), handleDownInterrupt, FALLING);
  mainInputInterruptsAttached = true;
}

void detachMainInputInterrupts() {
  if (!mainInputInterruptsAttached) {
    return;
  }

  detachInterrupt(digitalPinToInterrupt(MAIN_TOGGLE_KEY));
  detachInterrupt(digitalPinToInterrupt(UP_KEY));
  detachInterrupt(digitalPinToInterrupt(DOWN_KEY));
  mainInputInterruptsAttached = false;
}

void updateDirectionalInputArming() {
  unsigned long now = millis();

  if (digitalRead(UP_KEY) == HIGH) {
    if (upReleasedSinceMs == 0) {
      upReleasedSinceMs = now;
    } else if (now - upReleasedSinceMs >= DIRECTION_REARM_MS) {
      noInterrupts();
      upInputArmed = true;
      interrupts();
    }
  } else {
    upReleasedSinceMs = 0;
  }

  if (digitalRead(DOWN_KEY) == HIGH) {
    if (downReleasedSinceMs == 0) {
      downReleasedSinceMs = now;
    } else if (now - downReleasedSinceMs >= DIRECTION_REARM_MS) {
      noInterrupts();
      downInputArmed = true;
      interrupts();
    }
  } else {
    downReleasedSinceMs = 0;
  }
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

void drawVerticalLsbBitmap(int destX, int destY, const uint8_t* data, int width, int height) {
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int screenX = destX + x;
      int screenY = destY + y;

      if (screenX < 0 || screenX >= SCREEN_W || screenY < 0 || screenY >= SCREEN_H) {
        continue;
      }

      bool pixelOn = getVerticalLsbPixel(data, width, x, y);
      EPD_DrawPoint(screenX, screenY, pixelOn ? BLACK : WHITE);
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

void drawHappinessRow() {
  EPD_ShowString(ACTION_LABEL_X, HAPPY_Y, HAPPY_LABEL, BLACK, ACTION_TEXT_SIZE);
  drawBar(HAPPY_BAR_X, HAPPY_Y + 4, HAPPY_BAR_W, HAPPY_BAR_H, happinessValue);
}

void drawActionRow(const char* label, int y, uint8_t value, bool selected) {
  if (selected) {
    fillRect(ACTION_MARKER_X, y + 3, ACTION_MARKER_SIZE, ACTION_MARKER_SIZE, BLACK);
  } else {
    fillRect(ACTION_MARKER_X, y + 3, ACTION_MARKER_SIZE, ACTION_MARKER_SIZE, WHITE);
    EPD_DrawRectangle(
      ACTION_MARKER_X,
      y + 3,
      ACTION_MARKER_X + ACTION_MARKER_SIZE,
      y + 3 + ACTION_MARKER_SIZE,
      BLACK
    );
  }

  EPD_ShowString(ACTION_LABEL_X, y, label, BLACK, ACTION_TEXT_SIZE);
  drawBar(ACTION_BAR_X, y + 4, ACTION_BAR_W, ACTION_BAR_H, value);
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

void drawWrappedFullScreenMessage(const char* message, int textSize, int x, int y, int maxWidth) {
  if (message == NULL || message[0] == '\0') {
    return;
  }

  const int charW = textSize / 2;
  const int maxChars = maxWidth / charW;
  const int lineH = textSize + 2;
  char line[64] = {};
  int lineLen = 0;
  const char* p = message;

  while (*p != '\0' && y < SCREEN_H - lineH) {
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
      EPD_ShowString(x, y, line, BLACK, textSize);
      y += lineH;
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
    EPD_ShowString(x, y, line, BLACK, textSize);
  }
}

void selectActivityMessage(const char* const messages[], uint8_t count) {
  if (count == 0) {
    currentActivityMessage = "";
    return;
  }

  randomSeed((uint32_t)esp_timer_get_time() ^ (uint32_t)micros());
  currentActivityMessage = messages[random(count)];
}

void resetPetAgeTimer() {
  // The next intro starts a new age timer using the configured starting age.
  petAgeStarted = false;
  petBirthEpoch = 0;
  rtcPetState.petAgeStarted = 0;
  rtcPetState.petBirthEpoch = 0;
}

uint32_t consumeStartingPetAgeDays() {
  Preferences prefs;
  uint32_t days = DEFAULT_PET_START_AGE_DAYS;
  if (!prefs.begin("pet-age", false)) {
    Serial.println("Age override unavailable; using the normal starting age");
    return days;
  }
  // Consume only once, after the first intro. Reboots and normal uploads must
  // not grant the 17-day starting age to another life.
  if (!prefs.getBool("start17-used", false)) {
    if (prefs.putBool("start17-used", true) == 1) {
      days = ONE_TIME_PET_START_AGE_DAYS;
    } else {
      Serial.println("Age override could not be saved; using the normal starting age");
    }
  }
  prefs.end();
  return days;
}

void startPetAgeTimerIfNeeded() {
  if (petAgeStarted) {
    return;
  }

  petBirthEpoch = (uint32_t)time(NULL) - (consumeStartingPetAgeDays() * 24UL * 3600UL);
  petAgeStarted = true;
  rtcPetState.petAgeStarted = 1;
  rtcPetState.petBirthEpoch = petBirthEpoch;
  refreshRtcEpochFromClock();
}

uint8_t daysInMonth(int month, int year) {
  static const uint8_t lengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 0 || month > 11) {
    return 30;
  }
  if (month == 1) {
    bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    return leap ? 29 : 28;
  }
  return lengths[month];
}

void computePetAge(uint32_t& years, uint32_t& months, uint32_t& days) {
  years = 0;
  months = 0;
  days = 0;

  if (!petAgeStarted || petBirthEpoch == 0) {
    return;
  }

  uint32_t nowEpoch = (uint32_t)time(NULL);
  if (nowEpoch <= petBirthEpoch) {
    return;
  }

  time_t birthLocal = (time_t)petBirthEpoch + TIMEZONE_OFFSET_SEC;
  time_t nowLocal = (time_t)nowEpoch + TIMEZONE_OFFSET_SEC;
  struct tm birth;
  struct tm now;
  gmtime_r(&birthLocal, &birth);
  gmtime_r(&nowLocal, &now);

  int y = now.tm_year - birth.tm_year;
  int mo = now.tm_mon - birth.tm_mon;
  int d = now.tm_mday - birth.tm_mday;

  if (d < 0) {
    int pm = now.tm_mon - 1;
    int py = now.tm_year + 1900;
    if (pm < 0) {
      pm = 11;
      py--;
    }
    d += daysInMonth(pm, py);
    mo--;
  }
  if (mo < 0) {
    mo += 12;
    y--;
  }
  if (y < 0) {
    y = 0;
    mo = 0;
    d = 0;
  }

  years = (uint32_t)y;
  months = (uint32_t)mo;
  days = (uint32_t)d;
}

void formatPetAgeLabel(char* out, size_t outSize) {
  uint32_t years = 0;
  uint32_t months = 0;
  uint32_t days = 0;
  computePetAge(years, months, days);

  if (years == 0 && months == 0) {
    snprintf(out, outSize, "%lu %s", (unsigned long)days, days == 1 ? "Day" : "Days");
    return;
  }

  if (years == 0) {
    snprintf(out, outSize, "%lu %s", (unsigned long)months, months == 1 ? "Month" : "Months");
    return;
  }

  snprintf(
    out,
    outSize,
    "%lu %s %lu %s",
    (unsigned long)years,
    years == 1 ? "Year" : "Years",
    (unsigned long)months,
    months == 1 ? "Month" : "Months"
  );
}

void drawAgeLabel() {
  char label[AGE_LABEL_MAX_CHARS + 1] = {};
  formatPetAgeLabel(label, sizeof(label));
  fillRect(AGE_LABEL_X, AGE_LABEL_Y, AGE_LABEL_W, AGE_LABEL_H, WHITE);

  int charW = AGE_TEXT_SIZE / 2;
  int textW = strlen(label) * charW;
  int x = SCREEN_W - textW - AGE_LABEL_PAD;
  if (x < 0) {
    x = 0;
  }

  EPD_ShowString(x, AGE_LABEL_Y, label, BLACK, AGE_TEXT_SIZE);
}

void drawHud() {
  if (petMode == PET_RAN_AWAY || petMode == PET_INTRO || petMode == PET_SLEEP_SELECT ||
      petMode == PET_COOLDOWN_MSG) {
    return;
  }

  clearLeftHudArea();

  if (petMode == PET_ACTION) {
    drawWrappedActivityMessage(currentActivityMessage);
    return;
  }

  if (petMode == PET_SLEEPING) {
    drawSleepScreen();
    return;
  }

  drawAgeLabel();

  drawHappinessRow();
  drawActionRow("PEE", PEE_Y, peeValue, selectedAction == ACTION_PEE);
  drawActionRow("FOOD", FOOD_Y, foodValue, selectedAction == ACTION_FOOD);
  drawActionRow("PLAY", PLAY_Y, playValue, selectedAction == ACTION_PLAY);
  drawActionRow("PETS", LOVE_Y, loveValue, selectedAction == ACTION_PET);
  drawSleepRow();
}

bool isSleepOnCooldown() {
  return sleepCooldownUntilEpoch != 0 && (uint32_t)time(NULL) < sleepCooldownUntilEpoch;
}

bool isActionOnCooldown(SelectedAction action) {
  uint32_t nowEpoch = (uint32_t)time(NULL);
  if (action == ACTION_PEE) {
    return peeCooldownUntilEpoch != 0 && nowEpoch < peeCooldownUntilEpoch;
  }
  if (action == ACTION_FOOD) {
    return foodCooldownUntilEpoch != 0 && nowEpoch < foodCooldownUntilEpoch;
  }
  if (action == ACTION_PLAY) {
    return playCooldownUntilEpoch != 0 && nowEpoch < playCooldownUntilEpoch;
  }
  return false;
}

void drawSleepRow() {
  if (isSleepOnCooldown()) {
    EPD_ShowString(ACTION_MARKER_X, SLEEP_DISABLED_LINE1_Y, SLEEP_DISABLED_LINE1, BLACK, ACTION_TEXT_SIZE);
    EPD_ShowString(ACTION_MARKER_X, SLEEP_DISABLED_LINE2_Y, SLEEP_DISABLED_LINE2, BLACK, ACTION_TEXT_SIZE);
    return;
  }

  bool selected = (selectedAction == ACTION_SLEEP);
  if (selected) {
    fillRect(ACTION_MARKER_X, SLEEP_Y + 3, ACTION_MARKER_SIZE, ACTION_MARKER_SIZE, BLACK);
  } else {
    fillRect(ACTION_MARKER_X, SLEEP_Y + 3, ACTION_MARKER_SIZE, ACTION_MARKER_SIZE, WHITE);
    EPD_DrawRectangle(
      ACTION_MARKER_X,
      SLEEP_Y + 3,
      ACTION_MARKER_X + ACTION_MARKER_SIZE,
      SLEEP_Y + 3 + ACTION_MARKER_SIZE,
      BLACK
    );
  }

  EPD_ShowString(ACTION_LABEL_X, SLEEP_Y, SLEEP_LABEL, BLACK, ACTION_TEXT_SIZE);
}

void drawSleepScreen() {
  drawHappinessRow();

  uint32_t nowEpoch = (uint32_t)time(NULL);
  uint32_t elapsed = (nowEpoch > sleepStartEpoch) ? (nowEpoch - sleepStartEpoch) : 0;
  uint32_t remaining = (elapsed < sleepDurationSec) ? (sleepDurationSec - elapsed) : 0;
  uint8_t pct = sleepDurationSec ? (uint8_t)(((uint64_t)remaining * 100) / sleepDurationSec) : 0;

  EPD_ShowString(SLEEP_REMAIN_BAR_X, SLEEP_REMAIN_LABEL_Y, "WAKES IN", BLACK, ACTION_TEXT_SIZE);
  drawBar(SLEEP_REMAIN_BAR_X, SLEEP_REMAIN_BAR_Y, SLEEP_REMAIN_BAR_W, SLEEP_REMAIN_BAR_H, pct);

  if (elapsed < SLEEP_WAKE_WINDOW_SEC) {
    EPD_DrawRectangle(
      SLEEP_WAKE_BTN_X,
      SLEEP_WAKE_BTN_Y,
      SLEEP_WAKE_BTN_X + SLEEP_WAKE_BTN_W,
      SLEEP_WAKE_BTN_Y + SLEEP_WAKE_BTN_H,
      BLACK
    );
    EPD_ShowString(SLEEP_WAKE_BTN_X + 8, SLEEP_WAKE_BTN_Y + 3, "WAKE UP", BLACK, ACTION_TEXT_SIZE);
  }
}

void drawSleepDialog() {
  EPD_Init();
  clearImageBuffer();

  EPD_DrawRectangle(
    SLEEP_DIALOG_X,
    SLEEP_DIALOG_Y,
    SLEEP_DIALOG_X + SLEEP_DIALOG_W,
    SLEEP_DIALOG_Y + SLEEP_DIALOG_H,
    BLACK
  );

  EPD_ShowString(SLEEP_DIALOG_X + 8, SLEEP_DIALOG_Y + 6, "Sleep how long?", BLACK, ACTION_TEXT_SIZE);

  int opt1Y = SLEEP_DIALOG_Y + 24;
  int opt2Y = SLEEP_DIALOG_Y + 42;
  int markerX = SLEEP_DIALOG_X + 12;
  int labelX = SLEEP_DIALOG_X + 26;

  for (int i = 0; i < 2; i++) {
    int oy = (i == 0) ? opt1Y : opt2Y;
    bool sel = (sleepDialogChoice == i);
    if (sel) {
      fillRect(markerX, oy + 1, ACTION_MARKER_SIZE, ACTION_MARKER_SIZE, BLACK);
    } else {
      EPD_DrawRectangle(markerX, oy + 1, markerX + ACTION_MARKER_SIZE, oy + 1 + ACTION_MARKER_SIZE, BLACK);
    }
    EPD_ShowString(labelX, oy, i == 0 ? "2h" : "10h", BLACK, ACTION_TEXT_SIZE);
  }

  fullRefresh();
}

void openSleepDialog() {
  petMode = PET_SLEEP_SELECT;
  sleepDialogChoice = 0;
  activeAction = ACTIVE_NONE;
  currentActivityMessage = "";
  clearInputQueue();
  forceFullRefreshNextFrame = true;
  drawSleepDialog();
  inputLockoutUntil = millis() + 150;
  lastActivityMs = millis();
}

void cancelSleepDialog() {
  petMode = PET_IDLE;
  sleepDialogChoice = 0;
  activeAction = ACTIVE_NONE;
  currentActivityMessage = "";
  clearInputQueue();
  forceFullRefreshNextFrame = true;
  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );
  inputLockoutUntil = millis() + 100;
  lastActivityMs = millis();
}

void drawCooldownDialog() {
  EPD_Init();
  clearImageBuffer();

  EPD_DrawRectangle(
    CD_DIALOG_X,
    CD_DIALOG_Y,
    CD_DIALOG_X + CD_DIALOG_W,
    CD_DIALOG_Y + CD_DIALOG_H,
    BLACK
  );

  drawWrappedFullScreenMessage(
    cooldownDialogMessage,
    ACTIVITY_TEXT_SIZE,
    CD_DIALOG_X + CD_DIALOG_MARGIN,
    CD_DIALOG_Y + CD_DIALOG_MARGIN,
    CD_DIALOG_W - (CD_DIALOG_MARGIN * 2)
  );

  fullRefresh();
}

void openCooldownDialog(SelectedAction action) {
  randomSeed((uint32_t)esp_timer_get_time() ^ (uint32_t)micros());

  if (action == ACTION_FOOD) {
    cooldownDialogMessage = foodCooldownMessages[random(FOOD_COOLDOWN_MESSAGE_COUNT)];
  } else if (action == ACTION_PEE) {
    cooldownDialogMessage = peeCooldownMessages[random(PEE_COOLDOWN_MESSAGE_COUNT)];
  } else if (action == ACTION_PLAY) {
    cooldownDialogMessage = playCooldownMessages[random(PLAY_COOLDOWN_MESSAGE_COUNT)];
  } else {
    cooldownDialogMessage = "";
  }

  petMode = PET_COOLDOWN_MSG;
  activeAction = ACTIVE_NONE;
  currentActivityMessage = "";
  clearInputQueue();
  forceFullRefreshNextFrame = true;
  drawCooldownDialog();
  inputLockoutUntil = millis() + 150;
  lastActivityMs = millis();
}

void closeCooldownDialog() {
  petMode = PET_IDLE;
  cooldownDialogMessage = "";
  clearInputQueue();
  forceFullRefreshNextFrame = true;
  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );
  inputLockoutUntil = millis() + 100;
  lastActivityMs = millis();
}

void resetPetLifeState() {
  tracePetEvent("life_reset_before");
  careState = {};
  careNoticeAwaitRelease = false;
  peeValue = 80;
  foodValue = 80;
  playValue = 80;
  loveValue = 80;
  happinessValue = 80;
  happinessCrisisCarry = 0;
  happinessCrisisPenalty = 0;
  activeSadNeed = SAD_NEED_NONE;
  selectedAction = ACTION_PEE;
  pendingRunaway = false;
  runawayDiagnosticReason = "reset_pet_life_state";
  rtcRunawayDeathLogged = 0;
  hasSeenDeliveryIntro = false;
  sleepStartEpoch = 0;
  sleepDurationSec = 0;
  sleepCooldownUntilEpoch = 0;
  sleepWakeButtonShown = false;
  peeCooldownUntilEpoch = 0;
  foodCooldownUntilEpoch = 0;
  playCooldownUntilEpoch = 0;
  resetPetAgeTimer();
  resetNeedDrainClock();
  updateHappiness();
  tracePetEvent("life_reset_after");
}

void logRunawayDeathIfNeeded(const char* reason) {
  if (rtcRunawayDeathLogged != 0) {
    return;
  }

  const char* loggedReason = reason;
  if (happinessValue > 0 || !hasRunawayCriticalNeed()) {
    loggedReason = "unexpected_runaway_show";
  } else if (loggedReason == nullptr || loggedReason[0] == '\0' || strcmp(loggedReason, "none") == 0) {
    loggedReason = pendingRunaway ? "pending_runaway_displayed" : "happiness_zero_food_or_pee";
  }

  rtcRunawayDeathLogged = 1;
  serialLibraryPortalAppendDeathLog(
    loggedReason,
    (uint32_t)time(NULL),
    millis(),
    (int)lastWakeCause,
    (int)appMode,
    (int)petMode,
    (int)activeAction,
    pendingRunaway,
    happinessValue,
    peeValue,
    foodValue,
    playValue,
    loveValue,
    lastNeedDrainEpoch,
    peeDrainCarry,
    foodDrainCarry,
    playDrainCarry,
    loveDrainCarry,
    happinessCrisisCarry,
    happinessCrisisPenalty,
    sleepDurationSec > 0 && (uint32_t)time(NULL) < sleepStartEpoch + sleepDurationSec,
    sleepStartEpoch,
    sleepDurationSec,
    careState
  );
}

void showRunawayScreen() {
  tracePetEvent("runaway_display", runawayDiagnosticReason);
  logRunawayDeathIfNeeded(runawayDiagnosticReason);
  pendingRunaway = false;

  petMode = PET_RAN_AWAY;
  petCheckpointDirty = true;
  activeAction = ACTIVE_NONE;
  currentActivityMessage = "";
  clearInputQueue();

  EPD_Init();
  clearImageBuffer();
  drawWrappedFullScreenMessage(
    RUNAWAY_MESSAGE,
    ACTIVITY_TEXT_SIZE,
    RUNAWAY_TEXT_MARGIN,
    RUNAWAY_TEXT_MARGIN,
    SCREEN_W - (RUNAWAY_TEXT_MARGIN * 2)
  );
  fullRefresh();

  inputCaptureLocked = false;
  inputLockoutUntil = millis() + 150;
  lastActivityMs = millis();

  Serial.println("Runaway screen shown");
}

void startDeliveryIntro() {
  petMode = PET_INTRO;
  activeAction = ACTIVE_NONE;
  currentActivityMessage = "";
  currentAnimation = &deliveryIntroAnimation;
  currentFrame = 0;
  inputCaptureLocked = true;
  forceFullRefreshNextFrame = false;
  clearInputQueue();

  EPD_Init();
  clearImageBuffer();
  fullRefresh();
  delay(DELIVERY_INTRO_REFRESH_PAUSE_MS);

  lastFrameMs = millis();
  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );

  lastActivityMs = millis();
  Serial.println("Delivery intro animation started");
}

void completeDeliveryIntro() {
  hasSeenDeliveryIntro = true;
  rtcPetState.hasSeenDeliveryIntro = 1;
  resetNeedDrainClock();
  PetNeeds::State state = captureNeedState();
  PetNeeds::observeCare(state, lastNeedDrainEpoch);
  applyNeedState(state);
  inputCaptureLocked = false;
  clearInputQueue();
  inputLockoutUntil = millis() + 100;
  startPetAgeTimerIfNeeded();
  savePetStateCheckpoint();
  tracePetEvent("intro_completed");
  eraseFullScreenNextSpriteFrame = true;
  startIdle();
  Serial.println("Delivery intro complete");
}

uint32_t getDeliveryIntroCurrentFrameHoldMs() {
  if (currentFrame >= DELIVERY_INTRO_FRAME_COUNT - 2) {
    return DELIVERY_INTRO_LAST_FRAME_HOLD_MS;
  }

  return currentAnimation->frameHoldMs;
}

void advanceDeliveryIntroAnimation() {
  currentFrame++;

  if (currentFrame >= DELIVERY_INTRO_FRAME_COUNT) {
    completeDeliveryIntro();
    return;
  }

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );
  lastActivityMs = millis();
}

void onRunawayAcknowledged() {
  resetPetLifeState();
  enterHomeScreen();
}

void enterTamagotchiInitialScreen() {
  if (shouldShowRunaway() || petMode == PET_RAN_AWAY) {
    showRunawayScreen();
    return;
  }

  if (hasSeenDeliveryIntro && showNextCareNotice()) {
    return;
  }

  // Resume an in-progress user-initiated sleep (e.g. after deep sleep).
  if (sleepDurationSec > 0) {
    uint32_t nowEpoch = (uint32_t)time(NULL);
    uint32_t elapsed = (nowEpoch > sleepStartEpoch) ? (nowEpoch - sleepStartEpoch) : 0;
    if (elapsed >= sleepDurationSec) {
      wakeCatFromSleep(false);
    } else {
      petMode = PET_SLEEPING;
      activeAction = ACTIVE_NONE;
      currentActivityMessage = "";
      currentAnimation = &sleepingAnimation;
      currentFrame = 0;
      sleepWakeButtonShown = (elapsed < SLEEP_WAKE_WINDOW_SEC);
      forceFullRefreshNextFrame = true;
      lastFrameMs = millis();
      drawAnimationFrame(
        currentAnimation,
        getAnimationFrame(currentAnimation, currentFrame)
      );
    }
    return;
  }

  if (petMode == PET_INTRO) {
    startDeliveryIntro();
    return;
  }

  if (!hasSeenDeliveryIntro) {
    startDeliveryIntro();
    return;
  }

  startPetAgeTimerIfNeeded();
  startIdle();
}

void drawCareNoticeScreen() {
  inputCaptureLocked = true;
  clearAllPendingInput();
  careNoticeAwaitRelease = true;
  careNoticeReleasedSinceMs = 0;
  EPD_Init();
  clearImageBuffer();
  const char* heading = CareNotificationText::heading(currentCareNotice.category);
  uint16_t headingWidth = static_cast<uint16_t>(strlen(heading) * 8 + 1);
  EPD_ShowStringBold16(static_cast<uint16_t>(SCREEN_W - 6 - headingWidth), 6, heading, BLACK);
  drawWrappedFullScreenMessage(
    CareNotificationText::message(currentCareNotice), 12, 6, 30, SCREEN_W - 12
  );
  fullRefresh();
  clearAllPendingInput();
  lastActivityMs = millis();
}

bool showNextCareNotice() {
  if (!PetCare::nextNotice(careState, currentCareNotice)) {
    return false;
  }
  petMode = PET_CARE_NOTICE;
  activeAction = ACTIVE_NONE;
  currentActivityMessage = "";
  drawCareNoticeScreen();
  return true;
}

void acknowledgeCareNotice() {
  // Finish all elapsed time at the old rate, then revalidate the visible notice.
  drainNeedsOverTime();
  if (petMode != PET_CARE_NOTICE) {
    return;
  }
  PetCare::acknowledge(careState, currentCareNotice);
  rtcPetState.care = careState;
  savePetStateCheckpoint();
  tracePetEvent("care_notice_acknowledged");
  if (showNextCareNotice()) {
    return;
  }

  inputCaptureLocked = false;
  careNoticeAwaitRelease = false;
  clearAllPendingInput();
  discardWakeInput = true;
  forceFullRefreshNextFrame = true;
  enterTamagotchiInitialScreen();
  clearAllPendingInput();
  lastActivityMs = millis();
}

void loopCareNotice() {
  drainNeedsOverTime();
  if (petMode != PET_CARE_NOTICE) {
    return;
  }
  if (careNoticeAwaitRelease) {
    clearAllPendingInput();
    if (!allButtonsReleased()) {
      careNoticeReleasedSinceMs = 0;
    } else if (careNoticeReleasedSinceMs == 0) {
      careNoticeReleasedSinceMs = millis();
    } else if (millis() - careNoticeReleasedSinceMs >= DIRECTION_REARM_MS) {
      resetMainInputState();
      discardWakeInput = false;
      careNoticeAwaitRelease = false;
      inputCaptureLocked = false;
    }
  } else {
    updateMenuButtonInput();
    if (appMode != APP_TAMAGOTCHI) {
      return;
    }
    InputEvent event = popInputEvent();
    if (event == INPUT_MAIN) {
      lastActivityMs = millis();
      acknowledgeCareNotice();
      return;
    }
    // Back and directional buttons cannot dismiss or apply a notice.
  }
  if (millis() - lastActivityMs >= INACTIVITY_TIMEOUT_MS && allButtonsReleased()) {
    enterDeepSleep();
  }
}

void drawSelectedTileFrame(int x, int y, int w, int h, bool selected) {
  EPD_DrawRectangle(x, y, x + w, y + h, BLACK);

  if (!selected) {
    return;
  }

  EPD_DrawRectangle(x + 2, y + 2, x + w - 2, y + h - 2, BLACK);
  fillRect(x + 5, y + 5, 10, 3, BLACK);
  fillRect(x + 5, y + 5, 3, 10, BLACK);
  fillRect(x + w - 14, y + 5, 10, 3, BLACK);
  fillRect(x + w - 7, y + 5, 3, 10, BLACK);
  fillRect(x + 5, y + h - 7, 10, 3, BLACK);
  fillRect(x + 5, y + h - 14, 3, 10, BLACK);
  fillRect(x + w - 14, y + h - 7, 10, 3, BLACK);
  fillRect(x + w - 7, y + h - 14, 3, 10, BLACK);
}

void drawCatMenuIcon(int cx, int cy) {
  drawVerticalLsbBitmap(
    cx - (MENU_CAT_ICON_W / 2),
    cy - (MENU_CAT_ICON_H / 2),
    menuCatIcon,
    MENU_CAT_ICON_W,
    MENU_CAT_ICON_H
  );
}

void drawBookMenuIcon(int cx, int cy) {
  drawVerticalLsbBitmap(
    cx - (MENU_READER_ICON_W / 2),
    cy - (MENU_READER_ICON_H / 2),
    menuReaderIcon,
    MENU_READER_ICON_W,
    MENU_READER_ICON_H
  );
}

void drawHomeTile(int x, const char* label, bool selected, bool catIcon) {
  drawSelectedTileFrame(x, HOME_TILE_Y, HOME_TILE_W, HOME_TILE_H, selected);

  int centerX = x + (HOME_TILE_W / 2);
  int iconY = HOME_TILE_Y + 24;
  if (catIcon) {
    drawCatMenuIcon(centerX, iconY);
  } else {
    drawBookMenuIcon(centerX, iconY);
  }

  int charW = HOME_OPTION_TEXT_SIZE / 2;
  int textW = strlen(label) * charW;
  int textX = centerX - (textW / 2);
  EPD_ShowString(textX, HOME_TILE_Y + 43, label, BLACK, HOME_OPTION_TEXT_SIZE);
}

void drawHomeContent() {
  EPD_ShowString(HOME_TITLE_X, HOME_TITLE_Y, "WELCOME TUBELIGHT <3<3", BLACK, HOME_TITLE_TEXT_SIZE);

  drawHomeTile(
    HOME_TAMAGOTCHI_X,
    "CATTO",
    homeSelection == HOME_OPTION_TAMAGOTCHI,
    true
  );

  drawHomeTile(
    HOME_EREADER_X,
    "READER",
    homeSelection == HOME_OPTION_EREADER,
    false
  );
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
  EPD_ShowString(70, 48, "EREADER", BLACK, HOME_OPTION_TEXT_SIZE);
  EPD_ShowString(52, 72, "Coming soon", BLACK, ACTIVITY_TEXT_SIZE);
  fullRefresh();
}

// ---------------- SPRITE HELPERS ----------------

const uint8_t* getAnimationFrame(const Animation* anim, uint8_t index) {
  return (const uint8_t*)pgm_read_ptr(&anim->frames[index]);
}

void getSpriteDrawBox(const Animation* anim, int& drawX, int& drawY, int& drawW, int& drawH, int& minEraseY) {
  if (anim == &deliveryIntroAnimation) {
    drawW = SCREEN_W;
    drawH = SCREEN_H;
    drawX = (SCREEN_W - drawW) / 2;
    drawY = (SCREEN_H - drawH) / 2;
    minEraseY = 0;
    return;
  }

  drawX = ANIMATION_BOX_X;
  drawY = ANIMATION_BOX_Y;
  drawW = ANIMATION_BOX_W;
  drawH = ANIMATION_BOX_H;
  minEraseY = 24;
}

void eraseSpriteArea(const Animation* anim) {
  int drawX = 0;
  int drawY = 0;
  int drawW = 0;
  int drawH = 0;
  int minEraseY = 0;

  if (eraseFullScreenNextSpriteFrame) {
    drawX = 0;
    drawY = 0;
    drawW = SCREEN_W;
    drawH = SCREEN_H;
    minEraseY = 0;
    eraseFullScreenNextSpriteFrame = false;
  } else {
    getSpriteDrawBox(anim, drawX, drawY, drawW, drawH, minEraseY);
  }

  for (int y = drawY - 2; y < drawY + drawH + 2; y++) {
    for (int x = drawX - 2; x < drawX + drawW + 2; x++) {
      if (x >= 0 && x < SCREEN_W && y >= minEraseY && y < SCREEN_H) {
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
  int drawX = 0;
  int drawY = 0;
  int drawW = 0;
  int drawH = 0;
  int minEraseY = 0;
  getSpriteDrawBox(anim, drawX, drawY, drawW, drawH, minEraseY);

  for (int y = 0; y < drawH; y++) {
    int sampleY = (y * anim->height) / drawH;

    for (int x = 0; x < drawW; x++) {
      int sampleX = (x * anim->width) / drawW;
      bool pixelOn = getSpritePixel(sprite, anim, sampleX, sampleY);
      EPD_DrawPoint(drawX + x, drawY + y, pixelOn ? BLACK : WHITE);
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

void drawSerialPortalConnectedScreen() {
  EPD_Init();
  clearImageBuffer();
  drawVerticalLsbBitmap(
    0,
    0,
    img_51f2d38d_9eaf_4d7d_b192_3abc2d570052__1___1_,
    img_51f2d38d_9eaf_4d7d_b192_3abc2d570052__1___1__width,
    img_51f2d38d_9eaf_4d7d_b192_3abc2d570052__1___1__height
  );
  fullRefresh();
}

void redrawTamagotchiAfterSerialPortal() {
  forceFullRefreshNextFrame = true;

  if (shouldShowRunaway() && petMode != PET_RAN_AWAY) {
    showRunawayScreen();
    return;
  }

  if (petMode == PET_SLEEP_SELECT) {
    drawSleepDialog();
    return;
  }

  if (petMode == PET_COOLDOWN_MSG) {
    drawCooldownDialog();
    return;
  }

  if (petMode == PET_RAN_AWAY) {
    showRunawayScreen();
    return;
  }

  if (petMode == PET_INTRO) {
    startDeliveryIntro();
    return;
  }

  if (petMode == PET_CARE_NOTICE) {
    if (!showNextCareNotice()) {
      inputCaptureLocked = false;
      discardWakeInput = true;
      enterTamagotchiInitialScreen();
    }
    return;
  }

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );
}

void enterSerialPortalDisplay() {
  if (serialPortalDisplayActive) {
    return;
  }

  serialPortalDisplayActive = true;
  tracePetEvent("usb_connected");
  serialPortalPreviousInputCaptureLocked = inputCaptureLocked;
  inputCaptureLocked = true;
  clearAllPendingInput();

  if (appMode == APP_EREADER) {
    ereaderLeave();
  }

  drawSerialPortalConnectedScreen();
  lastActivityMs = millis();
}

void exitSerialPortalDisplay() {
  if (!serialPortalDisplayActive) {
    return;
  }

  serialPortalDisplayActive = false;
  tracePetEvent("usb_disconnected");
  inputCaptureLocked = serialPortalPreviousInputCaptureLocked;
  clearAllPendingInput();

  if (appMode == APP_HOME) {
    attachMainInputInterrupts();
    resetMainInputState();
    showHomeScreenFull();
  } else if (appMode == APP_EREADER) {
    detachMainInputInterrupts();
    resetMainInputState();
    ereaderEnter();
  } else {
    attachMainInputInterrupts();
    resetMainInputState();
    redrawTamagotchiAfterSerialPortal();
  }

  clearAllPendingInput();
  lastActivityMs = millis();
}

bool handleSerialPortalDisplayState() {
  bool transitioned = false;

  if (serialLibraryPortalConsumeConnectedEvent() ||
      (serialLibraryPortalIsConnected() && !serialPortalDisplayActive)) {
    enterSerialPortalDisplay();
    transitioned = true;
  }

  if (serialLibraryPortalConsumeDisconnectedEvent() ||
      (!serialLibraryPortalIsConnected() && serialPortalDisplayActive)) {
    exitSerialPortalDisplay();
    transitioned = true;
  }

  return transitioned;
}

// ---------------- NEED / METER LOGIC ----------------

uint8_t valueForSadNeed(SadNeed need) {
  if (need == SAD_NEED_PEE) {
    return peeValue;
  }

  if (need == SAD_NEED_FOOD) {
    return foodValue;
  }

  if (need == SAD_NEED_PLAY) {
    return playValue;
  }

  if (need == SAD_NEED_PETS) {
    return loveValue;
  }

  return 100;
}

bool hasSadAnimationForNeed(SadNeed need) {
  return need == SAD_NEED_PEE || need == SAD_NEED_FOOD || need == SAD_NEED_PLAY || need == SAD_NEED_PETS;
}

bool isSadNeedLow(SadNeed need) {
  return hasSadAnimationForNeed(need) && valueForSadNeed(need) < PetNeeds::SadThreshold;
}

SadNeed firstExistingLowNeed() {
  if (isSadNeedLow(SAD_NEED_PEE)) {
    return SAD_NEED_PEE;
  }

  if (isSadNeedLow(SAD_NEED_FOOD)) {
    return SAD_NEED_FOOD;
  }

  if (isSadNeedLow(SAD_NEED_PLAY)) {
    return SAD_NEED_PLAY;
  }

  if (isSadNeedLow(SAD_NEED_PETS)) {
    return SAD_NEED_PETS;
  }

  return SAD_NEED_NONE;
}

void refreshSadNeedAfterNeedChange(SadNeed newlyLowNeed) {
  if (activeSadNeed != SAD_NEED_NONE && isSadNeedLow(activeSadNeed)) {
    return;
  }

  activeSadNeed = SAD_NEED_NONE;

  if (newlyLowNeed != SAD_NEED_NONE && isSadNeedLow(newlyLowNeed)) {
    activeSadNeed = newlyLowNeed;
    return;
  }

  activeSadNeed = firstExistingLowNeed();
}

const Animation* getIdleAnimationForNeeds() {
  if (activeSadNeed == SAD_NEED_PEE) {
    return &sadPeeAnimation;
  }

  if (activeSadNeed == SAD_NEED_FOOD) {
    return &sadHungryAnimation;
  }

  if (activeSadNeed == SAD_NEED_PLAY) {
    return &sadPlayAnimation;
  }

  if (activeSadNeed == SAD_NEED_PETS) {
    return &sadPetAnimation;
  }

  return &idleAnimation;
}

bool syncIdleAnimationToNeeds() {
  if (appMode != APP_TAMAGOTCHI || petMode != PET_IDLE) {
    return false;
  }

  if (activeSadNeed == SAD_NEED_NONE && currentAnimation == &sleepingAnimation) {
    return false;
  }

  const Animation* targetAnimation = getIdleAnimationForNeeds();
  if (currentAnimation == targetAnimation) {
    return false;
  }

  currentAnimation = targetAnimation;
  currentFrame = 0;
  lastFrameMs = millis();

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );

  return true;
}

PetNeeds::State captureNeedState() {
  return {
    {
      {peeValue, peeDrainCarry, peeCooldownUntilEpoch},
      {foodValue, foodDrainCarry, foodCooldownUntilEpoch},
      {playValue, playDrainCarry, playCooldownUntilEpoch},
      {loveValue, loveDrainCarry, 0}
    },
    happinessValue, happinessCrisisPenalty, happinessCrisisCarry, hasSeenDeliveryIntro,
    careState
  };
}

void applyNeedState(const PetNeeds::State& state) {
  peeValue = state.needs[PetNeeds::Pee].value;
  foodValue = state.needs[PetNeeds::Food].value;
  playValue = state.needs[PetNeeds::Play].value;
  loveValue = state.needs[PetNeeds::Pets].value;
  peeDrainCarry = state.needs[PetNeeds::Pee].drainCarry;
  foodDrainCarry = state.needs[PetNeeds::Food].drainCarry;
  playDrainCarry = state.needs[PetNeeds::Play].drainCarry;
  loveDrainCarry = state.needs[PetNeeds::Pets].drainCarry;
  peeCooldownUntilEpoch = state.needs[PetNeeds::Pee].cooldownUntilEpoch;
  foodCooldownUntilEpoch = state.needs[PetNeeds::Food].cooldownUntilEpoch;
  playCooldownUntilEpoch = state.needs[PetNeeds::Play].cooldownUntilEpoch;
  happinessValue = state.happiness;
  happinessCrisisPenalty = state.crisisPenalty;
  happinessCrisisCarry = state.crisisCarry;
  careState = state.care;
}

bool hasRunawayCriticalNeed() {
  return foodValue == 0 || peeValue == 0;
}

PetTrace::Frame capturePetTraceFrame() {
  PetTrace::Frame frame = {};
  frame.epoch = (uint32_t)time(NULL);
  frame.millis = (uint32_t)millis();
  frame.uptimeUs = (uint64_t)esp_timer_get_time();
  frame.lastDrainEpoch = lastNeedDrainEpoch;
  frame.rtcEpoch = rtcPetState.epochSeconds;
  frame.rtcDrainEpoch = rtcPetState.lastNeedDrainEpoch;
  frame.birthEpoch = petBirthEpoch;
  frame.sleepStartEpoch = sleepStartEpoch;
  frame.sleepDurationSec = sleepDurationSec;
  frame.wakeCause = (int32_t)lastWakeCause;
  frame.resetReason = (int32_t)esp_reset_reason();
  frame.appMode = (int32_t)appMode;
  frame.petMode = (int32_t)petMode;
  frame.activeAction = (int32_t)activeAction;
  frame.introSeen = hasSeenDeliveryIntro;
  frame.pendingRunaway = pendingRunaway;
  frame.usbActive = serialPortalDisplayActive;
  frame.needs = captureNeedState();
  return frame;
}

void tracePetEvent(const char* event, const char* detail) {
  PetTrace::record(event, capturePetTraceFrame(), nullptr, detail);
}

bool shouldShowRunaway() {
  if (!hasSeenDeliveryIntro || !hasRunawayCriticalNeed() || happinessValue > 0) {
    pendingRunaway = false;
    runawayDiagnosticReason = "critical_need_recovered";
    return false;
  }

  return true;
}

void updateHappiness() {
  PetNeeds::State state = captureNeedState();
  PetNeeds::refreshHappiness(state);
  applyNeedState(state);
  rtcPetState.happinessCrisisCarry = happinessCrisisCarry;
  rtcPetState.happinessCrisisPenalty = happinessCrisisPenalty;
  maybeTriggerRunaway();
}

void maybeTriggerRunaway() {
  if (!hasSeenDeliveryIntro || !hasRunawayCriticalNeed()) {
    pendingRunaway = false;
    runawayDiagnosticReason = "critical_need_recovered";
    return;
  }

  if (happinessValue > 0) {
    pendingRunaway = false;
    return;
  }

  runawayDiagnosticReason = "happiness_zero_food_or_pee";
  if (!pendingRunaway && petMode != PET_RAN_AWAY) tracePetEvent("runaway_decision", runawayDiagnosticReason);

  if (serialPortalDisplayActive) {
    pendingRunaway = true;
    runawayDiagnosticReason = "happiness_zero_food_or_pee_deferred_serial_portal";
    return;
  }

  if (appMode != APP_TAMAGOTCHI) {
    pendingRunaway = true;
    runawayDiagnosticReason = "happiness_zero_food_or_pee_deferred_other_mode";
    return;
  }

  if (petMode == PET_ACTION) {
    pendingRunaway = true;
    runawayDiagnosticReason = "happiness_zero_food_or_pee_deferred_activity";
    return;
  }

  if (petMode == PET_RAN_AWAY) {
    return;
  }

  showRunawayScreen();
}

void initNeedDrainClockIfNeeded() {
  if (lastNeedDrainEpoch >= 100000) {
    return;
  }

  lastNeedDrainEpoch = (uint32_t)time(NULL);
  peeDrainCarry = 0;
  foodDrainCarry = 0;
  playDrainCarry = 0;
  loveDrainCarry = 0;
  rtcPetState.lastNeedDrainEpoch = lastNeedDrainEpoch;
  rtcPetState.peeDrainCarry = peeDrainCarry;
  rtcPetState.foodDrainCarry = foodDrainCarry;
  rtcPetState.playDrainCarry = playDrainCarry;
  rtcPetState.loveDrainCarry = loveDrainCarry;
}

void resetNeedDrainClock() {
  lastNeedDrainEpoch = (uint32_t)time(NULL);
  peeDrainCarry = 0;
  foodDrainCarry = 0;
  playDrainCarry = 0;
  loveDrainCarry = 0;
  rtcPetState.lastNeedDrainEpoch = lastNeedDrainEpoch;
  rtcPetState.peeDrainCarry = peeDrainCarry;
  rtcPetState.foodDrainCarry = foodDrainCarry;
  rtcPetState.playDrainCarry = playDrainCarry;
  rtcPetState.loveDrainCarry = loveDrainCarry;
}

void drainNeedsOverTime() {
  initNeedDrainClockIfNeeded();
  uint32_t nowEpoch = (uint32_t)time(NULL);
  PetTrace::Frame traceBefore = capturePetTraceFrame();
  traceBefore.epoch = nowEpoch;
  if (nowEpoch <= lastNeedDrainEpoch) {
    PetTrace::drain(traceBefore, traceBefore);
    return;
  }

  PetNeeds::State state = captureNeedState();
  uint32_t pausedUntil = sleepDurationSec > 0 ? sleepStartEpoch + sleepDurationSec : 0;
  PetNeeds::Need firstLow = PetNeeds::advance(state, lastNeedDrainEpoch, nowEpoch, pausedUntil);
  applyNeedState(state);
  PetTrace::Frame traceAfter = capturePetTraceFrame();
  PetTrace::drain(traceBefore, traceAfter);
  const SadNeed sadNeeds[PetNeeds::Count] = {SAD_NEED_PEE, SAD_NEED_FOOD, SAD_NEED_PLAY, SAD_NEED_PETS};
  refreshSadNeedAfterNeedChange(firstLow == PetNeeds::None ? SAD_NEED_NONE : sadNeeds[firstLow]);
  maybeTriggerRunaway();

  // Carries have already absorbed elapsedSeconds, so always advance the clock
  // (otherwise the next call would double-count this interval).
  lastNeedDrainEpoch = nowEpoch;
  rtcPetState.lastNeedDrainEpoch = lastNeedDrainEpoch;
  rtcPetState.peeDrainCarry = peeDrainCarry;
  rtcPetState.foodDrainCarry = foodDrainCarry;
  rtcPetState.playDrainCarry = playDrainCarry;
  rtcPetState.loveDrainCarry = loveDrainCarry;
  rtcPetState.happinessCrisisCarry = happinessCrisisCarry;
  rtcPetState.happinessCrisisPenalty = happinessCrisisPenalty;
  rtcPetState.care = careState;
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
    time_t retainedTime = time(NULL);
    if (retainedTime >= (time_t)rtcPetState.epochSeconds) {
      refreshRtcEpochFromClock();
      return;
    }

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

bool isSplashRotationStateValid() {
  if (rtcSplashRotationState.magic != RTC_SPLASH_ROTATION_MAGIC ||
      rtcSplashRotationState.position > DEEP_SLEEP_SPLASH_COUNT) {
    return false;
  }

  bool seen[DEEP_SLEEP_SPLASH_COUNT] = {};
  for (uint8_t i = 0; i < DEEP_SLEEP_SPLASH_COUNT; i++) {
    uint8_t idx = rtcSplashRotationState.order[i];
    if (idx >= DEEP_SLEEP_SPLASH_COUNT || seen[idx]) {
      return false;
    }
    seen[idx] = true;
  }

  return true;
}

void shuffleSplashRotationState() {
  rtcSplashRotationState.magic = RTC_SPLASH_ROTATION_MAGIC;
  rtcSplashRotationState.position = 0;

  for (uint8_t i = 0; i < DEEP_SLEEP_SPLASH_COUNT; i++) {
    rtcSplashRotationState.order[i] = i;
  }

  for (int i = DEEP_SLEEP_SPLASH_COUNT - 1; i > 0; i--) {
    uint8_t j = (uint8_t)(esp_random() % (uint32_t)(i + 1));
    uint8_t tmp = rtcSplashRotationState.order[i];
    rtcSplashRotationState.order[i] = rtcSplashRotationState.order[j];
    rtcSplashRotationState.order[j] = tmp;
  }
}

uint8_t nextDeepSleepSplashIndex(uint8_t& cyclePosition) {
  if (!isSplashRotationStateValid() ||
      rtcSplashRotationState.position >= DEEP_SLEEP_SPLASH_COUNT) {
    shuffleSplashRotationState();
  }

  uint8_t idx = rtcSplashRotationState.order[rtcSplashRotationState.position];
  cyclePosition = rtcSplashRotationState.position + 1;
  rtcSplashRotationState.position++;
  return idx;
}

void showDeepSleepSplashScreen() {
  uint8_t cyclePosition = 0;
  uint8_t idx = nextDeepSleepSplashIndex(cyclePosition);
  const uint8_t* image = (const uint8_t*)pgm_read_ptr(&deepSleepSplashes[idx]);

  Serial.printf(
    "Deep sleep splash: %u (%u/%u)\n",
    idx,
    cyclePosition,
    DEEP_SLEEP_SPLASH_COUNT
  );

  EPD_Init();
  EPD_ALL_Fill(WHITE);
  drawVerticalLsbImageCrop(
    image,
    DEEP_SLEEP_SPLASH_W,
    DEEP_SLEEP_SPLASH_H,
    0,
    0,
    SCREEN_W,
    SCREEN_H
  );
  fullRefresh();
}

void initDefaultPetState() {
  careState = {};
  careNoticeAwaitRelease = false;
  peeValue = 80;
  foodValue = 80;
  playValue = 80;
  loveValue = 80;
  happinessValue = 80;
  activeSadNeed = SAD_NEED_NONE;
  selectedAction = ACTION_PEE;
  appMode = APP_HOME;
  homeSelection = HOME_OPTION_TAMAGOTCHI;
  lastNeedDrainEpoch = 0;
  peeDrainCarry = 0;
  foodDrainCarry = 0;
  playDrainCarry = 0;
  loveDrainCarry = 0;
  happinessCrisisCarry = 0;
  happinessCrisisPenalty = 0;
  hasSeenDeliveryIntro = false;
  pendingRunaway = false;
  runawayDiagnosticReason = "init_default_pet_state";
  rtcRunawayDeathLogged = 0;
  sleepStartEpoch = 0;
  sleepDurationSec = 0;
  sleepCooldownUntilEpoch = 0;
  sleepWakeButtonShown = false;
  peeCooldownUntilEpoch = 0;
  foodCooldownUntilEpoch = 0;
  playCooldownUntilEpoch = 0;
  resetPetAgeTimer();
}

void savePetStateToRtc() {
  refreshRtcEpochFromClock();

  rtcPetState.magic = RTC_PET_STATE_MAGIC;
  rtcPetState.peeValue = peeValue;
  rtcPetState.foodValue = foodValue;
  rtcPetState.playValue = playValue;
  rtcPetState.loveValue = loveValue;
  rtcPetState.activeSadNeed = (uint8_t)activeSadNeed;
  rtcPetState.selectedAction = (uint8_t)selectedAction;
  rtcPetState.appMode = (uint8_t)appMode;
  rtcPetState.homeSelection = (uint8_t)homeSelection;
  rtcPetState.petAgeStarted = petAgeStarted ? 1 : 0;
  rtcPetState.petBirthEpoch = petBirthEpoch;
  rtcPetState.sleepEntryEpoch = rtcPetState.epochSeconds;
  if (lastNeedDrainEpoch < 100000) {
    lastNeedDrainEpoch = rtcPetState.epochSeconds;
  }
  rtcPetState.lastNeedDrainEpoch = lastNeedDrainEpoch;
  rtcPetState.peeDrainCarry = peeDrainCarry;
  rtcPetState.foodDrainCarry = foodDrainCarry;
  rtcPetState.playDrainCarry = playDrainCarry;
  rtcPetState.loveDrainCarry = loveDrainCarry;
  rtcPetState.happinessValue = happinessValue;
  rtcPetState.happinessCrisisCarry = happinessCrisisCarry;
  rtcPetState.happinessCrisisPenalty = happinessCrisisPenalty;
  rtcPetState.hasSeenDeliveryIntro = hasSeenDeliveryIntro ? 1 : 0;
  rtcPetState.sleeping = (sleepDurationSec > 0) ? 1 : 0;
  rtcPetState.sleepStartEpoch = sleepStartEpoch;
  rtcPetState.sleepDurationSec = sleepDurationSec;
  rtcPetState.sleepCooldownUntilEpoch = sleepCooldownUntilEpoch;
  rtcPetState.peeCooldownUntilEpoch = peeCooldownUntilEpoch;
  rtcPetState.foodCooldownUntilEpoch = foodCooldownUntilEpoch;
  rtcPetState.playCooldownUntilEpoch = playCooldownUntilEpoch;
  rtcPetState.care = careState;
}

bool restorePetStateFromRtc() {
  if (rtcPetState.magic != RTC_PET_STATE_MAGIC) {
    return false;
  }

  peeValue = rtcPetState.peeValue;
  foodValue = rtcPetState.foodValue;
  playValue = rtcPetState.playValue;
  loveValue = rtcPetState.loveValue;
  activeSadNeed = (SadNeed)rtcPetState.activeSadNeed;
  selectedAction = (SelectedAction)rtcPetState.selectedAction;
  appMode = (AppMode)rtcPetState.appMode;
  homeSelection = (HomeOption)rtcPetState.homeSelection;
  petAgeStarted = rtcPetState.petAgeStarted != 0;
  petBirthEpoch = rtcPetState.petBirthEpoch;
  lastNeedDrainEpoch = rtcPetState.lastNeedDrainEpoch;
  peeDrainCarry = rtcPetState.peeDrainCarry;
  foodDrainCarry = rtcPetState.foodDrainCarry;
  playDrainCarry = rtcPetState.playDrainCarry;
  loveDrainCarry = rtcPetState.loveDrainCarry;
  happinessValue = rtcPetState.happinessValue;
  happinessCrisisCarry = rtcPetState.happinessCrisisCarry;
  happinessCrisisPenalty = rtcPetState.happinessCrisisPenalty;
  hasSeenDeliveryIntro = rtcPetState.hasSeenDeliveryIntro != 0;
  sleepStartEpoch = rtcPetState.sleepStartEpoch;
  sleepDurationSec = rtcPetState.sleepDurationSec;
  sleepCooldownUntilEpoch = rtcPetState.sleepCooldownUntilEpoch;
  peeCooldownUntilEpoch = rtcPetState.peeCooldownUntilEpoch;
  foodCooldownUntilEpoch = rtcPetState.foodCooldownUntilEpoch;
  playCooldownUntilEpoch = rtcPetState.playCooldownUntilEpoch;
  careState = rtcPetState.care;

  if (appMode > APP_EREADER) {
    appMode = APP_TAMAGOTCHI;
  }

  if (homeSelection > HOME_OPTION_EREADER) {
    homeSelection = HOME_OPTION_TAMAGOTCHI;
  }

  if (selectedAction > ACTION_SLEEP) {
    selectedAction = ACTION_PEE;
  }

  if (activeSadNeed > SAD_NEED_PETS) {
    activeSadNeed = SAD_NEED_NONE;
  }

  refreshSadNeedAfterNeedChange(SAD_NEED_NONE);

  return true;
}

void savePetStateCheckpoint() {
  savePetStateToRtc();
  petCheckpointDirty = !PetStateStore::save(rtcPetState);
  lastPetCheckpointMs = millis();
  if (petCheckpointDirty) {
    Serial.println("Pet checkpoint could not be saved");
  }
}

void servicePetStateCheckpoint() {
  unsigned long elapsed = millis() - lastPetCheckpointMs;
  if ((petCheckpointDirty && elapsed >= PET_CHECKPOINT_RETRY_MS) ||
      elapsed >= PET_CHECKPOINT_INTERVAL_MS) {
    savePetStateCheckpoint();
  }
}

PetStateRestoreSource restorePetStateForBoot(esp_sleep_wakeup_cause_t wakeCause) {
  if (wakeCause == ESP_SLEEP_WAKEUP_EXT1 && restorePetStateFromRtc()) {
    return PET_STATE_RTC;
  }

  PetStateStore::Snapshot checkpoint = {};
  if (PetStateStore::load(checkpoint)) {
    rtcPetState = checkpoint;
    if (restorePetStateFromRtc()) {
      Serial.println("Restored pet after restart");
      return PET_STATE_CHECKPOINT;
    }
  }

  initDefaultPetState();
  return PET_STATE_FRESH;
}

void applyNeedsSinceSleep() {
  uint32_t previousDrainEpoch = lastNeedDrainEpoch;
  uint32_t nowEpoch = (uint32_t)time(NULL);

  if (previousDrainEpoch < 100000 || nowEpoch <= previousDrainEpoch) {
    Serial.println("Skipping sleep need drain: no elapsed clock time");
    return;
  }

  drainNeedsOverTime();

  Serial.printf(
    "Applied sleep need drain: elapsed=%lu seconds\n",
    (unsigned long)(nowEpoch - previousDrainEpoch)
  );
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
  tracePetEvent("deep_sleep_enter");

  if (appMode == APP_EREADER) {
    ereaderLeave();
  }

  showDeepSleepSplashScreen();

  digitalWrite(EPD_POWER, LOW);
  prepareWirelessForSleep();

  esp_sleep_enable_ext1_wakeup(WAKE_BUTTON_PIN_MASK, ESP_EXT1_WAKEUP_ANY_LOW);

  Serial.flush();
  esp_deep_sleep_start();
}

// ---------------- ANIMATION CONTROL ----------------

void enterHomeScreen() {
  Serial.println("Entering home screen");
  tracePetEvent("home_enter_before");

  if (appMode == APP_EREADER) {
    ereaderLeave();
  }

  attachMainInputInterrupts();
  resetMainInputState();
  appMode = APP_HOME;
  petMode = PET_IDLE;
  activeAction = ACTIVE_NONE;
  currentActivityMessage = "";
  inputCaptureLocked = false;
  inputLockoutUntil = 0;
  discardWakeInput = false;
  lastActivityMs = millis();
  savePetStateCheckpoint();
  showHomeScreenFull();
  tracePetEvent("home_enter_after");
}

void enterTamagotchi() {
  Serial.println("Entering tamagotchi");
  tracePetEvent("tamagotchi_enter_before");

  drainNeedsOverTime();
  attachMainInputInterrupts();
  resetMainInputState();
  appMode = APP_TAMAGOTCHI;
  savePetStateCheckpoint();
  forceFullRefreshNextFrame = true;
  discardWakeInput = false;
  enterTamagotchiInitialScreen();
  lastActivityMs = millis();
  tracePetEvent("tamagotchi_enter_after");
}

void enterEreaderMode() {
  tracePetEvent("reader_enter_before");
  Serial.println("Entering ereader");

  detachMainInputInterrupts();
  resetMainInputState();
  appMode = APP_EREADER;
  savePetStateCheckpoint();
  discardWakeInput = false;
  lastActivityMs = millis();
  ereaderRequestEnter();
  tracePetEvent("reader_enter_after");
}

void startIdle() {
  refreshSadNeedAfterNeedChange(SAD_NEED_NONE);
  currentAnimation = getIdleAnimationForNeeds();
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

void startSleep(uint32_t durationSec) {
  tracePetEvent("scheduled_sleep_before");
  // Count need drain up to this moment, then pause for the sleep period.
  drainNeedsOverTime();
  if (petMode == PET_RAN_AWAY) {
    return;
  }

  sleepStartEpoch = (uint32_t)time(NULL);
  sleepDurationSec = durationSec;
  sleepCooldownUntilEpoch = 0;
  sleepWakeButtonShown = true;
  sleepOkIgnoredUntilMs = millis() + SLEEP_START_OK_LOCKOUT_MS;

  petMode = PET_SLEEPING;
  activeAction = ACTIVE_NONE;
  currentActivityMessage = "";
  currentAnimation = &sleepingAnimation;
  currentFrame = 0;

  lastNeedDrainEpoch = (uint32_t)time(NULL);
  rtcPetState.lastNeedDrainEpoch = lastNeedDrainEpoch;

  clearInputQueue();
  inputLockoutUntil = millis() + 150;
  forceFullRefreshNextFrame = true;
  lastFrameMs = millis();
  lastActivityMs = millis();

  savePetStateCheckpoint();

  Serial.printf("Sleep started: %lu seconds\n", (unsigned long)durationSec);

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );
}

void wakeCatFromSleep(bool early) {
  tracePetEvent("scheduled_wake_before", early ? "early" : "completed");
  // Catch up through the scheduled end before clearing the sleep interval.
  drainNeedsOverTime();
  if (early) {
    sleepCooldownUntilEpoch = 0;
  } else {
    // Cooldown counts from the natural wake time (sleep start + duration).
    uint32_t cd = (sleepDurationSec >= SLEEP_LONG_SEC) ? SLEEP_CD_LONG_SEC : SLEEP_CD_SHORT_SEC;
    sleepCooldownUntilEpoch = sleepStartEpoch + sleepDurationSec + cd;
  }

  sleepStartEpoch = 0;
  sleepDurationSec = 0;
  sleepWakeButtonShown = false;
  sleepOkIgnoredUntilMs = 0;

  if (selectedAction == ACTION_SLEEP && isSleepOnCooldown()) {
    selectedAction = ACTION_PET;
  }

  clearInputQueue();
  inputLockoutUntil = millis() + 150;
  forceFullRefreshNextFrame = true;

  savePetStateCheckpoint();

  Serial.println(early ? "Woke early (no cooldown)" : "Woke naturally (cooldown set)");

  if (petMode == PET_RAN_AWAY) {
    return;
  }
  if (shouldShowRunaway()) {
    showRunawayScreen();
    return;
  }
  startIdle();
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

  refreshSadNeedAfterNeedChange(SAD_NEED_NONE);
  currentAnimation = getIdleAnimationForNeeds();
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
  selectActivityMessage(eatActivityMessages, EAT_ACTIVITY_MESSAGE_COUNT);
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
  selectActivityMessage(playActivityMessages, PLAY_ACTIVITY_MESSAGE_COUNT);
  lastFrameMs = millis();

  Serial.println("Play animation started");

  drawAnimationFrame(
    currentAnimation,
    getAnimationFrame(currentAnimation, currentFrame)
  );
}

void startPetting() {
  currentAnimation = &pettingAnimation;
  currentFrame = 0;
  petMode = PET_ACTION;
  activeAction = ACTIVE_PET;
  selectActivityMessage(petActivityMessages, PET_ACTIVITY_MESSAGE_COUNT);
  lastFrameMs = millis();

  Serial.println("Petting animation started");

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
  selectActivityMessage(peeActivityMessages, PEE_ACTIVITY_MESSAGE_COUNT);
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
  } else if (selectedAction == ACTION_PLAY) {
    startPlaying();
  } else {
    startPetting();
  }
}

void finishActionAndReturnToIdle() {
  Serial.println("Action finished, returning to idle");

  drainNeedsOverTime();

  // Use the same sampled epoch as the drain, so refills never precede their clock.
  uint32_t nowEpoch = lastNeedDrainEpoch;
  tracePetEvent("activity_refill_before");
  PetNeeds::State state = captureNeedState();

  if (activeAction == ACTIVE_PEE) {
    PetNeeds::refill(state, PetNeeds::Pee, nowEpoch + PEE_CD_SEC, nowEpoch);
  }

  if (activeAction == ACTIVE_FOOD) {
    PetNeeds::refill(state, PetNeeds::Food, nowEpoch + FOOD_CD_SEC, nowEpoch);
  }

  if (activeAction == ACTIVE_PLAY) {
    PetNeeds::refill(state, PetNeeds::Play, nowEpoch + PLAY_CD_SEC, nowEpoch);
  }

  if (activeAction == ACTIVE_PET) {
    PetNeeds::refill(state, PetNeeds::Pets, 0, nowEpoch);
  }

  applyNeedState(state);
  updateHappiness();
  savePetStateCheckpoint();
  tracePetEvent("activity_refill_after");

  if (shouldShowRunaway()) {
    showRunawayScreen();
    clearInputQueue();
    inputLockoutUntil = millis() + 100;
    inputCaptureLocked = false;
    return;
  }

  refreshSadNeedAfterNeedChange(SAD_NEED_NONE);
  currentAnimation = getIdleAnimationForNeeds();
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
  if (selectedAction == ACTION_SLEEP) {
    selectedAction = ACTION_PET;
  } else if (selectedAction == ACTION_PET) {
    selectedAction = ACTION_PLAY;
  } else if (selectedAction == ACTION_PLAY) {
    selectedAction = ACTION_FOOD;
  } else if (selectedAction == ACTION_FOOD) {
    selectedAction = ACTION_PEE;
  }

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
  } else if (selectedAction == ACTION_PLAY) {
    selectedAction = ACTION_PET;
  } else if (selectedAction == ACTION_PET && !isSleepOnCooldown()) {
    selectedAction = ACTION_SLEEP;
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

  if (appMode == APP_TAMAGOTCHI && petMode == PET_RAN_AWAY) {
    onRunawayAcknowledged();
    return;
  }

  if (appMode == APP_TAMAGOTCHI && petMode == PET_INTRO) {
    return;
  }

  if (appMode == APP_TAMAGOTCHI && petMode == PET_CARE_NOTICE) {
    acknowledgeCareNotice();
    return;
  }

  if (appMode == APP_TAMAGOTCHI && petMode == PET_SLEEP_SELECT) {
    startSleep(sleepDialogChoice == 0 ? SLEEP_SHORT_SEC : SLEEP_LONG_SEC);
    return;
  }

  if (appMode == APP_TAMAGOTCHI && petMode == PET_COOLDOWN_MSG) {
    closeCooldownDialog();
    return;
  }

  if (appMode == APP_TAMAGOTCHI && petMode == PET_SLEEPING) {
    uint32_t nowEpoch = (uint32_t)time(NULL);
    uint32_t elapsed = (nowEpoch > sleepStartEpoch) ? (nowEpoch - sleepStartEpoch) : 0;
    if (elapsed < SLEEP_WAKE_WINDOW_SEC) {
      wakeCatFromSleep(true);
    }
    return;
  }

  if (appMode == APP_TAMAGOTCHI && petMode == PET_IDLE) {
    if (currentAnimation == &sleepingAnimation) {
      returnToIdleFromDrowsy();
    }

    if (selectedAction == ACTION_SLEEP) {
      if (!isSleepOnCooldown()) {
        openSleepDialog();
      }
      return;
    }

    if ((selectedAction == ACTION_PEE || selectedAction == ACTION_FOOD ||
         selectedAction == ACTION_PLAY) && isActionOnCooldown(selectedAction)) {
      openCooldownDialog(selectedAction);
      return;
    }

    startSelectedAction();
  }
}

bool isMenuButtonPressed() {
  return digitalRead(MENU_BUTTON_KEY) == LOW;
}

bool canMenuButtonEnterHome() {
  if (appMode == APP_HOME) {
    return false;
  }

  if (appMode == APP_TAMAGOTCHI &&
      (petMode == PET_ACTION || petMode == PET_RAN_AWAY || petMode == PET_INTRO)) {
    return false;
  }

  return true;
}

void updateMenuButtonInput() {
  bool pendingPress = false;
  noInterrupts();
  if (menuButtonPressPending) {
    menuButtonPressPending = false;
    pendingPress = true;
  }
  interrupts();

  if (pendingPress) {
    lastActivityMs = millis();
    menuButtonRawDown = isMenuButtonPressed();
    menuButtonDown = menuButtonRawDown;
    menuButtonLastChangeMs = millis();
    if (canMenuButtonEnterHome()) {
      enterHomeScreen();
      return;
    }
  }

  bool rawDown = isMenuButtonPressed();
  unsigned long now = millis();

  if (rawDown != menuButtonRawDown) {
    menuButtonRawDown = rawDown;
    menuButtonLastChangeMs = now;
  }

  if (now - menuButtonLastChangeMs < MENU_BUTTON_DEBOUNCE_MS) {
    return;
  }

  if (rawDown == menuButtonDown) {
    return;
  }

  menuButtonDown = rawDown;

  if (menuButtonDown) {
    lastActivityMs = millis();
    if (canMenuButtonEnterHome()) {
      enterHomeScreen();
    }
  }
}

// ---------------- INPUT HANDLING ----------------

void processInputEvent(InputEvent event) {
  if (event == INPUT_NONE) {
    return;
  }

  lastActivityMs = millis();

  if (appMode == APP_TAMAGOTCHI && petMode == PET_SLEEPING &&
      event == INPUT_MAIN && millis() < sleepOkIgnoredUntilMs) {
    return;
  }

  if (event == INPUT_MAIN) {
    onMainToggleTap();
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

  if (petMode == PET_RAN_AWAY || petMode == PET_INTRO || petMode == PET_CARE_NOTICE) {
    return;
  }

  // Sleep duration chooser: up/down toggle between 2h and 10h.
  if (petMode == PET_SLEEP_SELECT) {
    if (event == INPUT_UP || event == INPUT_DOWN) {
      sleepDialogChoice = sleepDialogChoice == 0 ? 1 : 0;
      drawSleepDialog();
    }
    return;
  }

  // While asleep, directional inputs do nothing (wake is via main tap).
  if (petMode == PET_SLEEPING) {
    return;
  }

  // Cooldown message dialog: directional inputs do nothing (close is OK/Exit).
  if (petMode == PET_COOLDOWN_MSG) {
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
  serialLibraryPortalBegin();
  delay(500);

  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  lastWakeCause = wakeCause;
  PetTrace::begin(esp_random());

  pinMode(EPD_POWER, OUTPUT);
  digitalWrite(EPD_POWER, HIGH);

  pinMode(HOME_KEY, INPUT);
  pinMode(EXIT_KEY, INPUT);
  pinMode(PRV_KEY, INPUT);
  pinMode(NEXT_KEY, INPUT);
  pinMode(OK_KEY, INPUT);

  clearInputQueue();

  attachInterrupt(digitalPinToInterrupt(MENU_BUTTON_KEY), handleMenuButtonInterrupt, FALLING);
  attachMainInputInterrupts();
  resetMainInputState();

  Serial.println("AdventureCattoEink");
  Serial.println("Interrupt input enabled");
  Serial.println("Top button tap: select action / enter mode");
  Serial.println("Toggle press: exit to home screen");
  Serial.println("Up/down: select pee, food, play, or home menu");
  Serial.println("Inputs are queued immediately on button press");
  Serial.println("Input capture locks during actions to prevent duplicate actions");

  lastActivityMs = millis();

  PetStateRestoreSource restoreSource = restorePetStateForBoot(wakeCause);
  bool restoredPet = restoreSource != PET_STATE_FRESH;
  tracePetEvent("boot_restored", restoreSource == PET_STATE_RTC ? "rtc" :
                restoreSource == PET_STATE_CHECKPOINT ? "checkpoint" : "fresh");
  if (wakeCause == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.println("Woke from deep sleep");
  }
  if (restoredPet) {
    discardWakeInput = true;
    forceFullRefreshNextFrame = true;
    clearInputQueue();
  }

  initDeviceTime(restoredPet);
  tracePetEvent("boot_clock_initialized", "expected_clock_rebase_if_not_retained");

  if (restoredPet) {
    applyNeedsSinceSleep();
  }
  savePetStateCheckpoint();

  if (appMode == APP_HOME) {
    showHomeScreenFull();
  } else if (appMode == APP_EREADER) {
    detachMainInputInterrupts();
    resetMainInputState();
    ereaderEnter();
  } else {
    forceFullRefreshNextFrame = true;
    enterTamagotchiInitialScreen();
  }

  lastFrameMs = millis();
  lastNeedDrainMs = millis();
  lastActivityMs = millis();
}

void loop() {
  unsigned long now = millis();
  // Save completed updates only; runaway can be drawn inside a drain update.
  servicePetStateCheckpoint();
  bool serialPortalSawInput = serialLibraryPortalLoop();
  bool serialPortalTransitioned = handleSerialPortalDisplayState();
  bool serialPortalKeepAwake = serialPortalSawInput || serialLibraryPortalIsBusy();
  if (serialPortalKeepAwake) {
    lastActivityMs = now;
  }

  if (serialPortalTransitioned) {
    delay(5);
    return;
  }

  if (serialPortalDisplayActive) {
    drainNeedsOverTime();
    clearAllPendingInput();
    lastActivityMs = now;
    delay(5);
    return;
  }

  updateDirectionalInputArming();

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

    updateMenuButtonInput();

    InputEvent event = popInputEvent();
    if (event != INPUT_NONE) {
      processInputEvent(event);
      if (appMode != APP_HOME) {
        delay(5);
        return;
      }
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

    ereaderActivateIfPending();

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

    updateMenuButtonInput();
    if (appMode != APP_EREADER) {
      delay(5);
      return;
    }

    while (popInputEvent() != INPUT_NONE) {}

    ereaderLoop();

    if (!serialPortalKeepAwake && ereaderWantsSleep()) {
      enterDeepSleep();
      return;
    }

    delay(5);
    return;
  }

  if (petMode == PET_CARE_NOTICE) {
    loopCareNotice();
    delay(5);
    return;
  }

  updateMenuButtonInput();
  if (appMode != APP_TAMAGOTCHI) {
    delay(5);
    return;
  }

  if (petMode == PET_INTRO) {
    if (now - lastFrameMs >= getDeliveryIntroCurrentFrameHoldMs()) {
      lastFrameMs = now;
      advanceDeliveryIntroAnimation();
    }

    delay(5);
    return;
  }

  if (petMode == PET_SLEEP_SELECT) {
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

    if (now < inputLockoutUntil) {
      delay(5);
      return;
    }

    if (digitalRead(EXIT_KEY) == LOW) {
      cancelSleepDialog();
      delay(5);
      return;
    }

    InputEvent dialogEvent = popInputEvent();
    if (dialogEvent != INPUT_NONE) {
      processInputEvent(dialogEvent);
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

  if (petMode == PET_COOLDOWN_MSG) {
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

    if (now < inputLockoutUntil) {
      delay(5);
      return;
    }

    // Exit key closes the dialog (same direct-read pattern as the runaway screen).
    if (digitalRead(EXIT_KEY) == LOW) {
      closeCooldownDialog();
      delay(5);
      return;
    }

    InputEvent dialogEvent = popInputEvent();
    if (dialogEvent != INPUT_NONE) {
      processInputEvent(dialogEvent);
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

  if (petMode == PET_SLEEPING) {
    uint32_t nowEpoch = (uint32_t)time(NULL);
    uint32_t elapsed = (nowEpoch > sleepStartEpoch) ? (nowEpoch - sleepStartEpoch) : 0;

    // Natural wake when the chosen duration has elapsed.
    if (elapsed >= sleepDurationSec) {
      wakeCatFromSleep(false);
      delay(5);
      return;
    }

    // Remove the wake button at the 30-minute mark with a full refresh.
    if (sleepWakeButtonShown && elapsed >= SLEEP_WAKE_WINDOW_SEC) {
      sleepWakeButtonShown = false;
      forceFullRefreshNextFrame = true;
    }

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

    if (now < inputLockoutUntil) {
      delay(5);
      return;
    }

    // Main tap within the window wakes early (handled in processInputEvent).
    InputEvent sleepEvent = popInputEvent();
    if (sleepEvent != INPUT_NONE) {
      processInputEvent(sleepEvent);
      delay(5);
      return;
    }

    if (now - lastActivityMs >= INACTIVITY_TIMEOUT_MS) {
      enterDeepSleep();
      return;
    }

    // Advance the sleeping animation; each frame refreshes the remaining bar.
    if (now - lastFrameMs >= currentAnimation->frameHoldMs) {
      lastFrameMs = now;
      advanceIdleAnimation();
    }

    delay(5);
    return;
  }

  if (petMode == PET_RAN_AWAY) {
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

    if (now < inputLockoutUntil) {
      delay(5);
      return;
    }

    InputEvent runawayEvent = popInputEvent();
    bool menuPressed = isMenuButtonPressed();
    bool exitPressed = (digitalRead(EXIT_KEY) == LOW);

    if (runawayEvent != INPUT_NONE || menuPressed || exitPressed) {
      onRunawayAcknowledged();
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
      if (petMode != PET_IDLE) {
        delay(5);
        return;
      }
      bool animationChanged = syncIdleAnimationToNeeds();

      if (!animationChanged && now - lastFrameMs >= currentAnimation->frameHoldMs) {
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
    if (petMode != PET_IDLE) {
      delay(5);
      return;
    }
    bool animationChanged = syncIdleAnimationToNeeds();

    unsigned long inactiveMs = now - lastActivityMs;

    if (inactiveMs >= INACTIVITY_TIMEOUT_MS) {
      enterDeepSleep();
      return;
    }

    if (inactiveMs >= SLEEP_ANIMATION_TIMEOUT_MS && activeSadNeed == SAD_NEED_NONE) {
      if (currentAnimation != &sleepingAnimation) {
        startDrowsySleeping();
      }
    } else if (currentAnimation == &sleepingAnimation) {
      returnToIdleFromDrowsy();
    }

    if (!animationChanged && now - lastFrameMs >= currentAnimation->frameHoldMs) {
      lastFrameMs = now;
      advanceIdleAnimation();
    }
  }

  delay(5);
}
