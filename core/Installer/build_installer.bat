@echo off
rem =====================================================================
rem Builds the standalone CKFlip3D installer (CKFlip3D.Setup.exe).
rem
rem Pipeline:
rem   1. Build the Settings app (net10.0-windows) into <repo>\build
rem   2. Verify the C++ core (build\CKFlip3D.exe) exists (built by build.bat)
rem   3. Generate Resources\CKFlip3D.ico from the logo PNG
rem   4. Stage + zip the payload, embedded INTO the setup exe at publish
rem   5. Publish the setup exe: self-contained single-file win-x64, so it
rem      runs on machines with no .NET installed
rem
rem Output: <repo>\dist\CKFlip3D.Setup.exe  (ONE file, payload inside)
rem Requires the .NET 10 SDK (winget install Microsoft.DotNet.SDK.10).
rem =====================================================================
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%..\.."
set "BUILD=%ROOT%\build"
set "DIST=%ROOT%\dist"
set "STAGING=%HERE%Payload\staging"

rem ---- locate the .NET SDK (PATH, then the user-local install) ----------
where dotnet >nul 2>nul
if errorlevel 1 (
    if exist "%LOCALAPPDATA%\Microsoft\dotnet\dotnet.exe" (
        set "PATH=%LOCALAPPDATA%\Microsoft\dotnet;%PATH%"
    ) else (
        echo ERROR: .NET SDK not found. Install with:
        echo   winget install Microsoft.DotNet.SDK.10
        exit /b 1
    )
)

rem ---- 1. Settings app ---------------------------------------------------
echo === Building CKFlip3D.Settings ===
call "%HERE%..\Settings\build_settings.bat"
if errorlevel 1 exit /b 1

rem ---- 2. Core exe check -------------------------------------------------
if not exist "%BUILD%\CKFlip3D.exe" (
    echo ERROR: %BUILD%\CKFlip3D.exe not found. Build the core first: build.bat
    exit /b 1
)

rem ---- 3. Icons (pre-built in assets\icons, see make_icons.ps1) ----------
if not exist "%ROOT%\assets\icons\CKFlip3D.ico" (
    echo === Generating icons ===
    powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\assets\icons\make_icons.ps1"
    if errorlevel 1 exit /b 1
)

rem ---- 4. Payload --------------------------------------------------------
echo === Staging payload ===
if exist "%STAGING%" rmdir /s /q "%STAGING%"
mkdir "%STAGING%"
copy /y "%BUILD%\CKFlip3D.exe"                        "%STAGING%" >nul
copy /y "%BUILD%\CKFlip3D.Settings.exe"               "%STAGING%" >nul
copy /y "%BUILD%\CKFlip3D.Settings.dll"               "%STAGING%" >nul
copy /y "%BUILD%\CKFlip3D.Settings.deps.json"         "%STAGING%" >nul
copy /y "%BUILD%\CKFlip3D.Settings.runtimeconfig.json" "%STAGING%" >nul
rem CKFlip3D.ico is NOT shipped: the icon is embedded in CKFlip3D.exe via
rem app.rc, and shortcuts / Apps & Features fall back to the exe path, so a
rem loose .ico in the install dir is redundant (InstallEngine also removes a
rem stale copy left by older payloads on upgrade).  The uninstall icon IS
rem shipped — the uninstaller is a copy of the Setup exe, whose embedded
rem icon is the setup one, so the distinct Start-Menu uninstall icon needs
rem the file on disk.
copy /y "%ROOT%\assets\icons\CKFlip3D.Uninstall.ico"  "%STAGING%" >nul

if exist "%HERE%Payload\payload.zip" del /q "%HERE%Payload\payload.zip"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Compress-Archive -Path '%STAGING%\*' -DestinationPath '%HERE%Payload\payload.zip' -CompressionLevel Optimal"
if errorlevel 1 exit /b 1
rmdir /s /q "%STAGING%"

rem ---- 5. Publish the setup exe ------------------------------------------
echo === Publishing CKFlip3D.Setup.exe ===
dotnet publish "%HERE%CKFlip3D.Setup.csproj" -c Release -r win-x64 --self-contained true ^
  -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true ^
  -p:EnableCompressionInSingleFile=true -o "%DIST%"
if errorlevel 1 exit /b 1

rem The zip was compiled into the exe as an embedded resource. Remove the
rem build-time intermediate and the pdb so dist\ ships exactly one file.
del /q "%HERE%Payload\payload.zip" >nul 2>nul
del /q "%DIST%\CKFlip3D.Setup.pdb" >nul 2>nul

echo.
echo Build OK: %DIST%\CKFlip3D.Setup.exe  (single-file installer, payload embedded)
endlocal
