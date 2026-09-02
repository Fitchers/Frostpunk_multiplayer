[CmdletBinding()]
param(
    [string]$VisualStudioRoot
)

$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$source = Join-Path $projectRoot 'src\FrostProbe\FrostProbe.cpp'
$testSource = Join-Path $projectRoot 'tests\ProbeTarget.cpp'
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
$quotedSource = '"' + $source + '"'
$quotedOutput = '"/Fe:' + (Join-Path $outputDirectory 'FrostProbe.exe') + '"'
$quotedObject = '"/Fo:' + (Join-Path $outputDirectory 'FrostProbe.obj') + '"'
$quotedTestSource = '"' + $testSource + '"'
$quotedTestOutput = '"/Fe:' + (Join-Path $outputDirectory 'ProbeTarget.exe') + '"'
$quotedTestObject = '"/Fo:' + (Join-Path $outputDirectory 'ProbeTarget.obj') + '"'
$command = "$quotedVcVars && cl.exe /nologo /std:c++20 /EHsc /W4 /O2 $quotedSource $quotedOutput $quotedObject && cl.exe /nologo /std:c++20 /EHsc /W4 /O2 $quotedTestSource $quotedTestOutput $quotedTestObject"

& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "C++ build failed with exit code $LASTEXITCODE."
}

Write-Host "Built: $(Join-Path $outputDirectory 'FrostProbe.exe')"
