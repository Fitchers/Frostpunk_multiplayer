param([string]$Bridge = (Join-Path $PSScriptRoot '..\bin\FrostBridgeNet.exe'))
$ErrorActionPreference='Stop'
$fakePids=@(4000000101,4000000102)
$maps=@(); $views=@(); $children=@(); $outputs=@()
function Start-Bridge([string[]]$Arguments) {
 $si=[Diagnostics.ProcessStartInfo]::new((Resolve-Path -LiteralPath $Bridge).Path)
 $si.UseShellExecute=$false; $si.CreateNoWindow=$true
 $si.RedirectStandardInput=$true; $si.RedirectStandardOutput=$true; $si.RedirectStandardError=$true
 $si.StandardOutputEncoding=[Text.Encoding]::UTF8
 foreach($arg in $Arguments) { $si.ArgumentList.Add($arg) }
 [Diagnostics.Process]::Start($si)
}
try {
 foreach($gameId in $fakePids) {
  $map=[IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew("Local\FrostBridgeOverlayV3-$gameId",580)
  $view=$map.CreateViewAccessor()
  $view.Write(0,[uint32]0x324F4246); $view.Write(4,[uint32]2); $view.Write(8,[int]1)
  $maps+=,$map; $views+=,$view
 }
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
 $listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
 $hostBridge=Start-Bridge @('--lan-host',"$port",'--name','Анна','--pid',"$($fakePids[0])")
 $children+=,$hostBridge; $outputs+=,$hostBridge.StandardOutput.ReadToEndAsync()
 Start-Sleep -Milliseconds 300
 $guest=Start-Bridge @('--lan-join',"127.0.0.1:$port",'--name','Борис','--pid',"$($fakePids[1])")
 $children+=,$guest; $outputs+=,$guest.StandardOutput.ReadToEndAsync()
 Start-Sleep -Seconds 1
 $hostBridge.StandardInput.WriteLine('status 100 30 20 3 80 0 -1 -1'); $hostBridge.StandardInput.Flush()
 $guest.StandardInput.WriteLine('status 60 40 25 4 15 90 -1 -1'); $guest.StandardInput.Flush()
 Start-Sleep -Milliseconds 300
 $balances=@([int[]](100,30,20,3,80,0),[int[]](60,40,25,4,15,90))
 $seen=@(0,0)
 function Invoke-ClickAndPump([int]$resource,[int]$sequence,[scriptblock]$complete,[int]$amount=50,[int]$sender=0) {
  $views[$sender].Write(204,$resource); $views[$sender].Write(208,$amount); $views[$sender].Write(212,$sequence)
  $deadline=[DateTime]::UtcNow.AddSeconds(8); $wasBusy=$false
  do {
   for($side=0;$side -lt 2;$side++) {
    $request=$views[$side].ReadInt32(228)
    if($request -and $request -ne $seen[$side]) {
     $kind=$views[$side].ReadInt32(220); $delta=$views[$side].ReadInt32(224)
     if($kind -lt 0 -or $kind -ge 6) { $applied=0 }
     elseif($delta -lt 0) { $applied=-[Math]::Min(-$delta,$balances[$side][$kind]) }
     elseif($side -eq 1 -and $kind -eq 2) { $applied=[Math]::Min($delta,30-$balances[$side][$kind]) }
     else { $applied=$delta }
     if($kind -ge 0 -and $kind -lt 6) { $balances[$side][$kind]+=$applied }
     $views[$side].Write(232,[int]$applied); $views[$side].Write(236,[int]$request)
     $seen[$side]=$request
    }
   }
   if($views[$sender].ReadInt32(216) -eq 1){$wasBusy=$true}
   if($wasBusy -and $views[$sender].ReadInt32(216) -eq 0 -and (& $complete)){return}
   Start-Sleep -Milliseconds 20
  } while([DateTime]::UtcNow -lt $deadline)
  throw "Trade $sequence did not complete"
 }
 # Repeated clicks use distinct transactions. The real injected DLL publishes these fields.
 Invoke-ClickAndPump 0 1 { $balances[0][0] -eq 50 -and $balances[1][0] -eq 110 }
 Invoke-ClickAndPump 4 2 { $balances[0][4] -eq 30 -and $balances[1][4] -eq 65 }
 # Stale UI/cache may still allow a click. Native debit clamps to -30; bridge refunds
 # exactly 30 and never sends a partial credit to the peer.
 Invoke-ClickAndPump 4 3 { $balances[0][4] -eq 30 -and $balances[1][4] -eq 65 }
 Invoke-ClickAndPump 3 4 { $balances[0][3] -eq 2 -and $balances[1][3] -eq 5 } 1
 Invoke-ClickAndPump 3 1 { $balances[0][3] -eq 3 -and $balances[1][3] -eq 4 } 1 1
 Invoke-ClickAndPump 0 5 { $balances[0][0] -eq 33 -and $balances[1][0] -eq 127 } 17
 Invoke-ClickAndPump 0 2 { $balances[0][0] -eq 50 -and $balances[1][0] -eq 110 } 17 1
 # Guest steel capacity is 30: credit only 5 of 17, refund exactly 12, not all 17.
 Invoke-ClickAndPump 2 6 { $balances[0][2] -eq 15 -and $balances[1][2] -eq 30 } 17
 # Reject invalid/overflow amounts without asking either native game to mutate.
 foreach($amount in 0,-1,1000001) {
  $views[0].Write(208,[int]$amount); $views[0].Write(212,[int](10+$amount))
  Start-Sleep -Milliseconds 150
  if($views[0].ReadInt32(228) -ne $seen[0]) { throw 'Invalid amount reached native apply' }
 }
 if($balances[0][0] -ne 50 -or $balances[1][0] -ne 110) {
  throw "Trade balances are wrong: host=$($balances[0][0]) guest=$($balances[1][0])"
 }
 foreach($child in $children) { $child.StandardInput.WriteLine('quit'); $child.StandardInput.Flush() }
 foreach($child in $children) { if(!$child.WaitForExit(5000)) { throw 'Bridge did not exit' } }
 $hostLog=$outputs[0].Result; $guestLog=$outputs[1].Result
 if(!$hostLog.Contains('[trade] Отправлено 50 угля игроку Борис.')) { throw 'Sender confirmation missing' }
 if(!$guestLog.Contains('[trade] Получено 50 угля от игрока Анна.')) { throw 'Recipient confirmation missing' }
 if(!$hostLog.Contains('[trade] Отправлено 50 сырой еды игроку Борис.')) { throw 'Repeated/resource-specific sender confirmation missing' }
 if(!$guestLog.Contains('[trade] Получено 50 сырой еды от игрока Анна.')) { throw 'Repeated/resource-specific recipient confirmation missing' }
 if($balances[0][4] -ne 30 -or $balances[1][4] -ne 65) { throw 'Partial debit was not refunded safely' }
 if($views[0].ReadInt32(216) -ne 0) { throw 'Overlay stayed busy after completion' }
 'PASS: arbitrary amounts, single core, bidirectional returns, invalid amounts rejected, repeated clicks, native apply handshake, debit-before-credit and partial-debit refund.'
} finally {
 foreach($child in $children) { if(!$child.HasExited) { $child.Kill(); $child.WaitForExit() }; $child.Dispose() }
 foreach($view in $views) { $view.Dispose() }
 foreach($map in $maps) { $map.Dispose() }
}
