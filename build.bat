@echo off
:: build.bat  —  Build d3d9capture.dll, inject_tool.exe, and shm_reader.exe
::
:: Prerequisites:
::   • Visual Studio 2017+ (or Build Tools) — run from a Developer Command Prompt
::   • Windows SDK (for d3d9.h / d3d9.lib)
::   • Target architecture must match the game: x86 for 32-bit games (most D3D9),
::     x64 only for the rare 64-bit D3D9 title.
::
:: Quick start (32-bit):
::   "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
::   cd /d <this directory>
::   build.bat

setlocal

set OUTDIR=..\bin
if not exist %OUTDIR% mkdir %OUTDIR%

echo.
echo [1/3] Building d3d9capture.dll ...
cl /nologo /W3 /O2 /MD /LD ^
   /Fe:%OUTDIR%\d3d9capture.dll ^
   dllmain.cpp capture.cpp consumer_backend.cpp ^
   /link d3d9.lib user32.lib gdi32.lib
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
echo    2. Start the game
echo    3. inject_tool.exe  ^<pid^>  %OUTDIR%\d3d9capture.dll
echo ============================================================
goto end

:fail
echo.
echo BUILD FAILED.
exit /b 1

:end
endlocal
