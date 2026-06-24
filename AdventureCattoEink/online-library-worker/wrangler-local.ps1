param(
  [Parameter(ValueFromRemainingArguments = $true)]
  [string[]] $WranglerArgs
)

$NodeBin = Join-Path $env:USERPROFILE ".cache\codex-runtimes\codex-primary-runtime\dependencies\node\bin"
$NodeExe = Join-Path $NodeBin "node.exe"
$WranglerCmd = Join-Path $PSScriptRoot "node_modules\.bin\wrangler.CMD"

if (-not (Test-Path $NodeExe)) {
  Write-Error "Bundled Node was not found at $NodeExe. Install Node.js or run this from Codex again."
  exit 1
}

if (-not (Test-Path $WranglerCmd)) {
  Write-Error "Wrangler is not installed yet. Ask Codex to run the Worker dependency install again."
  exit 1
}

$env:PATH = "$NodeBin;$env:PATH"
& $WranglerCmd @WranglerArgs
exit $LASTEXITCODE
