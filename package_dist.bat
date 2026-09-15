@echo off
setlocal
:: package_dist.bat - stage dist\TempCtl_vX.Y.Z (the CONTROLLER package: libraries,
:: header, DBC + tables, the vendored CanTp subset, docs, sources), run the release
:: gates into TESTLOG.txt, write DEPENDENCIES.txt + MANIFEST.txt, zip (AES-256 + plain).
:: The simulator is a separate package: see package_sim.bat.
:: Usage: package_dist.bat X.Y.Z [password]
:: Run first:  build.bat all   python tools\make_tempctl_dbc.py --tables
:: Optional:   set TEMPCTL_PYLIBS=<dir with cantools> for the oracle
:: Everything under docs\package\ ships flat at the package root.

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
            "build\linux-arm64\libtempctl.so" "build\linux-arm64\test_tempctl"
            "third_party\cantp\cantp.h" "third_party\cantp\cantp.dll" "third_party\cantp\VENDORED.txt"
            "third_party\cantp\tools\dbc2tables.py"
            "dbc\tempctl.dbc" "dbc\tables\TempCtl.json" "dbc\tables\TempCtl.sig.csv"
            "src\tempctl.h" "LICENSE" "CHANGELOG.md"
            "docs\package\DISTRIBUTION_README.md" "docs\package\TEMPCTL_PACKAGE_GUIDE.md"
            "docs\package\LABVIEW_INTEGRATION.md" "docs\package\TESTING.md"
            "tests\oracle_test.py" "tools\elfinfo.py" "tools\make_tempctl_dbc.py") do (
    if not exist "%ROOT%%%~F" (
        echo ERROR: %%~F not found. Run build.bat all and make_tempctl_dbc.py --tables first.
        exit /b 1
    )
)

echo Checking version references...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\validate_package_version.ps1" -Version "%VERSION%" -Root "%ROOT_NOSLASH%"
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
echo.

echo Cleaning previous package...
if exist "%OUT%" rmdir /s /q "%OUT%"
if exist "%ZIP%" del /q "%ZIP%"
if exist "%ZIP_PLAIN%" del /q "%ZIP_PLAIN%"
if not exist "%ROOT%dist" mkdir "%ROOT%dist"
mkdir "%OUT%" "%OUT%\x86" "%OUT%\linux-x64" "%OUT%\linux-arm64" "%OUT%\examples" "%OUT%\src" "%OUT%\tools" "%OUT%\dbc"

echo Copying files...
copy "%ROOT%build\win-x64\tempctl.dll"        "%OUT%\" >nul
copy "%ROOT%build\win-x64\tempctl.lib"        "%OUT%\" >nul
copy "%ROOT%build\win-x64\test_tempctl.exe"   "%OUT%\" >nul
copy "%ROOT%build\win-x86\tempctl.dll"        "%OUT%\x86\" >nul
copy "%ROOT%build\win-x86\tempctl.lib"        "%OUT%\x86\" >nul
copy "%ROOT%build\win-x86\test_tempctl.exe"   "%OUT%\x86\" >nul
copy "%ROOT%build\linux-x64\libtempctl.so"    "%OUT%\linux-x64\" >nul
copy "%ROOT%build\linux-x64\test_tempctl"     "%OUT%\linux-x64\" >nul
copy "%ROOT%build\linux-arm64\libtempctl.so"  "%OUT%\linux-arm64\" >nul
copy "%ROOT%build\linux-arm64\test_tempctl"   "%OUT%\linux-arm64\" >nul
copy "%ROOT%src\tempctl.h"                    "%OUT%\" >nul
:: shipped documentation = docs\package\*.md, flat at the package root
copy "%ROOT%docs\package\*.md"                "%OUT%\" >nul
copy "%ROOT%CHANGELOG.md"                     "%OUT%\" >nul
copy "%ROOT%LICENSE"                          "%OUT%\LICENSE.txt" >nul
if exist "%ROOT%docs\testlogs" xcopy "%ROOT%docs\testlogs" "%OUT%\docs\testlogs\" /s /q /i >nul
:: vendored CanTp subset (header, binaries, dbc2tables.py, license, VENDORED.txt), unmodified
xcopy "%ROOT%third_party\cantp" "%OUT%\third_party\cantp\" /s /q /i >nul
:: DBC + tables
copy "%ROOT%dbc\tempctl.dbc"                  "%OUT%\dbc\" >nul
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
echo ============ DBC / table regeneration check ============ >> "%OUT%\TESTLOG.txt"
python "%ROOT%tools\make_tempctl_dbc.py" --out "%TEMP%\tempctl_check.dbc" >> "%OUT%\TESTLOG.txt" 2>&1
if %ERRORLEVEL% neq 0 ( echo ERROR: DBC generator failed. & exit /b 1 )
fc /b "%TEMP%\tempctl_check.dbc" "%ROOT%dbc\tempctl.dbc" >nul
if %ERRORLEVEL% neq 0 ( echo ERROR: dbc\tempctl.dbc is stale; rerun make_tempctl_dbc.py --tables & exit /b 1 )
echo dbc\tempctl.dbc matches the generator output. >> "%OUT%\TESTLOG.txt"

