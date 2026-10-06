#include "SerialLibraryPortal.h"
#include "PetTrace.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_random.h>
#include <vector>

#include "tiny-reader/src/Config.h"
#include "tiny-reader/src/Storage.h"

namespace {

constexpr size_t MAX_COMMAND_LINE = 1800;
constexpr const char* RESPONSE_PREFIX = "ACAT ";
constexpr uint32_t PORTAL_KEEP_AWAKE_MS = 90UL * 1000UL;
constexpr const char* DEVICE_ID_NAMESPACE = "acat-device";
constexpr const char* DEVICE_ID_KEY = "deviceId";
constexpr const char* DIAGNOSTICS_DIR = "/system";
constexpr const char* DEATH_LOG_PATH = "/system/death_log.txt";
constexpr const char* DEATH_LOG_TMP_PATH = "/system/death_log.tmp";
constexpr size_t DEATH_LOG_MAX_BYTES = 64UL * 1024UL;
constexpr size_t DEATH_LOG_TRIM_TO_BYTES = 48UL * 1024UL;
constexpr size_t DIAG_READ_CHUNK_BYTES = 192;

String commandLine;
bool storageReady = false;
bool uploadActive = false;
bool portalConnected = false;
bool portalConnectedEvent = false;
bool portalDisconnectedEvent = false;
uint32_t keepAwakeUntilMs = 0;
File uploadFile;
String uploadPath;
size_t uploadExpected = 0;
size_t uploadReceived = 0;
String cachedDeviceId;

void cancelUpload(bool removePartial);

bool ensureStorage() {
  if (storageReady) {
    return true;
  }

  if (!storageBegin(false)) {
    return false;
  }

  storageReady = storageEnsureDirs();
  return storageReady;
}

bool isValidDeviceId(const String& value) {
  if (!value.startsWith("acat-") || value.length() != 37) {
    return false;
  }

  for (size_t i = 5; i < value.length(); ++i) {
    char c = value[i];
    bool hex = (c >= '0' && c <= '9') ||
               (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    if (!hex) {
      return false;
    }
  }

  return true;
}

String generateDeviceId() {
  char id[38] = {};
  uint32_t parts[4] = {
    esp_random(),
    esp_random(),
    esp_random(),
    esp_random()
  };

  snprintf(
    id,
    sizeof(id),
    "acat-%08lx%08lx%08lx%08lx",
    static_cast<unsigned long>(parts[0]),
    static_cast<unsigned long>(parts[1]),
    static_cast<unsigned long>(parts[2]),
    static_cast<unsigned long>(parts[3])
  );

  return String(id);
}

String getDeviceId() {
  if (isValidDeviceId(cachedDeviceId)) {
    return cachedDeviceId;
  }

  Preferences prefs;
  if (!prefs.begin(DEVICE_ID_NAMESPACE, false)) {
    cachedDeviceId = generateDeviceId();
    return cachedDeviceId;
  }

  cachedDeviceId = prefs.getString(DEVICE_ID_KEY, "");
  if (!isValidDeviceId(cachedDeviceId)) {
    cachedDeviceId = generateDeviceId();
    prefs.putString(DEVICE_ID_KEY, cachedDeviceId);
  }

  prefs.end();
  return cachedDeviceId;
}

void markPortalAwake() {
  keepAwakeUntilMs = millis() + PORTAL_KEEP_AWAKE_MS;
}

bool portalKeepAwakeActive() {
  return keepAwakeUntilMs != 0 && static_cast<int32_t>(keepAwakeUntilMs - millis()) > 0;
}

void markPortalConnected() {
  markPortalAwake();
  if (!portalConnected) {
    portalConnected = true;
    portalConnectedEvent = true;
  }
}

void markPortalDisconnected(bool cancelPartialUpload) {
  if (uploadActive) {
    cancelUpload(cancelPartialUpload);
  }

  keepAwakeUntilMs = 0;
  if (portalConnected) {
    portalConnected = false;
    portalDisconnectedEvent = true;
  }
}

void updatePortalConnectionTimeout() {
  if (portalConnected && !portalKeepAwakeActive()) {
    markPortalDisconnected(true);
  }
}

String lowerCopy(String value) {
  value.toLowerCase();
  return value;
}

String fileNameOnly(String name) {
  name.trim();
  name.replace('\\', '/');

  int slash = name.lastIndexOf('/');
  if (slash >= 0) {
    name = name.substring(slash + 1);
  }

  name.trim();
  return name;
}

bool isTxtName(const String& name) {
  return lowerCopy(name).endsWith(".txt");
}

bool validBookName(const String& name) {
  if (name.length() == 0 || name.length() > 96 || !isTxtName(name)) {
    return false;
  }

  for (size_t i = 0; i < name.length(); ++i) {
    char c = name[i];
    if (c < 32 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      return false;
    }
  }

  return true;
}

String bookPathForName(const String& rawName) {
  String name = fileNameOnly(rawName);
  if (!validBookName(name)) {
    return String();
  }

  return String(Config::BOOKS_DIR) + "/" + name;
}

String metadataBaseForBook(const String& bookPath) {
  String base = bookPath;
  int slash = base.lastIndexOf('/');
  if (slash >= 0) {
    base = base.substring(slash + 1);
  }

  String safe = storageSanitizeFilename(base);
  int dot = safe.lastIndexOf('.');
  if (dot > 0) {
    safe = safe.substring(0, dot);
  }

  return String(Config::PROGRESS_DIR) + "/" + safe;
}

String progressPathForBook(const String& bookPath) {
  return metadataBaseForBook(bookPath) + ".pos";
}

String navigationPathForBook(const String& bookPath) {
  return metadataBaseForBook(bookPath) + ".nav";
}

void printJsonString(const String& value) {
  Serial.print('"');
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    if (c == '"' || c == '\\') {
      Serial.print('\\');
      Serial.print(c);
    } else if (c == '\n') {
      Serial.print("\\n");
    } else if (c == '\r') {
      Serial.print("\\r");
    } else if (c == '\t') {
      Serial.print("\\t");
    } else if (c >= 32) {
      Serial.print(c);
    }
  }
  Serial.print('"');
}

void appendJsonStringTo(String& out, const char* value) {
  out += '"';
  if (value == nullptr) {
    out += '"';
    return;
  }

  for (size_t i = 0; value[i] != '\0'; ++i) {
    char c = value[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c >= 32) {
      out += c;
    }
  }

  out += '"';
}

void printStorageJson() {
  StorageStats stats = storageGetStats();
  size_t freeBytes = stats.totalBytes > stats.usedBytes ? stats.totalBytes - stats.usedBytes : 0;

  Serial.print("\"storage\":{\"total\":");
  Serial.print(stats.totalBytes);
  Serial.print(",\"used\":");
  Serial.print(stats.usedBytes);
  Serial.print(",\"free\":");
  Serial.print(freeBytes);
  Serial.print('}');
}

bool ensureDiagnosticsDir() {
  if (!ensureStorage()) {
    return false;
  }

  if (LittleFS.exists(DIAGNOSTICS_DIR)) {
    return true;
  }

  return LittleFS.mkdir(DIAGNOSTICS_DIR);
}

size_t countDeathLogEntries() {
  if (!LittleFS.exists(DEATH_LOG_PATH)) {
    return 0;
  }

  File file = LittleFS.open(DEATH_LOG_PATH, "r");
  if (!file) {
    return 0;
  }

  size_t count = 0;
  bool sawContent = false;
  bool endedWithNewline = true;
  while (file.available()) {
    char c = static_cast<char>(file.read());
    sawContent = true;
    endedWithNewline = (c == '\n');
    if (c == '\n') {
      count++;
    }
  }
  file.close();

  if (sawContent && !endedWithNewline) {
    count++;
  }

  return count;
}

size_t deathLogSize() {
  if (!LittleFS.exists(DEATH_LOG_PATH)) {
    return 0;
  }

  File file = LittleFS.open(DEATH_LOG_PATH, "r");
  if (!file) {
    return 0;
  }

  size_t size = file.size();
  file.close();
  return size;
}

bool trimDeathLogForAppend(size_t incomingBytes) {
  if (incomingBytes >= DEATH_LOG_MAX_BYTES) {
    LittleFS.remove(DEATH_LOG_PATH);
    LittleFS.remove(DEATH_LOG_TMP_PATH);
    return true;
  }

  if (!LittleFS.exists(DEATH_LOG_PATH)) {
    return true;
  }

  File file = LittleFS.open(DEATH_LOG_PATH, "r");
  if (!file) {
    return false;
  }

  size_t size = file.size();
  if (size + incomingBytes <= DEATH_LOG_MAX_BYTES) {
    file.close();
    return true;
  }

  size_t keepBytes = min(DEATH_LOG_TRIM_TO_BYTES, size);
  size_t start = size - keepBytes;
  if (!file.seek(start)) {
    file.close();
    return false;
  }

  String tail;
  tail.reserve(keepBytes);
  while (file.available()) {
    tail += static_cast<char>(file.read());
  }
  file.close();

  if (start > 0) {
    int newlineAt = tail.indexOf('\n');
    if (newlineAt >= 0) {
      tail.remove(0, newlineAt + 1);
    }
  }

  LittleFS.remove(DEATH_LOG_TMP_PATH);
  File trimmed = LittleFS.open(DEATH_LOG_TMP_PATH, "w");
  if (!trimmed) {
    return false;
  }
  trimmed.print(tail);
  trimmed.close();

  LittleFS.remove(DEATH_LOG_PATH);
  return LittleFS.rename(DEATH_LOG_TMP_PATH, DEATH_LOG_PATH);
}

bool appendDeathLogLine(const String& line) {
  if (!ensureDiagnosticsDir()) {
    return false;
  }

  String entry = line;
  entry += '\n';
  if (!trimDeathLogForAppend(entry.length())) {
    return false;
  }

  File file = LittleFS.open(DEATH_LOG_PATH, "a");
  if (!file) {
    return false;
  }

  size_t written = file.print(entry);
  file.close();
  return written == entry.length();
}

void sendError(const String& type, const String& message) {
  Serial.print(RESPONSE_PREFIX);
  Serial.print("{\"ok\":false,\"type\":");
  printJsonString(type);
  Serial.print(",\"message\":");
  printJsonString(message);
  Serial.println('}');
}

void sendDeathLogInfo() {
  if (!ensureDiagnosticsDir()) {
    sendError("diag-deathlog-info", "LittleFS is not mounted");
    return;
  }

  Serial.print(RESPONSE_PREFIX);
  Serial.print("{\"ok\":true,\"type\":\"diag-deathlog-info\",\"bytes\":");
  Serial.print(deathLogSize());
  Serial.print(",\"entries\":");
  Serial.print(countDeathLogEntries());
  Serial.print(',');
  printStorageJson();
  Serial.println('}');
}

void printBase64Encoded(const uint8_t* data, size_t length) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  for (size_t i = 0; i < length; i += 3) {
    uint32_t value = static_cast<uint32_t>(data[i]) << 16;
    bool hasSecond = (i + 1) < length;
    bool hasThird = (i + 2) < length;

    if (hasSecond) {
      value |= static_cast<uint32_t>(data[i + 1]) << 8;
    }
    if (hasThird) {
      value |= data[i + 2];
    }

    Serial.print(alphabet[(value >> 18) & 0x3F]);
    Serial.print(alphabet[(value >> 12) & 0x3F]);
    Serial.print(hasSecond ? alphabet[(value >> 6) & 0x3F] : '=');
    Serial.print(hasThird ? alphabet[value & 0x3F] : '=');
  }
}

