#pragma once
#include <Arduino.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>

namespace FakeTraceFS {
inline std::map<std::string, std::shared_ptr<std::string>> files;
inline std::set<std::string> directories;
inline bool unavailable, failWrites;
inline size_t capacity = 1024 * 1024;
inline void reset() {
  files.clear(); directories.clear(); unavailable = failWrites = false;
  capacity = 1024 * 1024; fakeTraceMillis = 0;
}
}

class File {
  std::shared_ptr<std::string> data;
  size_t position = 0;
 public:
  File() = default;
  File(std::shared_ptr<std::string> content, size_t offset) : data(content), position(offset) {}
  explicit operator bool() const { return static_cast<bool>(data); }
  size_t size() const { return data ? data->size() : 0; }
  bool available() const { return position < size(); }
  bool seek(size_t offset) { position = offset; return offset <= size(); }
  int read() { return available() ? static_cast<unsigned char>((*data)[position++]) : -1; }
  size_t read(uint8_t* buffer, size_t count) {
    if (!data) return 0;
    count = std::min(count, size() - position);
    std::memcpy(buffer, data->data() + position, count); position += count;
    return count;
  }
  size_t print(const String& text) {
    if (!data || FakeTraceFS::failWrites) return 0;
    data->append(text); position = data->size(); return text.size();
  }
  void close() { data.reset(); }
};

class FakeLittleFS {
 public:
  bool begin(bool) { return !FakeTraceFS::unavailable; }
  bool exists(const char* path) { return FakeTraceFS::files.count(path) || FakeTraceFS::directories.count(path); }
  bool mkdir(const char* path) { FakeTraceFS::directories.insert(path); return true; }
  File open(const char* path, const char* mode) {
    auto it = FakeTraceFS::files.find(path);
    if (*mode == 'r') return it == FakeTraceFS::files.end() ? File() : File(it->second, 0);
    auto& content = FakeTraceFS::files[path];
    if (!content) content = std::make_shared<std::string>();
    if (*mode == 'w') content->clear();
    return File(content, content->size());
  }
  bool remove(const char* path) { return FakeTraceFS::files.erase(path) != 0; }
  bool rename(const char* from, const char* to) {
    auto it = FakeTraceFS::files.find(from);
    if (it == FakeTraceFS::files.end() || exists(to)) return false;
    FakeTraceFS::files[to] = it->second; FakeTraceFS::files.erase(it); return true;
  }
  size_t totalBytes() { return FakeTraceFS::capacity; }
  size_t usedBytes() {
    size_t bytes = 0;
    for (auto& file : FakeTraceFS::files) bytes += file.second->size();
    return bytes;
  }
} inline LittleFS;
