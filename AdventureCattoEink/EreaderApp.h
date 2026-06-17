#pragma once

#include <Arduino.h>

void ereaderBegin();
void ereaderEnter();
void ereaderLoop();
void ereaderLeave();
bool ereaderWantsSleep();
