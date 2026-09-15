@echo off
setlocal
:: package_sim.bat - stage and zip the TempSim SIMULATOR packages, one per target:
::   dist\TempSim_vX.Y.Z_win-x64\      TempSim.exe (WPF) + TempSim.Cli.exe   (self-contained .NET 10)
::   dist\TempSim_vX.Y.Z_linux-x64\    TempSim.Cli                           (cRIO-class x86_64 Linux)
::   dist\TempSim_vX.Y.Z_linux-arm64\  TempSim.Cli                           (Raspberry Pi 4/5)
:: Each folder: TempSim\ (the publish), SIMULATOR.md, CHANGELOG.md, LICENSE.txt, TESTLOG.txt,
:: MANIFEST.txt, examples\ (screenshot + the failover scenario's CSV/.ncl from the gate run),
:: testlogs\ (Raspberry Pi logs), src\ (the simulator's C# source). Zipped AES-256 + plain.
:: TempSim has its own version (sim\Directory.Build.props); the tempctl / cantp versions it
:: embeds are printed by the programs and recorded in TESTLOG.txt.
:: Usage: package_sim.bat X.Y.Z [password] [rids]     rids default: win-x64 linux-x64 linux-arm64
:: Run first:  build.bat all   python tools\make_tempctl_dbc.py --tables   build_sim.bat all

set "VERSION=%~1"
set "PASSWORD=%~2"
set "RIDS=%~3"
if "%VERSION%"=="" (
    echo ERROR: Version is required.
    echo   Usage: package_sim.bat X.Y.Z [password] ["win-x64 linux-x64 linux-arm64"]
    exit /b 1
)
if "%PASSWORD%"=="" set "PASSWORD=scott"
if "%RIDS%"=="" set "RIDS=win-x64 linux-x64 linux-arm64"

set "ROOT=%~dp0"
set "ROOT_NOSLASH=%ROOT:~0,-1%"
set "SIMBUILD=%ROOT%build\sim"
set "GATE=%TEMP%\tempsim_gate"

echo ============================================================
echo  Packaging TempSim v%VERSION%  (simulator packages: %RIDS%)
echo ============================================================
echo.

call :Find7Zip
if not defined SEVENZIP ( echo ERROR: 7-Zip was not found. & exit /b 1 )
echo 7-Zip: %SEVENZIP%

for %%F in ("build\sim\win-x64\TempSim.exe" "build\sim\win-x64\TempSim.Cli.exe"
            "sim\SIMULATOR.md" "sim\CHANGELOG.md" "sim\Directory.Build.props" "LICENSE") do (
    if not exist "%ROOT%%%~F" (
        echo ERROR: %%~F not found. Run build.bat all, make_tempctl_dbc.py --tables and build_sim.bat all first.
        exit /b 1
    )
)
for %%R in (%RIDS%) do (
    if not exist "%SIMBUILD%\%%R\TempSim.Cli*" (
        echo ERROR: build\sim\%%R has no TempSim.Cli. Run build_sim.bat all first.
        exit /b 1
    )
)

echo Checking version references...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\validate_sim_version.ps1" -Version "%VERSION%" -Root "%ROOT_NOSLASH%"
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
echo.

:: ---- gates, run once on the Windows build (the Linux builds are the same source; their Pi logs ship in testlogs\) ----
echo Running the simulator gates (win-x64)...
if exist "%GATE%" rmdir /s /q "%GATE%"
mkdir "%GATE%"
(
    echo TempSim v%VERSION% release-gate log, captured by package_sim.bat
    echo.
    echo ============ TempSim.Cli, all scenarios ^(win-x64^) ============
) > "%GATE%\TESTLOG.txt"
"%SIMBUILD%\win-x64\TempSim.Cli.exe" --scenario all --out "%GATE%\out" --quiet >> "%GATE%\TESTLOG.txt" 2>&1
if %ERRORLEVEL% neq 0 ( echo ERROR: simulator CLI gate failed ^(unpack mismatches^). & exit /b 1 )
echo. >> "%GATE%\TESTLOG.txt"
echo ============ TempSim.exe headless screenshot ^(win-x64, sensor-failover, 70 s^) ============ >> "%GATE%\TESTLOG.txt"
"%SIMBUILD%\win-x64\TempSim.exe" --screenshot "%GATE%\tempsim-screenshot.png" --scenario sensor-failover --seconds 70
if %ERRORLEVEL% neq 0 ( echo ERROR: simulator screenshot gate failed. & exit /b 1 )
type "%GATE%\tempsim-screenshot.perf.txt" >> "%GATE%\TESTLOG.txt"
if exist "%ROOT%sim\testlogs\*.txt" (
    echo. >> "%GATE%\TESTLOG.txt"
    echo ============ Linux runs ^(pasted from sim\testlogs, captured on the Raspberry Pi bench^) ============ >> "%GATE%\TESTLOG.txt"
    for %%L in ("%ROOT%sim\testlogs\*.txt") do (
        echo --- %%~nxL --- >> "%GATE%\TESTLOG.txt"
        type "%%~L" >> "%GATE%\TESTLOG.txt"
        echo. >> "%GATE%\TESTLOG.txt"
    )
)

if not exist "%ROOT%dist" mkdir "%ROOT%dist"
for %%R in (%RIDS%) do (
    call :stage %%R
    if errorlevel 1 exit /b 1
)

echo.
echo Done. Packages in dist\:
dir /b "%ROOT%dist\TempSim_v%VERSION%_*"
exit /b 0

:: ---- subroutine: stage + zip one RID ----------------------------------------
:stage
setlocal
set "RID=%~1"
set "OUT=%ROOT%dist\TempSim_v%VERSION%_%RID%"
set "ZIP=%OUT%.zip"
set "ZIP_PLAIN=%OUT%_unencrypted.zip"
echo === %RID% ===
if exist "%OUT%" rmdir /s /q "%OUT%"
if exist "%ZIP%" del /q "%ZIP%"
if exist "%ZIP_PLAIN%" del /q "%ZIP_PLAIN%"
mkdir "%OUT%" "%OUT%\examples" "%OUT%\src"
xcopy "%SIMBUILD%\%RID%" "%OUT%\TempSim\" /s /q /i >nul
del /q "%OUT%\TempSim\*.pdb" 2>nul
copy "%ROOT%sim\SIMULATOR.md"                 "%OUT%\" >nul
copy "%ROOT%sim\CHANGELOG.md"                 "%OUT%\" >nul
copy "%ROOT%LICENSE"                          "%OUT%\LICENSE.txt" >nul
copy "%GATE%\TESTLOG.txt"                     "%OUT%\" >nul
copy "%GATE%\tempsim-screenshot.png"          "%OUT%\examples\" >nul
copy "%GATE%\out\sensor-failover.csv"         "%OUT%\examples\" >nul
copy "%GATE%\out\sensor-failover.ncl"         "%OUT%\examples\" >nul
if exist "%ROOT%sim\testlogs" xcopy "%ROOT%sim\testlogs" "%OUT%\testlogs\" /s /q /i >nul
for %%P in (TempSim.Core TempSim.Cli TempSim.Wpf) do (
    xcopy "%ROOT%sim\%%P" "%OUT%\src\%%P\" /s /q /i /exclude:%ROOT%scripts\xcopy_exclude.txt >nul
)
copy "%ROOT%sim\NativeAssets.targets"         "%OUT%\src\" >nul
copy "%ROOT%sim\Directory.Build.props"        "%OUT%\src\" >nul
copy "%ROOT%build_sim.bat"                    "%OUT%\src\" >nul
copy "%ROOT%README.md"                        "%OUT%\src\repo-readme.md" >nul

powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\report_package_assets.ps1" ^
    -PackageDir "%OUT%" -Version "%VERSION%" -Product "TempSim %RID%"
if %ERRORLEVEL% neq 0 ( echo ERROR: manifest generation failed. & exit /b 1 )

pushd "%OUT%" >nul
"%SEVENZIP%" a -tzip -mem=AES256 -p%PASSWORD% "%ZIP%" * >nul
set "ZIP_RC=%ERRORLEVEL%"
popd >nul
if not "%ZIP_RC%"=="0" ( echo FAILED to create zip. & exit /b %ZIP_RC% )
"%SEVENZIP%" t -p%PASSWORD% "%ZIP%" >nul
if %ERRORLEVEL% neq 0 ( echo FAILED to verify zip. & exit /b 1 )
pushd "%OUT%" >nul
"%SEVENZIP%" a -tzip "%ZIP_PLAIN%" * >nul
set "ZIP_PLAIN_RC=%ERRORLEVEL%"
popd >nul
if not "%ZIP_PLAIN_RC%"=="0" ( echo FAILED to create unencrypted zip. & exit /b %ZIP_PLAIN_RC% )
"%SEVENZIP%" t "%ZIP_PLAIN%" >nul
if %ERRORLEVEL% neq 0 ( echo FAILED to verify zip. & exit /b 1 )
echo   %ZIP%
echo   %ZIP_PLAIN%
exit /b 0

:Find7Zip
for %%I in (7z.exe) do (
    if not "%%~$PATH:I"=="" (
        set "SEVENZIP=%%~$PATH:I"
        exit /b 0
    )
)
if exist "C:\Program Files\7-Zip\7z.exe" set "SEVENZIP=C:\Program Files\7-Zip\7z.exe"
exit /b 0
