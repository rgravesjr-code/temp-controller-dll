param(
    [Parameter(Mandatory = $true)][string]$PublishDir,
    [Parameter(Mandatory = $true)][string]$OutDir
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
foreach ($suite in @('TempSim.FixtureChecks', 'TempSim.RegressionChecks')) {
    $dest = Join-Path $OutDir $suite
    & dotnet publish (Join-Path $root "tests\$suite\$suite.csproj") -c Release -r win-x64 --self-contained false -o $dest -nologo -v q
    if ($LASTEXITCODE -ne 0) { throw "$suite build failed" }
    # Exercise the exact Core and native binaries being packaged, even if their publish is stale.
    foreach ($file in @('TempSim.Core.dll', 'tempctl.dll', 'cantp.dll', 'TempCtl.json', 'tempctl.ecd')) {
        Copy-Item -LiteralPath (Join-Path $PublishDir $file) -Destination $dest -Force
    }
    & (Join-Path $dest "$suite.exe")
    if ($LASTEXITCODE -ne 0) { throw "$suite failed" }
}
$partial = & (Join-Path $PublishDir 'TempSim.Cli.exe') --scenario all --seconds 0 --quiet --out (Join-Path $OutDir 'partial') 2>&1
if ($LASTEXITCODE -ne 2 -or $partial -match 'ALL OK' -or -not ($partial -match 'INCOMPLETE')) {
    throw 'A truncated scenario must exit 2 and report INCOMPLETE, never ALL OK'
}
Write-Output 'CLI truncated-run gate: correctly reports INCOMPLETE and exit 2'
foreach ($scenario in @('blocked-start', 'permissive-trip', 'reset')) {
    $png = Join-Path $OutDir "$scenario.png"
    $argsForShot = @('--screenshot', ('"' + $png + '"'), '--scenario', $scenario, '--seconds', '40')
    $shot = Start-Process -FilePath (Join-Path $PublishDir 'TempSim.exe') -ArgumentList $argsForShot -WindowStyle Hidden -Wait -PassThru
    if ($shot.ExitCode -ne 0) { throw "WPF $scenario failed (exit $($shot.ExitCode))" }
    $perf = Get-Content -LiteralPath ([IO.Path]::ChangeExtension($png, '.perf.txt')) -Raw
    if ($perf -notmatch '0 failed / 0 not yet due') { throw "WPF $scenario did not complete its expectations: $perf" }
    Write-Output $perf.Trim()
}
