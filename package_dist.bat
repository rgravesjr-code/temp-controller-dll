@echo off
setlocal
:: package_dist.bat - stage dist\TempCtl_vX.Y.Z (the CONTROLLER package: libraries,
:: header, DBC + tables + ECD, the vendored CanTp subset, docs, sources), run the release
:: gates into TESTLOG.txt, write DEPENDENCIES.txt + MANIFEST.txt, zip (AES-256 + plain).
:: The simulator is a separate package: see package_sim.bat.
:: Usage: package_dist.bat X.Y.Z [password]
:: Run first:  build.bat all   python tools\make_tempctl_dbc.py --tables --ecd
:: Optional:   set TEMPCTL_PYLIBS=<dir with cantools> for the oracle and the regeneration check
:: Everything under docs\package\ ships flat at the package root.
:: Target execution logs (owner-run): docs\testlogs\pi-*.txt, crio-*.txt, myrio-*.txt.
:: Windows x64/x86 gates, the regeneration check and the oracle are hard gates; a Linux target
:: without a log is recorded as "built and inspected, not executed" for cRIO/myRIO (V4-D5).
:: A matching successful Pi execution log remains required.

set "VERSION=%~1"
set "PASSWORD=%~2"
if "%VERSION%"=="" (
    echo ERROR: Version is required.
    echo   Usage: package_dist.bat X.Y.Z [password]
    exit /b 1
)
if "%PASSWORD%"=="" set "PASSWORD=scott"

set "ROOT=%~dp0"
set "ROOT_NOSLASH=%ROOT:~0,-1%"
set "OUT=%ROOT%dist\TempCtl_v%VERSION%"
set "ZIP=%ROOT%dist\TempCtl_v%VERSION%.zip"
set "ZIP_PLAIN=%ROOT%dist\TempCtl_v%VERSION%_unencrypted.zip"
set "PYARGS="
if defined TEMPCTL_PYLIBS set "PYARGS=--pylibs "%TEMPCTL_PYLIBS%""

echo ============================================================
echo  Packaging TempCtl v%VERSION%  (controller package)
echo ============================================================
echo.

call :Find7Zip
if not defined SEVENZIP (
    echo ERROR: 7-Zip was not found. Install 7-Zip or add 7z.exe to PATH.
    exit /b 1
)
echo 7-Zip: %SEVENZIP%

for %%F in ("build\win-x64\tempctl.dll" "build\win-x64\tempctl.lib" "build\win-x64\test_tempctl.exe"
            "build\win-x86\tempctl.dll" "build\win-x86\tempctl.lib" "build\win-x86\test_tempctl.exe"
            "build\linux-x64\libtempctl.so" "build\linux-x64\test_tempctl"
            "build\linux-armhf\libtempctl.so" "build\linux-armhf\test_tempctl"
            "build\linux-arm64\libtempctl.so" "build\linux-arm64\test_tempctl"
            "third_party\cantp\cantp.h" "third_party\cantp\cantp.dll" "third_party\cantp\VENDORED.txt"
            "third_party\cantp\tools\dbc2tables.py" "third_party\cantp\tools\ecdflat.py"
            "third_party\cantp\linux-armhf\libcantp.so"
            "dbc\tempctl.dbc" "dbc\tempctl.ecd" "dbc\tables\TempCtl.json" "dbc\tables\TempCtl.sig.csv"
            "src\tempctl.h" "LICENSE" "CHANGELOG.md"
            "docs\package\DISTRIBUTION_README.md" "docs\package\TEMPCTL_PACKAGE_GUIDE.md"
            "docs\package\LABVIEW_INTEGRATION.md" "docs\package\TESTING.md"
            "docs\package\TEMPCTL-SPEC-v%VERSION%.md" "docs\package\TEMPCTL-CAPABILITY-v%VERSION%.md"
            "docs\package\TEMPCTL-v%VERSION%-API-AND-LABVIEW-GUIDE.md"
            "tests\oracle_test.py" "tools\elfinfo.py" "tools\make_tempctl_dbc.py") do (
    if not exist "%ROOT%%%~F" (
        echo ERROR: %%~F not found. Run build.bat all and make_tempctl_dbc.py --tables --ecd first.
        exit /b 1
    )
)

