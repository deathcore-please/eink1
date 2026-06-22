#include "Ui.h"
#include "FreeSerif9pt8b.h"
#include "TextEncoding.h"

static UiLayout layout;
static UiLayout readerLayout;
static bool readerDarkMode = false;
static const char* kMenuLabels[] = {"Library", "Info"};
static const uint8_t kMenuCount = sizeof(kMenuLabels) / sizeof(kMenuLabels[0]);

static void computeLayout(EpdDisplay& display) {
  layout.width = display.width();
  layout.height = display.height();
  layout.margin = max<int16_t>(Config::UI_MIN_MARGIN, layout.width / 40);
  layout.sidebarW = max<int16_t>(38, layout.width / 4);
  layout.headerH = 0;
  layout.footerH = 0;
  layout.contentX = layout.sidebarW + layout.margin;
  layout.contentY = layout.margin;
  layout.contentW = layout.width - layout.sidebarW - layout.margin * 2;
  layout.contentH = layout.height - layout.margin * 2;
  layout.lineHeight = max<int16_t>(12, static_cast<int16_t>(8 * Config::UI_TEXT_SIZE + 4));
  int16_t charWidth = max<int16_t>(6, static_cast<int16_t>(6 * Config::UI_TEXT_SIZE));
  layout.charsPerLine = max<int16_t>(10, layout.contentW / charWidth);
  layout.maxLines = max<int16_t>(1, layout.contentH / layout.lineHeight);
}

static void computeReaderLayout(EpdDisplay& display) {
  readerLayout.width = display.width();
  readerLayout.height = display.height();
  readerLayout.margin = max<int16_t>(2, readerLayout.width / 80);
  readerLayout.headerH = 0;
  readerLayout.footerH = 2;
  readerLayout.contentX = readerLayout.margin;
  readerLayout.contentY = readerLayout.margin;
  readerLayout.contentW = readerLayout.width - readerLayout.margin * 2;
  readerLayout.contentH = readerLayout.height - readerLayout.footerH - readerLayout.margin * 2;
  // FreeSerif9pt8b needs more line spacing to avoid overlap
  readerLayout.lineHeight = max<int16_t>(16, static_cast<int16_t>(8 * Config::READER_TEXT_SIZE + 8));
  int16_t charWidth = max<int16_t>(6, static_cast<int16_t>(6 * Config::READER_TEXT_SIZE));
  readerLayout.charsPerLine = max<int16_t>(10, readerLayout.contentW / charWidth);
  readerLayout.maxLines = max<int16_t>(1, readerLayout.contentH / readerLayout.lineHeight);
}

static void drawSidebar(EpdDisplay& display, uint8_t activeIndex) {
  display.fillRect(0, 0, layout.sidebarW, layout.height, GxEPD_WHITE);
  int16_t segmentH = layout.height / kMenuCount;
  for (uint8_t i = 0; i < kMenuCount; ++i) {
    int16_t y = segmentH * i;
    int16_t h = (i == kMenuCount - 1) ? (layout.height - y) : segmentH;
    bool active = (i == activeIndex);
    int16_t inset = active ? 1 : 4;
    int16_t boxX = inset;
    int16_t boxY = y + inset;
    int16_t boxW = layout.sidebarW - inset * 2;
    int16_t boxH = h - inset * 2;
    display.fillRoundRect(boxX, boxY, boxW, boxH, 5, active ? GxEPD_BLACK : GxEPD_WHITE);
    display.drawRoundRect(boxX, boxY, boxW, boxH, 5, GxEPD_BLACK);
    if (active) {
      display.drawRoundRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2, 4, GxEPD_BLACK);
    }
    display.setTextColor(active ? GxEPD_WHITE : GxEPD_BLACK);
    int16_t labelX = boxX + layout.margin;
    int16_t labelY = boxY + boxH / 2 + 4;
    display.setCursor(labelX, labelY);
    display.print(kMenuLabels[i]);
  }
  display.setTextColor(GxEPD_BLACK);
}

static String trimToWidth(const String& text, int16_t maxChars) {
  if (text.length() <= static_cast<size_t>(maxChars)) {
    return text;
  }
  if (maxChars < 3) {
    return text.substring(0, maxChars);
  }
  return text.substring(0, maxChars - 3) + "...";
}

// Note: wrapping now handled directly in uiDrawReader for raw text buffers

void uiInit(EpdDisplay& display) {
  display.init(115200);
  display.setRotation(Config::DISPLAY_ROTATION);
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(Config::UI_TEXT_SIZE);
  computeLayout(display);
  computeReaderLayout(display);
}

