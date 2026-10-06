#include "PetTrace.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <stdio.h>

namespace PetTrace {
namespace {

constexpr const char* Directory = "/system";
constexpr const char* Current = "/system/care_trace.jsonl";
constexpr const char* Previous = "/system/care_trace.previous.jsonl";
constexpr size_t RotateAtBytes = 48UL * 1024UL;
uint32_t bootId, sequence, dropped;
Frame lastRecorded;
bool haveLast, mounted, reading, backwardReported;
size_t readPreviousBytes, readCurrentBytes;
uint32_t lastReadMs;

bool ready() {
  if (!mounted) mounted = LittleFS.begin(false);
  return mounted && (LittleFS.exists(Directory) || LittleFS.mkdir(Directory));
}

size_t fileSize(const char* path) {
  File file = LittleFS.open(path, "r");
  return file ? file.size() : 0;
}

void number(String& line, const char* key, int64_t value) {
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "\"%s\":%lld", key, static_cast<long long>(value));
  line += buffer;
}

void text(String& line, const char* value) {
  line += '"';
  for (; *value; ++value) {
    if (*value == '"' || *value == '\\') line += '\\';
    if (static_cast<unsigned char>(*value) >= 32) line += *value;
  }
  line += '"';
}

void frameJson(String& line, const Frame& frame) {
  line += '{';
  number(line, "epoch", frame.epoch);
  line += ','; number(line, "millis", frame.millis);
  line += ','; number(line, "uptimeUs", static_cast<int64_t>(frame.uptimeUs));
  line += ','; number(line, "lastNeedDrainEpoch", frame.lastDrainEpoch);
  line += ','; number(line, "rtcEpoch", frame.rtcEpoch);
  line += ','; number(line, "rtcDrainEpoch", frame.rtcDrainEpoch);
  line += ','; number(line, "petBirthEpoch", frame.birthEpoch);
  line += ','; number(line, "sleepStartEpoch", frame.sleepStartEpoch);
  line += ','; number(line, "sleepDurationSec", frame.sleepDurationSec);
  line += ','; number(line, "wakeCause", frame.wakeCause);
  line += ','; number(line, "resetReason", frame.resetReason);
  line += ','; number(line, "appMode", frame.appMode);
  line += ','; number(line, "petMode", frame.petMode);
  line += ','; number(line, "activeAction", frame.activeAction);
  line += ','; number(line, "introSeen", frame.introSeen);
  line += ','; number(line, "pendingRunaway", frame.pendingRunaway);
  line += ','; number(line, "usbActive", frame.usbActive);
  line += ','; number(line, "trackingStarted", frame.needs.started);
  line += ','; number(line, "happiness", frame.needs.happiness);
  uint8_t weighted = PetNeeds::weightedHappiness(frame.needs);
  uint8_t computed = weighted > frame.needs.crisisPenalty ? weighted - frame.needs.crisisPenalty : 0;
  line += ','; number(line, "computedHappiness", computed);
  line += ','; number(line, "happinessMatchesNeeds", frame.needs.happiness == computed);
  line += ','; number(line, "happinessCrisisPenalty", frame.needs.crisisPenalty);
  line += ','; number(line, "happinessCrisisCarry", frame.needs.crisisCarry);
  line += ",\"needs\":{";
  const char* names[] = {"pee", "food", "play", "pets"};
  const PetCare::Category categories[] = {PetCare::Pee, PetCare::Food, PetCare::Play, PetCare::Count};
  for (uint8_t i = 0; i < PetNeeds::Count; ++i) {
    if (i) line += ',';
    text(line, names[i]); line += ":{";
    number(line, "value", frame.needs.needs[i].value);
    line += ','; number(line, "carry", frame.needs.needs[i].drainCarry);
    line += ','; number(line, "cooldownUntil", frame.needs.needs[i].cooldownUntilEpoch);
    line += ','; number(line, "rateBps", PetCare::ratePercent(frame.needs.care, PetCare::Overall) *
                                     PetCare::ratePercent(frame.needs.care, categories[i]));
    line += '}';
  }
  line += "},\"care\":[";
  for (uint8_t i = 0; i < PetCare::Count; ++i) {
    if (i) line += ',';
    const auto& category = frame.needs.care.categories[i];
    line += '{'; number(line, "applied", category.applied);
    line += ','; number(line, "qualified", category.qualified);
    line += ','; number(line, "high", category.high);
    line += ','; number(line, "happySince", category.happySince);
    line += ','; number(line, "recovering", category.recovering);
    line += ','; number(line, "recoverySince", category.recoverySince);
    line += ",\"dips\":[";
    // A corrupt count must not let the logger itself read beyond the retained state.
    size_t count = category.dipCount;
    if (count > sizeof(category.dips) / sizeof(category.dips[0])) count = sizeof(category.dips) / sizeof(category.dips[0]);
    for (size_t n = 0; n < count; ++n) {
      if (n) line += ',';
      char value[16]; snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(category.dips[n]));
      line += value;
    }
    line += "]}";
  }
  line += "]}";
}

bool clockMismatch(const Frame& current) {
  if (!haveLast || current.uptimeUs < lastRecorded.uptimeUs) return false;
  int64_t wall = static_cast<int64_t>(current.epoch) - lastRecorded.epoch;
  int64_t uptime = static_cast<int64_t>((current.uptimeUs - lastRecorded.uptimeUs) / 1000000ULL);
  return wall - uptime > 5 || wall - uptime < -5;
}

