# Checks that the requested TempSim package version matches sim\Directory.Build.props
# and is mentioned in sim\CHANGELOG.md. Called by package_sim.bat.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)
$ErrorActionPreference = 'Stop'
$fail = [System.Collections.Generic.List[string]]::new()

if ($Version -notmatch '^\d+\.\d+\.\d+$') { Write-Host "ERROR: version must be X.Y.Z"; exit 1 }

$props = Get-Content -LiteralPath (Join-Path $Root 'sim\Directory.Build.props') -Raw
if ($props -notmatch '<Version>([^<]+)</Version>') { $fail.Add('sim\Directory.Build.props has no <Version>') }
elseif ($Matches[1] -ne $Version) { $fail.Add("sim\Directory.Build.props says $($Matches[1]), package says $Version") }

$esc = [regex]::Escape($Version)
$t = Get-Content -LiteralPath (Join-Path $Root 'sim\CHANGELOG.md') -Raw
if ($t -notmatch "v$esc") { $fail.Add("sim\CHANGELOG.md does not mention v$Version") }

if ($fail.Count) { foreach ($f in $fail) { Write-Host "ERROR: $f" }; exit 1 }
Write-Host "TempSim version $Version consistent across sim\Directory.Build.props and sim\CHANGELOG.md."
exit 0