const UiLayout& uiLayout() {
  return layout;
}

const UiLayout& uiReaderLayout() {
  return readerLayout;
}

void uiSetReaderDarkMode(bool on) {
  readerDarkMode = on;
}

bool uiReaderDarkMode() {
  return readerDarkMode;
}

static void prepareReaderTextMetrics(EpdDisplay& display) {
  display.setTextSize(Config::READER_TEXT_SIZE);
  display.setFont(&FreeSerif9pt8b);
  display.setTextWrap(false);
}

static bool isReaderPunctuation(uint8_t c) {
  return c == '\'' || c == '"' || c == '-' || c == '_';
}

static int16_t readerPunctAdvance(uint8_t c) {
  switch (c) {
    case '\'':
      return 3;
    case '"':
      return 7;
    case '-':
      return 6;
    case '_':
      return 8;
    default:
      return 0;
  }
}

static void expandBounds(int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                         int16_t& minx, int16_t& miny, int16_t& maxx, int16_t& maxy) {
  if (x1 < minx) {
    minx = x1;
  }
  if (y1 < miny) {
    miny = y1;
  }
  if (x2 > maxx) {
    maxx = x2;
  }
  if (y2 > maxy) {
    maxy = y2;
  }
}

static int16_t fontGlyphAdvance(uint8_t c) {
  if (c < 0x20 || c > 0xFE) {
    return 0;
  }
  const GFXglyph* glyph = &FreeSerif9ptGlyphs[c - 0x20];
  return static_cast<int16_t>(pgm_read_byte(&glyph->xAdvance)) * Config::READER_TEXT_SIZE;
}

static void getDisplayLineMetrics(EpdDisplay& display, const String& line,
                                  int16_t* x1, int16_t* y1, uint16_t* w, uint16_t* h) {
  int16_t x = 0;
  int16_t minx = 0x7FFF;
  int16_t miny = 0x7FFF;
  int16_t maxx = -1;
  int16_t maxy = -1;

  for (size_t i = 0; i < line.length(); ++i) {
    uint8_t c = static_cast<uint8_t>(line[i]);
    if (isReaderPunctuation(c)) {
      switch (c) {
        case '-':
          expandBounds(x + 1, -3, x + 5, -2, minx, miny, maxx, maxy);
          break;
        case '\'':
          expandBounds(x + 1, -10, x + 2, -7, minx, miny, maxx, maxy);
          break;
        case '"':
          expandBounds(x + 1, -10, x + 5, -7, minx, miny, maxx, maxy);
          break;
        case '_':
          expandBounds(x + 1, 1, x + 6, 2, minx, miny, maxx, maxy);
          break;
        default:
          break;
      }
      x += readerPunctAdvance(c);
    } else {
      char ch[2] = {static_cast<char>(c), '\0'};
      int16_t cx1 = 0;
      int16_t cy1 = 0;
      uint16_t cw = 0;
      uint16_t chh = 0;
      display.getTextBounds(ch, x, 0, &cx1, &cy1, &cw, &chh);
      expandBounds(cx1, cy1, cx1 + static_cast<int16_t>(cw) - 1,
                   cy1 + static_cast<int16_t>(chh) - 1, minx, miny, maxx, maxy);
      x += fontGlyphAdvance(c);
    }
  }

  *x1 = (maxx >= minx) ? minx : 0;
  *y1 = (maxy >= miny) ? miny : 0;
  *w = (maxx >= minx) ? static_cast<uint16_t>(maxx - minx + 1) : 0;
  *h = (maxy >= miny) ? static_cast<uint16_t>(maxy - miny + 1) : 0;
}

static void printDisplayLine(EpdDisplay& display, const String& line, int16_t baselineY) {
  uint16_t fg = readerDarkMode ? GxEPD_WHITE : GxEPD_BLACK;
  int16_t x = display.getCursorX();
  for (size_t i = 0; i < line.length(); ++i) {
    uint8_t c = static_cast<uint8_t>(line[i]);
    if (isReaderPunctuation(c)) {
      switch (c) {
        case '-':
          display.fillRect(x + 1, baselineY - 3, 5, 2, fg);
          x += 6;
          break;
        case '\'':
          display.fillRect(x + 1, baselineY - 10, 2, 4, fg);
          x += 3;
          break;
        case '"':
          display.fillRect(x + 1, baselineY - 10, 2, 4, fg);
          display.fillRect(x + 4, baselineY - 10, 2, 4, fg);
          x += 7;
          break;
        case '_':
          display.fillRect(x + 1, baselineY + 1, 6, 2, fg);
          x += 8;
          break;
        default:
          break;
      }
    } else {
      display.setCursor(x, baselineY);
      display.write(c);
      x = display.getCursorX();
    }
  }
  display.setCursor(x, baselineY);
}

