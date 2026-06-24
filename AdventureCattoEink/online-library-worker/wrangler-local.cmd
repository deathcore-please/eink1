@echo off
set "NODE_BIN=%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\node\bin"
set "NODE_EXE=%NODE_BIN%\node.exe"
set "WRANGLER_CMD=%~dp0node_modules\.bin\wrangler.CMD"

if not exist "%NODE_EXE%" (
  echo Bundled Node was not found at "%NODE_EXE%".
  echo Install Node.js or run this from Codex again.
  exit /b 1
)

if not exist "%WRANGLER_CMD%" (
  echo Wrangler is not installed yet. Ask Codex to run the Worker dependency install again.
  exit /b 1
)

set "PATH=%NODE_BIN%;%PATH%"
call "%WRANGLER_CMD%" %*
exit /b %ERRORLEVEL%