void handleDiagDeathLogRead(const String& offsetText) {
  if (!ensureDiagnosticsDir()) {
    sendError("diag-deathlog-read", "LittleFS is not mounted");
    return;
  }

  uint32_t offset = offsetText.length() > 0
                    ? static_cast<uint32_t>(strtoul(offsetText.c_str(), nullptr, 10))
                    : 0;
  size_t totalBytes = deathLogSize();
  uint8_t buffer[DIAG_READ_CHUNK_BYTES] = {};
  size_t bytesRead = 0;

  if (offset < totalBytes && LittleFS.exists(DEATH_LOG_PATH)) {
    File file = LittleFS.open(DEATH_LOG_PATH, "r");
    if (!file) {
      sendError("diag-deathlog-read", "Death log could not be opened");
      return;
    }

    if (!file.seek(offset)) {
      file.close();
      sendError("diag-deathlog-read", "Death log offset is invalid");
      return;
    }

    size_t bytesToRead = DIAG_READ_CHUNK_BYTES;
    size_t remaining = totalBytes - offset;
    if (remaining < bytesToRead) {
      bytesToRead = remaining;
    }
    bytesRead = file.read(buffer, bytesToRead);
    file.close();
  } else if (offset > totalBytes) {
    offset = totalBytes;
  }

  size_t nextOffset = offset + bytesRead;
  bool done = nextOffset >= totalBytes;

  Serial.print(RESPONSE_PREFIX);
  Serial.print("{\"ok\":true,\"type\":\"diag-deathlog-read\",\"offset\":");
  Serial.print(offset);
  Serial.print(",\"nextOffset\":");
  Serial.print(nextOffset);
  Serial.print(",\"bytes\":");
  Serial.print(totalBytes);
  Serial.print(",\"done\":");
  Serial.print(done ? "true" : "false");
  Serial.print(",\"chunk\":\"");
  printBase64Encoded(buffer, bytesRead);
  Serial.println("\"}");
}

