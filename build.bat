@echo off
rem Builds the main CKFlip3D.exe into build\ (shared output folder with the
rem Settings app — core\Settings\build_settings.bat targets the same place,
rem so the tray-menu launch and CoreLocator find each other side by side).
cd /d "%~dp0"
if not exist build mkdir build
if not exist build\obj mkdir build\obj
if not exist build\obj\launcher mkdir build\obj\launcher
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
rc /nologo /fo build\obj\app.res app.rc
if not %errorlevel%==0 (echo RC_FAILED & exit /b 1)
rem /utf-8 is not cosmetic.  These sources are UTF-8 without a BOM, so without
rem it cl reads them in the system ANSI codepage, and every multi-byte character
rem inside a WIDE literal becomes several wrong wide characters.  Real damage,
rem not theory: the em dash in the diagnostics separator reached the log (and the
rem Settings tiles, which print the text verbatim) as three mojibake characters.
rem It is also why a few literals in this tree spell the dash as — by hand —
rem those were the individual ones somebody noticed and worked around.
rem Narrow literals are ASCII throughout, so the execution charset changes none.
cl /utf-8 /EHsc /std:c++20 /O2 /W4 /DUNICODE /D_UNICODE /I. build\obj\app.res main.cpp core\app.cpp core\Config.cpp core\Diagnostics.cpp core\ThemePlate.cpp core\SearchFilter.cpp core\SearchBox.cpp core\flipcontroller.cpp scene\FlipScene.cpp scene\CoverFlowLayout.cpp render\QuadRenderer.cpp render\Renderer.cpp capture\windowscanner.cpp capture\WGCCapture.cpp capture\windowcloaker.cpp capture\TaskbarButtonLocator.cpp hook\keyboardhook.cpp hook\hotkeymanager.cpp hook\touchpadhook.cpp input\TileHitTest.cpp animation\CycleAnimator.cpp animation\EntryExitTimeline.cpp animation\FlatStackBuilder.cpp animation\EntryExitAnimator.cpp animation\CloseAnimator.cpp animation\LabelAnimator.cpp animation\HoverAnimator.cpp /Fo:build\obj\ /Fe:build\CKFlip3D.exe /link d3d11.lib dxgi.lib d3dcompiler.lib dcomp.lib user32.lib gdi32.lib shell32.lib dwmapi.lib ole32.lib oleaut32.lib oleacc.lib windowsapp.lib /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:CKFlip3D.exe.manifest
if not %errorlevel%==0 (echo BUILD_FAILED & exit /b 1)
rem ---------------------------------------------------------------------------
rem CKFlip3D.Launch.exe — the launch shortcut's target (launcher\launch.cpp).
rem Its own link, not another object in the one above: it must run asInvoker so
rem clicking the shortcut never raises a UAC prompt, and the core's manifest
rem requires administrator.  One translation unit, no shared code — a click
rem should reach the cascade in milliseconds.  /I . so the .rc resolves
rem assets\icons from the repo root like app.rc does.
rc /nologo /I . /fo build\obj\launch.res launcher\launch.rc
if not %errorlevel%==0 (echo RC_LAUNCH_FAILED & exit /b 1)
cl /utf-8 /EHsc /std:c++20 /O2 /W4 /DUNICODE /D_UNICODE /I. build\obj\launch.res launcher\launch.cpp /Fo:build\obj\launcher\ /Fe:build\CKFlip3D.Launch.exe /link user32.lib shell32.lib /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:launcher\CKFlip3D.Launch.exe.manifest
if not %errorlevel%==0 (echo BUILD_LAUNCH_FAILED & exit /b 1)
rem Authenticode-sign as publisher CYMERKAROL (best-effort, never fails the build).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sign_binaries.ps1" "%~dp0build\CKFlip3D.exe" "%~dp0build\CKFlip3D.Launch.exe"
echo BUILD_OK
