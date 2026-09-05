[CmdletBinding()]
param(
    [string]$HostName = 'Fitchers',
    [string]$ClientName = 'User',
    [ValidateRange(1,65535)][int]$Port = 27020
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root 'bin'
$staged = Join-Path $root 'artifacts\shared-speed'
$launcher = Join-Path $bin 'FrostpunkMultiplayerLauncher.exe'
$bridge = Join-Path $bin 'FrostBridgeNet.exe'
$dll = Join-Path $bin 'FrostMenuMod.dll'
$stagedBridge = Join-Path $staged 'FrostBridgeNet.exe'
$stagedDll = Join-Path $staged 'FrostMenuMod.dll'

function Get-Games { @(Get-Process Frostpunk -ErrorAction SilentlyContinue | Sort-Object StartTime) }
function Install-StagedRuntime {
    if((Get-Games).Count -ne 0) { return }
    foreach($file in $stagedBridge,$stagedDll) {
        if(!(Test-Path -LiteralPath $file -PathType Leaf)) {
            throw "Prepared multiplayer file is missing: $file"
        }
    }
    Copy-Item -LiteralPath $stagedBridge -Destination $bridge -Force
    Copy-Item -LiteralPath $stagedDll -Destination $dll -Force
}
function Invoke-GameLauncher([string[]]$Arguments=@()) {
    $options=@{
        FilePath=$launcher
        WorkingDirectory=$bin
        PassThru=$true
        WindowStyle='Hidden'
    }
    if($Arguments.Count -gt 0) { $options.ArgumentList=$Arguments }
    $process=Start-Process @options
    if(!$process.WaitForExit(90000)) {
        throw 'The launcher did not finish within 90 seconds.'
    }
    if($process.ExitCode -ne 0) {
        throw "The launcher failed with exit code $($process.ExitCode)."
    }
}
function Wait-GameCount([int]$Count,[int]$Seconds=35) {
    $deadline=[DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        $games=Get-Games
        if($games.Count -ge $Count) { return $games }
        Start-Sleep -Milliseconds 250
    } while([DateTime]::UtcNow -lt $deadline)
    throw "Frostpunk did not start within $Seconds seconds (expected processes: $Count)."
}
function Has-CurrentMod([int]$GameId) {
    try {
        $map=[IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting("Local\FrostBridgeLaunchV3-$GameId")
        $view=$map.CreateViewAccessor()
        $valid=$view.ReadUInt32(0)-eq[uint32]0x324C4246 -and $view.ReadUInt32(4)-eq[uint32]3
        $view.Dispose();$map.Dispose()
        if(!$valid) { return $false }
        $map=[IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting("Local\FrostBridgeOverlayV3-$GameId")
        $view=$map.CreateViewAccessor()
        $valid=$view.ReadUInt32(0)-eq[uint32]0x324F4246 -and $view.ReadUInt32(4)-eq[uint32]2
        $view.Dispose();$map.Dispose()
        if(!$valid) { return $false }
        $map=[IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting("Local\FrostBridgeSessionV4-$GameId")
        $view=$map.CreateViewAccessor()
        $valid=$view.ReadUInt32(0)-eq[uint32]0x31534246 -and $view.ReadUInt32(4)-eq[uint32]4
        $view.Dispose();$map.Dispose(); return $valid
    } catch { return $false }
}
function Wait-CurrentMod([int]$GameId,[int]$Seconds=45) {
    $deadline=[DateTime]::UtcNow.AddSeconds($Seconds)
    do { if(Has-CurrentMod $GameId){return}; Start-Sleep -Milliseconds 200 }
    while([DateTime]::UtcNow -lt $deadline)
    throw "The current DLL was not loaded into Frostpunk PID $GameId."
}

$games=Get-Games
if($games.Count -gt 2) { throw 'Close extra Frostpunk instances; exactly two are supported.' }
if($games.Count -gt 0 -and @($games | Where-Object { !(Has-CurrentMod $_.Id) }).Count -gt 0) {
    throw 'A running game uses an old DLL. Save both cities, close both Frostpunk instances, and run this file again.'
}
Install-StagedRuntime
foreach($file in $launcher,$bridge,$dll) {
    if(!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Required file is missing: $file" }
}
if($games.Count -eq 0) {
    Invoke-GameLauncher
    $games=Wait-GameCount 1
    Wait-CurrentMod $games[0].Id
}
if($games.Count -eq 1) {
    Invoke-GameLauncher @('--new-instance')
    $games=Wait-GameCount 2
}
$games=Get-Games
foreach($game in $games) { Wait-CurrentMod $game.Id }

# One executable directory is mandatory for both peers, so protocol versions
# and shared-control layouts cannot diverge again.
$hostGame=$games[0]
$clientGame=$games[1]
Start-Process -FilePath $bridge -WorkingDirectory $bin -ArgumentList @(
    '--ui','--lan','--pid',"$($hostGame.Id)",'--name',$HostName,
    '--port',"$Port",'--auto-host') | Out-Null
Start-Sleep -Milliseconds 500
Start-Process -FilePath $bridge -WorkingDirectory $bin -ArgumentList @(
    '--ui','--lan','--pid',"$($clientGame.Id)",'--name',$ClientName,
    '--address','127.0.0.1','--port',"$Port",'--auto-join') | Out-Null

Write-Host "Ready: host '$HostName' PID $($hostGame.Id), client '$ClientName' PID $($clientGame.Id), LAN 127.0.0.1:$Port."
Write-Host 'Only one action remains: click Start Game in the host connection window.'
