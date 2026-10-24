param([int]$N = 300)
$B = '192.168.1.200'
function Pct($v, $name) {
  $s = $v | Sort-Object; $n = $s.Count
  "{0}: n={1} min={2:N1} p50={3:N1} p95={4:N1} p99={5:N1} max={6:N1} ms" -f $name, $n, $s[0], $s[[int]($n/2)], $s[[int]($n*0.95)-1], $s[[int]($n*0.99)-1], $s[$n-1]
}
function ModbusRun($count, $name) {
  $c = New-Object System.Net.Sockets.TcpClient; $c.NoDelay = $true; $c.Connect($B, 502)
  $st = $c.GetStream(); $st.ReadTimeout = 3000; $buf = New-Object byte[] 64
  $lat = New-Object System.Collections.Generic.List[double]; $err = 0
  for ($i = 0; $i -lt $count; $i++) {
    [byte[]]$req = 0, ($i -band 255), 0, 0, 0, 6, 1, 3, 0, 0, 0, 4
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try { $st.Write($req, 0, 12); $n = $st.Read($buf, 0, 64); if ($n -lt 9) { $err++ }; $lat.Add($sw.Elapsed.TotalMilliseconds) } catch { $err++ }
    Start-Sleep -Milliseconds 20
  }
  $c.Close()
  (Pct $lat $name) + " errors=$err"
}
ModbusRun $N 'modbus_idle'
$lat = New-Object System.Collections.Generic.List[double]
for ($i = 0; $i -lt 30; $i++) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 10 "http://$B/api/status"
  $lat.Add($sw.Elapsed.TotalMilliseconds)
}
(Pct $lat 'http_status') + " bytes=" + $r.RawContentLength
# Concurrent: log pages (flash reads under sdMutex) while Modbus TCP runs
$job = Start-Job -ScriptBlock { param($B) $t = New-Object System.Collections.Generic.List[double]
  for ($k = 0; $k -lt 8; $k++) { $sw = [Diagnostics.Stopwatch]::StartNew(); try { $null = Invoke-WebRequest -UseBasicParsing -TimeoutSec 20 "http://$B/api/logs?limit=50&offset=$($k*50)" } catch {}; $t.Add($sw.Elapsed.TotalMilliseconds) }
  $t } -ArgumentList $B
ModbusRun $N 'modbus_during_logs'
$t = Receive-Job $job -Wait; Remove-Job $job
Pct $t 'http_logs_page'
$j = (Invoke-WebRequest -UseBasicParsing "http://$B/api/status").Content | ConvertFrom-Json
"sys: cpu=$($j.sys.cpu_pct) heap_free=$($j.sys.heap_free) fw=$($j.fw) uptime=$($j.uptime_s)"