size_t uiMeasureReaderBytes(EpdDisplay& display, const String& text) {
  const UiLayout& r = readerLayout;
  prepareReaderTextMetrics(display);

  size_t bytesRendered = 0;
  const size_t textLen = text.length();
  size_t charPos = 0;
  int16_t visualLineIndex = 0;

  while (charPos < textLen && visualLineIndex < r.maxLines) {
    String lineText = "";

    while (charPos < textLen) {
      while (charPos < textLen && (text[charPos] == ' ' || text[charPos] == '\t' || text[charPos] == '\r')) {
        charPos++;
        bytesRendered = charPos;
      }

      if (charPos >= textLen) {
        break;
      }

      if (text[charPos] == '\n') {
        charPos++;
        bytesRendered = charPos;
        break;
      }

      size_t wordEnd = charPos;
      while (wordEnd < textLen && text[wordEnd] != ' ' && text[wordEnd] != '\t' && text[wordEnd] != '\n' && text[wordEnd] != '\r') {
        wordEnd++;
      }

      String word = text.substring(charPos, wordEnd);
      String candidateLine = lineText.isEmpty() ? word : lineText + " " + word;
      String displayLine = toDisplayText(candidateLine);
      int16_t x1 = 0, y1 = 0;
      uint16_t w = 0, h = 0;
      getDisplayLineMetrics(display, displayLine, &x1, &y1, &w, &h);

      if (static_cast<int16_t>(w) > r.contentW && !lineText.isEmpty()) {
        break;
      }

      lineText = candidateLine;
      charPos = wordEnd;
      bytesRendered = charPos;
    }

    visualLineIndex++;
  }

  display.setTextSize(Config::UI_TEXT_SIZE);
  display.setFont(nullptr);
  display.setTextWrap(true);
  return bytesRendered;
}

void uiDrawReader(EpdDisplay& display, const ReaderView& view, bool partial) {
  const UiLayout& r = readerLayout;
  prepareReaderTextMetrics(display);

  uint16_t bg = readerDarkMode ? GxEPD_BLACK : GxEPD_WHITE;
  uint16_t fg = readerDarkMode ? GxEPD_WHITE : GxEPD_BLACK;
  display.setTextColor(fg);

  int16_t sampleX1 = 0;
  int16_t sampleY1 = 0;
  uint16_t sampleW = 0;
  uint16_t sampleH = 0;
  display.getTextBounds("jAg", 0, 0, &sampleX1, &sampleY1, &sampleW, &sampleH);
  int16_t lineBaselineY = r.contentY + 2 - sampleY1;
  
  if (partial) {
    display.setPartialWindow(r.contentX, r.contentY, r.contentW, r.height - r.contentY);
  } else {
    display.setFullWindow();
  }
  
  display.firstPage();
  size_t bytesRendered = 0;
  const size_t textLen = view.text.length(); // Cache length to save method calls
  
  do {
    if (!partial) {
      display.fillScreen(bg);
    } else {
      display.fillRect(r.contentX, r.contentY, r.contentW, r.height - r.contentY, bg);
    }

    int16_t visualLineIndex = 0;
    bytesRendered = 0;
    size_t charPos = 0;

    // Text Parsing and Rendering
    while (charPos < textLen && visualLineIndex < r.maxLines) {
      String lineText = "";
      int16_t lastLineX1 = 0; // Cached to avoid recalculating text bounds

      while (charPos < textLen) {
        // Skip spaces, tabs, and carriage returns
        while (charPos < textLen && (view.text[charPos] == ' ' || view.text[charPos] == '\t' || view.text[charPos] == '\r')) {
          charPos++;
          bytesRendered = charPos;
        }

        if (charPos >= textLen) break;

        // If explicit newline, consume it, save progress, and move to next visual line
        if (view.text[charPos] == '\n') {
          charPos++;
          bytesRendered = charPos;
          break; 
        }

        // Find the end of the current word
        size_t wordEnd = charPos;
        while (wordEnd < textLen && view.text[wordEnd] != ' ' && view.text[wordEnd] != '\t' && view.text[wordEnd] != '\n' && view.text[wordEnd] != '\r') {
          wordEnd++;
        }

        // Build and measure a test line
        String word = view.text.substring(charPos, wordEnd);
        String candidateLine = lineText.isEmpty() ? word : lineText + " " + word;
        String displayLine = toDisplayText(candidateLine);
        int16_t x1 = 0, y1 = 0;
        uint16_t w = 0, h = 0;
        getDisplayLineMetrics(display, displayLine, &x1, &y1, &w, &h);

        // If the word doesn't fit (and it's not the only word on the line), leave it for the next line
        if (static_cast<int16_t>(w) > r.contentW && !lineText.isEmpty()) {
          break; 
        }

        // Accept the word into the current line
        lineText = candidateLine;
        lastLineX1 = x1;       // Save x1 offset for printing later
        charPos = wordEnd;     // Advance pointer past this word
        bytesRendered = charPos;
      }

      // Render the compiled line (if any text was collected)
      if (!lineText.isEmpty()) {
        int16_t lineY = lineBaselineY + static_cast<int16_t>(visualLineIndex) * r.lineHeight;
        display.setCursor(r.contentX - lastLineX1, lineY);
        printDisplayLine(display, toDisplayText(lineText), lineY);
      }

      // Always advance the line index (handles both printed lines and explicit empty \n lines)
      visualLineIndex++;
    }

    // Render Progress Bar
    int16_t barY = r.height - 1;
    int16_t barW = map(view.progressPercent, 0, 100, 0, r.contentW);
    display.fillRect(r.contentX, barY, r.contentW, 1, bg);
    display.fillRect(r.contentX, barY, barW, 1, fg);

  } while (display.nextPage());

  // Report consumed bytes
  const_cast<ReaderView&>(view).bytesConsumed = bytesRendered;

  display.setTextSize(Config::UI_TEXT_SIZE);
  display.setFont(nullptr);
  display.setTextWrap(true);
  display.setTextColor(GxEPD_BLACK);
  display.powerOff();
}

