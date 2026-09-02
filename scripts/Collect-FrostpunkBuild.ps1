[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$GamePath,

    [string]$OutputRoot
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $PSScriptRoot '..\artifacts'
}

function Get-SteamRoots {
    $roots = [System.Collections.Generic.List[string]]::new()
    $registryKeys = @(
        'HKCU:\Software\Valve\Steam',
        'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam',
        'HKLM:\SOFTWARE\Valve\Steam'
    )

    foreach ($key in $registryKeys) {
        if (-not (Test-Path -LiteralPath $key)) { continue }
        $item = Get-ItemProperty -LiteralPath $key
        foreach ($name in @('SteamPath', 'InstallPath')) {
            $candidate = $item.$name
            if ($candidate -and (Test-Path -LiteralPath $candidate)) {
                $roots.Add([System.IO.Path]::GetFullPath($candidate))
            }
        }
    }

    foreach ($root in @($roots)) {
        $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
        if (-not (Test-Path -LiteralPath $vdf)) { continue }
        foreach ($line in Get-Content -LiteralPath $vdf) {
            if ($line -match '^\s*"path"\s+"([^"]+)"') {
                $candidate = $Matches[1] -replace '\\\\', '\'
                if (Test-Path -LiteralPath $candidate) {
                    $roots.Add([System.IO.Path]::GetFullPath($candidate))
                }
            }
        }
    }

    $roots | Sort-Object -Unique
}

function Resolve-FrostpunkExecutable([string]$RequestedPath) {
    if ($RequestedPath) {
        $resolved = (Resolve-Path -LiteralPath $RequestedPath).Path
        if ((Get-Item -LiteralPath $resolved).PSIsContainer) {
            $resolved = Join-Path $resolved 'Frostpunk.exe'
        }
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            throw "Frostpunk.exe not found at '$resolved'."
        }
        return $resolved
    }

    $candidates = [System.Collections.Generic.List[string]]::new()
    foreach ($root in Get-SteamRoots) {
        $candidates.Add((Join-Path $root 'steamapps\common\Frostpunk\Frostpunk.exe'))
    }
    $candidates.Add('C:\Program Files (x86)\GOG Galaxy\Games\Frostpunk\Frostpunk.exe')
    $candidates.Add('C:\Program Files\Epic Games\Frostpunk\Frostpunk.exe')

    $found = @($candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Sort-Object -Unique)
    if ($found.Count -eq 0) {
        throw 'Frostpunk.exe was not found automatically. Pass -GamePath with the install directory or executable path.'
    }
    if ($found.Count -gt 1) {
        throw "Multiple installations found. Pass one with -GamePath:`n$($found -join "`n")"
    }
    return (Resolve-Path -LiteralPath $found[0]).Path
}

function Read-PeSummary([string]$Path) {
    $stream = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    $reader = [System.IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'Missing MZ signature.' }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw 'Missing PE signature.' }
        $machine = $reader.ReadUInt16()
        $sections = $reader.ReadUInt16()
        $timestamp = $reader.ReadUInt32()
        $stream.Position += 8
        $optionalHeaderSize = $reader.ReadUInt16()
        $characteristics = $reader.ReadUInt16()
        $optionalMagic = $reader.ReadUInt16()

        [ordered]@{
            machine = ('0x{0:X4}' -f $machine)
            architecture = switch ($machine) {
                0x8664 { 'x64' }
                0x014C { 'x86' }
                0xAA64 { 'ARM64' }
                default { 'unknown' }
            }
            sectionCount = $sections
            coffTimestampUtc = [DateTimeOffset]::FromUnixTimeSeconds($timestamp).UtcDateTime.ToString('o')
            optionalHeaderSize = $optionalHeaderSize
            optionalHeaderMagic = ('0x{0:X4}' -f $optionalMagic)
            characteristics = ('0x{0:X4}' -f $characteristics)
        }
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

$exePath = Resolve-FrostpunkExecutable $GamePath
$gameDirectory = Split-Path -Parent $exePath
$exe = Get-Item -LiteralPath $exePath
$hash = Get-FileHash -LiteralPath $exePath -Algorithm SHA256
$version = $exe.VersionInfo
$shortHash = $hash.Hash.Substring(0, 12).ToLowerInvariant()
$outputDirectory = Join-Path ([System.IO.Path]::GetFullPath($OutputRoot)) "build-$shortHash"
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

$libraries = @(
    Get-ChildItem -LiteralPath $gameDirectory -Filter '*.dll' -File -ErrorAction SilentlyContinue |
        Sort-Object Name |
        ForEach-Object {
            [ordered]@{
                name = $_.Name
                size = $_.Length
                fileVersion = $_.VersionInfo.FileVersion
            }
        }
)

$manifestPath = Join-Path (Split-Path -Parent $gameDirectory) '..\appmanifest_323190.acf'
$manifestPath = [System.IO.Path]::GetFullPath($manifestPath)

$inventory = [ordered]@{
    collectedAtUtc = [DateTime]::UtcNow.ToString('o')
    executable = [ordered]@{
        path = $exe.FullName
        size = $exe.Length
        lastWriteTimeUtc = $exe.LastWriteTimeUtc.ToString('o')
        sha256 = $hash.Hash.ToLowerInvariant()
        fileVersion = $version.FileVersion
        productVersion = $version.ProductVersion
        pe = Read-PeSummary $exePath
    }
    steamManifest = if (Test-Path -LiteralPath $manifestPath) { $manifestPath } else { $null }
    topLevelLibraries = $libraries
}

$jsonPath = Join-Path $outputDirectory 'inventory.json'
$inventory | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $jsonPath -Encoding utf8

Write-Host "Frostpunk executable: $exePath"
Write-Host "SHA-256:             $($hash.Hash.ToLowerInvariant())"
Write-Host "Architecture:        $($inventory.executable.pe.architecture)"
Write-Host "Inventory:           $jsonPath"
