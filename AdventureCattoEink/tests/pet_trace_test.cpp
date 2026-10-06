#include "../PetTrace.h"
#include "test_support.h"
#include <LittleFS.h>
#include <fstream>

using TestSupport::require;
constexpr const char* Current = "/system/care_trace.jsonl";
constexpr const char* Previous = "/system/care_trace.previous.jsonl";

PetTrace::Frame healthy() {
  PetTrace::Frame frame = {};
  frame.epoch = frame.lastDrainEpoch = frame.rtcEpoch = frame.rtcDrainEpoch = 1790112333;
  frame.uptimeUs = 1000000; frame.millis = 1000;
  frame.introSeen = frame.needs.started = true;
  frame.appMode = 1; frame.wakeCause = 3; frame.resetReason = 5;
  frame.needs.happiness = 80;
  for (auto& need : frame.needs.needs) { need.value = 80; need.drainCarry = 123; }
  return frame;
}

std::string contents(const char* path) {
  auto file = FakeTraceFS::files.find(path);
  return file == FakeTraceFS::files.end() ? "" : *file->second;
}

void reset() { FakeTraceFS::reset(); PetTrace::begin(1234); }

void testSamplingAndClockJumps() {
  reset(); auto before = healthy();
  require(PetTrace::record("boot", before), "initial trace was not saved");
  size_t bytes = 0, entries = 0;
  for (unsigned i = 1; i < 300; ++i) {
    auto tick = before; tick.epoch += i; tick.uptimeUs += i * 1000000ULL;
    tick.lastDrainEpoch = tick.epoch;
    require(PetTrace::drain(tick, tick), "ordinary tick failed");
  }
  PetTrace::info(bytes, entries);
  require(entries == 1, "normal per-second updates wrote the filesystem");
  auto jump = before; jump.epoch += 27191; jump.uptimeUs += 3000000;
  auto dead = jump; dead.needs.happiness = 0;
  for (auto& need : dead.needs.needs) need.value = 0;
  require(PetTrace::drain(jump, dead), "suspicious drain was not recorded");
  auto text = contents(Current);
  require(text.find("clock_jump") != text.npos && text.find("\"clockMismatch\":1") != text.npos,
          "wall-clock/uptime mismatch was not identified");
  require(text.find("\"before\":") != text.npos && text.find("\"after\":") != text.npos &&
          text.find("\"happiness\":80") != text.npos && text.find("\"happiness\":0") != text.npos,
          "pre-drain healthy bars or resulting zero bars were lost");
  std::ofstream("care-trace-sample.jsonl") << text;
}

void testBackwardsAndSampling() {
  reset(); auto frame = healthy(); PetTrace::record("boot", frame);
  frame.epoch -= 10; frame.uptimeUs += 1000000;
  PetTrace::drain(frame, frame);
  for (unsigned i = 1; i < 1000; ++i) {
    frame.uptimeUs += 5000; PetTrace::drain(frame, frame);
  }
  size_t bytes = 0, entries = 0; PetTrace::info(bytes, entries);
  require(entries == 2, "backwards clock was logged on every loop");
  reset(); frame = healthy(); PetTrace::record("boot", frame);
  frame.epoch += 300; frame.lastDrainEpoch = frame.epoch; frame.uptimeUs += PetTrace::SampleIntervalUs;
  PetTrace::drain(frame, frame); PetTrace::info(bytes, entries);
  require(entries == 2 && contents(Current).find("\"detail\":\"sample\"") != std::string::npos,
          "five-minute sample missing");
}

