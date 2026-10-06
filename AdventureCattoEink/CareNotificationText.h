#pragma once

#include "PetCare.h"

namespace CareNotificationText {

const char* heading(PetCare::Category category);
const char* message(const PetCare::Notice& notice);

}  // namespace CareNotificationText
