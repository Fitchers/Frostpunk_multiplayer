[CmdletBinding()]
param(
    [string]$VisualStudioRoot
)

$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$modSource = Join-Path $projectRoot 'src\FrostMenuMod\FrostMenuMod.cpp'
$injectorSource = Join-Path $projectRoot 'src\FrostMenuInjector\FrostMenuInjector.cpp'
$launcherSource = Join-Path $projectRoot 'src\FrostpunkMultiplayerLauncher\FrostpunkMultiplayerLauncher.cpp'
$outputDirectory = Join-Path $projectRoot 'bin'

if ([string]::IsNullOrWhiteSpace($VisualStudioRoot)) {
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vsWhere -PathType Leaf) {
        $VisualStudioRoot = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
}

if ([string]::IsNullOrWhiteSpace($VisualStudioRoot)) {
    throw 'Visual Studio with the Desktop development with C++ workload was not found. Pass its installation path with -VisualStudioRoot.'
}

$vcVars = Join-Path $VisualStudioRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcVars -PathType Leaf)) {
    throw "vcvars64.bat not found. Pass the Visual Studio installation path with -VisualStudioRoot."
}

New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

$quotedVcVars = '"' + $vcVars + '"'
$quotedModSource = '"' + $modSource + '"'
$quotedModOutput = '"/Fe:' + (Join-Path $outputDirectory 'FrostMenuMod.dll') + '"'
$quotedModObject = '"/Fo:' + (Join-Path $outputDirectory 'FrostMenuMod.obj') + '"'
$quotedInjectorSource = '"' + $injectorSource + '"'
$quotedInjectorOutput = '"/Fe:' + (Join-Path $outputDirectory 'FrostMenuInjector.exe') + '"'
$quotedInjectorObject = '"/Fo:' + (Join-Path $outputDirectory 'FrostMenuInjector.obj') + '"'
$quotedLauncherSource = '"' + $launcherSource + '"'
$quotedLauncherOutput = '"/Fe:' + (Join-Path $outputDirectory 'FrostpunkMultiplayerLauncher.exe') + '"'
$quotedLauncherObject = '"/Fo:' + (Join-Path $outputDirectory 'FrostpunkMultiplayerLauncher.obj') + '"'
$common = '/nologo /std:c++20 /EHsc /W4 /O2 /MT /utf-8'
$command = "$quotedVcVars && cl.exe $common /LD $quotedModSource $quotedModOutput $quotedModObject user32.lib && cl.exe $common $quotedInjectorSource $quotedInjectorOutput $quotedInjectorObject bcrypt.lib && cl.exe $common $quotedLauncherSource $quotedLauncherOutput $quotedLauncherObject /link /SUBSYSTEM:WINDOWS bcrypt.lib shell32.lib user32.lib"

& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "C++ build failed with exit code $LASTEXITCODE."
}

Write-Host "Built: $(Join-Path $outputDirectory 'FrostMenuMod.dll')"
Write-Host "Built: $(Join-Path $outputDirectory 'FrostMenuInjector.exe')"
Write-Host "Built: $(Join-Path $outputDirectory 'FrostpunkMultiplayerLauncher.exe')"
