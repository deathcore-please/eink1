#include "PetStateStore.h"

#include <Preferences.h>
#include <esp_app_desc.h>
#include <esp_rom_crc.h>
#include <string.h>
#include <type_traits>

namespace PetStateStore {
namespace {

constexpr uint32_t StoreVersion = 1;
constexpr const char* StoreNamespace = "pet-life";
constexpr const char* StoreKey = "checkpoint";

struct Record {
  uint32_t version;
  uint8_t firmwareHash[32];
  uint32_t checksum;
  Snapshot snapshot;
};

static_assert(std::is_trivially_copyable<Snapshot>::value, "Pet snapshots must remain plain data");

uint32_t checksum(const Snapshot& snapshot) {
  return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&snapshot), sizeof(snapshot));
}

}  // namespace

bool load(Snapshot& snapshot) {
  Preferences prefs;
  if (!prefs.begin(StoreNamespace, true)) {
    return false;
  }

  Record record = {};
  bool read = prefs.getBytesLength(StoreKey) == sizeof(record) &&
              prefs.getBytes(StoreKey, &record, sizeof(record)) == sizeof(record);
  prefs.end();
  if (!read || record.version != StoreVersion ||
      memcmp(record.firmwareHash, esp_app_get_description()->app_elf_sha256, sizeof(record.firmwareHash)) != 0 ||
      record.checksum != checksum(record.snapshot)) {
    return false;
  }

  snapshot = record.snapshot;
  return true;
}

bool save(const Snapshot& snapshot) {
  Record record = {};
  record.version = StoreVersion;
  memcpy(record.firmwareHash, esp_app_get_description()->app_elf_sha256, sizeof(record.firmwareHash));
  record.snapshot = snapshot;
  record.checksum = checksum(record.snapshot);

  Preferences prefs;
  if (!prefs.begin(StoreNamespace, false)) {
    return false;
  }
  // Keep identity and state in one NVS blob so a failed write cannot mix builds.
  bool saved = prefs.putBytes(StoreKey, &record, sizeof(record)) == sizeof(record);
  prefs.end();
  return saved;
}

}  // namespace PetStateStore
