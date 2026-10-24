param([string]$B = '192.168.1.200')
# Read-only smoke test: every GET route once, with status, size, time and
# JSON validity. Skips /api/sdcard/download (streams the whole log partition).
$routes = @(
  '/', '/dashboard', '/api/status', '/api/hardware', '/api/interfaces', '/api/analog', '/api/pwm', '/api/pto',
  '/api/config/actuators', '/api/config/mqtt', '/api/config/relays', '/api/config/sensors',
  '/api/rules', '/api/rules/history', '/api/rules/pending', '/api/layout',
  '/api/logs?limit=20', '/api/mqtt/log', '/api/modbus/scan', '/api/auth/me',
  '/api/sdcard/status', '/api/sdcard/browse', '/api/sdcard/read?file=system.log&offset=0&limit=20'
)
foreach ($r in $routes) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $code = 0; $len = 0; $json = '-'; $note = ''
  try {
    $resp = Invoke-WebRequest -UseBasicParsing -TimeoutSec 15 "http://$B$r"
    $code = [int]$resp.StatusCode; $len = $resp.RawContentLength
    $ct = "$($resp.Headers['Content-Type'])"
    if ($ct -match 'json') {
      try { $null = $resp.Content | ConvertFrom-Json; $json = 'ok' } catch { $json = 'INVALID'; $note = $resp.Content.Substring(0, [Math]::Min(80, $resp.Content.Length)) }
    }
  } catch {
    if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode } else { $note = $_.Exception.Message }
  }
  "{0,-58} {1,3} {2,7}B {3,7:N1}ms json={4} {5}" -f $r, $code, $len, $sw.Elapsed.TotalMilliseconds, $json, $note
}
