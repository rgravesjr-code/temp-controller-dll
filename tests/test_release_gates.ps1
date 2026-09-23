$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$scratch = Join-Path $root ('build\release-gate-checks-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path "$scratch\build\linux-arm64", "$scratch\docs\testlogs", "$scratch\package" -Force | Out-Null
[IO.File]::WriteAllText("$scratch\build\linux-arm64\test_tempctl", 'test binary')
[IO.File]::WriteAllText("$scratch\build\linux-arm64\libtempctl.so", 'library binary')
$testHash = (Get-FileHash "$scratch\build\linux-arm64\test_tempctl").Hash
$libHash = (Get-FileHash "$scratch\build\linux-arm64\libtempctl.so").Hash
$logPath = "$scratch\docs\testlogs\pi-test_tempctl-2099-01-01.txt"
$scriptPath = Join-Path $root 'scripts\report_target_tests.ps1'
$count = 0
function Assert-Status([string]$body, [int]$code, [string]$pattern) {
    [IO.File]::WriteAllText($logPath, $body)
    $report = & powershell -NoProfile -ExecutionPolicy Bypass -File $scriptPath -Root $scratch -Version 4.0.0 -Rid linux-arm64 -Pattern 'pi-*.txt' 2>&1
    if ($LASTEXITCODE -ne $code -or -not ($report -match $pattern)) { throw "Unexpected target report: $report" }
    $script:count++
}
$valid = "# target: linux-arm64`n# test_sha256: $testHash`n# library_sha256: $libHash`nTempCtl 4.0.0 unit tests: 2194 passed, 0 failed`nexit=0`n"
Assert-Status 'old filename alone' 0 'NOT verified'
Assert-Status ($valid.Replace($testHash, '0' * 64)) 0 'NOT verified'
Assert-Status ($valid.Replace($libHash, '0' * 64)) 0 'NOT verified'
Assert-Status $valid 0 'test_tempctl EXECUTED'
Assert-Status ($valid.Replace('exit=0', 'exit=1')) 1 'ERROR'
Assert-Status ($valid.Replace('0 failed', '1 failed')) 1 'ERROR'
Assert-Status ($valid.Replace('exit=0', '')) 1 'ERROR'
Assert-Status ($valid.Replace('4.0.0', '3.0.0')) 1 'ERROR'
[IO.File]::WriteAllText($logPath, 'stale Pi log')
$report = & powershell -NoProfile -ExecutionPolicy Bypass -File $scriptPath -Root $scratch -Version 4.0.0 -Rid linux-arm64 -Pattern 'pi-*.txt' -RequireExecution 2>&1
if ($LASTEXITCODE -eq 0 -or -not ($report -match 'requires a successful execution log')) { throw 'Required Pi evidence was bypassed' }
$count++

# Execute the actual oracle guard with an external command that fails without printing FAIL.
$batch = Get-Content -LiteralPath (Join-Path $root 'package_dist.bat') -Raw
$guard = [regex]::Match($batch, '(?s)python "%ROOT%tests\\oracle_test.py"[^\r\n]*\r?\n(if %ERRORLEVEL% neq 0 \(.*?\r?\n\))').Groups[1].Value
if (-not $guard) { throw 'Cannot locate the package oracle exit-code guard' }
$probe = "@echo off`r`ncmd /c exit 7`r`n$guard`r`necho BAD_GATE_CONTINUED`r`nexit /b 0`r`n"
[IO.File]::WriteAllText("$scratch\oracle-guard.bat", $probe)
$report = & "$scratch\oracle-guard.bat" 2>&1
if ($LASTEXITCODE -eq 0 -or $report -match 'BAD_GATE_CONTINUED') { throw 'Oracle nonzero exit did not stop packaging' }
$count++

$ErrorActionPreference = 'Continue' # stderr is expected for this deliberately invalid package
$report = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\report_package_assets.ps1') -PackageDir "$scratch\package" -Version 4.0.0 -PeFiles 'missing.dll' 2>&1
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE -eq 0) { throw 'Missing required binary was accepted by the asset report' }
$count++
Write-Output "Release gate regressions: $count passed, 0 failed"
