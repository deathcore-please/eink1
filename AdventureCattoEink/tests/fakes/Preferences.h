#pragma once

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace FakePetStore {
inline std::map<std::string, std::vector<unsigned char>> blobs;
inline bool unavailable = false;
inline bool failWrites = false;
inline bool shortReads = false;
inline unsigned reads = 0;
inline unsigned writes = 0;
inline void reset() {
  blobs.clear();
  unavailable = failWrites = shortReads = false;
  reads = writes = 0;
}
}

class Preferences {
 public:
  bool begin(const char* name, bool readOnly) {
    prefix_ = std::string(name) + '/';
    readOnly_ = readOnly;
    return !FakePetStore::unavailable;
  }
  void end() {}
  size_t getBytesLength(const char* key) {
    auto found = FakePetStore::blobs.find(prefix_ + key);
    return found == FakePetStore::blobs.end() ? 0 : found->second.size();
  }
  size_t getBytes(const char* key, void* output, size_t size) {
    ++FakePetStore::reads;
    auto found = FakePetStore::blobs.find(prefix_ + key);
    if (found == FakePetStore::blobs.end() || found->second.size() != size) return 0;
    if (FakePetStore::shortReads && size) --size;
    std::memcpy(output, found->second.data(), size);
    return size;
  }
  size_t putBytes(const char* key, const void* data, size_t size) {
    if (readOnly_ || FakePetStore::failWrites) return 0;
    ++FakePetStore::writes;
    auto* bytes = static_cast<const unsigned char*>(data);
    FakePetStore::blobs[prefix_ + key] = {bytes, bytes + size};
    return size;
  }

 private:
  std::string prefix_;
  bool readOnly_ = false;
};
