#pragma once

#include <stdint.h>
#include "PetCare.h"

void serialLibraryPortalBegin();
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
);
bool serialLibraryPortalLoop();
bool serialLibraryPortalIsBusy();
bool serialLibraryPortalIsConnected();
bool serialLibraryPortalConsumeConnectedEvent();
bool serialLibraryPortalConsumeDisconnectedEvent();
