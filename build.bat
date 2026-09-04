@echo off
setlocal
:: build.bat - tempctl.dll (x64 + x86, MSVC) + test exes, then libtempctl.so for
:: linux-x64 (cRIO-904x/905x/906x) and linux-arm64 (Raspberry Pi 4/5) via zig.
:: Usage: build.bat [all|win|test|linux]   (default all)
set "ROOT=%~dp0"
set "MODE=%~1"
if "%MODE%"=="" set "MODE=all"
set "BLIB=tempctl"
set "BTEST=test_tempctl"
set "BDEFINE=TEMPCTL_BUILD"
set "BSRCS="%ROOT%src\tempctl.c""

set "VCVARS="
for %%d in ("%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools" "%ProgramFiles%\Microsoft Visual Studio\18\Community" "%ProgramFiles%\Microsoft Visual Studio\2022\Community" "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools") do (
    if not defined VCVARS if exist "%%~d\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%~d\VC\Auxiliary\Build\vcvars64.bat"
)

if /i "%MODE%"=="linux" goto :linux

if not defined VCVARS ( echo ERROR: vcvars64.bat not found. Install VS 2022+ Build Tools with the C++ workload. & exit /b 1 )

call :msvc x64 "%VCVARS%"
if errorlevel 1 exit /b 1
call :msvc x86 "%VCVARS:vcvars64=vcvars32%"
if errorlevel 1 exit /b 1

if /i "%MODE%"=="win" goto :done
echo === running tests (x64) ===
"%ROOT%build\win-x64\%BTEST%.exe"
if errorlevel 1 ( echo TESTS FAILED & exit /b 1 )
echo === running tests (x86) ===
"%ROOT%build\win-x86\%BTEST%.exe"
if errorlevel 1 ( echo TESTS FAILED & exit /b 1 )
if /i "%MODE%"=="test" goto :done

:linux
set "ZIG="
for /d %%d in ("%ROOT%..\tools\zig-x86_64-windows-*") do if exist "%%~d\zig.exe" set "ZIG=%%~d\zig.exe"
if defined ZIG_HOME if exist "%ZIG_HOME%\zig.exe" set "ZIG=%ZIG_HOME%\zig.exe"
if not defined ZIG (
    echo WARNING: zig not found ^(expected ..\tools\zig-x86_64-windows-*\zig.exe or ZIG_HOME^). Skipping Linux builds.
    goto :done
)
call :zigbuild x64   x86_64-linux-gnu.2.24  "x86_64 NI Linux RT / cRIO"
if errorlevel 1 exit /b 1
call :zigbuild arm64 aarch64-linux-gnu.2.24 "aarch64 Linux / Raspberry Pi"
if errorlevel 1 exit /b 1

:done
echo.
echo Outputs:
if exist "%ROOT%build\win-x64\%BLIB%.dll"        echo   build\win-x64\%BLIB%.dll
if exist "%ROOT%build\win-x86\%BLIB%.dll"        echo   build\win-x86\%BLIB%.dll
if exist "%ROOT%build\linux-x64\lib%BLIB%.so"    echo   build\linux-x64\lib%BLIB%.so
if exist "%ROOT%build\linux-arm64\lib%BLIB%.so"  echo   build\linux-arm64\lib%BLIB%.so
exit /b 0

:: ---- subroutine: MSVC build for one architecture -------------------------
:: %1 arch tag (x64|x86)  %2 vcvars script
:msvc
setlocal
set "ARCH=%~1"
set "VCSCRIPT=%~2"
call "%VCSCRIPT%" >nul 2>&1
if errorlevel 1 ( echo ERROR: vcvars script failed for %ARCH% & exit /b 1 )
if not exist "%ROOT%build\win-%ARCH%" mkdir "%ROOT%build\win-%ARCH%"
pushd "%ROOT%build\win-%ARCH%"
echo === %BLIB%.dll (win-%ARCH%) ===
rc /nologo /I "%ROOT%src" /fo %BLIB%.res "%ROOT%src\%BLIB%.rc"
if errorlevel 1 ( popd & exit /b 1 )
cl /nologo /W4 /O2 /MT /std:c11 /D%BDEFINE% /DNDEBUG /LD %BSRCS% %BLIB%.res ^
   /Fe:%BLIB%.dll /link /DEF:"%ROOT%src\%BLIB%.def"
if errorlevel 1 ( popd & exit /b 1 )
echo === %BTEST%.exe (win-%ARCH%) ===
cl /nologo /W4 /O2 /MT /std:c11 /D%BDEFINE% "%ROOT%tests\test_main.c" %BSRCS% /Fe:%BTEST%.exe
if errorlevel 1 ( popd & exit /b 1 )
del /q *.obj *.res %BTEST%.lib %BTEST%.exp 2>nul
popd
exit /b 0

:: ---- subroutine: zig cross-build for one Linux target ----------------------
:: %1 dir tag (x64|arm64)  %2 zig target triple  %3 description
:zigbuild
setlocal
set "TAG=%~1"
set "TARGET=%~2"
if not exist "%ROOT%build\linux-%TAG%" mkdir "%ROOT%build\linux-%TAG%"
echo === lib%BLIB%.so (linux-%TAG%: %~3) ===
"%ZIG%" cc -target %TARGET% -O2 -std=c11 -fPIC -shared -fvisibility=hidden ^
   -Wall -Wextra -D%BDEFINE% -DNDEBUG -Wl,-soname,lib%BLIB%.so ^
   -o "%ROOT%build\linux-%TAG%\lib%BLIB%.so" %BSRCS%
if errorlevel 1 exit /b 1
echo === %BTEST% (linux-%TAG%) ===
"%ZIG%" cc -target %TARGET% -O2 -std=c11 -Wall -D%BDEFINE% ^
   -o "%ROOT%build\linux-%TAG%\%BTEST%" "%ROOT%tests\test_main.c" %BSRCS%
if errorlevel 1 exit /b 1
exit /b 0
