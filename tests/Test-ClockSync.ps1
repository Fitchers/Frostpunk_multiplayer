[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsRoot) { throw 'Visual Studio C++ x64 tools not found.' }
$vcVars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$source = Join-Path $PSScriptRoot 'ClockSyncTests.cpp'
$output = Join-Path $root 'artifacts\clock-sync-tests'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$exe = Join-Path $output 'ClockSyncTests.exe'
$obj = Join-Path $output 'ClockSyncTests.obj'
& cmd.exe /d /s /c "`"$vcVars`" && cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /utf-8 `"$source`" `"/Fe:$exe`" `"/Fo:$obj`""
if ($LASTEXITCODE -ne 0) { throw 'Clock sync test build failed.' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Clock sync tests failed.' }
