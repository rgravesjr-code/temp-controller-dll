@echo off
setlocal
:: package_dist.bat - stage dist\TempCtl_vX.Y.Z, run the release gates into
:: TESTLOG.txt, write DEPENDENCIES.txt + MANIFEST.txt, zip (AES-256 + plain).
:: Usage: package_dist.bat X.Y.Z [password]     (run build.bat all first)

set "VERSION=%~1"
set "PASSWORD=%~2"
if "%VERSION%"=="" (
    echo ERROR: Version is required.
    echo   Usage: package_dist.bat X.Y.Z [password]
    echo   Example: package_dist.bat 1.0.0 scott
    exit /b 1
)
if "%PASSWORD%"=="" set "PASSWORD=scott"

set "ROOT=%~dp0"
set "ROOT_NOSLASH=%ROOT:~0,-1%"
set "OUT=%ROOT%dist\TempCtl_v%VERSION%"
set "ZIP=%ROOT%dist\TempCtl_v%VERSION%.zip"
set "ZIP_PLAIN=%ROOT%dist\TempCtl_v%VERSION%_unencrypted.zip"

echo ============================================================
echo  Packaging tempctl v%VERSION%
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
            "src\tempctl.h" "LICENSE" "DISTRIBUTION_README.md" "TEMPCTL_PACKAGE_GUIDE.md"
            "LABVIEW_INTEGRATION.md" "TESTING.md" "CHANGELOG.md"
            "examples\make_sample_ncl.py" "tests\oracle_test.py" "tools\elfinfo.py") do (
    if not exist "%ROOT%%%~F" (
        echo ERROR: %%~F not found. Run build.bat all first.
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
mkdir "%OUT%" "%OUT%\x86" "%OUT%\linux-x64" "%OUT%\examples" "%OUT%\src"

echo Copying files...
copy "%ROOT%build\win-x64\tempctl.dll"        "%OUT%\" >nul
copy "%ROOT%build\win-x64\tempctl.lib"        "%OUT%\" >nul
copy "%ROOT%build\win-x64\test_tempctl.exe"   "%OUT%\" >nul
copy "%ROOT%build\win-x86\tempctl.dll"        "%OUT%\x86\" >nul
copy "%ROOT%build\win-x86\tempctl.lib"        "%OUT%\x86\" >nul
copy "%ROOT%build\win-x86\test_tempctl.exe"   "%OUT%\x86\" >nul
copy "%ROOT%build\linux-x64\libtempctl.so"    "%OUT%\linux-x64\" >nul
copy "%ROOT%build\linux-x64\test_tempctl"     "%OUT%\linux-x64\" >nul
copy "%ROOT%src\tempctl.h"                    "%OUT%\" >nul
copy "%ROOT%DISTRIBUTION_README.md"           "%OUT%\" >nul
copy "%ROOT%TEMPCTL_PACKAGE_GUIDE.md"         "%OUT%\" >nul
copy "%ROOT%LABVIEW_INTEGRATION.md"           "%OUT%\" >nul
copy "%ROOT%TESTING.md"                       "%OUT%\" >nul
copy "%ROOT%CHANGELOG.md"                     "%OUT%\" >nul
copy "%ROOT%LICENSE"                          "%OUT%\LICENSE.txt" >nul
copy "%ROOT%examples\make_sample_ncl.py"      "%OUT%\examples\" >nul
copy "%ROOT%tests\oracle_test.py"             "%OUT%\examples\" >nul
copy "%ROOT%src\*.c"                          "%OUT%\src\" >nul
copy "%ROOT%src\*.h"                          "%OUT%\src\" >nul
copy "%ROOT%src\tempctl.def"                  "%OUT%\src\" >nul
copy "%ROOT%src\tempctl.rc"                   "%OUT%\src\" >nul
copy "%ROOT%tests\test_main.c"                "%OUT%\src\" >nul
copy "%ROOT%build.bat"                        "%OUT%\src\" >nul
copy "%ROOT%README.md"                        "%OUT%\src\repo-readme.md" >nul

echo Running release gates to capture TESTLOG.txt...
(
    echo tempctl v%VERSION% release-gate log, captured by package_dist.bat
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
echo ============ Python oracle (cantools + pretty_j1939) against tempctl.dll ============ >> "%OUT%\TESTLOG.txt"
set "ORACLE_ARGS="
if defined TEMPCTL_PYLIBS set "ORACLE_ARGS=--pylibs "%TEMPCTL_PYLIBS%""
python "%ROOT%tests\oracle_test.py" "%ROOT%build\win-x64\tempctl.dll" %ORACLE_ARGS% >> "%OUT%\TESTLOG.txt" 2>&1
if %ERRORLEVEL% neq 0 (
    echo WARNING: oracle test did not pass or could not run ^(python/cantools/pretty_j1939 missing?^). See TESTLOG.txt.
    echo   ^(oracle test skipped or failed - see above^) >> "%OUT%\TESTLOG.txt"
    set "ORACLE_FAILED=1"
)
if defined ORACLE_FAILED (
    findstr /C:"FAIL" "%OUT%\TESTLOG.txt" >nul && ( echo ERROR: oracle reported FAIL lines. Not packaging. & exit /b 1 )
)

echo. >> "%OUT%\TESTLOG.txt"
echo ============ example log regeneration ============ >> "%OUT%\TESTLOG.txt"
python "%ROOT%examples\make_sample_ncl.py" --dll "%ROOT%build\win-x64\tempctl.dll" --out "%OUT%\examples\sample_tempctl.ncl" >> "%OUT%\TESTLOG.txt" 2>&1
if %ERRORLEVEL% neq 0 (
    echo WARNING: could not regenerate the sample .ncl with python; copying the checked-in one.
    copy "%ROOT%examples\sample_tempctl.ncl" "%OUT%\examples\" >nul
    copy "%ROOT%examples\sample_tempctl.csv" "%OUT%\examples\" >nul
)

echo Generating DEPENDENCIES.txt and MANIFEST.txt...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\report_package_assets.ps1" ^
    -PackageDir "%OUT%" -Version "%VERSION%" -Password "%PASSWORD%" -ElfInfoScript "%ROOT%tools\elfinfo.py"
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
dir /s /b "%OUT%"
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
