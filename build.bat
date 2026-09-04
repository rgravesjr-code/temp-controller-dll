@echo off
setlocal
:: build.bat - tempctl.dll (x64 + x86, MSVC) + test exes, then libtempctl.so (x64 NI Linux RT, zig).
:: Usage: build.bat [all|win|test|linux]   (default all)
set "ROOT=%~dp0"
set "MODE=%~1"
if "%MODE%"=="" set "MODE=all"

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
"%ROOT%build\win-x64\test_tempctl.exe"
if errorlevel 1 ( echo TESTS FAILED & exit /b 1 )
echo === running tests (x86) ===
"%ROOT%build\win-x86\test_tempctl.exe"
if errorlevel 1 ( echo TESTS FAILED & exit /b 1 )
if /i "%MODE%"=="test" goto :done

:linux
set "ZIG="
for /d %%d in ("%ROOT%..\tools\zig-x86_64-windows-*") do if exist "%%~d\zig.exe" set "ZIG=%%~d\zig.exe"
if defined ZIG_HOME if exist "%ZIG_HOME%\zig.exe" set "ZIG=%ZIG_HOME%\zig.exe"
if not defined ZIG (
    echo WARNING: zig not found ^(expected ..\tools\zig-x86_64-windows-*\zig.exe or ZIG_HOME^). Skipping libtempctl.so.
    goto :done
)
if not exist "%ROOT%build\linux-x64" mkdir "%ROOT%build\linux-x64"
echo === libtempctl.so (x86_64 NI Linux RT, glibc 2.24+) ===
"%ZIG%" cc -target x86_64-linux-gnu.2.24 -O2 -std=c11 -fPIC -shared -fvisibility=hidden ^
   -Wall -Wextra -DTEMPCTL_BUILD -DNDEBUG -Wl,-soname,libtempctl.so ^
   -o "%ROOT%build\linux-x64\libtempctl.so" ^
   "%ROOT%src\tempctl.c" "%ROOT%src\j1939.c" "%ROOT%src\canpack.c"
if errorlevel 1 exit /b 1
echo === test_tempctl (linux x64, run it on the cRIO) ===
"%ZIG%" cc -target x86_64-linux-gnu.2.24 -O2 -std=c11 -Wall -DTEMPCTL_BUILD ^
   -o "%ROOT%build\linux-x64\test_tempctl" ^
   "%ROOT%tests\test_main.c" "%ROOT%src\tempctl.c" "%ROOT%src\j1939.c" "%ROOT%src\canpack.c"
if errorlevel 1 exit /b 1

:done
echo.
echo Outputs:
if exist "%ROOT%build\win-x64\tempctl.dll" echo   build\win-x64\tempctl.dll
if exist "%ROOT%build\win-x86\tempctl.dll" echo   build\win-x86\tempctl.dll
if exist "%ROOT%build\linux-x64\libtempctl.so" echo   build\linux-x64\libtempctl.so
if exist "%ROOT%build\linux-arm64\libtempctl.so" echo   build\linux-arm64\libtempctl.so
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
echo === tempctl.dll (win-%ARCH%) ===
rc /nologo /I "%ROOT%src" /fo tempctl.res "%ROOT%src\tempctl.rc"
if errorlevel 1 ( popd & exit /b 1 )
cl /nologo /W4 /O2 /MT /std:c11 /DTEMPCTL_BUILD /DNDEBUG /LD ^
   "%ROOT%src\tempctl.c" "%ROOT%src\j1939.c" "%ROOT%src\canpack.c" tempctl.res ^
   /Fe:tempctl.dll /link /DEF:"%ROOT%src\tempctl.def"
if errorlevel 1 ( popd & exit /b 1 )
echo === test_tempctl.exe (win-%ARCH%) ===
cl /nologo /W4 /O2 /MT /std:c11 /DTEMPCTL_BUILD ^
   "%ROOT%tests\test_main.c" "%ROOT%src\tempctl.c" "%ROOT%src\j1939.c" "%ROOT%src\canpack.c" ^
   /Fe:test_tempctl.exe
if errorlevel 1 ( popd & exit /b 1 )
del /q *.obj *.res test_tempctl.lib test_tempctl.exp 2>nul
popd
exit /b 0
