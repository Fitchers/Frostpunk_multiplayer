param([string]$BuildDirectory='artifacts/player-package')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root $BuildDirectory
$destination=Join-Path $root ('dist/Frostpunk-Multiplayer-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $destination | Out-Null
foreach($name in 'FrostpunkMultiplayerLauncher.exe','FrostMenuMod.dll','FrostBridgeNet.exe','steam_api64.dll','steam_appid.txt') {
    Copy-Item -LiteralPath (Join-Path $build $name) -Destination $destination
}
Copy-Item -LiteralPath (Join-Path $root 'packaging/README.txt') -Destination $destination
Compress-Archive -LiteralPath $destination -DestinationPath ($destination+'.zip')
Write-Output $destination
Write-Output ($destination+'.zip')
