[CmdletBinding()]
param(
    [string]$VisualStudioRoot,
    [string]$SteamApiPath,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$source = Join-Path $projectRoot 'src\FrostBridgeNet\FrostBridgeNet.cpp'
$uiSource = Join-Path $projectRoot 'src\FrostConnectionUI\FrostConnectionUI.cpp'
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot 'bin'
}
$outputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)

if ([string]::IsNullOrWhiteSpace($VisualStudioRoot)) {
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vsWhere -PathType Leaf) {
        $VisualStudioRoot = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
}
if ([string]::IsNullOrWhiteSpace($VisualStudioRoot)) {
    throw 'Visual Studio with the Desktop development with C++ workload was not found.'
}

$vcVars = Join-Path $VisualStudioRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcVars -PathType Leaf)) {
    throw 'vcvars64.bat was not found.'
}

New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$quotedVcVars = '"' + $vcVars + '"'
$quotedSource = '"' + $source + '"'
$quotedOutput = '"/Fe:' + (Join-Path $outputDirectory 'FrostBridgeNet.exe') + '"'
$quotedUiSource = '"' + $uiSource + '"'
$quotedObjectDirectory = '"/Fo:' + $outputDirectory + '\\"'
$command = "$quotedVcVars && cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /utf-8 $quotedSource $quotedUiSource $quotedOutput $quotedObjectDirectory ws2_32.lib user32.lib gdi32.lib shell32.lib"

& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "C++ build failed with exit code $LASTEXITCODE."
}

[System.IO.File]::WriteAllText((Join-Path $outputDirectory 'steam_appid.txt'), "480`r`n", [System.Text.Encoding]::ASCII)

if (-not [string]::IsNullOrWhiteSpace($SteamApiPath)) {
    $resolvedSteamApi = [System.IO.Path]::GetFullPath($SteamApiPath)
    if (-not (Test-Path -LiteralPath $resolvedSteamApi -PathType Leaf)) {
        throw "steam_api64.dll was not found at $resolvedSteamApi"
    }
    Copy-Item -LiteralPath $resolvedSteamApi -Destination (Join-Path $outputDirectory 'steam_api64.dll') -Force
}

Write-Host "Built: $(Join-Path $outputDirectory 'FrostBridgeNet.exe')"
Write-Host 'The connection window is built into FrostBridgeNet.exe (--ui); no separate UI executable is required.'
Write-Host "Created development AppID marker: $(Join-Path $outputDirectory 'steam_appid.txt')"
if (-not (Test-Path -LiteralPath (Join-Path $outputDirectory 'steam_api64.dll') -PathType Leaf)) {
    Write-Warning 'Copy the official Steamworks redistributable steam_api64.dll into bin, or rebuild with -SteamApiPath.'
}
