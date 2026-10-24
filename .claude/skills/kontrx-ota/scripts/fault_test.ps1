<#
.SYNOPSIS
  Fault-recovery test for a KONTRX_FAULT_TEST image (fw "...-faulttest").
  The user runs it: it prompts for the admin password (hidden input).
  Each case makes one task fault and checks that the board did NOT reset,
  the fault was recorded, the task was restarted, and the rest kept working.
  Outputs are not commanded by this script.
#>
param(
    [string]$Ip = '192.168.1.200',
    [string]$Username = 'admin',
    [int]$WaitS = 4
)
$ErrorActionPreference = 'Stop'

function Get-Status { (Invoke-WebRequest -UseBasicParsing -TimeoutSec 8 "http://$Ip/api/status").Content | ConvertFrom-Json }
function Test-Modbus {
    try {
        $c = New-Object System.Net.Sockets.TcpClient; $c.Connect($Ip, 502); $s = $c.GetStream(); $s.ReadTimeout = 2000
        [byte[]]$q = 0, 1, 0, 0, 0, 6, 1, 3, 0, 0, 0, 1; $s.Write($q, 0, 12); $b = New-Object byte[] 32
        $n = $s.Read($b, 0, 32); $c.Close(); return ($n -ge 9)
    } catch { return $false }
}

$st = Get-Status
if ($st.fw -notmatch 'faulttest') { throw "Board runs fw $($st.fw); flash the -faulttest image first." }

$sec = Read-Host -Prompt "Password for '$Username'" -AsSecureString
$ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
try { $pw = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) } finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
$body = @{ username = $Username; password = $pw } | ConvertTo-Json -Compress; $pw = $null
$login = Invoke-WebRequest -UseBasicParsing -Method Post -ContentType 'application/json' -Body $body "http://$Ip/api/auth/login"
$body = $null
$token = ($login.Content | ConvertFrom-Json).token
if (-not $token) { throw 'Login failed.' }
$hdr = @{ Authorization = "Bearer $token" }

# task, type, label   (types: 1 bad read, 2 undefined instr, 3 stack overflow, 4 malloc fail, 5 fault holding configMutex)
$cases = @(
    @('ModbusPoll', 1, 'BusFault'), @('ModbusPoll', 2, 'UsageFault'), @('ModbusPoll', 3, 'Stack overflow'),
    @('MQTTClient', 1, 'BusFault'), @('IOWorker', 2, 'UsageFault'), @('LogWriter', 1, 'BusFault (supervisor itself)'),
    @('ControlEng', 1, 'BusFault'), @('ControlEng', 3, 'Stack overflow'),
    @('ModbusPoll', 5, 'Fault while holding configMutex'), @('HTTPServer', 4, 'Malloc failure'),
    @('HTTPServer', 1, 'BusFault in HTTP task')
)

$results = @()
foreach ($c in $cases) {
    $task, $type, $label = $c
    $before = Get-Status
    try {
        $null = Invoke-WebRequest -UseBasicParsing -Method Post -Headers $hdr -TimeoutSec 5 "http://$Ip/api/debug/fault?task=$task&type=$type"
    } catch { }   # the HTTP task may die before answering
    Start-Sleep -Seconds $WaitS
    $after = $null
    for ($k = 0; $k -lt 5 -and -not $after; $k++) { try { $after = Get-Status } catch { Start-Sleep -Seconds 2 } }
    if (-not $after) { $results += [pscustomobject]@{ Case = "$task/$label"; Result = 'FAIL'; Detail = 'no /api/status after fault' }; continue }

    $noReset   = ([int64]$after.uptime_s -ge [int64]$before.uptime_s)
    $ti        = $after.health.tasks | Where-Object { $_.n -eq $task }
    $tb        = $before.health.tasks | Where-Object { $_.n -eq $task }
    if ($type -eq 4) {
        $recorded = ([int64]$after.health.malloc_fail -gt [int64]$before.health.malloc_fail)
        $restarted = $true
    } else {
        $recorded  = ([int64]$after.health.faults -gt [int64]$before.health.faults)
        $restarted = ($ti -and [int]$ti.state -eq 0 -and [int64]$ti.restarts -gt [int64]$tb.restarts)
    }
    $modbus = Test-Modbus
    $mqtt   = ([int]$after.mqtt.connected -eq 1)
    $cfgOk  = $true
    if ($type -eq 5) { try { $null = Invoke-WebRequest -UseBasicParsing -TimeoutSec 5 "http://$Ip/api/config/sensors" } catch { $cfgOk = $false } }
    $pass = $noReset -and $recorded -and $restarted -and $modbus -and $cfgOk
    $results += [pscustomobject]@{
        Case   = "$task/$label"
        Result = $(if ($pass) { 'PASS' } else { 'FAIL' })
        Detail = "noReset=$noReset recorded=$recorded restarted=$restarted modbus=$modbus mqtt=$mqtt cfg=$cfgOk last=$($after.health.last_fault.type)/$($after.health.last_fault.task)"
    }
}
$results | Format-Table -AutoSize -Wrap
$final = Get-Status
"Final: fw=$($final.fw) uptime=$($final.uptime_s) faults=$($final.health.faults) restarts=$($final.health.restarts) malloc_fail=$($final.health.malloc_fail) heap_free=$($final.health.heap_free) heap_min=$($final.health.heap_min)"
"FAULT_TEST_RESULT pass=$(@($results | Where-Object Result -eq 'PASS').Count)/$($results.Count)"
