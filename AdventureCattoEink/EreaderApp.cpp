#include "EreaderApp.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <vector>

#include "tiny-reader/src/Config.h"
#include "tiny-reader/src/Input.h"
#include "tiny-reader/src/Storage.h"
#include "tiny-reader/src/Ui.h"

namespace {

EpdDisplay display(EPD_DRIVER_CLASS(Config::PIN_EPD_CS, Config::PIN_EPD_DC, Config::PIN_EPD_RST, Config::PIN_EPD_BUSY));

enum class ScreenId : uint8_t {
  Reader = 0,
  MenuLibrary = 1,
  ChooseBook = 2,
  MenuInfo = 3,
  Error = 4
};

struct PageData {
  String text;
  uint32_t startPos = 0;
  uint32_t endPos = 0;
  bool eof = false;
};

struct ReaderState {
  File file;
  String path;
  size_t size = 0;
  uint32_t pagePos = 0;
  uint32_t nextPagePos = 0;
  std::vector<uint32_t> history;
};

ReaderState reader;
ButtonManager buttons;
ScreenId screen = ScreenId::MenuLibrary;
unsigned long lastActivity = 0;
uint8_t partialCount = 0;
std::vector<BookInfo> libraryBooks;
int libraryIndex = 0;
int libraryScroll = 0;
bool initialized = false;
bool storageReady = false;
RTC_DATA_ATTR uint32_t ereaderRtcMagic = 0;
RTC_DATA_ATTR uint8_t ereaderRtcScreen = static_cast<uint8_t>(ScreenId::MenuLibrary);
constexpr uint32_t EREADER_RTC_MAGIC = 0xEAD30001UL;

bool canResumeReaderScreen() {
  return ereaderRtcMagic == EREADER_RTC_MAGIC &&
         ereaderRtcScreen == static_cast<uint8_t>(ScreenId::Reader);
}

void saveEreaderResumeScreen(ScreenId target) {
  ereaderRtcMagic = EREADER_RTC_MAGIC;
  ereaderRtcScreen = static_cast<uint8_t>(target);
}

void updateActivity() {
  lastActivity = millis();
}

String titleFromPath(const String& path) {
  int slash = path.lastIndexOf('/');
  String name = (slash >= 0) ? path.substring(slash + 1) : path;
  if (name.length() == 0) {
    return "Book";
  }
  return name;
}

PageData readPage(File& file) {
  PageData page;
  page.startPos = file.position();

  char buffer[Config::READ_BUFFER_SIZE];
  int bytesRead = file.read(reinterpret_cast<uint8_t*>(buffer), Config::READ_BUFFER_SIZE);

  if (bytesRead > 0) {
    page.text = String(reinterpret_cast<char*>(buffer), bytesRead);
  } else {
    page.text = "";
  }

  page.endPos = file.position();
  page.eof = !file.available();
  return page;
}

void refreshLibrary() {
  libraryBooks = storageListBooks();
  if (libraryBooks.empty()) {
    libraryIndex = 0;
    libraryScroll = 0;
    return;
  }

  String currentBook = storageGetCurrentBook();
  if (currentBook.length() > 0) {
    for (size_t i = 0; i < libraryBooks.size(); ++i) {
      if (libraryBooks[i].path == currentBook) {
        libraryIndex = static_cast<int>(i);
        break;
      }
    }
  }

  if (libraryIndex >= static_cast<int>(libraryBooks.size())) {
    libraryIndex = static_cast<int>(libraryBooks.size()) - 1;
  }
  if (libraryIndex < 0) {
    libraryIndex = 0;
  }

  int maxVisible = uiLayout().maxLines;
  if (libraryIndex < libraryScroll) {
    libraryScroll = libraryIndex;
  }
  if (libraryIndex >= libraryScroll + maxVisible) {
    libraryScroll = libraryIndex - maxVisible + 1;
  }
}

ScreenId previousMenu(ScreenId target) {
  switch (target) {
    case ScreenId::MenuLibrary:
      return ScreenId::MenuInfo;
    case ScreenId::MenuInfo:
      return ScreenId::MenuLibrary;
    default:
      return ScreenId::MenuLibrary;
  }
}

ScreenId nextMenu(ScreenId target) {
  switch (target) {
    case ScreenId::MenuLibrary:
      return ScreenId::MenuInfo;
    case ScreenId::MenuInfo:
      return ScreenId::MenuLibrary;
    default:
      return ScreenId::MenuLibrary;
  }
}

void openBook(const String& path, bool resetPos) {
  if (reader.file) {
    reader.file.close();
  }
  reader.path = storageNormalizeBookPath(path);
  reader.file = LittleFS.open(reader.path, "r");
  if (!reader.file) {
    Serial.printf("Failed to open book: %s\n", reader.path.c_str());
    reader.size = 0;
    return;
  }

  reader.size = reader.file.size();
  uint32_t pos = resetPos ? 0 : storageLoadProgress(reader.path);
  if (pos >= reader.size) {
    pos = 0;
  }
  reader.pagePos = pos;
  reader.nextPagePos = pos;
  reader.file.seek(reader.pagePos);
  reader.history.clear();
  storageSetCurrentBook(reader.path);
  partialCount = 0;
}

void renderCurrentPage(bool allowPartial) {
  if (!reader.file) {
    return;
  }

  reader.file.seek(reader.pagePos);
  PageData page = readPage(reader.file);
  storageSaveProgress(reader.path, reader.pagePos);

  ReaderView view;
  view.title = titleFromPath(reader.path);
  view.text = page.text;
  view.bytesConsumed = 0;
  view.progressPercent = (reader.size > 0)
                           ? static_cast<uint8_t>(min<uint32_t>(100, (reader.pagePos * 100UL) / reader.size))
                           : 0;

  bool usePartial = allowPartial && (partialCount < Config::PARTIAL_REFRESH_LIMIT);
  uiDrawReader(display, view, usePartial);

  reader.nextPagePos = reader.pagePos + view.bytesConsumed;
  if (reader.nextPagePos >= reader.size) {
    reader.nextPagePos = reader.size;
  }

  if (usePartial) {
    partialCount++;
  } else {
    partialCount = 0;
  }
}

void renderNextPage() {
  if (!reader.file || reader.pagePos >= reader.size) {
    return;
  }
  reader.history.push_back(reader.pagePos);
  if (reader.history.size() > 50) {
    reader.history.erase(reader.history.begin());
  }
  reader.pagePos = reader.nextPagePos;
  renderCurrentPage(true);
}

void renderPrevPage() {
  if (reader.history.empty()) {
    return;
  }
  reader.pagePos = reader.history.back();
  reader.history.pop_back();
  reader.nextPagePos = reader.pagePos;
  renderCurrentPage(true);
}

float readBatteryVoltage() {
  if (Config::BATTERY_ADC_PIN < 0) {
    return -1.0f;
  }
  uint16_t raw = analogRead(Config::BATTERY_ADC_PIN);
  float v = (static_cast<float>(raw) / Config::BATTERY_ADC_MAX) * Config::BATTERY_ADC_REF;
  return v * Config::BATTERY_DIVIDER;
}

int batteryPercentFromVoltage(float v) {
  if (v < 0.0f) {
    return 0;
  }
  float clamped = min(max(v, Config::BATTERY_MIN_V), Config::BATTERY_MAX_V);
  float pct = (clamped - Config::BATTERY_MIN_V) / (Config::BATTERY_MAX_V - Config::BATTERY_MIN_V);
  return static_cast<int>(pct * 100.0f + 0.5f);
}

void showScreen(ScreenId target) {
  if (target == ScreenId::Reader && !reader.file) {
    target = ScreenId::MenuLibrary;
  }
  screen = target;
  saveEreaderResumeScreen(screen);

  switch (screen) {
    case ScreenId::Reader:
      renderCurrentPage(false);
      break;
    case ScreenId::MenuLibrary:
      refreshLibrary();
      uiDrawLibrary(display, libraryBooks, -1, libraryScroll);
      break;
    case ScreenId::ChooseBook:
      refreshLibrary();
      if (libraryBooks.empty()) {
        libraryIndex = 0;
        libraryScroll = 0;
      } else if (libraryIndex >= static_cast<int>(libraryBooks.size())) {
        libraryIndex = static_cast<int>(libraryBooks.size()) - 1;
      }
      uiDrawLibrary(display, libraryBooks, libraryIndex, libraryScroll);
      break;
    case ScreenId::MenuInfo: {
      StorageStats stats = storageGetStats();
      float v = readBatteryVoltage();
      int pct = batteryPercentFromVoltage(v);
      uiDrawInfo(display, stats, v, pct);
      break;
    }
    case ScreenId::Error:
      break;
  }
}

bool ensureStorageReady() {
  if (storageBegin(false)) {
    storageEnsureDirs();
    return true;
  }

  screen = ScreenId::Error;
  uiDrawError(display, "LittleFS error", "Mount failed", "Check filesystem");
  return false;
}

void handleButtons() {
  bool action = false;

  if (buttons.consumeShortPress(ButtonId::Exit)) {
    switch (screen) {
      case ScreenId::Reader:
      case ScreenId::ChooseBook:
        showScreen(ScreenId::MenuLibrary);
        break;
      case ScreenId::MenuInfo:
        showScreen(ScreenId::MenuLibrary);
        break;
      case ScreenId::MenuLibrary:
        if (reader.file) {
          showScreen(ScreenId::Reader);
        }
        break;
      case ScreenId::Error:
        break;
    }
    action = true;
  }

  switch (screen) {
    case ScreenId::Reader:
      if (buttons.consumeShortPress(ButtonId::Next)) {
        renderNextPage();
        action = true;
      }
      if (buttons.consumeShortPress(ButtonId::Prev)) {
        renderPrevPage();
        action = true;
      }
      break;
    case ScreenId::MenuLibrary:
      if (buttons.consumeShortPress(ButtonId::Next)) {
        showScreen(nextMenu(screen));
        action = true;
      }
      if (buttons.consumeShortPress(ButtonId::Prev)) {
        showScreen(previousMenu(screen));
        action = true;
      }
      if (buttons.consumeShortPress(ButtonId::Ok)) {
        refreshLibrary();
        if (!libraryBooks.empty()) {
          if (libraryIndex >= static_cast<int>(libraryBooks.size())) {
            libraryIndex = static_cast<int>(libraryBooks.size()) - 1;
          }
          showScreen(ScreenId::ChooseBook);
        }
        action = true;
      }
      break;
    case ScreenId::ChooseBook:
      if (buttons.consumeShortPress(ButtonId::Next)) {
        if (!libraryBooks.empty()) {
          libraryIndex = min(libraryIndex + 1, static_cast<int>(libraryBooks.size()) - 1);
          int maxVisible = uiLayout().maxLines;
          if (libraryIndex >= libraryScroll + maxVisible) {
            libraryScroll = libraryIndex - maxVisible + 1;
          }
          uiDrawLibrary(display, libraryBooks, libraryIndex, libraryScroll);
        }
        action = true;
      }
      if (buttons.consumeShortPress(ButtonId::Prev)) {
        if (!libraryBooks.empty()) {
          libraryIndex = max(libraryIndex - 1, 0);
          if (libraryIndex < libraryScroll) {
            libraryScroll = libraryIndex;
          }
          uiDrawLibrary(display, libraryBooks, libraryIndex, libraryScroll);
        }
        action = true;
      }
      if (buttons.consumeShortPress(ButtonId::Ok)) {
        if (!libraryBooks.empty()) {
          openBook(libraryBooks[libraryIndex].path, false);
          showScreen(ScreenId::Reader);
        } else {
          showScreen(ScreenId::MenuLibrary);
        }
        action = true;
      }
      break;
    case ScreenId::MenuInfo:
      if (buttons.consumeShortPress(ButtonId::Next)) {
        showScreen(nextMenu(screen));
        action = true;
      }
      if (buttons.consumeShortPress(ButtonId::Prev)) {
        showScreen(previousMenu(screen));
        action = true;
      }
      if (buttons.consumeShortPress(ButtonId::Ok)) {
        showScreen(ScreenId::MenuInfo);
        action = true;
      }
      break;
    case ScreenId::Error:
      break;
  }

  if (action) {
    updateActivity();
  }
}

}  // namespace

