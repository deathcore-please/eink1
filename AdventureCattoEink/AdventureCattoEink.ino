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
#include "petting_animation.h"
#include "sad_hungry_animation.h"
#include "sad_pee_animation.h"
#include "sad_play_animation.h"
#include "sad_pet_animation.h"
#include "sleeping_animation.h"
#include "sleep_splash_night.h"
#include "sleep_splash_day.h"
#include "menu_cat_icon.h"
#include "menu_reader_icon.h"
#include "EreaderApp.h"

extern uint8_t ImageBW[ALLSCREEN_BYTES];

// Board input pins from Elecrow example
#define HOME_KEY 2
#define EXIT_KEY 1
#define PRV_KEY  6
#define NEXT_KEY 4
#define OK_KEY   5

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

// Need/meter settings
// Time for a full (100 -> 0) bar to drain, per need.
#define PEE_FULL_DRAIN_SECONDS  (6UL * 3600UL)
#define FOOD_FULL_DRAIN_SECONDS (4UL * 3600UL)
#define PLAY_FULL_DRAIN_SECONDS (6UL * 3600UL)
#define PET_FULL_DRAIN_SECONDS  (2UL * 3600UL)
#define NEED_MAX_VALUE 100UL
#define NEED_SAD_THRESHOLD 10

// Deep sleep settings
#define SLEEP_ANIMATION_TIMEOUT_MS 15000
#define INACTIVITY_TIMEOUT_MS 30000
#define MENU_BUTTON_DEBOUNCE_MS 30
#define RTC_PET_STATE_MAGIC 0xCA77000BUL
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

#define PEE_Y 28
#define FOOD_Y 48
#define PLAY_Y 68
#define LOVE_Y 88

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
#define AGE_TEXT_SIZE 12
#define AGE_LABEL_MAX_CHARS 22
#define AGE_LABEL_PAD 2
#define AGE_LABEL_W ((AGE_LABEL_MAX_CHARS * (AGE_TEXT_SIZE / 2)) + (AGE_LABEL_PAD * 2))
#define AGE_LABEL_H (AGE_TEXT_SIZE + 2)
#define AGE_LABEL_X (SCREEN_W - AGE_LABEL_W)
#define AGE_LABEL_Y 0

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
  ACTION_PLAY,
  ACTION_PET
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

RTC_DATA_ATTR struct {
  uint32_t magic;
  uint8_t peeValue;
  uint8_t foodValue;
  uint8_t playValue;
  uint8_t loveValue;
  uint8_t activeSadNeed;
  uint8_t selectedAction;
  uint8_t appMode;
  uint8_t homeSelection;
  uint8_t petAgeStarted;
  uint32_t epochSeconds;
  uint64_t epochSetUs;
  uint32_t petBirthEpoch;
  uint32_t sleepEntryEpoch;
  uint32_t lastNeedDrainEpoch;
  uint32_t peeDrainCarry;
  uint32_t foodDrainCarry;
  uint32_t playDrainCarry;
  uint32_t loveDrainCarry;
} rtcPetState;

// These ones actually change over time
uint8_t peeValue = 100;
uint8_t foodValue = 100;
uint8_t playValue = 100;
uint8_t loveValue = 100;
bool petAgeStarted = false;
uint32_t petBirthEpoch = 0;
uint32_t lastNeedDrainEpoch = 0;
uint32_t peeDrainCarry = 0;
uint32_t foodDrainCarry = 0;
uint32_t playDrainCarry = 0;
uint32_t loveDrainCarry = 0;

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

// #region agent log
volatile uint32_t mainEdgeCount = 0;
// #endregion

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
  // #region agent log
  mainEdgeCount++;
  // #endregion
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

void selectActivityMessage(const char* const messages[], uint8_t count) {
  if (count == 0) {
    currentActivityMessage = "";
    return;
  }

  randomSeed((uint32_t)esp_timer_get_time() ^ (uint32_t)micros());
  currentActivityMessage = messages[random(count)];
}

void resetPetAgeTimer() {
  // Reset hook for handoff: call this before delivery to start the pet age at zero.
  petAgeStarted = false;
  petBirthEpoch = 0;
  rtcPetState.petAgeStarted = 0;
  rtcPetState.petBirthEpoch = 0;
}

