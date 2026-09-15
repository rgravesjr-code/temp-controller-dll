# Generates DEPENDENCIES.txt (PE import tables of the DLLs/exes, ELF report of
# the .so files) and MANIFEST.txt (file list with SHA-256) inside a staged
# distribution folder. Called by the package_*.bat scripts. Generic: the
# product name and the audited files are parameters, so the same script serves
# every native-DLL package (CanTp, TempCtl, ...). Pass no -PeFiles/-ElfFiles to
# write only the manifest (pure .NET or document packages). File lists are
# semicolon-separated strings (comma lists do not survive powershell -File).
param(
    [Parameter(Mandatory = $true)][string]$PackageDir,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Product = 'package',
    [string]$PeFiles = '',      # semicolon-separated, relative to PackageDir
    [string]$ElfFiles = '',     # semicolon-separated, relative to PackageDir
    [string]$ElfInfoScript = '',
    [string]$Note = ''
)

$ErrorActionPreference = 'Stop'
# New variables on purpose: assigning an array back to a [string]-typed parameter
# would coerce it to one space-joined string.
[string[]]$peList  = @($PeFiles  -split ';' | Where-Object { $_ })
[string[]]$elfList = @($ElfFiles -split ';' | Where-Object { $_ })

function Get-PeImports {
    param([string]$Path)
    $b = [System.IO.File]::ReadAllBytes($Path)
    $peOff = [BitConverter]::ToInt32($b, 0x3C)
    if ([BitConverter]::ToUInt32($b, $peOff) -ne 0x4550) { throw "$Path is not a PE file" }
    $machine = [BitConverter]::ToUInt16($b, $peOff + 4)
    $numSections = [BitConverter]::ToUInt16($b, $peOff + 6)
    $optSize = [BitConverter]::ToUInt16($b, $peOff + 20)
    $optOff = $peOff + 24
    $magic = [BitConverter]::ToUInt16($b, $optOff)
    $dirOff = $optOff + $(if ($magic -eq 0x20B) { 112 } else { 96 })
    $importRva = [BitConverter]::ToUInt32($b, $dirOff + 8)
    $sections = @()
    $secOff = $optOff + $optSize
    for ($i = 0; $i -lt $numSections; $i++) {
        $o = $secOff + $i * 40
        $sections += [pscustomobject]@{
            VirtualAddress = [BitConverter]::ToUInt32($b, $o + 12)
            SizeOfRawData  = [BitConverter]::ToUInt32($b, $o + 16)
            PointerToRaw   = [BitConverter]::ToUInt32($b, $o + 20)
        }
    }
    function RvaToOff([uint32]$rva) {
        foreach ($s in $sections) {
            if ($rva -ge $s.VirtualAddress -and $rva -lt ($s.VirtualAddress + $s.SizeOfRawData)) {
                return $s.PointerToRaw + ($rva - $s.VirtualAddress)
            }
        }
        throw "RVA 0x$($rva.ToString('X')) not mapped"
    }
    $imports = @()
    if ($importRva -ne 0) {
        $desc = RvaToOff $importRva
        while ($true) {
            $nameRva = [BitConverter]::ToUInt32($b, $desc + 12)
            if ($nameRva -eq 0) { break }
            $nOff = RvaToOff $nameRva
            $end = $nOff
            while ($b[$end] -ne 0) { $end++ }
            $imports += [System.Text.Encoding]::ASCII.GetString($b, $nOff, $end - $nOff)
            $desc += 20
        }
    }
    [pscustomobject]@{
        Machine = if ($machine -eq 0x8664) { 'x64' } elseif ($machine -eq 0x14C) { 'x86' } else { "0x$($machine.ToString('X'))" }
        Imports = $imports
    }
}

$bad = $false
if ($peList.Count -or $elfList.Count) {
    $lines = @("$Product v$Version dependency report (generated at package time)", '')
    if ($Note) { $lines += $Note; $lines += '' }
    foreach ($rel in $peList) {
        $p = Join-Path $PackageDir $rel
        if (-not (Test-Path -LiteralPath $p)) { continue }
        $info = Get-PeImports $p
        $lines += "$rel  [$($info.Machine)]"
        foreach ($imp in $info.Imports) { $lines += "  imports $imp" }
        $vcDeps = $info.Imports | Where-Object { $_ -match '(?i)vcruntime|msvcp|api-ms-win-crt|msvcr' }
        if ($vcDeps) {
            $lines += "  *** UNEXPECTED VC RUNTIME DEPENDENCY: $($vcDeps -join ', ') ***"
            $bad = $true
        } else {
            $lines += '  -> operating-system DLLs only; no VC++ runtime'
        }
        $lines += ''
    }

    # ELF report for the Linux artefacts (tools\elfinfo.py, needs python on PATH)
    foreach ($rel in $elfList) {
        $p = Join-Path $PackageDir $rel
        if (-not (Test-Path -LiteralPath $p)) { continue }
        $lines += "$rel  [ELF]"
        $py = Get-Command python -ErrorAction SilentlyContinue
        if ($ElfInfoScript -and $py -and (Test-Path -LiteralPath $ElfInfoScript)) {
            $out = & $py.Source $ElfInfoScript $p 2>&1
            foreach ($l in $out) { $lines += "  $l" }
            $needed = ($out | Where-Object { $_ -like 'NEEDED*' }) -join ' '
            if ($needed -match 'libstdc|libgcc_s|libm\.') {
                $lines += "  *** UNEXPECTED SHARED-LIBRARY DEPENDENCY: $needed ***"
                $bad = $true
            } else {
                $lines += '  -> libc only'
            }
        } else {
            $lines += '  (python / tools\elfinfo.py not available; ELF report skipped)'
        }
        $lines += ''
    }
    Set-Content -LiteralPath (Join-Path $PackageDir 'DEPENDENCIES.txt') -Value $lines -Encoding utf8
}
if ($bad) { Write-Host 'ERROR: unexpected runtime dependency found.'; exit 1 }

$manifest = @(
    "$Product distribution package v$Version",
    'Generated at package time',
    '',
    'Files (SHA-256):'
)
Get-ChildItem -LiteralPath $PackageDir -Recurse -File |
    Where-Object { $_.Name -ne 'MANIFEST.txt' } |
    Sort-Object { $_.FullName } |
    ForEach-Object {
        $rel = $_.FullName.Substring($PackageDir.Length).TrimStart('\')
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        $manifest += ('  {0,-40} {1}' -f $rel, $hash)
    }
Set-Content -LiteralPath (Join-Path $PackageDir 'MANIFEST.txt') -Value $manifest -Encoding utf8
if ($peList.Count -or $elfList.Count) { Write-Host 'DEPENDENCIES.txt and MANIFEST.txt written.' } else { Write-Host 'MANIFEST.txt written.' }
exit 0
