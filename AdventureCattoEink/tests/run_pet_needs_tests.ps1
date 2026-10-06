$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw 'Install Visual Studio C++ Build Tools or run the portable command in tests/README.md.' }
    $devCmd = Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'
    $environmentLines = & cmd.exe /d /s /c "`"$devCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'Unable to initialize the C++ compiler environment.' }
    foreach ($line in $environmentLines) {
        $parts = $line -split '=', 2
        if ($parts.Length -eq 2 -and $parts[0]) {
            [Environment]::SetEnvironmentVariable($parts[0], $parts[1], 'Process')
        }
    }
}

$buildPath = Join-Path ([IO.Path]::GetTempPath()) ('AdventureCattoNeedsTests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $buildPath | Out-Null
# Extract complete top-level functions for host testing without copying their logic.
$firmware = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'AdventureCattoEink.ino')
$epd = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'EPD.cpp')
$font = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'EPDfont.h')
$extracted = [Collections.Generic.List[string]]::new()
foreach ($name in @('ascii_1206', 'ascii_1608')) {
    $match = [regex]::Match($font, "(?ms)^const unsigned char $name\[.*?^\};")
    if (-not $match.Success) { throw "Missing font definition: $name" }
    $extracted.Add([regex]::Replace($match.Value, '/\*.*?\*/', '', 'Singleline'))
}
foreach ($name in @('EPD_ShowStringBold16', 'drawWrappedFullScreenMessage', 'drawCareNoticeScreen', 'showNextCareNotice', 'acknowledgeCareNotice', 'loopCareNotice', 'enterTamagotchiInitialScreen', 'redrawTamagotchiAfterSerialPortal', 'canMenuButtonEnterHome')) {
    $source = if ($name -eq 'EPD_ShowStringBold16') { $epd } else { $firmware }
    $matches = [regex]::Matches($source, "(?ms)^(?:void|bool) $name\([^;{}]*\)\s*\{.*?^\}")
    if ($matches.Count -ne 1) { throw "Expected one complete function: $name" }
    $extracted.Add($matches[0].Value)
}
[IO.File]::WriteAllText((Join-Path $buildPath 'care_firmware_under_test.h'), ($extracted -join "`n"))
$age = [Collections.Generic.List[string]]::new()
foreach ($name in @('DEFAULT_PET_START_AGE_DAYS', 'ONE_TIME_PET_START_AGE_DAYS')) {
    $match = [regex]::Match($firmware, "(?m)^#define $name .+$")
    if (-not $match.Success) { throw "Missing starting-age constant: $name" }
    $age.Add($match.Value)
}
foreach ($name in @('resetPetAgeTimer', 'consumeStartingPetAgeDays', 'startPetAgeTimerIfNeeded')) {
    $matches = [regex]::Matches($firmware, "(?ms)^(?:void|uint32_t) $name\([^;{}]*\)\s*\{.*?^\}")
    if ($matches.Count -ne 1) { throw "Expected one complete age function: $name" }
    $age.Add($matches[0].Value)
}
[IO.File]::WriteAllText((Join-Path $buildPath 'pet_age_firmware_under_test.h'), ($age -join "`n"))
$boot = [Collections.Generic.List[string]]::new()
foreach ($name in @('RTC_PET_STATE_MAGIC', 'PET_CHECKPOINT_INTERVAL_MS', 'PET_CHECKPOINT_RETRY_MS')) {
    $match = [regex]::Match($firmware, "(?m)^#define $name .+$")
    if (-not $match.Success) { throw "Missing boot constant: $name" }
    $boot.Add($match.Value)
}
foreach ($name in @('resetPetAgeTimer', 'captureNeedState', 'applyNeedState', 'capturePetTraceFrame', 'hasRunawayCriticalNeed', 'shouldShowRunaway', 'showRunawayScreen', 'updateHappiness', 'maybeTriggerRunaway', 'resetNeedDrainClock', 'resetPetLifeState', 'initDefaultPetState', 'refreshRtcEpochFromClock', 'savePetStateToRtc', 'restorePetStateFromRtc', 'savePetStateCheckpoint', 'servicePetStateCheckpoint', 'restorePetStateForBoot', 'initDeviceTime', 'initNeedDrainClockIfNeeded', 'drainNeedsOverTime', 'applyNeedsSinceSleep', 'setup')) {
    $matches = [regex]::Matches($firmware, "(?ms)^(?:void|bool|PetNeeds::State|PetTrace::Frame|PetStateRestoreSource) $name\([^;{}]*\)\s*\{.*?^\}")
    if ($matches.Count -ne 1) { throw "Expected one complete boot function: $name" }
    $boot.Add($matches[0].Value)
}
[IO.File]::WriteAllText((Join-Path $buildPath 'pet_boot_firmware_under_test.h'), ($boot -join "`n"))
Push-Location $buildPath
try {
    foreach ($suite in @('pet_needs_test', 'pet_care_test', 'care_notice_test', 'pet_age_test')) {
        & cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 "/I$buildPath" (Join-Path $projectRoot 'PetCare.cpp') (Join-Path $projectRoot 'PetNeeds.cpp') (Join-Path $projectRoot 'CareNotificationText.cpp') (Join-Path $PSScriptRoot "$suite.cpp") "/Fe:$suite.exe"
        if ($LASTEXITCODE -ne 0) { throw "$suite compilation failed." }
        & (Join-Path $buildPath "$suite.exe")
        if ($LASTEXITCODE -ne 0) { throw "$suite regression tests failed." }
    }
    & cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 "/I$buildPath" "/I$(Join-Path $PSScriptRoot 'fakes')" (Join-Path $projectRoot 'PetCare.cpp') (Join-Path $projectRoot 'PetNeeds.cpp') (Join-Path $projectRoot 'PetStateStore.cpp') (Join-Path $PSScriptRoot 'pet_boot_test.cpp') /Fe:pet_boot_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'pet_boot_test compilation failed.' }
    & (Join-Path $buildPath 'pet_boot_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'pet_boot_test regression tests failed.' }
    & cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 "/I$(Join-Path $PSScriptRoot 'fakes')" (Join-Path $projectRoot 'PetCare.cpp') (Join-Path $projectRoot 'PetNeeds.cpp') (Join-Path $projectRoot 'PetTrace.cpp') (Join-Path $PSScriptRoot 'pet_trace_test.cpp') /Fe:pet_trace_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'pet_trace_test compilation failed.' }
    & (Join-Path $buildPath 'pet_trace_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'pet_trace_test regression tests failed.' }
    Write-Output "Trace sample: $(Join-Path $buildPath 'care-trace-sample.jsonl')"
    Write-Output "Notification render: $(Join-Path $buildPath 'care-notifications.ppm')"
} finally {
    Pop-Location
}