void testRotationAndDownloads() {
  reset(); auto frame = healthy();
  for (unsigned i = 0; i < 200; ++i) {
    frame.epoch += 1; frame.uptimeUs += 1000000;
    require(PetTrace::record("home_enter_before", frame), "trace rollover write failed");
    require(contents(Current).size() <= PetTrace::BankBytes && contents(Previous).size() <= PetTrace::BankBytes,
            "trace exceeded its two-bank limit");
  }
  auto expected = contents(Previous) + contents(Current);
  uint8_t buffer[768]; size_t count = 0, total = 0; uint32_t offset = 0;
  std::string downloaded;
  do {
    require(PetTrace::read(offset, buffer, sizeof(buffer), count, total), "chunk download failed");
    downloaded.append(reinterpret_cast<char*>(buffer), count);
    offset += static_cast<uint32_t>(count);
    if (offset == count) PetTrace::record("usb_connected", frame);
  } while (offset < total);
  require(downloaded == expected, "download snapshot changed when a new event was appended");
  require(!PetTrace::read(static_cast<uint32_t>(total + 10), buffer, sizeof(buffer), count, total),
          "invalid read offset accepted");
  FakeTraceFS::files["/books/test.txt"] = std::make_shared<std::string>("book");
  FakeTraceFS::files["/system/death_log.txt"] = std::make_shared<std::string>("death");
  require(PetTrace::clear() && contents(Current).empty() && contents(Previous).empty(), "clear failed");
  require(contents("/books/test.txt") == "book" && contents("/system/death_log.txt") == "death",
          "clearing trace changed books or the old death log");
}

void testFailuresAndCorruptDiagnosticState() {
  reset(); auto frame = healthy();
  FakeTraceFS::failWrites = true;
  require(!PetTrace::record("failed_write", frame), "failed write reported success");
  FakeTraceFS::failWrites = false;
  frame.needs.care.categories[0].dipCount = 255;
  frame.needs.happiness = 0;
  frame.petMode = 65538;
  require(PetTrace::record("runaway_display", frame, nullptr, "a \"quoted\" reason"), "corrupt dip count crashed trace");
  auto text = contents(Current);
  require(text.find("\"droppedThisBoot\":1") != text.npos && text.find("\\\"quoted\\\"") != text.npos,
          "write failure count or JSON escaping missing");
  require(text.find("\"computedHappiness\":80") != text.npos && text.find("\"happinessMatchesNeeds\":0") != text.npos &&
          text.find("\"petMode\":65538") != text.npos, "cached happiness or raw enum corruption was hidden");
  FakeTraceFS::capacity = LittleFS.usedBytes() + 4096;
  require(!PetTrace::record("full_storage", frame), "full storage allowed a new trace write");
  require(contents(Current) == text, "full storage damaged existing diagnostics");
}

void testIncidentDuringDownload() {
  reset(); auto frame = healthy();
  auto padding = std::make_shared<std::string>(48 * 1024 - 1, '\n');
  FakeTraceFS::files[Current] = padding;
  uint8_t buffer[768]; size_t count = 0, total = 0;
  require(PetTrace::read(0, buffer, sizeof(buffer), count, total), "download did not start");
  size_t originalBytes = total;
  require(PetTrace::record("runaway_display", frame), "incident lost at the usual rotation boundary during download");
  require(contents(Current).find("runaway_display") != std::string::npos && contents(Previous).empty(),
          "recording the incident moved banks during download");
  uint32_t offset = static_cast<uint32_t>(count);
  while (offset < total) {
    require(PetTrace::read(offset, buffer, sizeof(buffer), count, total), "continued incident download failed");
    offset += static_cast<uint32_t>(count);
  }
  require(total == originalBytes, "incident changed the existing snapshot length");
  require(PetTrace::record("usb_disconnected", frame) && contents(Previous).find("runaway_display") != std::string::npos,
          "incident did not survive the next normal bank rotation");
}

int main() {
  TestSupport::Runner runner;
  runner.run("before/after drain, independent clock jump detection and sparse writes", testSamplingAndClockJumps);
  runner.run("backwards-clock deduplication and five-minute sampling", testBackwardsAndSampling);
  runner.run("bounded rollover, stable chunk downloads, clear preserves books/death log", testRotationAndDownloads);
  runner.run("write/full-storage failures, JSON escaping, corrupt dip-count bounds", testFailuresAndCorruptDiagnosticState);
  runner.run("incident preserved at a rotation boundary during a USB download", testIncidentDuringDownload);
  return runner.finish();
}
