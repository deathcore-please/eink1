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

struct PageCheckpoint {
  uint32_t pageIndex = 0;
  uint32_t pos = 0;
};

struct ReaderState {
  File file;
  String path;
  size_t size = 0;
  uint32_t pageIndex = 0;
  uint32_t prevPagePos = 0;
  uint32_t pagePos = 0;
  uint32_t nextPagePos = 0;
  std::vector<PageCheckpoint> checkpoints;
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
constexpr uint32_t NAV_STATE_VERSION = 1001UL;
constexpr uint32_t NAV_CHECKPOINT_INTERVAL = 20UL;
constexpr size_t NAV_MAX_CHECKPOINTS = 512;

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

String navigationPathForBook(const String& bookPath) {
  int slash = bookPath.lastIndexOf('/');
  String base = (slash >= 0) ? bookPath.substring(slash + 1) : bookPath;
  String safe = storageSanitizeFilename(base);
  int dot = safe.lastIndexOf('.');
  if (dot > 0) {
    safe = safe.substring(0, dot);
  }
  return String(Config::PROGRESS_DIR) + "/" + safe + ".nav";
}

void resetNavigationState() {
  reader.pageIndex = 0;
  reader.prevPagePos = 0;
  reader.pagePos = 0;
  reader.nextPagePos = 0;
  reader.checkpoints.clear();
  PageCheckpoint first;
  first.pageIndex = 0;
  first.pos = 0;
  reader.checkpoints.push_back(first);
}

void addOrUpdateCheckpoint(uint32_t pageIndex, uint32_t pos) {
  if (pageIndex % NAV_CHECKPOINT_INTERVAL != 0) {
    return;
  }
  if (pos > reader.size) {
    return;
  }

  for (size_t i = 0; i < reader.checkpoints.size(); ++i) {
    if (reader.checkpoints[i].pageIndex == pageIndex) {
      reader.checkpoints[i].pos = pos;
      return;
    }
    if (reader.checkpoints[i].pageIndex > pageIndex) {
      PageCheckpoint checkpoint;
      checkpoint.pageIndex = pageIndex;
      checkpoint.pos = pos;
      reader.checkpoints.insert(reader.checkpoints.begin() + i, checkpoint);
      return;
    }
  }

  PageCheckpoint checkpoint;
  checkpoint.pageIndex = pageIndex;
  checkpoint.pos = pos;
  reader.checkpoints.push_back(checkpoint);
}

const PageCheckpoint& nearestCheckpointForPage(uint32_t pageIndex) {
  size_t bestIndex = 0;
  for (size_t i = 0; i < reader.checkpoints.size(); ++i) {
    if (reader.checkpoints[i].pageIndex > pageIndex) {
      break;
    }
    bestIndex = i;
  }
  return reader.checkpoints[bestIndex];
}

bool loadNavigationState(uint32_t savedPos) {
  String navPath = navigationPathForBook(reader.path);
  File file = LittleFS.open(navPath, "r");
  if (!file) {
    return false;
  }

  uint32_t version = file.parseInt();
  uint32_t bookSize = file.parseInt();
  uint32_t pageIndex = file.parseInt();
  uint32_t pagePos = file.parseInt();
  uint32_t count = file.parseInt();

  if (version != NAV_STATE_VERSION ||
      bookSize != reader.size ||
      pagePos != savedPos ||
      pagePos >= reader.size ||
      count == 0 ||
      count > NAV_MAX_CHECKPOINTS) {
    file.close();
    return false;
  }

  reader.checkpoints.clear();
  for (uint32_t i = 0; i < count; ++i) {
    PageCheckpoint checkpoint;
    checkpoint.pageIndex = file.parseInt();
    checkpoint.pos = file.parseInt();
    if (checkpoint.pos <= reader.size) {
      reader.checkpoints.push_back(checkpoint);
    }
  }
  file.close();

  if (reader.checkpoints.empty() || reader.checkpoints[0].pageIndex != 0 || reader.checkpoints[0].pos != 0) {
    resetNavigationState();
    return false;
  }

  reader.pageIndex = pageIndex;
  reader.pagePos = pagePos;
  reader.prevPagePos = pagePos;
  reader.nextPagePos = pagePos;
  return true;
}

void saveNavigationState() {
  if (!reader.file || reader.path.length() == 0) {
    return;
  }

  String navPath = navigationPathForBook(reader.path);
  File file = LittleFS.open(navPath, "w");
  if (!file) {
    return;
  }

  file.println(NAV_STATE_VERSION);
  file.println(static_cast<uint32_t>(reader.size));
  file.println(reader.pageIndex);
  file.println(reader.pagePos);
  file.println(static_cast<uint32_t>(reader.checkpoints.size()));
  for (size_t i = 0; i < reader.checkpoints.size(); ++i) {
    file.print(reader.checkpoints[i].pageIndex);
    file.print(' ');
    file.println(reader.checkpoints[i].pos);
  }
  file.close();
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

uint32_t measureNextPagePos(uint32_t fromPos);
bool pagePosForIndex(uint32_t targetPageIndex, uint32_t& outPos);
void rebuildNavigationToPosition(uint32_t targetPos);

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

  if (resetPos) {
    LittleFS.remove(navigationPathForBook(reader.path));
  }

  if (pos == 0 || !loadNavigationState(pos)) {
    rebuildNavigationToPosition(pos);
  }

  reader.file.seek(reader.pagePos);
  storageSetCurrentBook(reader.path);
  partialCount = 0;
}

void renderCurrentPage(bool allowPartial) {
  if (!reader.file) {
    return;
  }

  reader.file.seek(reader.pagePos);
  PageData page = readPage(reader.file);

  ReaderView view;
  view.title = titleFromPath(reader.path);
  view.text = page.text;
  view.bytesConsumed = 0;
  view.progressPercent = (reader.size > 0)
                           ? static_cast<uint8_t>(min<uint32_t>(100, (reader.pagePos * 100UL) / reader.size))
                           : 0;

  bool usePartial = allowPartial && (partialCount < Config::PARTIAL_REFRESH_LIMIT);
  uiDrawReader(display, view, usePartial);

  if (view.bytesConsumed == 0 && page.endPos > reader.pagePos) {
    view.bytesConsumed = page.endPos - reader.pagePos;
  }

  reader.nextPagePos = reader.pagePos + view.bytesConsumed;
  if (reader.nextPagePos <= reader.pagePos && reader.pagePos < reader.size) {
    reader.nextPagePos = reader.pagePos + 1;
  }
  if (reader.nextPagePos >= reader.size) {
    reader.nextPagePos = reader.size;
  }

  addOrUpdateCheckpoint(reader.pageIndex, reader.pagePos);
  if (reader.pageIndex == 0) {
    reader.prevPagePos = reader.pagePos;
  } else {
    uint32_t previousPos = reader.pagePos;
    if (pagePosForIndex(reader.pageIndex - 1, previousPos)) {
      reader.prevPagePos = previousPos;
    }
  }

  storageSaveProgress(reader.path, reader.pagePos);
  saveNavigationState();

  if (usePartial) {
    partialCount++;
  } else {
    partialCount = 0;
  }
}

uint32_t measureNextPagePos(uint32_t fromPos) {
  if (!reader.file || fromPos >= reader.size) {
    return reader.size;
  }

  reader.file.seek(fromPos);
  PageData page = readPage(reader.file);
  size_t bytesConsumed = uiMeasureReaderBytes(display, page.text);

  if (bytesConsumed == 0 && page.endPos > fromPos) {
    bytesConsumed = page.endPos - fromPos;
  }

  uint32_t nextPos = fromPos + static_cast<uint32_t>(bytesConsumed);
  if (nextPos <= fromPos) {
    nextPos = fromPos + 1;
  }
  if (nextPos > reader.size) {
    nextPos = reader.size;
  }

  return nextPos;
}

bool pagePosForIndex(uint32_t targetPageIndex, uint32_t& outPos) {
  if (!reader.file) {
    return false;
  }
  if (targetPageIndex == reader.pageIndex) {
    outPos = reader.pagePos;
    return true;
  }

  const PageCheckpoint& checkpoint = nearestCheckpointForPage(targetPageIndex);
  uint32_t cursorIndex = checkpoint.pageIndex;
  uint32_t cursorPos = checkpoint.pos;

  while (cursorIndex < targetPageIndex && cursorPos < reader.size) {
    uint32_t nextPos = measureNextPagePos(cursorPos);
    if (nextPos <= cursorPos) {
      return false;
    }

    cursorIndex++;
    cursorPos = nextPos;
    addOrUpdateCheckpoint(cursorIndex, cursorPos);
  }

  outPos = cursorPos;
  return cursorIndex == targetPageIndex;
}

void rebuildNavigationToPosition(uint32_t targetPos) {
  resetNavigationState();

  uint32_t cursorIndex = 0;
  uint32_t cursorPos = 0;
  while (cursorPos < targetPos && cursorPos < reader.size) {
    uint32_t nextPos = measureNextPagePos(cursorPos);
    if (nextPos <= cursorPos) {
      break;
    }

    uint32_t nextIndex = cursorIndex + 1;
    if (nextPos > targetPos) {
      break;
    }

    cursorIndex = nextIndex;
    cursorPos = nextPos;
    addOrUpdateCheckpoint(cursorIndex, cursorPos);
  }

  reader.pageIndex = cursorIndex;
  reader.pagePos = cursorPos;
  reader.prevPagePos = cursorPos;
  reader.nextPagePos = cursorPos;
}

void renderNextPage() {
  if (!reader.file || reader.pagePos >= reader.size) {
    return;
  }
  if (reader.nextPagePos <= reader.pagePos || reader.nextPagePos >= reader.size) {
    return;
  }

  reader.pageIndex++;
  reader.pagePos = reader.nextPagePos;
  renderCurrentPage(true);
}

void renderPrevPage() {
  if (!reader.file || reader.pageIndex == 0) {
    return;
  }

  uint32_t targetPos = reader.prevPagePos;
  if (targetPos >= reader.pagePos && !pagePosForIndex(reader.pageIndex - 1, targetPos)) {
    return;
  }
  if (targetPos >= reader.pagePos) {
    return;
  }

  reader.pageIndex--;
  reader.pagePos = targetPos;
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

  if (Config::BATTERY_ADC_PIN >= 0) {
    analogReadResolution(12);
  }

  uiInit(display);
  storageReady = ensureStorageReady();
  initialized = true;
  lastActivity = millis();
}

void ereaderEnter() {
  ereaderBegin();
  digitalWrite(Config::PIN_EPD_POWER, HIGH);
  uiInit(display);
  buttons.begin();

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
    saveNavigationState();
  }
  saveEreaderResumeScreen(screen);
  display.powerOff();
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