void startPetAgeTimerIfNeeded() {
  if (petAgeStarted) {
    return;
  }

  petBirthEpoch = (uint32_t)time(NULL);
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
  clearLeftHudArea();

  if (petMode == PET_ACTION) {
    drawWrappedActivityMessage(currentActivityMessage);
    return;
  }

  drawAgeLabel();

  drawActionRow("PEE", PEE_Y, peeValue, selectedAction == ACTION_PEE);
  drawActionRow("FOOD", FOOD_Y, foodValue, selectedAction == ACTION_FOOD);
  drawActionRow("PLAY", PLAY_Y, playValue, selectedAction == ACTION_PLAY);
  drawActionRow("PETS", LOVE_Y, loveValue, selectedAction == ACTION_PET);
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
  return hasSadAnimationForNeed(need) && valueForSadNeed(need) < NEED_SAD_THRESHOLD;
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

void considerSadNeedCrossing(
  SadNeed need,
  uint8_t previousValue,
  uint32_t drain,
  uint32_t fullDrainSeconds,
  SadNeed* bestNeed,
  uint64_t* bestCrossKey
) {
  if (!hasSadAnimationForNeed(need)) {
    return;
  }

  if (previousValue < NEED_SAD_THRESHOLD || drain == 0) {
    return;
  }

  uint8_t nextValue = previousValue > drain ? previousValue - drain : 0;
  if (nextValue >= NEED_SAD_THRESHOLD) {
    return;
  }

  // Rank by approximate time-to-cross so the need that actually hit the
  // threshold first (in wall-clock terms) wins, even with different rates.
  // seconds-per-point is fullDrainSeconds/100; the common /100 is dropped.
  uint32_t pointsToCross = previousValue - (NEED_SAD_THRESHOLD - 1);
  uint64_t crossKey = (uint64_t)pointsToCross * fullDrainSeconds;
  if (crossKey < *bestCrossKey) {
    *bestCrossKey = crossKey;
    *bestNeed = need;
  }
}

SadNeed firstNeedCrossedByDrain(
  uint8_t previousPee,
  uint8_t previousFood,
  uint8_t previousPlay,
  uint8_t previousPets,
  uint32_t peeTicks,
  uint32_t foodTicks,
  uint32_t playTicks,
  uint32_t loveTicks
) {
  SadNeed bestNeed = SAD_NEED_NONE;
  uint64_t bestCrossKey = UINT64_MAX;

  considerSadNeedCrossing(SAD_NEED_PEE, previousPee, peeTicks, PEE_FULL_DRAIN_SECONDS, &bestNeed, &bestCrossKey);
  considerSadNeedCrossing(SAD_NEED_FOOD, previousFood, foodTicks, FOOD_FULL_DRAIN_SECONDS, &bestNeed, &bestCrossKey);
  considerSadNeedCrossing(SAD_NEED_PLAY, previousPlay, playTicks, PLAY_FULL_DRAIN_SECONDS, &bestNeed, &bestCrossKey);
  considerSadNeedCrossing(SAD_NEED_PETS, previousPets, loveTicks, PET_FULL_DRAIN_SECONDS, &bestNeed, &bestCrossKey);

  return bestNeed;
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

uint32_t computeNeedTicks(uint32_t elapsedSeconds, uint32_t fullDrainSeconds, uint32_t& carry) {
  uint64_t units = (uint64_t)elapsedSeconds * NEED_MAX_VALUE + carry;
  uint32_t ticks = (uint32_t)(units / fullDrainSeconds);
  carry = (uint32_t)(units % fullDrainSeconds);
  return ticks;
}

void applyNeedDrainTicks(uint32_t peeTicks, uint32_t foodTicks, uint32_t playTicks, uint32_t loveTicks) {
  if (peeTicks == 0 && foodTicks == 0 && playTicks == 0 && loveTicks == 0) {
    return;
  }

  uint8_t previousPee = peeValue;
  uint8_t previousFood = foodValue;
  uint8_t previousPlay = playValue;
  uint8_t previousPets = loveValue;

  peeValue = peeValue > peeTicks ? peeValue - peeTicks : 0;
  foodValue = foodValue > foodTicks ? foodValue - foodTicks : 0;
  playValue = playValue > playTicks ? playValue - playTicks : 0;
  loveValue = loveValue > loveTicks ? loveValue - loveTicks : 0;

  refreshSadNeedAfterNeedChange(
    firstNeedCrossedByDrain(
      previousPee, previousFood, previousPlay, previousPets,
      peeTicks, foodTicks, playTicks, loveTicks
    )
  );
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
  if (nowEpoch <= lastNeedDrainEpoch) {
    return;
  }

  uint32_t elapsedSeconds = nowEpoch - lastNeedDrainEpoch;
  uint32_t peeTicks = computeNeedTicks(elapsedSeconds, PEE_FULL_DRAIN_SECONDS, peeDrainCarry);
  uint32_t foodTicks = computeNeedTicks(elapsedSeconds, FOOD_FULL_DRAIN_SECONDS, foodDrainCarry);
  uint32_t playTicks = computeNeedTicks(elapsedSeconds, PLAY_FULL_DRAIN_SECONDS, playDrainCarry);
  uint32_t loveTicks = computeNeedTicks(elapsedSeconds, PET_FULL_DRAIN_SECONDS, loveDrainCarry);

  applyNeedDrainTicks(peeTicks, foodTicks, playTicks, loveTicks);

  // Carries have already absorbed elapsedSeconds, so always advance the clock
  // (otherwise the next call would double-count this interval).
  lastNeedDrainEpoch = nowEpoch;
  rtcPetState.lastNeedDrainEpoch = lastNeedDrainEpoch;
  rtcPetState.peeDrainCarry = peeDrainCarry;
  rtcPetState.foodDrainCarry = foodDrainCarry;
  rtcPetState.playDrainCarry = playDrainCarry;
  rtcPetState.loveDrainCarry = loveDrainCarry;
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
  loveValue = 100;
  activeSadNeed = SAD_NEED_NONE;
  selectedAction = ACTION_PEE;
  appMode = APP_HOME;
  homeSelection = HOME_OPTION_TAMAGOTCHI;
  lastNeedDrainEpoch = 0;
  peeDrainCarry = 0;
  foodDrainCarry = 0;
  playDrainCarry = 0;
  loveDrainCarry = 0;
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
  rtcPetState.selectedAction = selectedAction;
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

  if (appMode > APP_EREADER) {
    appMode = APP_TAMAGOTCHI;
  }

  if (homeSelection > HOME_OPTION_EREADER) {
    homeSelection = HOME_OPTION_TAMAGOTCHI;
  }

  if (selectedAction > ACTION_PET) {
    selectedAction = ACTION_PEE;
  }

  if (activeSadNeed > SAD_NEED_PETS) {
    activeSadNeed = SAD_NEED_NONE;
  }

  refreshSadNeedAfterNeedChange(SAD_NEED_NONE);

  return true;
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

  if (appMode == APP_EREADER) {
    ereaderLeave();
  }

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
  showHomeScreenFull();
}

void enterTamagotchi() {
  Serial.println("Entering tamagotchi");

  // #region agent log
  Serial.printf(
    "{\"sessionId\":\"439210\",\"hypothesisId\":\"M2\",\"location\":\"enterTamagotchi\","
    "\"data\":{\"edgeCount\":%lu},\"timestamp\":%lu}\n",
    (unsigned long)mainEdgeCount, (unsigned long)millis()
  );
  // #endregion

  startPetAgeTimerIfNeeded();
  attachMainInputInterrupts();
  resetMainInputState();
  appMode = APP_TAMAGOTCHI;
  forceFullRefreshNextFrame = true;
  discardWakeInput = false;
  startIdle();
  lastActivityMs = millis();
}

void enterEreaderMode() {
  Serial.println("Entering ereader");

  detachMainInputInterrupts();
  resetMainInputState();
  appMode = APP_EREADER;
  discardWakeInput = false;
  lastActivityMs = millis();
  ereaderRequestEnter();
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

  // #region agent log
  Serial.printf(
    "{\"sessionId\":\"439210\",\"hypothesisId\":\"M3\",\"location\":\"startSelectedAction\","
    "\"data\":{\"selectedAction\":%d,\"edgeCount\":%lu},\"timestamp\":%lu}\n",
    (int)selectedAction, (unsigned long)mainEdgeCount, (unsigned long)millis()
  );
  // #endregion

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

  if (activeAction == ACTIVE_PEE) {
    peeValue = 100;
  }

  if (activeAction == ACTIVE_FOOD) {
    foodValue = 100;
  }

  if (activeAction == ACTIVE_PLAY) {
    playValue = 100;
  }

  if (activeAction == ACTIVE_PET) {
    loveValue = 100;
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
  if (selectedAction == ACTION_PET) {
    selectedAction = ACTION_PLAY;
  } else if (selectedAction == ACTION_PLAY) {
    selectedAction = ACTION_FOOD;
  } else if (selectedAction == ACTION_FOOD) {
    selectedAction = ACTION_PEE;
  }

  Serial.print("Selected: ");
  if (selectedAction == ACTION_PEE) {
    Serial.println("Pee");
  } else if (selectedAction == ACTION_FOOD) {
    Serial.println("Food");
  } else if (selectedAction == ACTION_PLAY) {
    Serial.println("Play");
  } else {
    Serial.println("Pets");
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
  }

  Serial.print("Selected: ");
  if (selectedAction == ACTION_PEE) {
    Serial.println("Pee");
  } else if (selectedAction == ACTION_FOOD) {
    Serial.println("Food");
  } else if (selectedAction == ACTION_PLAY) {
    Serial.println("Play");
  } else {
    Serial.println("Pets");
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

  // #region agent log
  static unsigned long s_lastTapMs = 0;
  unsigned long s_nowTap = millis();
  Serial.printf(
    "{\"sessionId\":\"439210\",\"hypothesisId\":\"M1\",\"location\":\"onMainToggleTap\","
    "\"data\":{\"appMode\":%d,\"petMode\":%d,\"edgeCount\":%lu,\"dtSinceLastTapMs\":%lu},"
    "\"timestamp\":%lu}\n",
    (int)appMode, (int)petMode, (unsigned long)mainEdgeCount,
    (unsigned long)(s_nowTap - s_lastTapMs), (unsigned long)s_nowTap
  );
  s_lastTapMs = s_nowTap;
  // #endregion

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

bool isMenuButtonPressed() {
  return digitalRead(MENU_BUTTON_KEY) == LOW;
}

bool canMenuButtonEnterHome() {
  if (appMode == APP_HOME) {
    return false;
  }

  return !(appMode == APP_TAMAGOTCHI && petMode == PET_ACTION);
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

  attachInterrupt(digitalPinToInterrupt(MENU_BUTTON_KEY), handleMenuButtonInterrupt, FALLING);
  attachMainInputInterrupts();
  resetMainInputState();

  Serial.println("AdventureCattoEink");
  Serial.println("Interrupt input enabled");
  Serial.println("Main toggle tap: select action / enter mode");
  Serial.println("Menu button tap: exit to home screen");
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
      restoredFromRtc = true;
    } else {
      initDefaultPetState();
    }
  } else {
    initDefaultPetState();
  }

  initDeviceTime(restoredFromRtc);

  if (wakeCause == ESP_SLEEP_WAKEUP_EXT1 && restoredFromRtc) {
    applyNeedsSinceSleep();
  }

  if (appMode == APP_HOME) {
    showHomeScreenFull();
  } else if (appMode == APP_EREADER) {
    detachMainInputInterrupts();
    resetMainInputState();
    ereaderEnter();
  } else {
    startIdle();
  }

  lastFrameMs = millis();
  lastNeedDrainMs = millis();
  lastActivityMs = millis();
}

void loop() {
  unsigned long now = millis();
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

    if (ereaderWantsSleep()) {
      enterDeepSleep();
      return;
    }

    delay(5);
    return;
  }

  updateMenuButtonInput();
  if (appMode != APP_TAMAGOTCHI) {
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
