# Care Trace Diagnostics

The separate `care_trace.jsonl` records context for false runaways without
changing the original death log or care rules. Download it through USB using
**Download Care Trace** in the website's connected-only Diagnostics section.
It is not a book and is never sent to the online library.

## Records

- `boot_restored`: `fresh`, `rtc`, or `checkpoint`, before clock initialization.
- `boot_clock_initialized`: clock after startup. A clock rebase at this stage
  can be expected; do not treat this alone as a runtime clock fault.
- `home_enter_before/after`, `tamagotchi_enter_before/after`, and
  `reader_enter_before/after`: state around mode changes.
- `usb_connected/disconnected`, `deep_sleep_enter`: mode and state at these boundaries.
- `activity_refill_before/after`, `intro_completed`, `life_reset_before/after`,
  `care_notice_acknowledged`, and scheduled sleep/wake requests.
- `drain`: both healthy/pre-update state and post-model state for clock
  mismatches, backwards clocks, long catch-up intervals, drops of at least
  ten points, happiness reaching zero, or a five-minute awake sample.
  `after` is sampled **before** the caller commits the updated drain timestamp
  and checks runaway; this preserves the exact state at that boundary.
- `runaway_decision` and `runaway_display`: actual state before deferral/display,
  independently of the old death logger's duplicate guard.

Each row has a firmware build, random boot ID, sequence number, and count of
failed/dropped records during that boot. Each frame includes epoch, independent
64-bit boot uptime, wake/reset cause, app/pet/action mode, intro and USB flags,
birth epoch, RTC and live drain timestamps, scheduled sleep, all bars and
fractional carries, cooldowns, effective rates, and applied/qualified care state.
`computedHappiness` independently applies the existing weighted-needs formula
and crisis penalty; `happinessMatchesNeeds` flags disagreement with the stored
happiness value. Mode/action enums are kept at their full width, not truncated.
Flags are numeric 0/1. `care` order is overall, pee, play, food.

`clockDeltaSec` versus `uptimeDeltaUs` compares successive recorded frames in
the same boot. A difference greater than five seconds sets `clockMismatch`.
Compare **within a boot ID**; deep-sleep wake and reset start new boot uptimes.
`drainElapsedSec` is the interval the model was asked to consume, not proof of
actual time spent asleep. These distinctions are central to the current bug.

## Bounds And Access

Two hidden `/system` banks hold at most 64 KiB each, for a 128 KiB total cap.
Rotation retains the newest complete bank plus current records.
Normal rotation starts at 48 KiB, leaving headroom for incident records during
a USB download before the hard 64 KiB bank cap is reached. There are no
timer wakeups or per-loop filesystem writes. Normal drain samples are limited
to once every five awake minutes; transitions and anomalies are saved promptly.
Full storage or unavailable flash can still prevent records from being saved.
Logging never deletes books to make space. All filesystem usage appears in the
normal device storage totals.

`ACAT DIAG CARETRACE INFO`, `READ <byteOffset>`, and `CLEAR` are USB-only commands.
READ begins at offset zero and freezes the bank lengths until the download ends.
New appends are excluded from that snapshot, and bank rotation is blocked during
the download. An abandoned download stops blocking rotation after 30 seconds
without a READ.
Clearing this trace does not touch books or the original death log.

## Verification

`tests/run_pet_needs_tests.ps1` includes native tests of the actual trace module
with fake flash: sparse sampling, clock-jump before/after data, backwards-clock
deduplication, rollover, snapshot downloads, clear isolation, failed writes,
full storage, escaping, and corrupt dip-count bounds.

`node tests/care_trace_website_test.cjs` uses Playwright to verify desktop/mobile
layout, chunked USB command routing, download content, clear isolation, and
busy/disconnected states. It does not substitute for physical USB testing.

Reflash the firmware and redeploy the website. Following the next false runaway,
download the care trace promptly, preferably together with the original death log.
