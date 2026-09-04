[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsRoot) { throw 'Visual Studio C++ x64 tools not found.' }
$vc = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$output = Join-Path $root 'artifacts\overlay-controls'
New-Item -ItemType Directory -Path $output -Force | Out-Null
Push-Location $root
try {
    & cmd.exe /d /s /c "`"$vc`" && cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /utf-8 tests\OverlayControlsTests.cpp /Fe:artifacts\overlay-controls\OverlayControlsTests.exe /Fo:artifacts\overlay-controls\OverlayControlsTests.obj user32.lib gdi32.lib comctl32.lib"
    if ($LASTEXITCODE -ne 0) { throw 'Overlay test build failed.' }
    & (Join-Path $output 'OverlayControlsTests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Overlay controls test failed.' }
} finally { Pop-Location }
