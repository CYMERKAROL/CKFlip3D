@echo off
rem ---------------------------------------------------------------
rem Builds the CKFlip3D Settings UI (separate C# WPF executable).
rem Requires the .NET 10 SDK (winget install Microsoft.DotNet.SDK.10).
rem Output: <repo>\build\CKFlip3D.Settings.exe — the same folder
rem build.bat puts CKFlip3D.exe in, so both programs live together.
rem ---------------------------------------------------------------
where dotnet >nul 2>nul
if errorlevel 1 (
    rem Fall back to a user-local SDK installed by dotnet-install.ps1.
    if exist "%LOCALAPPDATA%\Microsoft\dotnet\dotnet.exe" (
        set "PATH=%LOCALAPPDATA%\Microsoft\dotnet;%PATH%"
    ) else (
        echo ERROR: .NET SDK not found in PATH. Install with:
        echo   winget install Microsoft.DotNet.SDK.10
        exit /b 1
    )
)

dotnet build "%~dp0CKFlip3D.Settings.csproj" -c Release -o "%~dp0..\..\build"
if errorlevel 1 exit /b 1

rem Authenticode-sign as publisher CYMERKAROL (best-effort, never fails the build).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\..\sign_binaries.ps1" ^
  "%~dp0..\..\build\CKFlip3D.Settings.exe" "%~dp0..\..\build\CKFlip3D.Settings.dll"

echo.
echo Build OK: %~dp0..\..\build\CKFlip3D.Settings.exe