void uiDrawLibrary(EpdDisplay& display, const std::vector<BookInfo>& books, int selectedIndex, int scrollIndex) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawSidebar(display, 0);

    int16_t y = layout.contentY + layout.lineHeight;
    int16_t maxVisible = layout.maxLines;

    if (books.empty()) {
      display.setCursor(layout.contentX, y);
      display.print("No books yet");
      y += layout.lineHeight;
      display.setCursor(layout.contentX, y);
      display.print("Copy .txt files");
      y += layout.lineHeight;
      display.setCursor(layout.contentX, y);
      display.print("to /books");
    } else {
      for (int i = 0; i < maxVisible; ++i) {
        int bookIndex = scrollIndex + i;
        if (bookIndex >= static_cast<int>(books.size())) {
          break;
        }
        bool active = (bookIndex == selectedIndex);
        display.setCursor(layout.contentX, y + i * layout.lineHeight);
        display.print(active ? "> " : "  ");
        display.print(trimToWidth(books[bookIndex].name, layout.charsPerLine - 2));
      }
    }
  } while (display.nextPage());
  display.powerOff();
}

void uiDrawInfo(EpdDisplay& display, const StorageStats& stats, float battV, int battPercent) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawSidebar(display, 1);

    int16_t y = layout.contentY + layout.lineHeight;
    size_t usedKb = stats.usedBytes / 1024;
    size_t totalKb = stats.totalBytes / 1024;
    int usedPercent = (stats.totalBytes > 0) ? static_cast<int>((stats.usedBytes * 100UL) / stats.totalBytes) : 0;

    display.setCursor(layout.contentX, y);
    display.print("Storage: ");
    display.print(usedKb);
    display.print("/");
    display.print(totalKb);
    display.print(" KB (");
    display.print(usedPercent);
    display.print("%)");
    y += layout.lineHeight;

    display.setCursor(layout.contentX, y);
    if (battV < 0.0f) {
      display.print("Battery: n/a");
    } else {
      display.print("Battery: ");
      display.print(battV, 2);
      display.print("V (");
      display.print(battPercent);
      display.print("%)");
    }

    y += layout.lineHeight * 2;
    display.setCursor(layout.contentX, y);
    display.print("Theme: ");
    display.print(readerDarkMode ? "Dark" : "Light");
    y += layout.lineHeight;

    display.setCursor(layout.contentX, y);
    display.print("OK: toggle theme");
  } while (display.nextPage());
  display.powerOff();
}

void uiDrawError(EpdDisplay& display, const String& title, const String& message, const String& action) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setCursor(layout.contentX, layout.contentY + layout.lineHeight);
    display.print(title);
    display.setCursor(layout.contentX, layout.contentY + layout.lineHeight * 2);
    display.print(message);
    display.setCursor(layout.contentX, layout.contentY + layout.lineHeight * 4);
    display.print(action);
  } while (display.nextPage());
  display.powerOff();
}
