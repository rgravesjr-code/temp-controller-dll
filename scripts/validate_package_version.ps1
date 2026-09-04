# Checks that the requested package version matches tempctl.h and is
# mentioned in CHANGELOG.md and DISTRIBUTION_README.md. Called by package_dist.bat.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)
$ErrorActionPreference = 'Stop'
$fail = [System.Collections.Generic.List[string]]::new()

if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)$') { Write-Host "ERROR: version must be X.Y.Z"; exit 1 }
$maj = [int]$Matches[1]; $min = [int]$Matches[2]; $pat = [int]$Matches[3]

$h = Get-Content -LiteralPath (Join-Path $Root 'src\tempctl.h') -Raw
foreach ($pair in @(@('MAJOR', $maj), @('MINOR', $min), @('PATCH', $pat))) {
    $name = $pair[0]; $want = $pair[1]
    if ($h -notmatch "#define\s+TC_VERSION_$name\s+(\d+)") { $fail.Add("tempctl.h lacks TC_VERSION_$name"); continue }
    if ([int]$Matches[1] -ne $want) { $fail.Add("tempctl.h TC_VERSION_$name is $($Matches[1]), package says $want") }
}
$esc = [regex]::Escape($Version)
foreach ($doc in @('CHANGELOG.md', 'DISTRIBUTION_README.md')) {
    $t = Get-Content -LiteralPath (Join-Path $Root $doc) -Raw
    if ($t -notmatch "v$esc") { $fail.Add("$doc does not mention v$Version") }
}
if ($fail.Count) { foreach ($f in $fail) { Write-Host "ERROR: $f" }; exit 1 }
Write-Host "Version $Version consistent across tempctl.h, CHANGELOG.md, DISTRIBUTION_README.md."
exit 0
