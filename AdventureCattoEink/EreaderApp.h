#pragma once

#include <Arduino.h>

void ereaderBegin();
void ereaderEnter();
void ereaderRequestEnter();
bool ereaderActivateIfPending();
void ereaderLoop();
void ereaderLeave();
bool ereaderWantsSleep();
