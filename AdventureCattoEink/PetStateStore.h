#pragma once

#include <stdint.h>
#include "PetCare.h"

namespace PetStateStore {

struct Snapshot {
  uint32_t magic;
  uint8_t peeValue;
  uint8_t foodValue;
  uint8_t playValue;
  uint8_t loveValue;
  uint8_t activeSadNeed;
  uint8_t selectedAction;
  uint8_t appMode;
  uint8_t homeSelection;
  uint8_t petAgeStarted;
  uint32_t epochSeconds;
  uint64_t epochSetUs;
  uint32_t petBirthEpoch;
  uint32_t sleepEntryEpoch;
  uint32_t lastNeedDrainEpoch;
  uint32_t peeDrainCarry;
  uint32_t foodDrainCarry;
  uint32_t playDrainCarry;
  uint32_t loveDrainCarry;
  uint8_t happinessValue;
  uint32_t happinessCrisisCarry;
  uint8_t happinessCrisisPenalty;
  uint8_t hasSeenDeliveryIntro;
  uint8_t sleeping;
  uint32_t sleepStartEpoch;
  uint32_t sleepDurationSec;
  uint32_t sleepCooldownUntilEpoch;
  uint32_t peeCooldownUntilEpoch;
  uint32_t foodCooldownUntilEpoch;
  uint32_t playCooldownUntilEpoch;
  PetCare::State care;
};

bool load(Snapshot& snapshot);
bool save(const Snapshot& snapshot);

}  // namespace PetStateStore
