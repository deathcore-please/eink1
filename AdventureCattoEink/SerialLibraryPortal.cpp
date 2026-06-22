#include "SerialLibraryPortal.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <vector>

#include "tiny-reader/src/Config.h"
#include "tiny-reader/src/Storage.h"

namespace {

constexpr size_t MAX_COMMAND_LINE = 1800;
constexpr const char* RESPONSE_PREFIX = "ACAT ";
constexpr uint32_t PORTAL_KEEP_AWAKE_MS = 90UL * 1000UL;

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

void sendError(const String& type, const String& message) {
  Serial.print(RESPONSE_PREFIX);
  Serial.print("{\"ok\":false,\"type\":");
  printJsonString(type);
  Serial.print(",\"message\":");
  printJsonString(message);
  Serial.println('}');
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
  Serial.print("{\"ok\":true,\"type\":\"hello\",\"device\":\"AdventureCattoEink\",\"protocol\":1,");
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
  } else {
    sendError("command", "Unknown command");
  }
}

}  // namespace

void serialLibraryPortalBegin() {
  commandLine.reserve(MAX_COMMAND_LINE);
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
