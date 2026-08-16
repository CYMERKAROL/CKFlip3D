@echo off
rem ---------------------------------------------------------------
rem Builds the CKFlip3D Settings UI (separate C# WPF executable).
rem Requires the .NET 10 SDK (winget install Microsoft.DotNet.SDK.10).
rem Output: <repo>\build\CKFlip3D.Settings.exe — the same folder
rem build.bat puts CKFlip3D.exe in, so both programs live together.
rem ---------------------------------------------------------------
rem ---- locate a real .NET SDK -------------------------------------------
rem NOT `where dotnet`: this machine has the RUNTIME-only install in
rem Program Files, so dotnet.exe is on PATH, the old check passed, the
rem user-local SDK was never prepended and the build failed — silently, see
rem the errorlevel note below.  `dotnet --list-sdks` also exits 0 with no SDKs
rem installed, so the test has to be "did it list anything".
set "SDK_OK="
for /f "delims=" %%v in ('dotnet --list-sdks 2^>nul') do set "SDK_OK=1"
if not defined SDK_OK (
    if exist "%LOCALAPPDATA%\Microsoft\dotnet\dotnet.exe" (
        set "PATH=%LOCALAPPDATA%\Microsoft\dotnet;%PATH%"
    )
)
set "SDK_OK="
for /f "delims=" %%v in ('dotnet --list-sdks 2^>nul') do set "SDK_OK=1"
if not defined SDK_OK (
    echo ERROR: .NET SDK not found ^(runtime alone is not enough^). Install with:
    echo   winget install Microsoft.DotNet.SDK.10
    exit /b 1
)

dotnet build "%~dp0CKFlip3D.Settings.csproj" -c Release -o "%~dp0..\..\build"
rem NOT `if errorlevel 1`: that means "errorlevel >= 1", and the dotnet muxer
rem reports a missing SDK as a NEGATIVE code (-2147450725), which sails
rem straight through it.  Test for "not zero" instead.
if not "%errorlevel%"=="0" exit /b 1

rem Authenticode-sign as publisher CYMERKAROL (best-effort, never fails the build).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\..\sign_binaries.ps1" ^
  "%~dp0..\..\build\CKFlip3D.Settings.exe" "%~dp0..\..\build\CKFlip3D.Settings.dll"

echo.
echo Build OK: %~dp0..\..\build\CKFlip3D.Settings.exe
