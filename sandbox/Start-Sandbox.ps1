$ErrorActionPreference = 'Stop'
$bootstrapLog = 'C:\Persistent\bootstrap.log'
trap {
    $details = "[$((Get-Date).ToUniversalTime().ToString('O'))] ERROR: $($_ | Out-String)"
    [System.IO.File]::AppendAllText($bootstrapLog, $details, [System.Text.Encoding]::UTF8)
    Add-Type -AssemblyName PresentationFramework
    [System.Windows.MessageBox]::Show(
        "Sandbox setup failed. Details were saved to C:\Persistent\bootstrap.log.`n`n$($_.Exception.Message)",
        'Frostpunk Steam P2P Test', 'OK', 'Error') | Out-Null
    exit 1
}

$sourceDirectory = 'C:\FrostSource\bin'
$runtimeDirectory = 'C:\Persistent\Mod'
$steamDirectory = 'C:\Persistent\Steam'
$steamExecutable = Join-Path $steamDirectory 'steam.exe'
$steamInstaller = 'C:\Persistent\Installer\SteamSetup.exe'
$gameExecutable = 'C:\Frostpunk\Frostpunk.exe'
[System.IO.File]::AppendAllText(
    $bootstrapLog,
    "`r`n[$((Get-Date).ToUniversalTime().ToString('O'))] Bootstrap started.`r`n",
    [System.Text.Encoding]::UTF8)

$requiredFiles = @(
    'FrostpunkMultiplayerLauncher.exe',
    'FrostMenuMod.dll',
    'FrostBridgeNet.exe',
    'steam_api64.dll',
    'steam_appid.txt'
)

if (-not (Test-Path -LiteralPath $gameExecutable -PathType Leaf)) {
    throw "Frostpunk.exe is unavailable at $gameExecutable"
}
[System.IO.File]::AppendAllText($bootstrapLog, "Game mapping ready.`r`n", [System.Text.Encoding]::UTF8)

# Frostpunk imports MSVCP140.dll and VCRUNTIME140.dll. Sandbox does not include
# them, so use the signed app-local copies prepared in the persistent folder.
foreach ($runtimeFile in 'MSVCP140.dll', 'VCRUNTIME140.dll') {
    $runtimePath = Join-Path 'C:\Persistent\Runtime' $runtimeFile
    if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
        throw "Microsoft Visual C++ runtime file is unavailable at $runtimePath"
    }
}
[System.IO.File]::AppendAllText($bootstrapLog, "App-local Visual C++ runtime ready.`r`n", [System.Text.Encoding]::UTF8)

New-Item -ItemType Directory -Path $runtimeDirectory -Force | Out-Null
foreach ($name in $requiredFiles) {
    $source = Join-Path $sourceDirectory $name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "The current multiplayer build is missing $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $runtimeDirectory $name) -Force
}
[System.IO.File]::AppendAllText($bootstrapLog, "Multiplayer build synchronized.`r`n", [System.Text.Encoding]::UTF8)

if (-not (Test-Path -LiteralPath $steamExecutable -PathType Leaf)) {
    if (-not (Test-Path -LiteralPath $steamInstaller -PathType Leaf)) {
        throw "SteamSetup.exe is unavailable at $steamInstaller"
    }
    $installer = Start-Process -FilePath $steamInstaller -ArgumentList '/S', '/D=C:\Persistent\Steam' -Wait -PassThru
    if ($installer.ExitCode -ne 0) {
        throw "Steam installer failed with exit code $($installer.ExitCode)"
    }
}
[System.IO.File]::AppendAllText($bootstrapLog, "Steam files ready.`r`n", [System.Text.Encoding]::UTF8)

$deadline = [DateTime]::UtcNow.AddMinutes(2)
while (-not (Test-Path -LiteralPath $steamExecutable -PathType Leaf) -and
       [DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 500
}
if (-not (Test-Path -LiteralPath $steamExecutable -PathType Leaf)) {
    throw 'Steam installation did not create C:\Persistent\Steam\steam.exe'
}

$desktop = [Environment]::GetFolderPath('Desktop')
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut((Join-Path $desktop 'Start Frostpunk Multiplayer.lnk'))
$shortcut.TargetPath = 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
$shortcut.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "C:\FrostSource\sandbox\Launch-SandboxClient.ps1"'
$shortcut.WorkingDirectory = $runtimeDirectory
$shortcut.IconLocation = (Join-Path $runtimeDirectory 'FrostpunkMultiplayerLauncher.exe') + ',0'
$shortcut.Save()

if (-not (Get-Process -Name steam -ErrorAction SilentlyContinue)) {
    Start-Process -FilePath $steamExecutable -WorkingDirectory $steamDirectory
}

[System.IO.File]::WriteAllText(
    'C:\Persistent\bootstrap-ready.txt',
    (Get-Date).ToUniversalTime().ToString('O'),
    [System.Text.Encoding]::ASCII)

Add-Type -AssemblyName PresentationFramework
[System.Windows.MessageBox]::Show(
    "Sign in to the SECOND Steam account. Then double-click 'Start Frostpunk Multiplayer' on the desktop.`n`nThe game, current mod build, and saves are already connected to persistent host folders.",
    'Frostpunk Steam P2P Test',
    'OK',
    'Information'
) | Out-Null