void handleDiagDeathLogClear() {
  if (!ensureDiagnosticsDir()) {
    sendError("diag-deathlog-clear", "LittleFS is not mounted");
    return;
  }

  LittleFS.remove(DEATH_LOG_PATH);
  LittleFS.remove(DEATH_LOG_TMP_PATH);

  Serial.print(RESPONSE_PREFIX);
  Serial.print("{\"ok\":true,\"type\":\"diag-deathlog-clear\",\"bytes\":0,\"entries\":0,");
  printStorageJson();
  Serial.println('}');
}

void handleDiag(const String& args) {
  if (uploadActive) {
    sendError("diag", "Upload is active");
    return;
  }

  int split = args.indexOf(' ');
  String topic = split >= 0 ? args.substring(0, split) : args;
  String rest = split >= 0 ? args.substring(split + 1) : "";
  topic.toUpperCase();
  rest.trim();

  if (topic != "DEATHLOG" && topic != "CARETRACE") {
    sendError("diag", "Unknown diagnostics target");
    return;
  }

  int actionSplit = rest.indexOf(' ');
  String action = actionSplit >= 0 ? rest.substring(0, actionSplit) : rest;
  String actionArgs = actionSplit >= 0 ? rest.substring(actionSplit + 1) : "";
  action.toUpperCase();
  actionArgs.trim();

  if (topic == "CARETRACE") {
    String type = "diag-caretrace-";
    String lowerAction = action;
    lowerAction.toLowerCase();
    type += lowerAction;
    size_t bytes = 0, entries = 0;
    if (action == "INFO") {
      if (!PetTrace::info(bytes, entries)) { sendError(type, "Care trace could not be opened"); return; }
    } else if (action == "READ") {
      uint32_t offset = static_cast<uint32_t>(strtoul(actionArgs.c_str(), nullptr, 10));
      uint8_t buffer[768];
      size_t count = 0;
      if (!PetTrace::read(offset, buffer, sizeof(buffer), count, bytes)) {
        sendError(type, "Care trace read failed; restart the download"); return;
      }
      Serial.print(RESPONSE_PREFIX);
      Serial.print("{\"ok\":true,\"type\":\"diag-caretrace-read\",\"offset\":");
      Serial.print(offset);
      Serial.print(",\"nextOffset\":"); Serial.print(offset + count);
      Serial.print(",\"bytes\":"); Serial.print(bytes);
      Serial.print(",\"done\":"); Serial.print(offset + count >= bytes ? "true" : "false");
      Serial.print(",\"chunk\":\""); printBase64Encoded(buffer, count);
      Serial.println("\"}");
      return;
    } else if (action == "CLEAR") {
      if (!PetTrace::clear()) { sendError(type, "Care trace could not be cleared"); return; }
    } else {
      sendError("diag", "Unknown care trace command"); return;
    }
    Serial.print(RESPONSE_PREFIX);
    Serial.print("{\"ok\":true,\"type\":"); printJsonString(type);
    Serial.print(",\"bytes\":"); Serial.print(bytes);
    Serial.print(",\"entries\":"); Serial.print(entries);
    Serial.print(','); printStorageJson(); Serial.println('}');
    return;
  }

  if (action == "INFO") {
    sendDeathLogInfo();
  } else if (action == "READ") {
    handleDiagDeathLogRead(actionArgs);
  } else if (action == "CLEAR") {
    handleDiagDeathLogClear();
  } else {
    sendError("diag", "Unknown death log command");
  }
}