void ereaderBegin() {
  if (initialized) {
    return;
  }

  pinMode(Config::PIN_EPD_POWER, OUTPUT);
  digitalWrite(Config::PIN_EPD_POWER, HIGH);

  uiInit(display);
  buttons.begin();

  if (Config::BATTERY_ADC_PIN >= 0) {
    analogReadResolution(12);
  }

  storageReady = ensureStorageReady();
  initialized = true;
  lastActivity = millis();
}

void ereaderEnter() {
  ereaderBegin();
  digitalWrite(Config::PIN_EPD_POWER, HIGH);

  if (!storageReady) {
    ensureStorageReady();
  }

  String current = storageGetCurrentBook();
  if (canResumeReaderScreen() && current.length() > 0) {
    openBook(current, false);
    showScreen(ScreenId::Reader);
  } else {
    showScreen(ScreenId::MenuLibrary);
  }

  updateActivity();
}

void ereaderLoop() {
  if (!initialized) {
    ereaderEnter();
  }

  buttons.update();

  handleButtons();
}

void ereaderLeave() {
  if (reader.file) {
    storageSaveProgress(reader.path, reader.pagePos);
  }
  saveEreaderResumeScreen(screen);
  display.hibernate();
}

bool ereaderWantsSleep() {
  if (!initialized || screen == ScreenId::Error) {
    return false;
  }
  return millis() - lastActivity >= Config::INACTIVITY_SLEEP_MS;
}

#include "tiny-reader/src/Input.cpp"
#include "tiny-reader/src/Storage.cpp"
#include "tiny-reader/src/Ui.cpp"