echo Checking version references...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tests\test_release_gates.ps1"
if errorlevel 1 ( echo ERROR: release-gate regression checks failed. & exit /b 1 )
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\validate_package_version.ps1" -Version "%VERSION%" -Root "%ROOT_NOSLASH%"
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
echo.

echo Cleaning previous package...
if exist "%OUT%" rmdir /s /q "%OUT%"
if exist "%ZIP%" del /q "%ZIP%"
if exist "%ZIP_PLAIN%" del /q "%ZIP_PLAIN%"
if not exist "%ROOT%dist" mkdir "%ROOT%dist"
mkdir "%OUT%" "%OUT%\x86" "%OUT%\linux-x64" "%OUT%\linux-armhf" "%OUT%\linux-arm64" "%OUT%\examples" "%OUT%\src" "%OUT%\tools" "%OUT%\dbc"

echo Copying files...
copy "%ROOT%build\win-x64\tempctl.dll"        "%OUT%\" >nul
copy "%ROOT%build\win-x64\tempctl.lib"        "%OUT%\" >nul
copy "%ROOT%build\win-x64\test_tempctl.exe"   "%OUT%\" >nul
copy "%ROOT%build\win-x86\tempctl.dll"        "%OUT%\x86\" >nul
copy "%ROOT%build\win-x86\tempctl.lib"        "%OUT%\x86\" >nul
copy "%ROOT%build\win-x86\test_tempctl.exe"   "%OUT%\x86\" >nul
copy "%ROOT%build\linux-x64\libtempctl.so"    "%OUT%\linux-x64\" >nul
copy "%ROOT%build\linux-x64\test_tempctl"     "%OUT%\linux-x64\" >nul
copy "%ROOT%build\linux-armhf\libtempctl.so"  "%OUT%\linux-armhf\" >nul
copy "%ROOT%build\linux-armhf\test_tempctl"   "%OUT%\linux-armhf\" >nul
copy "%ROOT%build\linux-arm64\libtempctl.so"  "%OUT%\linux-arm64\" >nul
copy "%ROOT%build\linux-arm64\test_tempctl"   "%OUT%\linux-arm64\" >nul
:: the same header next to every binary (V4-11), byte-identical, verified below
for %%D in ("" "x86\" "linux-x64\" "linux-armhf\" "linux-arm64\") do copy "%ROOT%src\tempctl.h" "%OUT%\%%~D" >nul
for %%D in ("x86\" "linux-x64\" "linux-armhf\" "linux-arm64\") do (
    fc /b "%OUT%\tempctl.h" "%OUT%\%%~Dtempctl.h" >nul
    if errorlevel 1 ( echo ERROR: header copy %%~Dtempctl.h differs. & exit /b 1 )
)
:: shipped documentation = docs\package\*.md, flat at the package root
copy "%ROOT%docs\package\*.md"                "%OUT%\" >nul
copy "%ROOT%CHANGELOG.md"                     "%OUT%\" >nul
copy "%ROOT%LICENSE"                          "%OUT%\LICENSE.txt" >nul
if exist "%ROOT%docs\testlogs" xcopy "%ROOT%docs\testlogs" "%OUT%\docs\testlogs\" /s /q /i >nul
:: vendored CanTp subset (header, binaries, dbc2tables.py, ecdflat.py, license, VENDORED.txt), unmodified
xcopy "%ROOT%third_party\cantp" "%OUT%\third_party\cantp\" /s /q /i >nul
if exist "%OUT%\third_party\cantp\tools\__pycache__" rmdir /s /q "%OUT%\third_party\cantp\tools\__pycache__"
:: DBC + ECD + tables
copy "%ROOT%dbc\tempctl.dbc"                  "%OUT%\dbc\" >nul
copy "%ROOT%dbc\tempctl.ecd"                  "%OUT%\dbc\" >nul
xcopy "%ROOT%dbc\tables" "%OUT%\dbc\tables\" /s /q /i >nul
:: examples + tools
copy "%ROOT%tests\oracle_test.py"             "%OUT%\examples\" >nul
copy "%ROOT%tools\make_tempctl_dbc.py"        "%OUT%\tools\" >nul
copy "%ROOT%tools\elfinfo.py"                 "%OUT%\tools\" >nul
:: sources
copy "%ROOT%src\*.c"                          "%OUT%\src\" >nul
copy "%ROOT%src\*.h"                          "%OUT%\src\" >nul
copy "%ROOT%src\tempctl.def"                  "%OUT%\src\" >nul
copy "%ROOT%src\tempctl.rc"                   "%OUT%\src\" >nul
copy "%ROOT%tests\test_main.c"                "%OUT%\src\" >nul
copy "%ROOT%build.bat"                        "%OUT%\src\" >nul
copy "%ROOT%README.md"                        "%OUT%\src\repo-readme.md" >nul

echo Running release gates to capture TESTLOG.txt...
(
    echo TempCtl v%VERSION% release-gate log, captured by package_dist.bat
    echo.
    echo ============ x64 gate ============
) > "%OUT%\TESTLOG.txt"
"%ROOT%build\win-x64\test_tempctl.exe" >> "%OUT%\TESTLOG.txt" 2>&1
if %ERRORLEVEL% neq 0 ( echo ERROR: x64 gate failed. Not packaging a broken build. & exit /b 1 )
echo. >> "%OUT%\TESTLOG.txt"
echo ============ x86 gate ============ >> "%OUT%\TESTLOG.txt"
"%ROOT%build\win-x86\test_tempctl.exe" >> "%OUT%\TESTLOG.txt" 2>&1
if %ERRORLEVEL% neq 0 ( echo ERROR: x86 gate failed. Not packaging a broken build. & exit /b 1 )

echo. >> "%OUT%\TESTLOG.txt"
echo ============ DBC / table / ECD regeneration check ============ >> "%OUT%\TESTLOG.txt"
if exist "%TEMP%\tempctl_check" rmdir /s /q "%TEMP%\tempctl_check"
mkdir "%TEMP%\tempctl_check"
set "PYTHONPATH_SAVED=%PYTHONPATH%"
if defined TEMPCTL_PYLIBS set "PYTHONPATH=%TEMPCTL_PYLIBS%;%PYTHONPATH%"
python "%ROOT%tools\make_tempctl_dbc.py" --out "%TEMP%\tempctl_check\tempctl.dbc" --tables --ecd >> "%OUT%\TESTLOG.txt" 2>&1
set "GEN_RC=%ERRORLEVEL%"
set "PYTHONPATH=%PYTHONPATH_SAVED%"
if %GEN_RC% neq 0 ( echo ERROR: DBC / ECD generator failed. & exit /b 1 )
fc /b "%TEMP%\tempctl_check\tempctl.dbc" "%ROOT%dbc\tempctl.dbc" >nul
if %ERRORLEVEL% neq 0 ( echo ERROR: dbc\tempctl.dbc is stale; rerun make_tempctl_dbc.py --tables --ecd & exit /b 1 )
fc /b "%TEMP%\tempctl_check\tempctl.ecd" "%ROOT%dbc\tempctl.ecd" >nul
if %ERRORLEVEL% neq 0 ( echo ERROR: dbc\tempctl.ecd is stale; rerun make_tempctl_dbc.py --tables --ecd & exit /b 1 )
fc /b "%TEMP%\tempctl_check\tables\TempCtl.json" "%ROOT%dbc\tables\TempCtl.json" >nul
if %ERRORLEVEL% neq 0 ( echo ERROR: dbc\tables\TempCtl.json is stale; rerun make_tempctl_dbc.py --tables --ecd & exit /b 1 )
echo dbc\tempctl.dbc, dbc\tempctl.ecd and dbc\tables\TempCtl.json match the generator output. >> "%OUT%\TESTLOG.txt"

echo. >> "%OUT%\TESTLOG.txt"
echo ============ Python oracle (tempctl.dll + cantp.dll vs cantools; tables and ECD) ============ >> "%OUT%\TESTLOG.txt"
python "%ROOT%tests\oracle_test.py" %PYARGS% >> "%OUT%\TESTLOG.txt" 2>&1
if %ERRORLEVEL% neq 0 (
    echo ERROR: oracle failed or could not run. See TESTLOG.txt. Not packaging.
    exit /b 1
)

echo. >> "%OUT%\TESTLOG.txt"
echo ============ Linux targets ============ >> "%OUT%\TESTLOG.txt"
echo Every .so and test_tempctl was cross-built from the same source and ELF-inspected here ^(DEPENDENCIES.txt^). >> "%OUT%\TESTLOG.txt"
echo Execution on the target is recorded by a dated log in docs\testlogs\ ^(owner-run^): >> "%OUT%\TESTLOG.txt"
call :TargetStatus linux-arm64 "pi-test_tempctl-*.txt" -RequireExecution
if errorlevel 1 exit /b 1
call :TargetStatus linux-x64 "crio-test_tempctl-*.txt"
if errorlevel 1 exit /b 1
call :TargetStatus linux-armhf "myrio-test_tempctl-*.txt"
if errorlevel 1 exit /b 1
echo The closed-loop simulator runs on the Raspberry Pi bench ^(linux-arm64 tempctl + cantp under TempSim^) are >> "%OUT%\TESTLOG.txt"
echo recorded in the TempSim package ^(TESTLOG.txt and testlogs\ there^). LabVIEW and LabVIEW RT wrapper tests are >> "%OUT%\TESTLOG.txt"
echo owner-executed ^(TESTING.md^). >> "%OUT%\TESTLOG.txt"
if exist "%ROOT%docs\testlogs\*.txt" (
    for %%L in ("%ROOT%docs\testlogs\*.txt") do (
        echo. >> "%OUT%\TESTLOG.txt"
        echo --- %%~nxL --- >> "%OUT%\TESTLOG.txt"
        type "%%~L" >> "%OUT%\TESTLOG.txt"
    )
)

echo Generating DEPENDENCIES.txt and MANIFEST.txt...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\report_package_assets.ps1" ^
    -PackageDir "%OUT%" -Version "%VERSION%" -Product TempCtl -ElfInfoScript "%ROOT%tools\elfinfo.py" ^
    -PeFiles "tempctl.dll;x86\tempctl.dll;test_tempctl.exe;x86\test_tempctl.exe;third_party\cantp\cantp.dll;third_party\cantp\x86\cantp.dll" ^
    -ElfFiles "linux-x64\libtempctl.so;linux-x64\test_tempctl;linux-armhf\libtempctl.so;linux-armhf\test_tempctl;linux-arm64\libtempctl.so;linux-arm64\test_tempctl;third_party\cantp\linux-x64\libcantp.so;third_party\cantp\linux-armhf\libcantp.so;third_party\cantp\linux-arm64\libcantp.so"
if %ERRORLEVEL% neq 0 ( echo ERROR: asset report generation failed. & exit /b 1 )

echo Creating encrypted zip...
pushd "%OUT%" >nul
"%SEVENZIP%" a -tzip -mem=AES256 -p%PASSWORD% "%ZIP%" * >nul
set "ZIP_RC=%ERRORLEVEL%"
popd >nul
if not "%ZIP_RC%"=="0" ( echo FAILED to create zip. & exit /b %ZIP_RC% )
"%SEVENZIP%" t -p%PASSWORD% "%ZIP%" >nul
if %ERRORLEVEL% neq 0 ( echo FAILED to verify zip. & exit /b 1 )

echo Creating unencrypted zip...
pushd "%OUT%" >nul
"%SEVENZIP%" a -tzip "%ZIP_PLAIN%" * >nul
set "ZIP_PLAIN_RC=%ERRORLEVEL%"
popd >nul
if not "%ZIP_PLAIN_RC%"=="0" ( echo FAILED to create unencrypted zip. & exit /b %ZIP_PLAIN_RC% )
"%SEVENZIP%" t "%ZIP_PLAIN%" >nul
if %ERRORLEVEL% neq 0 ( echo FAILED to verify zip. & exit /b 1 )

echo.
echo Done:
echo   Folder: %OUT%
echo   Zip:    %ZIP%
echo   Plain:  %ZIP_PLAIN%
echo.
dir /b "%OUT%"
exit /b 0

:: ---- %1 RID, %2 log file pattern under docs\testlogs ----------------------
:TargetStatus
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\report_target_tests.ps1" -Root "%ROOT_NOSLASH%" -Version "%VERSION%" -Rid "%~1" -Pattern "%~2" %~3 >> "%OUT%\TESTLOG.txt" 2>&1
exit /b %ERRORLEVEL%

:Find7Zip
for %%I in (7z.exe) do (
    if not "%%~$PATH:I"=="" (
        set "SEVENZIP=%%~$PATH:I"
        exit /b 0
    )
)
if exist "C:\Program Files\7-Zip\7z.exe" set "SEVENZIP=C:\Program Files\7-Zip\7z.exe"
exit /b 0