void sendSimpleOk(const String& type) {
  Serial.print(RESPONSE_PREFIX);
  Serial.print("{\"ok\":true,\"type\":");
  printJsonString(type);
  Serial.print(',');
  printStorageJson();
  Serial.println('}');
}

void sendList() {
  if (!ensureStorage()) {
    sendError("list", "LittleFS is not mounted");
    return;
  }

  std::vector<BookInfo> books = storageListBooks();

  Serial.print(RESPONSE_PREFIX);
  Serial.print("{\"ok\":true,\"type\":\"list\",");
  printStorageJson();
  Serial.print(",\"books\":[");

  for (size_t i = 0; i < books.size(); ++i) {
    if (i > 0) {
      Serial.print(',');
    }

    Serial.print("{\"name\":");
    printJsonString(books[i].name);
    Serial.print(",\"path\":");
    printJsonString(books[i].path);
    Serial.print(",\"size\":");
    Serial.print(books[i].size);
    Serial.print('}');
  }

  Serial.println("]}");
}

int8_t base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

struct DecodeContext {
  String* text = nullptr;
  File* file = nullptr;
  size_t written = 0;
};

bool writeDecodedByte(uint8_t value, DecodeContext& context) {
  if (context.text != nullptr) {
    *context.text += static_cast<char>(value);
    context.written++;
    return true;
  }

  if (context.file != nullptr) {
    if (context.file->write(&value, 1) != 1) {
      return false;
    }
    context.written++;
    return true;
  }

  return false;
}

