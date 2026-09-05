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
  $map=[IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew("Local\FrostBridgeSessionV4-$id",80)
  $view=$map.CreateViewAccessor()
  $view.Write(0,[uint32]0x31534246); $view.Write(4,[uint32]4); $view.Write(8,[int]1)
  $view.Write(12,[int]1); $view.Write(16,[int]0)
  $maps+=,$map; $views+=,$view
  $launch=[IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew("Local\FrostBridgeLaunchV3-$id",16)
  $launchView=$launch.CreateViewAccessor(); $launchView.Write(0,[uint32]0x324C4246); $launchView.Write(4,[uint32]4); $launchView.Write(8,[int]1); $launchView.Write(12,[int]-1)
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

 # All native speed modes, requests in both directions and no repeated echo.
 $views[0].Write(60,[int]1); $views[0].Write(56,[int]1)
 Wait-Until { $views[1].ReadInt32(68) -eq 1 } 'host middle speed reaches guest'
 $views[0].Write(60,[int]2); $views[0].Write(56,[int]2)
 Wait-Until { $views[1].ReadInt32(68) -eq 2 } 'host fastest speed reaches guest'
 $views[1].Write(60,[int]0); $views[1].Write(56,[int]1)
 Wait-Until { $views[0].ReadInt32(68) -eq 0 -and $views[1].ReadInt32(68) -eq 0 } 'guest normal speed reaches both'
 $speedSeq=$views[0].ReadInt32(64)
 Start-Sleep -Milliseconds 200
 if($views[0].ReadInt32(64) -ne $speedSeq) { throw 'Speed echo loop' }
 $views[1].Write(60,[int]7); $views[1].Write(56,[int]2)
 Start-Sleep -Milliseconds 200
 if($views[0].ReadInt32(64) -ne $speedSeq) { throw 'Invalid speed accepted' }
 $views[0].Write(60,[int]1); $views[0].Write(56,[int]3)
 $views[1].Write(60,[int]2); $views[1].Write(56,[int]3)
 Start-Sleep -Milliseconds 300
 if($views[0].ReadInt32(68) -ne $views[1].ReadInt32(68)) { throw 'Concurrent speed choices diverged' }

 # A pause/resume input from either game must become a command for its peer.
 $views[0].Write(32,[int]1); $views[0].Write(28,[int]1)
 Wait-Until { $views[1].ReadInt32(36) -gt 0 -and $views[1].ReadInt32(40) -eq 1 } 'host pause reaches guest'
 $views[1].Write(32,[int]0); $views[1].Write(28,[int]1)
 Wait-Until { $views[0].ReadInt32(36) -gt 0 -and $views[0].ReadInt32(40) -eq 0 } 'guest resume reaches host'
 # Releasing the guest cannot release the host's independent local pause.
 if($views[1].ReadInt32(40) -ne 1) { throw 'Guest release erased host pause' }
 $views[0].Write(32,[int]0); $views[0].Write(28,[int]2)
 Wait-Until { $views[1].ReadInt32(40) -eq 0 } 'host releases its own pause'

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
 # A loading/UI pause clears while the barrier is still active. The release
 # must reach the peer without allowing either city to run before both load.
 $views[0].Write(32,[int]1); $views[0].Write(28,[int]3)
 Start-Sleep -Milliseconds 300
 $views[0].Write(32,[int]0); $views[0].Write(28,[int]4)
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

 # 25 calendar minutes of accumulated drift: only the leading city waits.
 $views[0].Write(48,[int64]31500000); $views[1].Write(48,[int64]33000000)
 Wait-Until { $views[1].ReadInt32(40) -eq 1 -and $views[0].ReadInt32(40) -eq 0 } 'leading guest waits for host'
 $views[0].Write(48,[int64]33000000)
 Wait-Until { $views[1].ReadInt32(40) -eq 0 } 'guest resumes after catchup'
 $views[0].Write(48,[int64]34000000)
 Wait-Until { $views[0].ReadInt32(40) -eq 1 -and $views[1].ReadInt32(40) -eq 0 } 'leading host waits for guest'
 $views[1].Write(32,[int]1); $views[1].Write(28,[int]2)
 $views[1].Write(48,[int64]34000000)
 Start-Sleep -Milliseconds 200
 if($views[0].ReadInt32(40) -ne 1) { throw 'Clock catchup erased peer UI pause' }
 $views[1].Write(32,[int]0); $views[1].Write(28,[int]3)
 Wait-Until { $views[0].ReadInt32(40) -eq 0 } 'peer releases UI pause after catchup'

 # Normal one/two-frame jitter must not produce network pause chatter.
 $beforeJitterHost=$views[0].ReadInt32(36); $beforeJitterGuest=$views[1].ReadInt32(36)
 foreach($i in 1..40) {
  $views[0].Write(48,[int64](35000000 + $i*14400))
  $views[1].Write(48,[int64](35000000 + $i*14400 + ($i%3)*4800))
  Start-Sleep -Milliseconds 25
 }
 if($views[0].ReadInt32(36) -ne $beforeJitterHost -or $views[1].ReadInt32(36) -ne $beforeJitterGuest) {
  throw 'Frame jitter generated pause commands'
 }

 # Reload must reissue an unchanged pause value for the newly created timer.
 $beforeReload=$views[0].ReadInt32(36)
 $views[0].Write(24,[int]9)
 Wait-Until { $views[0].ReadInt32(36) -gt $beforeReload } 'pause command reapplied after load generation changes'
 foreach($child in $children) { $child.StandardInput.WriteLine('quit'); $child.StandardInput.Flush() }
 foreach($child in $children) { if(!$child.WaitForExit(5000)) { throw 'Bridge did not exit' } }
 'PASS: shared pause, startup hold, bidirectional catchup and zero frame-jitter pause commands.'
} finally {
 foreach($child in $children) { if(!$child.HasExited) { $child.Kill(); $child.WaitForExit() }; $child.Dispose() }
 foreach($view in $views) { $view.Dispose() }; foreach($map in $maps) { $map.Dispose() }
 foreach($view in $launchViews) { $view.Dispose() }; foreach($map in $launchMaps) { $map.Dispose() }
}
