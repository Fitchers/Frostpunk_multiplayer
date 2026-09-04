[CmdletBinding()]
param([int[]]$GameProcessIds = @())
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsRoot) { throw 'Visual Studio C++ x64 tools not found.' }
$vcVars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$source = Join-Path $PSScriptRoot 'ResourceReaderTests.cpp'
$output = Join-Path $root 'artifacts\resource-reader-tests'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$exe = Join-Path $output 'ResourceReaderTests.exe'
$obj = Join-Path $output 'ResourceReaderTests.obj'
& cmd.exe /d /s /c "`"$vcVars`" && cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /utf-8 `"$source`" `"/Fe:$exe`" `"/Fo:$obj`""
if ($LASTEXITCODE -ne 0) { throw 'Resource reader test build failed.' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Resource reader unit tests failed.' }
foreach ($gameId in $GameProcessIds) {
    $game = Get-Process -Id $gameId
    if ($game.ProcessName -ne 'Frostpunk') { throw "PID $gameId is not Frostpunk." }
    $module = $game.Modules | Where-Object ModuleName -eq 'Frostpunk.exe' | Select-Object -First 1
    if (!$module) { throw "Cannot find game module in PID $gameId." }
    & $exe $gameId $module.BaseAddress.ToInt64()
    if ($LASTEXITCODE -ne 0) { throw "Read-only resource check failed for PID $gameId." }
}