bool decodeBase64(const String& encoded, DecodeContext& context) {
  int value = 0;
  int valueBits = -8;
  bool padded = false;

  for (size_t i = 0; i < encoded.length(); ++i) {
    char c = encoded[i];
    if (c == '=') {
      padded = true;
      continue;
    }

    int8_t decoded = base64Value(c);
    if (decoded < 0 || padded) {
      return false;
    }

    value = (value << 6) | decoded;
    valueBits += 6;

    if (valueBits >= 0) {
      uint8_t out = (value >> valueBits) & 0xFF;
      if (!writeDecodedByte(out, context)) {
        return false;
      }
      valueBits -= 8;
    }
  }

  return true;
}

bool decodeBase64Text(const String& encoded, String& out) {
  out = "";
  out.reserve((encoded.length() * 3) / 4);

  DecodeContext context;
  context.text = &out;
  return decodeBase64(encoded, context);
}

void cancelUpload(bool removePartial) {
  if (uploadFile) {
    uploadFile.close();
  }

  if (removePartial && uploadPath.length() > 0) {
    LittleFS.remove(uploadPath);
  }

  uploadActive = false;
  uploadPath = "";
  uploadExpected = 0;
  uploadReceived = 0;
}

bool hasUploadSpace(const String& path, size_t incomingSize) {
  StorageStats stats = storageGetStats();
  size_t freeBytes = stats.totalBytes > stats.usedBytes ? stats.totalBytes - stats.usedBytes : 0;
  size_t replaceBytes = 0;

  if (LittleFS.exists(path)) {
    File existing = LittleFS.open(path, "r");
    if (existing) {
      replaceBytes = existing.size();
      existing.close();
    }
  }

  return incomingSize <= freeBytes + replaceBytes;
}

