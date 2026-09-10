@echo off
:: build.bat  —  Build d3d9capture.dll, inject_tool.exe, and shm_reader.exe
::
:: Prerequisites:
::   • Visual Studio 2017+ (or Build Tools) — run from a Developer Command Prompt
::   • Windows SDK (for d3d9.h / d3d11.h / dxgi.h)
::   • Target architecture must match the game:
::       - x86 for 32-bit games (e.g. GTA IV / Direct3D 9): vcvars32.bat
::       - x64 for 64-bit games (e.g. GTA V / Direct3D 11): vcvars64.bat
::
:: Quick start (64-bit for GTA V):
::   "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
::   cd /d <this directory>
::   build.bat

setlocal

set OUTDIR=..\bin
if not exist %OUTDIR% mkdir %OUTDIR%

echo.
echo [1/3] Building d3d9capture.dll ...
cl /nologo /W3 /O2 /MD /LD /I imgui /I imgui\backends ^
   /Fe:%OUTDIR%\d3d9capture.dll ^
   dllmain.cpp capture.cpp consumer_backend.cpp recorder.cpp overlay.cpp ^
   imgui\imgui.cpp imgui\imgui_draw.cpp imgui\imgui_widgets.cpp imgui\imgui_tables.cpp imgui\imgui_demo.cpp ^
   imgui\backends\imgui_impl_win32.cpp imgui\backends\imgui_impl_dx11.cpp imgui_impl_dx9_patched.cpp ^
   /link d3d9.lib d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib user32.lib gdi32.lib dxguid.lib shell32.lib mfplat.lib mfuuid.lib mfreadwrite.lib ole32.lib
if errorlevel 1 goto fail

echo.
echo [2/3] Building inject_tool.exe ...
cl /nologo /W3 /O2 /MT ^
   /Fe:%OUTDIR%\inject_tool.exe ^
   inject_tool.cpp
if errorlevel 1 goto fail

echo.
echo [3/3] Building shm_reader.exe ...
cl /nologo /W3 /O2 /MT ^
   /Fe:%OUTDIR%\shm_reader.exe ^
   shm_reader.cpp
if errorlevel 1 goto fail

echo.
echo ============================================================
echo  Build succeeded.  Outputs in %OUTDIR%
echo.
echo  Usage:
echo    1. Run shm_reader.exe  (opens the shared-memory channel)
echo       or shm_reader.exe --record out.mp4  to encode H.264/MP4
echo    2. Launch and inject before renderer initializes:
echo       inject_tool.exe --launch ^<game.exe^> %OUTDIR%\d3d9capture.dll
echo ============================================================
goto end

:fail
echo.
echo BUILD FAILED.
exit /b 1

:end
endlocal