bool append(const String& line) {
  if (!ready() || line.length() > BankBytes) return false;
  if (reading && static_cast<uint32_t>(millis() - lastReadMs) > 30000) reading = false;
  size_t incomingSize = fileSize(Current) + line.length();
  // Leave room for an incident during a download without moving its byte offsets.
  if (reading && incomingSize > BankBytes) return false;
  if (!reading && incomingSize > RotateAtBytes) {
    if (LittleFS.exists(Previous) && !LittleFS.remove(Previous)) return false;
    if (LittleFS.exists(Current) && !LittleFS.rename(Current, Previous)) return false;
  }
  size_t free = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (free < line.length() + 4096) return false;
  File file = LittleFS.open(Current, "a");
  if (!file) return false;
  bool saved = file.print(line) == line.length();
  file.close();
  return saved;
}

}  // namespace

void begin(uint32_t id) {
  bootId = id; sequence = dropped = 0;
  haveLast = mounted = reading = backwardReported = false;
}

bool record(const char* event, const Frame& before, const Frame* after, const char* detail) {
  String line;
  line.reserve(after ? 4096 : 2048);
  line = "{\"schema\":1,\"firmwareBuild\":"; text(line, __DATE__ " " __TIME__);
  line += ",\"drainCarryScale\":10000,\"crisisCarryScale\":100,"; number(line, "bootId", bootId);
  line += ','; number(line, "sequence", ++sequence);
  line += ','; number(line, "droppedThisBoot", dropped);
  line += ",\"event\":"; text(line, event);
  line += ",\"detail\":"; text(line, detail);
  line += ','; number(line, "clockMismatch", clockMismatch(before));
  if (haveLast) {
    line += ','; number(line, "clockDeltaSec", static_cast<int64_t>(before.epoch) - lastRecorded.epoch);
    line += ','; number(line, "uptimeDeltaUs", static_cast<int64_t>(before.uptimeUs) - static_cast<int64_t>(lastRecorded.uptimeUs));
  }
  line += ','; number(line, "drainElapsedSec", static_cast<int64_t>(before.epoch) - before.lastDrainEpoch);
  line += ",\"before\":"; frameJson(line, before);
  if (after) { line += ",\"after\":"; frameJson(line, *after); }
  line += "}\n";
  lastRecorded = after ? *after : before;
  haveLast = true;
  if (append(line)) return true;
  ++dropped;
  return false;
}

bool drain(const Frame& before, const Frame& after) {
  bool largeDrop = before.needs.happiness > after.needs.happiness + 9;
  for (uint8_t i = 0; i < PetNeeds::Count; ++i) {
    largeDrop |= before.needs.needs[i].value > after.needs.needs[i].value + 9;
  }
  bool becameCritical = before.needs.happiness > 0 && after.needs.happiness == 0;
  bool stale = static_cast<int64_t>(before.epoch) - before.lastDrainEpoch > 600;
  bool backwards = before.epoch < before.lastDrainEpoch;
  bool newBackward = backwards && !backwardReported;
  backwardReported = backwards;
  bool mismatch = clockMismatch(before);
  bool sampleDue = !haveLast || before.uptimeUs - lastRecorded.uptimeUs >= SampleIntervalUs;
  if (!largeDrop && !becameCritical && !stale && !newBackward && !mismatch && !sampleDue) return true;
  return record("drain", before, &after, backwards ? "clock_backwards" : mismatch ? "clock_jump" :
                largeDrop ? "large_drop" : becameCritical ? "happiness_reached_zero" : stale ? "long_elapsed_interval" : "sample");
}

bool info(size_t& bytes, size_t& entries) {
  bytes = entries = 0;
  if (!ready()) return false;
  for (const char* path : {Previous, Current}) {
    File file = LittleFS.open(path, "r");
    if (!file) continue;
    bytes += file.size();
    uint8_t buffer[512];
    size_t count = 0;
    while ((count = file.read(buffer, sizeof(buffer))) > 0) {
      for (size_t i = 0; i < count; ++i) if (buffer[i] == '\n') ++entries;
    }
  }
  return true;
}

bool read(uint32_t offset, uint8_t* buffer, size_t capacity, size_t& bytesRead, size_t& total) {
  bytesRead = total = 0;
  if (!ready()) return false;
  if (offset == 0) {
    readPreviousBytes = fileSize(Previous); readCurrentBytes = fileSize(Current);
    reading = true;
  } else if (!reading) return false;
  lastReadMs = millis();
  total = readPreviousBytes + readCurrentBytes;
  if (offset > total) return false;
  while (bytesRead < capacity && offset < total) {
    bool previous = offset < readPreviousBytes;
    const char* path = previous ? Previous : Current;
    size_t position = previous ? offset : offset - readPreviousBytes;
    size_t remaining = previous ? readPreviousBytes - position : total - offset;
    size_t count = capacity - bytesRead;
    if (count > remaining) count = remaining;
    File file = LittleFS.open(path, "r");
    if (!file || !file.seek(position)) return false;
    size_t got = file.read(buffer + bytesRead, count);
    if (got != count) return false;
    bytesRead += got; offset += static_cast<uint32_t>(got);
  }
  if (offset >= total) reading = false;
  return true;
}

bool clear() {
  if (!ready()) return false;
  reading = false;
  bool ok = !LittleFS.exists(Previous) || LittleFS.remove(Previous);
  ok = (!LittleFS.exists(Current) || LittleFS.remove(Current)) && ok;
  return ok;
}

}  // namespace PetTrace