void handleHello() {
  if (!ensureStorage()) {
    sendError("hello", "LittleFS is not mounted");
    return;
  }

  Serial.print(RESPONSE_PREFIX);
  Serial.print("{\"ok\":true,\"type\":\"hello\",\"device\":\"AdventureCattoEink\",\"protocol\":1,\"deviceId\":");
  printJsonString(getDeviceId());
  Serial.print(',');
  printStorageJson();
  Serial.println('}');
}

void handlePing() {
  Serial.print(RESPONSE_PREFIX);
  Serial.println("{\"ok\":true,\"type\":\"ping\"}");
}

void handleBye() {
  Serial.print(RESPONSE_PREFIX);
  Serial.println("{\"ok\":true,\"type\":\"bye\"}");
  markPortalDisconnected(false);
}

void handleDelete(const String& encodedName) {
  if (!ensureStorage()) {
    sendError("delete", "LittleFS is not mounted");
    return;
  }

  if (uploadActive) {
    sendError("delete", "Upload is active");
    return;
  }

  String name;
  if (!decodeBase64Text(encodedName, name)) {
    sendError("delete", "Invalid filename encoding");
    return;
  }

  String path = bookPathForName(name);
  if (path.length() == 0) {
    sendError("delete", "Invalid .txt filename");
    return;
  }

  if (!LittleFS.exists(path)) {
    sendError("delete", "Book not found");
    return;
  }

  if (!LittleFS.remove(path)) {
    sendError("delete", "Delete failed");
    return;
  }

  LittleFS.remove(progressPathForBook(path));
  LittleFS.remove(navigationPathForBook(path));

  if (storageGetCurrentBook() == path) {
    LittleFS.remove(Config::CURRENT_BOOK_FILE);
  }

  sendList();
}

void handleBegin(const String& args) {
  if (!ensureStorage()) {
    sendError("begin", "LittleFS is not mounted");
    return;
  }

  if (uploadActive) {
    sendError("begin", "Upload is already active");
    return;
  }

  int split = args.indexOf(' ');
  if (split <= 0) {
    sendError("begin", "Missing upload size");
    return;
  }

  String encodedName = args.substring(0, split);
  String sizeText = args.substring(split + 1);
  size_t expectedSize = static_cast<size_t>(strtoul(sizeText.c_str(), nullptr, 10));

  String name;
  if (!decodeBase64Text(encodedName, name)) {
    sendError("begin", "Invalid filename encoding");
    return;
  }

  String path = bookPathForName(name);
  if (path.length() == 0) {
    sendError("begin", "Only .txt files are allowed");
    return;
  }

  if (expectedSize == 0) {
    sendError("begin", "Empty files are not supported");
    return;
  }

  if (!hasUploadSpace(path, expectedSize)) {
    sendError("begin", "Not enough device storage");
    return;
  }

  if (LittleFS.exists(path)) {
    LittleFS.remove(path);
    LittleFS.remove(progressPathForBook(path));
    LittleFS.remove(navigationPathForBook(path));
  }

  uploadFile = LittleFS.open(path, "w");
  if (!uploadFile) {
    sendError("begin", "Could not open destination file");
    return;
  }

  uploadActive = true;
  uploadPath = path;
  uploadExpected = expectedSize;
  uploadReceived = 0;

  Serial.print(RESPONSE_PREFIX);
  Serial.print("{\"ok\":true,\"type\":\"begin\",\"path\":");
  printJsonString(uploadPath);
  Serial.print(",\"expected\":");
  Serial.print(uploadExpected);
  Serial.print(',');
  printStorageJson();
  Serial.println('}');
}

