# Pet Need and Daily Care Regression Tests

The tests compile the same `PetNeeds.cpp` and `PetCare.cpp` used by the firmware,
without Arduino or a connected board. The Windows runner also tests the actual
notification functions extracted from the sketch, using simulated hardware.
The runner also verifies startup persistence and the care-trace module against
fake flash. See [CARE_TRACE.md](CARE_TRACE.md) for the trace format, limits, and
the separate Playwright USB-download/UI checks (using installed Chrome).

On Windows with Visual Studio C++ Build Tools:

```powershell
powershell -ExecutionPolicy Bypass -File tests/run_pet_needs_tests.ps1
```

With a native GCC or Clang compiler:

```sh
c++ -std=c++17 -Wall -Wextra -Werror PetCare.cpp PetNeeds.cpp tests/pet_needs_test.cpp -o /tmp/pet-needs-tests
/tmp/pet-needs-tests
c++ -std=c++17 -Wall -Wextra -Werror PetCare.cpp PetNeeds.cpp tests/pet_care_test.cpp -o /tmp/pet-care-tests
/tmp/pet-care-tests
```

Coverage includes the 45%-happiness/45-minute report, exact critical-need crossings,
cooldown expiry during deep sleep, scheduled sleep expiry, intro gating, activity
refills, sad-animation order, long absences, and equal results for bulk versus
frequent updates. Tests do not flash the device or modify its stored state.

Daily-care tests cover every percentage threshold, rolling two/three-dip windows,
24/48-hour recovery, fresh happy streaks after recovery, stale/deferred notices,
all 81 applied-mode combinations, fractional carry continuity, RTC byte copies,
and 10,656 intervals with identical activities and acknowledgement times in bulk
and stepped simulations. Long gaps jump between state changes, not every second.

The notification suite tests Enter/Home, held/wake presses, inactivity sleep,
USB restoration, stale acknowledgement, runaway priority, and all 16 message
layouts. Its temporary `care-notifications.ppm` contact sheet uses the actual
device fonts. These simulated checks do not replace physical button/e-ink testing.

Starting-age tests use the actual firmware functions with simulated Preferences:
the first intro consumes a persistent 17-day override; subsequent lives start at
15 days. Deep sleep and ordinary power cycles/uploads do not regrant the override.
Erasing NVS (for example, an entire-flash erase) also erases this one-time marker.
Only the age timestamp is backdated, not need-drain or daily-care timers.

Startup tests exercise the actual sketch `setup()` and state-transfer functions
with simulated RTC, clock and NVS. Ordinary restarts recover a checksum-protected
checkpoint containing the existing pet and app mode. Valid deep-sleep RTC data
takes priority. A different firmware ELF hash starts fresh at the main menu;
uploading the identical binary cannot be distinguished from an ordinary restart.
Checkpoints are saved on boot, intro completion, refills, mode changes, sleep
changes and care acknowledgements, and every five minutes while awake. No extra
timer wakeups are added. If a reset also loses the clock, recovery uses the last
checkpoint's time; elapsed time without any power cannot be measured by software.
Corrupt/truncated records, failed writes, fresh-life resets, paused scheduled
sleep, clock-preserving catchup and write-rate limits are also tested.

Need drain durations are in `PetNeeds.h`. Happiness uses the existing weights:
40% food, 30% pee, 20% play, 10% pets. It is the rounded weighted score minus a
crisis penalty of one point per 45 minutes for each empty food/pee bar. The
penalty accrues only after a critical bar reaches zero, is retained across deep
sleep, and clears when both critical needs recover. Integer display steps are
expected; changing the update frequency must not change the result.

Daily-care rules are in `PetCare.cpp`; message text is in `CareNotificationText.cpp`.
Overall rates affect all four needs and the critical-need crisis drain. Individual
pee/play/food rates multiply the overall rate; pets has no individual effect.
Qualification is tracked continuously, including scheduled sleep, but an applied
rate changes only when its notice is acknowledged on Tamagotchi entry. Streaks
start after the intro. Fresh pets reset all modifiers and pending notices.

Need carries now use `FullDrainSeconds * DrainRateScale` as their denominator;
crisis carries use `CrisisSecondsPerPointPerNeed * CrisisRateScale`. Modifiers do
not reset these carries. Death logs include the carry scales, applied/qualified
modes, streaks, recent dips, and effective need rates in basis points (10000 is
normal). The firmware's RTC layout version was bumped, so an old retained pet
state is deliberately not interpreted using the new carry units.

On hardware after flashing, exercise notification entry from Home and deep sleep,
acknowledging multiple notices, Home deferral, USB connect/disconnect while reading
a notice, and resuming scheduled sleep. Check that opening/wake/held presses do
not dismiss a notice and that the screen refreshes cleanly before/after it.
