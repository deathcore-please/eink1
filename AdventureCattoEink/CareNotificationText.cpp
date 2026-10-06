#include "CareNotificationText.h"

namespace CareNotificationText {
namespace {

struct Text {
  const char* heading;
  const char* happyEntry;
  const char* happyExit;
  const char* sadEntry;
  const char* sadExit;
};

// Entries follow PetCare::Category order; preserve the supplied wording.
const Text Texts[PetCare::Count] = {
  {
    "OVERALL HAPPINESS",
    "Your baby was very happy today!!! Now she'll loose her happiness slower "
    "than before!",
    "Good job, but baby will now loose happiness as normal",
    "Baby has been sad multiple times today, she will loose happiness faster "
    "now and might run away unless you take care of her :((",
    "You took care of baby well, she has returned back up to normal happiness :))"
  },
  {
    "PEE NOTIFICATION",
    "Wowww you took baby to pee on time today, she's has become disciplined "
    "and will ask to go to pee less now!",
    "Aww baby will start peeing like normal again now",
    "Baby has been wanting to pee today but wasn't taken care of. She will "
    "ask to pee more frequently unless you train to take her more",
    "Babie got back to her usual pee routine all thanks to youuu, back to "
    "normal routine now!"
  },
  {
    "PLAY NOTIFICATION",
    "You trained her so well! So much effort!! She has grown muscles now "
    "and will not need exercise so frequently now!!",
    "Ya'll had a good run! She's back to normal play routine now :))",
    "She has been lazy for too long :(( train and play with her more "
    "frequently for a bit pls",
    "She's back to her usual strength!! Yayy!"
  },
  {
    "FOOD NOTIFICATION",
    "You have fed her so well that she's getting rotund!! She will ask for "
    "food less frequently now!",
    "Good job cutting back on those carbs! She's back to her usual diet!",
    "Come on now don't starve her :(( she will require to be fed more "
    "frequently now",
    "You have been feeding her on time yayyy. She's back to her normal "
    "food routing now :))"
  }
};

}  // namespace

const char* heading(PetCare::Category category) {
  if (category >= PetCare::Count) {
    return "";
  }
  return Texts[category].heading;
}

const char* message(const PetCare::Notice& notice) {
  if (notice.category >= PetCare::Count) {
    return "";
  }

  const Text& text = Texts[notice.category];
  if (notice.from == PetCare::Happy && notice.to == PetCare::Normal) {
    return text.happyExit;
  }
  if (notice.from == PetCare::Sad && notice.to == PetCare::Normal) {
    return text.sadExit;
  }
  if (notice.from == PetCare::Normal && notice.to == PetCare::Happy) {
    return text.happyEntry;
  }
  if (notice.from == PetCare::Normal && notice.to == PetCare::Sad) {
    return text.sadEntry;
  }
  return "";
}

}  // namespace CareNotificationText
