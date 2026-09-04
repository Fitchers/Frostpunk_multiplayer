param([Parameter(Mandatory)][int]$FirstPid, [Parameter(Mandatory)][int]$SecondPid)
$ErrorActionPreference='Stop'
if ($FirstPid -eq $SecondPid) { throw 'Use two different Frostpunk processes.' }
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class BridgeUiTest {
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls,string title);
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr w,int id);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr w,uint m,IntPtr p,string s);
 [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr Send(IntPtr w,uint m,IntPtr p,IntPtr l);
 [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr ReadText(IntPtr w,uint m,IntPtr n,StringBuilder s);
 [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr w,uint m,IntPtr p,IntPtr l);
}
'@
function Text-Of($window,$id) {
 $text=[Text.StringBuilder]::new(65536)
 [void][BridgeUiTest]::ReadText([BridgeUiTest]::GetDlgItem($window,$id),0xD,[IntPtr]$text.Capacity,$text)
 $text.ToString()
}
function Set-Field($window,$id,$text) { [void][BridgeUiTest]::SendMessageW([BridgeUiTest]::GetDlgItem($window,$id),0xC,[IntPtr]::Zero,$text) }
function Click($window,$id) { [void][BridgeUiTest]::Send([BridgeUiTest]::GetDlgItem($window,$id),0xF5,[IntPtr]::Zero,[IntPtr]::Zero) }
function Wait-For([scriptblock]$condition,[string]$description) {
 $deadline=[DateTime]::UtcNow.AddSeconds(12)
 do { if (& $condition) { return }; Start-Sleep -Milliseconds 100 } while([DateTime]::UtcNow -lt $deadline)
 throw "Timed out: $description"
}
$executable=Join-Path $PSScriptRoot '..\bin\FrostBridgeNet.exe'
$titles=@("FrostBridge — LAN — PID $FirstPid", "FrostBridge — LAN — PID $SecondPid")
foreach($title in $titles) { if([BridgeUiTest]::FindWindowW('FrostBridgeConnectionUI',$title) -ne [IntPtr]::Zero) { throw "Close existing form before test: $title" } }
$firstWindow=[IntPtr]::Zero; $secondWindow=[IntPtr]::Zero
try {
 $first=Start-Process -FilePath $executable -ArgumentList "--ui --lan --pid $FirstPid" -WindowStyle Hidden -PassThru
 $second=Start-Process -FilePath $executable -ArgumentList "--ui --lan --pid $SecondPid" -WindowStyle Hidden -PassThru
 Wait-For { [BridgeUiTest]::FindWindowW('FrostBridgeConnectionUI',$titles[0]) -ne [IntPtr]::Zero } 'host form'
 Wait-For { [BridgeUiTest]::FindWindowW('FrostBridgeConnectionUI',$titles[1]) -ne [IntPtr]::Zero } 'guest form'
 $firstWindow=[BridgeUiTest]::FindWindowW('FrostBridgeConnectionUI',$titles[0])
 $secondWindow=[BridgeUiTest]::FindWindowW('FrostBridgeConnectionUI',$titles[1])
 Click $firstWindow 104
 if((Text-Of $firstWindow 107) -notmatch 'Введите имя') { throw 'Blank name was not rejected.' }
 Set-Field $firstWindow 101 'Анна "Хост"'
 Set-Field $secondWindow 101 'Гость'
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
 $listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
 Set-Field $firstWindow 103 "$port"
 Set-Field $secondWindow 103 '0'
 Click $secondWindow 105
 if((Text-Of $secondWindow 107) -notmatch 'Порт должен') { throw 'Invalid port was not rejected.' }
 Set-Field $secondWindow 103 "$port"
 Click $firstWindow 104
 Wait-For { (Text-Of $firstWindow 107) -match 'Hosting on port' } 'listen'
 Click $secondWindow 105
 Wait-For { (Text-Of $firstWindow 107) -match 'player: Гость.*connected' } 'host handshake'
 Wait-For { (Text-Of $secondWindow 107) -match 'player: Анна "Хост".*connected' } 'guest handshake / argv quoting'
 Set-Field $firstWindow 109 'Привет из первого города'
 Click $firstWindow 110
 Wait-For { (Text-Of $secondWindow 107).Contains('[peer] Привет из первого города') } 'host chat'
 Set-Field $secondWindow 109 'Ответ из второго города'
 Click $secondWindow 110
 Wait-For { (Text-Of $firstWindow 107).Contains('[peer] Ответ из второго города') } 'guest chat'
 Write-Output (Text-Of $firstWindow 107)
 Write-Output (Text-Of $secondWindow 107)
 Click $firstWindow 106
 Wait-For { (Text-Of $secondWindow 108) -match 'Соединение закрыто' } 'disconnect detection'
 Click $secondWindow 106
 Click $firstWindow 104
 Start-Sleep -Milliseconds 300
 Click $firstWindow 106 # Cancel a host blocked in accept, no guest attached.
 Click $secondWindow 105 # No listener: must recover and permit retry.
 Wait-For { (Text-Of $secondWindow 108) -match 'Подключение не удалось' } 'connection refused recovery'
 Write-Output 'PASS: required names, port validation, Unicode/quoting, two PID-bound sessions, bidirectional chat, disconnect, cancel hosting, refused connection.'
} finally {
 foreach($window in @($firstWindow,$secondWindow)) {
  if($window -ne [IntPtr]::Zero) {
   Click $window 106
   [void][BridgeUiTest]::PostMessageW($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
  }
 }
}
