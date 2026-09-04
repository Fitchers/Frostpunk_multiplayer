param([string]$Bridge = (Join-Path $PSScriptRoot '..\bin\FrostBridgeNet.exe'))
$ErrorActionPreference='Stop'
$maps=@(); $views=@(); $launchMaps=@(); $launchViews=@(); $children=@()
function Wait-Until([scriptblock]$Condition,[string]$Description,[int]$Seconds=10) {
 $deadline=[DateTime]::UtcNow.AddSeconds($Seconds)
 do { if (& $Condition) { return }; Start-Sleep -Milliseconds 50 } while([DateTime]::UtcNow -lt $deadline)
 throw "Timeout: $Description"
}
function Start-Bridge([string[]]$Arguments) {
 $si=[Diagnostics.ProcessStartInfo]::new((Resolve-Path -LiteralPath $Bridge).Path)
 $si.UseShellExecute=$false; $si.CreateNoWindow=$true
 $si.RedirectStandardInput=$true; $si.RedirectStandardOutput=$true; $si.RedirectStandardError=$true
 $si.StandardOutputEncoding=[Text.Encoding]::UTF8
 foreach($arg in $Arguments) { $si.ArgumentList.Add($arg) }
 [Diagnostics.Process]::Start($si)
}
try {
 foreach($id in 4100000001,4100000002) {
  $map=[IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew("Local\FrostBridgeSessionV2-$id",56)
  $view=$map.CreateViewAccessor()
  $view.Write(0,[uint32]0x31534246); $view.Write(4,[uint32]2); $view.Write(8,[int]1)
  $view.Write(12,[int]1); $view.Write(16,[int]0)
  $maps+=,$map; $views+=,$view
  $launch=[IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew("Local\FrostBridgeLaunchV2-$id",16)
  $launchView=$launch.CreateViewAccessor(); $launchView.Write(0,[uint32]0x324C4246); $launchView.Write(4,[uint32]2); $launchView.Write(8,[int]1); $launchView.Write(12,[int]-1)
  $launchMaps+=,$launch; $launchViews+=,$launchView
 }
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
 $listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
 $hostProcess=Start-Bridge @('--lan-host',"$port",'--name','Host','--pid','4100000001')
 $children+=,$hostProcess; $hostOut=$hostProcess.StandardOutput.ReadToEndAsync()
 Start-Sleep -Milliseconds 300
 $guest=Start-Bridge @('--lan-join',"127.0.0.1:$port",'--name','Guest','--pid','4100000002')
 $children+=,$guest; $guestOut=$guest.StandardOutput.ReadToEndAsync()
 Start-Sleep -Seconds 1

 # A pause/resume input from either game must become a command for its peer.
 $views[0].Write(32,[int]1); $views[0].Write(28,[int]1)
 Wait-Until { $views[1].ReadInt32(36) -gt 0 -and $views[1].ReadInt32(40) -eq 1 } 'host pause reaches guest'
 $views[1].Write(32,[int]0); $views[1].Write(28,[int]1)
 Wait-Until { $views[0].ReadInt32(36) -gt 0 -and $views[0].ReadInt32(40) -eq 0 } 'guest resume reaches host'

 # Start from menus. Both launch callbacks dispatch, but resume is forbidden until
 # both game mappings report a loaded city and their clocks differ by <= 3000 ms.
 foreach($view in $views) { $view.Write(12,[int]0); $view.Write(16,[int]1); $view.Write(48,[int64]0) }
 $beforeHost=$views[0].ReadInt32(36); $beforeGuest=$views[1].ReadInt32(36)
 $hostProcess.StandardInput.WriteLine('start'); $hostProcess.StandardInput.Flush()
 Wait-Until { $launchViews[0].ReadInt32(8) -eq 2 } 'host map preparation'
 $launchViews[0].Write(12,[int]2); $launchViews[0].Write(8,[int]3)
 Wait-Until { $launchViews[1].ReadInt32(8) -eq 2 -and $launchViews[1].ReadInt32(12) -eq 2 } 'client receives host map'
 $launchViews[1].Write(12,[int]2); $launchViews[1].Write(8,[int]3)
 Wait-Until { $launchViews[0].ReadInt32(8) -eq 4 -and $launchViews[1].ReadInt32(8) -eq 4 } 'both native launches committed'
 foreach($view in $launchViews) { $view.Write(8,[int]5) }
 Start-Sleep -Milliseconds 300
 $views[0].Write(12,[int]1); $views[0].Write(16,[int]0); $views[0].Write(48,[int64]0)
 Wait-Until { $views[0].ReadInt32(36) -gt $beforeHost -and $views[0].ReadInt32(40) -eq 1 } 'host must pause while guest is still unloaded'
 $views[0].Write(16,[int]1)
 Start-Sleep -Seconds 1
 if($views[0].ReadInt32(36) -gt $beforeHost -and $views[0].ReadInt32(40) -eq 0) { throw 'Host resumed before guest loaded' }
 $views[1].Write(12,[int]1); $views[1].Write(16,[int]1); $views[1].Write(48,[int64]2000)
 Wait-Until { $views[0].ReadInt32(36) -gt $beforeHost -and $views[0].ReadInt32(40) -eq 0 } 'host barrier resume' 12
 Wait-Until { $views[1].ReadInt32(36) -gt $beforeGuest -and $views[1].ReadInt32(40) -eq 0 } 'guest barrier resume' 12

 foreach($child in $children) { $child.StandardInput.WriteLine('quit'); $child.StandardInput.Flush() }
 foreach($child in $children) { if(!$child.WaitForExit(5000)) { throw 'Bridge did not exit' } }
 if(!$hostOut.Result.Contains('одновременный старт')) { throw 'Host did not report barrier release' }
 'PASS: bidirectional shared pause and loaded-city start barrier with <=3 second clock skew.'
} finally {
 foreach($child in $children) { if(!$child.HasExited) { $child.Kill(); $child.WaitForExit() }; $child.Dispose() }
 foreach($view in $views) { $view.Dispose() }; foreach($map in $maps) { $map.Dispose() }
 foreach($view in $launchViews) { $view.Dispose() }; foreach($map in $launchMaps) { $map.Dispose() }
}