echo. >> "%OUT%\TESTLOG.txt"
echo ============ Python oracle (tempctl.dll + cantp.dll vs cantools) ============ >> "%OUT%\TESTLOG.txt"
set "ORACLE_ARGS="
if defined TEMPCTL_PYLIBS set "ORACLE_ARGS=--pylibs "%TEMPCTL_PYLIBS%""
python "%ROOT%tests\oracle_test.py" %ORACLE_ARGS% >> "%OUT%\TESTLOG.txt" 2>&1
if %ERRORLEVEL% neq 0 (
    echo WARNING: oracle test did not pass or could not run ^(python/cantools missing?^). See TESTLOG.txt.
    echo   ^(oracle test skipped or failed - see above^) >> "%OUT%\TESTLOG.txt"
    findstr /C:"FAIL" "%OUT%\TESTLOG.txt" >nul && ( echo ERROR: oracle reported FAIL lines. Not packaging. & exit /b 1 )
)

echo. >> "%OUT%\TESTLOG.txt"
echo ============ Linux ============ >> "%OUT%\TESTLOG.txt"
echo The closed-loop runs on the Raspberry Pi bench (linux-arm64 tempctl + cantp under the >> "%OUT%\TESTLOG.txt"
echo simulator) are recorded in the TempSim package ^(TESTLOG.txt and testlogs\ there^). >> "%OUT%\TESTLOG.txt"
if exist "%ROOT%docs\testlogs\*.txt" (
    for %%L in ("%ROOT%docs\testlogs\*.txt") do (
        echo --- %%~nxL --- >> "%OUT%\TESTLOG.txt"
        type "%%~L" >> "%OUT%\TESTLOG.txt"
        echo. >> "%OUT%\TESTLOG.txt"
    )
)

echo Generating DEPENDENCIES.txt and MANIFEST.txt...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\report_package_assets.ps1" ^
    -PackageDir "%OUT%" -Version "%VERSION%" -Product TempCtl -ElfInfoScript "%ROOT%tools\elfinfo.py" ^
    -PeFiles "tempctl.dll;x86\tempctl.dll;test_tempctl.exe;x86\test_tempctl.exe;third_party\cantp\cantp.dll;third_party\cantp\x86\cantp.dll" ^
    -ElfFiles "linux-x64\libtempctl.so;linux-x64\test_tempctl;linux-arm64\libtempctl.so;linux-arm64\test_tempctl;third_party\cantp\linux-x64\libcantp.so;third_party\cantp\linux-arm64\libcantp.so;third_party\cantp\linux-armhf\libcantp.so"
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

:Find7Zip
for %%I in (7z.exe) do (
    if not "%%~$PATH:I"=="" (
        set "SEVENZIP=%%~$PATH:I"
        exit /b 0
    )
)
if exist "C:\Program Files\7-Zip\7z.exe" set "SEVENZIP=C:\Program Files\7-Zip\7z.exe"
exit /b 0
