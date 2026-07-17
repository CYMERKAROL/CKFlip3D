@echo off
rem Builds the main CKFlip3D.exe into build\ (shared output folder with the
rem Settings app — core\Settings\build_settings.bat targets the same place,
rem so the tray-menu launch and CoreLocator find each other side by side).
cd /d "%~dp0"
if not exist build mkdir build
if not exist build\obj mkdir build\obj
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
rc /nologo /fo build\obj\app.res app.rc
if not %errorlevel%==0 (echo RC_FAILED & exit /b 1)
cl /EHsc /std:c++20 /O2 /W4 /DUNICODE /D_UNICODE /I. build\obj\app.res main.cpp core\app.cpp core\Config.cpp core\flipcontroller.cpp scene\FlipScene.cpp render\QuadRenderer.cpp render\Renderer.cpp capture\windowscanner.cpp capture\WGCCapture.cpp capture\windowcloaker.cpp capture\TaskbarButtonLocator.cpp hook\keyboardhook.cpp hook\hotkeymanager.cpp animation\CycleAnimator.cpp animation\EntryExitTimeline.cpp animation\FlatStackBuilder.cpp animation\EntryExitAnimator.cpp animation\CloseAnimator.cpp /Fo:build\obj\ /Fe:build\CKFlip3D.exe /link d3d11.lib dxgi.lib d3dcompiler.lib dcomp.lib user32.lib gdi32.lib shell32.lib dwmapi.lib ole32.lib oleaut32.lib oleacc.lib windowsapp.lib /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:CKFlip3D.exe.manifest
if %errorlevel%==0 (echo BUILD_OK) else (echo BUILD_FAILED)
