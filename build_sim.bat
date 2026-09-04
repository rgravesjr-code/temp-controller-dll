@echo off
setlocal
:: build_sim.bat - publish the TempSim simulator (self-contained .NET 10) for every target:
::   build\sim\win-x64\      TempSim.exe (WPF) + TempSim.Cli.exe
::   build\sim\linux-x64\    TempSim.Cli  (cRIO-904x/905x/906x class x86_64 Linux)
::   build\sim\linux-arm64\  TempSim.Cli  (Raspberry Pi 4/5)
:: Run build.bat all first (tempctl binaries); cantp comes from third_party\cantp. Usage: build_sim.bat [all|win|linux|shot]
set "ROOT=%~dp0"
set "MODE=%~1"
if "%MODE%"=="" set "MODE=all"
set "SIM=%ROOT%sim"
set "OUT=%ROOT%build\sim"
where dotnet >nul 2>&1 || ( echo ERROR: dotnet SDK not found ^(.NET 10 SDK required^). & exit /b 1 )
if not exist "%ROOT%dbc\tables\TempCtl.json" ( echo ERROR: dbc\tables\TempCtl.json missing - run: python tools\make_tempctl_dbc.py --tables & exit /b 1 )

if /i "%MODE%"=="linux" goto :linux
echo === TempSim (WPF) + TempSim.Cli win-x64 ===
dotnet publish "%SIM%\TempSim.Wpf\TempSim.Wpf.csproj" -c Release -r win-x64 --self-contained -o "%OUT%\win-x64" -nologo -v q
if errorlevel 1 exit /b 1
dotnet publish "%SIM%\TempSim.Cli\TempSim.Cli.csproj" -c Release -r win-x64 --self-contained -o "%OUT%\win-x64" -nologo -v q
if errorlevel 1 exit /b 1
if /i "%MODE%"=="win" goto :done
if /i "%MODE%"=="shot" goto :shot

:linux
for %%R in (linux-x64 linux-arm64) do (
    echo === TempSim.Cli %%R ===
    dotnet publish "%SIM%\TempSim.Cli\TempSim.Cli.csproj" -c Release -r %%R --self-contained -o "%OUT%\%%R" -nologo -v q
    if errorlevel 1 exit /b 1
)
if /i "%MODE%"=="linux" goto :done

:shot
echo === headless checks (win-x64) ===
"%OUT%\win-x64\TempSim.Cli.exe" --scenario all --out "%OUT%\win-x64-out" --quiet
if errorlevel 1 ( echo ERROR: TempSim.Cli reported unpack mismatches & exit /b 1 )
"%OUT%\win-x64\TempSim.exe" --screenshot "%OUT%\tempsim-screenshot.png" --scenario sensor-failover --seconds 70
if errorlevel 1 ( echo ERROR: TempSim screenshot run failed & exit /b 1 )
type "%OUT%\tempsim-screenshot.perf.txt"

:done
echo.
echo Simulator outputs under build\sim\
exit /b 0