void handleData(const String& encodedChunk) {
  if (!uploadActive || !uploadFile) {
    sendError("data", "No upload is active");
    return;
  }

  DecodeContext context;
  context.file = &uploadFile;
  if (!decodeBase64(encodedChunk, context)) {
    cancelUpload(true);
    sendError("data", "Invalid upload chunk");
    return;
  }

  if (uploadReceived + context.written > uploadExpected) {
    cancelUpload(true);
    sendError("data", "Upload exceeded expected size");
    return;
  }

  uploadReceived += context.written;
  uploadFile.flush();

  Serial.print(RESPONSE_PREFIX);
  Serial.print("{\"ok\":true,\"type\":\"data\",\"received\":");
  Serial.print(uploadReceived);
  Serial.print(",\"expected\":");
  Serial.print(uploadExpected);
  Serial.println('}');
}

void handleEnd() {
  if (!uploadActive || !uploadFile) {
    sendError("upload", "No upload is active");
    return;
  }

  if (uploadReceived != uploadExpected) {
    cancelUpload(true);
    sendError("upload", "Upload size did not match");
    return;
  }

  cancelUpload(false);
  sendList();
}

void handleCommand(String line) {
  line.trim();
  if (!line.startsWith("ACAT ")) {
    return;
  }

  String payload = line.substring(5);
  payload.trim();
  int split = payload.indexOf(' ');
  String command = split >= 0 ? payload.substring(0, split) : payload;
  String args = split >= 0 ? payload.substring(split + 1) : "";
  command.toUpperCase();
  args.trim();

  if (command != "BYE") {
    markPortalConnected();
  }

  if (command == "HELLO") {
    handleHello();
  } else if (command == "PING") {
    handlePing();
  } else if (command == "BYE") {
    handleBye();
  } else if (command == "LIST") {
    sendList();
  } else if (command == "DELETE") {
    handleDelete(args);
  } else if (command == "BEGIN") {
    handleBegin(args);
  } else if (command == "DATA") {
    handleData(args);
  } else if (command == "END") {
    handleEnd();
  } else if (command == "CANCEL") {
    cancelUpload(true);
    sendSimpleOk("cancel");
  } else if (command == "DIAG") {
    handleDiag(args);
  } else {
    sendError("command", "Unknown command");
  }
}

}  // namespace

void serialLibraryPortalBegin() {
  commandLine.reserve(MAX_COMMAND_LINE);
}

