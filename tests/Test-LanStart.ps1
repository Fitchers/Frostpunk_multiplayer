param([string]$Bridge = (Join-Path $PSScriptRoot '..\bin\FrostBridgeNet.exe'))
$ErrorActionPreference='Stop'
$maps=@(); $views=@(); $children=@(); $outputs=@()
function Wait-Until([scriptblock]$Condition,[string]$Description) {
 $deadline=[DateTime]::UtcNow.AddSeconds(8)
 do { if (& $Condition) { return }; Start-Sleep -Milliseconds 50 } while([DateTime]::UtcNow -lt $deadline)
 throw "Timeout: $Description"
}
function Start-Bridge([string[]]$Arguments) {
 $si=[Diagnostics.ProcessStartInfo]::new((Resolve-Path -LiteralPath $Bridge).Path)
 $si.UseShellExecute=$false; $si.CreateNoWindow=$true
 $si.RedirectStandardInput=$true; $si.RedirectStandardOutput=$true; $si.RedirectStandardError=$true
 $si.StandardOutputEncoding=[Text.Encoding]::UTF8
 foreach($arg in $Arguments) { $si.ArgumentList.Add($arg) }
 $child=[Diagnostics.Process]::Start($si)
 return $child
}
try {
 # Synthetic per-PID mailboxes. No live Frostpunk memory or files are modified.
 foreach($id in 4000000001,4000000002) {
  $map=[IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew("Local\FrostBridgeLaunchV2-$id",16)
  $view=$map.CreateViewAccessor(); $view.Write(0,[uint32]0x324C4246); $view.Write(4,[uint32]2); $view.Write(8,[int]1); $view.Write(12,[int]-1)
  $maps+=,$map; $views+=,$view
 }
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
 $listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
 $hostBridge=Start-Bridge @('--lan-host',"$port",'--name','Анна','--pid','4000000001')
 $children+=,$hostBridge; $outputs+=,$hostBridge.StandardOutput.ReadToEndAsync()
 Start-Sleep -Milliseconds 400
 $guestBridge=Start-Bridge @('--lan-join',"127.0.0.1:$port",'--name','Борис','--pid','4000000002')
 $children+=,$guestBridge; $outputs+=,$guestBridge.StandardOutput.ReadToEndAsync()
 Start-Sleep -Seconds 1
 $guestBridge.StandardInput.WriteLine('start'); $guestBridge.StandardInput.Flush()
 Start-Sleep -Milliseconds 200
 if($views[0].ReadInt32(8) -ne 1 -or $views[1].ReadInt32(8) -ne 1) { throw 'Client initiated launch' }
 $views[1].Write(8,[int]0)
 $hostBridge.StandardInput.WriteLine('start'); $hostBridge.StandardInput.Flush()
 Start-Sleep -Milliseconds 500
 if($views[0].ReadInt32(8) -ne 1 -or $views[1].ReadInt32(8) -ne 0) { throw 'Unready client was launched' }
 $views[1].Write(8,[int]1)
 $hostBridge.StandardInput.WriteLine('start'); $hostBridge.StandardInput.Flush()
 Wait-Until { $views[0].ReadInt32(8) -eq 2 } 'host map preparation request'
 if($views[1].ReadInt32(8) -ne 1) { throw 'Client prepared before host selected a map' }
 $views[0].Write(12,[int]4); $views[0].Write(8,[int]3)
 Wait-Until { $views[1].ReadInt32(8) -eq 2 -and $views[1].ReadInt32(12) -eq 4 } 'exact host map reaches client'
 $views[1].Write(12,[int]4); $views[1].Write(8,[int]3)
 Wait-Until { $views[0].ReadInt32(8) -eq 4 -and $views[1].ReadInt32(8) -eq 4 } 'both synchronized commits'
 foreach($view in $views) { $view.Write(8,[int]5) }
 $hostBridge.StandardInput.WriteLine('start'); $hostBridge.StandardInput.Flush()
 $guestBridge.StandardInput.WriteLine('start'); $guestBridge.StandardInput.Flush()
 $hostBridge.StandardInput.WriteLine('status 50 30 20 3 80 0 -1 -1'); $hostBridge.StandardInput.Flush()
 $guestBridge.StandardInput.WriteLine('status 60 40 25 4 15 90 -1 -1'); $guestBridge.StandardInput.Flush()
 Start-Sleep -Milliseconds 600
 if($views[0].ReadInt32(8) -ne 5 -or $views[1].ReadInt32(8) -ne 5) { throw 'Duplicate launch changed game state' }
 foreach($child in $children) { $child.StandardInput.WriteLine('quit'); $child.StandardInput.Flush() }
 foreach($child in $children) { if(!$child.WaitForExit(5000)) { throw 'Bridge did not exit' } }
 $hostLog=$outputs[0].Result; $guestLog=$outputs[1].Result
 if(!$hostLog.Contains('Борис: ресурсы: уголь 60; древесина 40; сталь 25; паровые ядра 4; сырая еда 15; пищевые пайки 90')) { throw 'Missing named guest resources including food' }
 if(!$guestLog.Contains('Анна: ресурсы: уголь 50; древесина 30; сталь 20; паровые ядра 3; сырая еда 80; пищевые пайки 0')) { throw 'Missing named host resources including food' }
 if(!$hostLog.Contains('[game] loading:') -or !$guestLog.Contains('[game] loading:')) { throw 'Missing native dispatch results' }
 $hostLog; $guestLog
 'PASS: host authority, readiness rejection, exact host map propagation, two-phase commit, exactly-once dispatch, Unicode resource labels.'
} finally {
 foreach($child in $children) { if(!$child.HasExited) { $child.Kill(); $child.WaitForExit() }; $child.Dispose() }
 foreach($view in $views) { $view.Dispose() }
 foreach($map in $maps) { $map.Dispose() }
}
