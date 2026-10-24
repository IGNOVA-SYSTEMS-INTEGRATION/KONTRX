param([string]$B = '192.168.1.200', [int]$Coil = 7, [int]$Cycles = 20, [int]$HoldMs = 300)
# Toggles ONE actuator ON/OFF via Modbus TCP FC05 (works only in test mode)
# and times each command round trip. Relay_SetState() drives the GPIO before
# the echo is sent, so this is an upper bound on command-to-output latency.
# Ends with the coil OFF. Run only with the user's approval for that output.
function Pct($v, $name) {
  $s = $v | Sort-Object; $n = $s.Count
  "{0}: n={1} min={2:N1} p50={3:N1} p95={4:N1} max={5:N1} ms" -f $name, $n, $s[0], $s[[int]($n/2)], $s[[int]($n*0.95)-1], $s[$n-1]
}
$c = New-Object System.Net.Sockets.TcpClient; $c.NoDelay = $true; $c.Connect($B, 502)
$st = $c.GetStream(); $st.ReadTimeout = 3000; $buf = New-Object byte[] 64
$on = New-Object System.Collections.Generic.List[double]; $off = New-Object System.Collections.Generic.List[double]; $err = 0
function Send-Coil([int]$tid, [bool]$state) {
  [byte[]]$req = 0, ($tid -band 255), 0, 0, 0, 6, 1, 5, 0, $Coil, ($(if ($state) { 0xFF } else { 0 })), 0
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $st.Write($req, 0, 12); $n = $st.Read($buf, 0, 64)
  $t = $sw.Elapsed.TotalMilliseconds
  if ($n -ne 12) { $script:err++ }
  return $t
}
try {
  for ($i = 0; $i -lt $Cycles; $i++) {
    $on.Add((Send-Coil (2 * $i) $true)); Start-Sleep -Milliseconds $HoldMs
    $off.Add((Send-Coil (2 * $i + 1) $false)); Start-Sleep -Milliseconds $HoldMs
  }
} finally {
  try { $null = Send-Coil 250 $false } catch { }
  $c.Close()
}
Pct $on "coil$Coil ON"
Pct $off "coil$Coil OFF"
"errors=$err"
$j = (Invoke-WebRequest -UseBasicParsing "http://$B/api/status").Content | ConvertFrom-Json
"final state coil$Coil = $($j.relays[$Coil].state) name=$($j.relays[$Coil].name) test_mode=$($j.test_mode)"
