param(
    [string]$BuildDirectory='artifacts/player-package',
    [string]$Version
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root $BuildDirectory
if ([string]::IsNullOrWhiteSpace($Version)) {
    $packageSuffix=Get-Date -Format 'yyyyMMdd-HHmmss'
} else {
    if ($Version -notmatch '^v?[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$') {
        throw "Invalid package version: $Version"
    }
    $packageSuffix=$Version.TrimStart('v')
    $packageSuffix='v'+$packageSuffix
}
$destination=Join-Path $root ('dist/Frostpunk-Multiplayer-'+$packageSuffix)
New-Item -ItemType Directory -Path $destination | Out-Null
foreach($name in 'FrostpunkMultiplayerLauncher.exe','FrostMenuMod.dll','FrostBridgeNet.exe','steam_api64.dll','steam_appid.txt') {
    Copy-Item -LiteralPath (Join-Path $build $name) -Destination $destination
}
Copy-Item -LiteralPath (Join-Path $root 'packaging/README.txt') -Destination $destination
Compress-Archive -LiteralPath $destination -DestinationPath ($destination+'.zip')
Write-Output $destination
Write-Output ($destination+'.zip')
