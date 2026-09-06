$ErrorActionPreference = 'Stop'

$gameExecutable = 'C:\Frostpunk\Frostpunk.exe'
$modDirectory = 'C:\Persistent\Mod'
$launcher = Join-Path $modDirectory 'FrostpunkMultiplayerLauncher.exe'
$steamExecutable = 'C:\Persistent\Steam\steam.exe'
$visualCppDirectory = 'C:\Persistent\Runtime'

Add-Type -AssemblyName PresentationFramework

if (-not (Test-Path -LiteralPath $gameExecutable -PathType Leaf)) {
    [System.Windows.MessageBox]::Show(
        "Frostpunk.exe is unavailable at $gameExecutable",
        'Frostpunk Steam P2P Test', 'OK', 'Error') | Out-Null
    exit 1
}
if (-not (Test-Path -LiteralPath $launcher -PathType Leaf)) {
    [System.Windows.MessageBox]::Show(
        "The multiplayer launcher is unavailable at $launcher",
        'Frostpunk Steam P2P Test', 'OK', 'Error') | Out-Null
    exit 1
}
if (-not (Get-Process -Name steam -ErrorAction SilentlyContinue)) {
    if (Test-Path -LiteralPath $steamExecutable -PathType Leaf) {
        Start-Process -FilePath $steamExecutable -WorkingDirectory (Split-Path $steamExecutable)
    }
    [System.Windows.MessageBox]::Show(
        'Steam is starting. Sign in to the second account, then use this shortcut again.',
        'Frostpunk Steam P2P Test', 'OK', 'Information') | Out-Null
    exit 0
}

$env:FROSTPUNK_EXE = $gameExecutable
$env:PATH = $visualCppDirectory + ';' + $env:PATH
Start-Process -FilePath $launcher -WorkingDirectory $modDirectory
