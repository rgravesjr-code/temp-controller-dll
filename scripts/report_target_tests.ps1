param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][ValidateSet('linux-x64','linux-armhf','linux-arm64')][string]$Rid,
    [Parameter(Mandatory = $true)][string]$Pattern,
    [switch]$RequireExecution
)
$ErrorActionPreference = 'Stop'
$testHash = (Get-FileHash -LiteralPath (Join-Path $Root "build\$Rid\test_tempctl") -Algorithm SHA256).Hash
$libHash = (Get-FileHash -LiteralPath (Join-Path $Root "build\$Rid\libtempctl.so") -Algorithm SHA256).Hash
$logs = @(Get-ChildItem -Path (Join-Path $Root "docs\testlogs\$Pattern") -File -ErrorAction SilentlyContinue | Sort-Object Name -Descending)
foreach ($log in $logs) {
    $body = Get-Content -LiteralPath $log.FullName -Raw
    if ($body -notmatch "(?im)^# target: $([regex]::Escape($Rid))\s*$" -or
        $body -notmatch "(?im)^# test_sha256: $testHash\s*$" -or
        $body -notmatch "(?im)^# library_sha256: $libHash\s*$") { continue }
    $summary = [regex]::Match($body, "(?m)^TempCtl $([regex]::Escape($Version)) unit tests: ([1-9][0-9]*) passed, 0 failed\s*$")
    if (-not $summary.Success -or $body -notmatch '(?m)^exit=0\s*$' -or
        $body -match '(?m)^exit=(?!0\s*$)' -or $body -match '(?m)^TempCtl .* unit tests: .* [1-9][0-9]* failed') {
        Write-Output "ERROR: $Rid current-build target log failed or is incomplete: $($log.Name)"
        exit 1
    }
    Write-Output "$Rid`: test_tempctl EXECUTED on target; $($summary.Groups[1].Value) passed; version and both SHA-256 hashes match; log $($log.Name)"
    exit 0
}
Write-Output "$Rid`: built and ELF-inspected, NOT verified as executed for these binaries (V4-D5). $($logs.Count) historical/unqualified log(s)."
if ($RequireExecution) {
    Write-Output "ERROR: $Rid requires a successful execution log matching the current binaries."
    exit 1
}
exit 0