void serialLibraryPortalAppendDeathLog(
  const char* reason,
  uint32_t epochSeconds,
  uint32_t millisValue,
  int wakeCause,
  int appMode,
  int petMode,
  int activeAction,
  bool pendingRunaway,
  uint8_t happiness,
  uint8_t pee,
  uint8_t food,
  uint8_t play,
  uint8_t pets,
  uint32_t lastNeedDrainEpoch,
  uint32_t peeDrainCarry,
  uint32_t foodDrainCarry,
  uint32_t playDrainCarry,
  uint32_t petsDrainCarry,
  uint32_t happinessCrisisCarry,
  uint8_t happinessCrisisPenalty,
  bool sleeping,
  uint32_t sleepStartEpoch,
  uint32_t sleepDurationSec,
  const PetCare::State& care
) {
  String line;
  line.reserve(1800);
  line += "{\"event\":\"runaway\",\"reason\":";
  appendJsonStringTo(line, reason);
  line += ",\"epoch\":";
  line += String(epochSeconds);
  line += ",\"millis\":";
  line += String(millisValue);
  line += ",\"wakeCause\":";
  line += String(wakeCause);
  line += ",\"appMode\":";
  line += String(appMode);
  line += ",\"petMode\":";
  line += String(petMode);
  line += ",\"activeAction\":";
  line += String(activeAction);
  line += ",\"happiness\":";
  line += String(happiness);
  line += ",\"pee\":";
  line += String(pee);
  line += ",\"food\":";
  line += String(food);
  line += ",\"play\":";
  line += String(play);
  line += ",\"pets\":";
  line += String(pets);
  line += ",\"pendingRunaway\":";
  line += pendingRunaway ? "true" : "false";
  line += ",\"lastNeedDrainEpoch\":";
  line += String(lastNeedDrainEpoch);
  line += ",\"peeDrainCarry\":";
  line += String(peeDrainCarry);
  line += ",\"foodDrainCarry\":";
  line += String(foodDrainCarry);
  line += ",\"playDrainCarry\":";
  line += String(playDrainCarry);
  line += ",\"petsDrainCarry\":";
  line += String(petsDrainCarry);
  line += ",\"happinessCrisisCarry\":";
  line += String(happinessCrisisCarry);
  line += ",\"happinessCrisisPenalty\":";
  line += String(happinessCrisisPenalty);
  line += ",\"sleeping\":";
  line += sleeping ? "true" : "false";
  line += ",\"sleepStartEpoch\":";
  line += String(sleepStartEpoch);
  line += ",\"sleepDurationSec\":";
  line += String(sleepDurationSec);
  line += ",\"drainCarryScale\":10000,\"crisisCarryScale\":100,\"care\":{";
  const char* categoryNames[PetCare::Count] = {"overall", "pee", "play", "food"};
  const char* modeNames[] = {"normal", "happy", "sad"};
  for (uint8_t i = 0; i < PetCare::Count; ++i) {
    const PetCare::CategoryState& category = care.categories[i];
    if (i) line += ',';
    appendJsonStringTo(line, categoryNames[i]);
    line += ":{\"applied\":";
    appendJsonStringTo(line, modeNames[category.applied <= PetCare::Sad ? category.applied : PetCare::Normal]);
    line += ",\"qualified\":";
    appendJsonStringTo(line, modeNames[category.qualified <= PetCare::Sad ? category.qualified : PetCare::Normal]);
    line += ",\"ratePercent\":";
    line += String(PetCare::ratePercent(care, static_cast<PetCare::Category>(i)));
    line += ",\"happySince\":";
    line += String(category.high ? category.happySince : 0);
    line += ",\"recoverySince\":";
    line += String(category.recovering ? category.recoverySince : 0);
    line += ",\"dips\":[";
    for (uint8_t n = 0; n < category.dipCount; ++n) {
      if (n) line += ',';
      line += String(category.dips[n]);
    }
    line += "]}";
  }
  line += "},\"needRatesBps\":{";
  const char* needNames[] = {"pee", "food", "play", "pets"};
  const PetCare::Category individualCategories[] = {PetCare::Pee, PetCare::Food, PetCare::Play, PetCare::Count};
  uint32_t overallRate = PetCare::ratePercent(care, PetCare::Overall);
  for (uint8_t i = 0; i < 4; ++i) {
    if (i) line += ',';
    appendJsonStringTo(line, needNames[i]);
    line += ':';
    line += String(overallRate * PetCare::ratePercent(care, individualCategories[i]));
  }
  line += "}";
  line += "}";

  if (!appendDeathLogLine(line)) {
    Serial.println("Death diagnostic log append failed");
  }
}

bool serialLibraryPortalLoop() {
  bool sawInput = false;
  updatePortalConnectionTimeout();

  while (Serial.available() > 0) {
    sawInput = true;
    char c = static_cast<char>(Serial.read());

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      handleCommand(commandLine);
      commandLine = "";
      continue;
    }

    if (commandLine.length() >= MAX_COMMAND_LINE) {
      commandLine = "";
      sendError("command", "Command line too long");
      continue;
    }

    commandLine += c;
  }

  updatePortalConnectionTimeout();
  return sawInput;
}

bool serialLibraryPortalIsBusy() {
  return uploadActive || portalConnected || portalKeepAwakeActive();
}

bool serialLibraryPortalIsConnected() {
  updatePortalConnectionTimeout();
  return portalConnected;
}

bool serialLibraryPortalConsumeConnectedEvent() {
  bool event = portalConnectedEvent;
  portalConnectedEvent = false;
  return event;
}

bool serialLibraryPortalConsumeDisconnectedEvent() {
  bool event = portalDisconnectedEvent;
  portalDisconnectedEvent = false;
  return event;
}
