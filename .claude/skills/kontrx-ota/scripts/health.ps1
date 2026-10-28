param([string]$B = '192.168.1.200')
# Read-only health snapshot after an OTA: status, sensors, MQTT, recent logs.
# Gated routes need the session saved by login.ps1.
. (Join-Path $PSScriptRoot 'session.ps1')
$H = Get-AuthHeaders
function Get-Json($path) {
    try { return ((Invoke-WebRequest -UseBasicParsing -TimeoutSec 10 -Headers $H "http://$B$path").Content | ConvertFrom-Json) }
    catch { Write-Host "GET $path failed: $($_.Exception.Message)"; return $null }
}
$s = Get-Json '/api/status'
if ($s) {
    "fw=$($s.fw) uptime=$($s.uptime_s) cpu=$($s.sys.cpu_pct) heap_free=$($s.sys.heap_free) test_mode=$($s.test_mode) rules=$($s.rules_version_id)"
    "mqtt: connected=$($s.mqtt.connected) broker=$($s.mqtt.broker) published=$($s.mqtt.publish_count) last_error=$($s.mqtt.last_error) log_in_status=$($null -ne $s.mqtt.log)"
    foreach ($x in $s.sensors) { "sensor $($x.type) id=$($x.id) valid=$($x.valid) value=$($x.value)" }
    if ($s.multi_sensor) { "multi_sensor: " + ($s.multi_sensor | ConvertTo-Json -Compress -Depth 4).Substring(0, [Math]::Min(300, ($s.multi_sensor | ConvertTo-Json -Compress -Depth 4).Length)) }
    "relays: " + ($s.relays | ConvertTo-Json -Compress)
}
$m = Get-Json '/api/mqtt/log'
if ($m) { "mqtt_log entries=$(@($m.log).Count)" }
$l = Get-Json '/api/logs?limit=15'
if ($l) { foreach ($e in $l.logs) { "log up_s=$($e.up_s) [$($e.cat)] $($e.msg)" } }
