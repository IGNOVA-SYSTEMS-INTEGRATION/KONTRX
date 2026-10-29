<#
.SYNOPSIS
  Kontrx OTA uploader for Windows PowerShell 5.1 (no Python needed).
  Run it yourself on a machine on the controller's LAN. It prompts for the
  admin password (used for login and, on current firmware, the OTA
  step-up check) and, only for older firmware, the OTA OTP. Nothing is
  stored or passed on the command line.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File ota_upload.ps1 -BinPath .\KontrxRTOS.bin
#>
param(
    [string]$Ip = '192.168.1.200',
    [Parameter(Mandatory = $true)][string]$BinPath,
    [string]$ExpectedSha256 = '',
    [string]$Username = 'admin'
)

$ErrorActionPreference = 'Stop'
$MaxFlashBytes = 224 * 1024

function Read-Secret([string]$prompt) {
    $sec = Read-Host -Prompt $prompt -AsSecureString
    $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
}

# One HTTP/1.1 POST over a raw socket (the firmware's HTTP server expects
# Content-Length and closes the connection). Returns @{Code; Body}.
function Invoke-RawPost([string]$Path, [hashtable]$Headers, [byte[]]$Body, [int]$TimeoutMs = 5000) {
    if (-not $Body) { $Body = [byte[]]@() }
    $c = New-Object System.Net.Sockets.TcpClient
    $c.ReceiveTimeout = $TimeoutMs; $c.SendTimeout = $TimeoutMs
    $c.Connect($Ip, 80)
    $s = $c.GetStream()
    $h = "POST $Path HTTP/1.1`r`nHost: $Ip`r`nContent-Length: $($Body.Length)`r`nConnection: close`r`n"
    if ($Headers) { foreach ($k in $Headers.Keys) { $h += "${k}: $($Headers[$k])`r`n" } }
    $h += "`r`n"
    $hb = [Text.Encoding]::ASCII.GetBytes($h)
    $s.Write($hb, 0, $hb.Length)
    if ($Body.Length -gt 0) { $s.Write($Body, 0, $Body.Length) }
    $raw = Read-AllResponse $s
    $c.Close()
    return (Split-Response $raw)
}

function Read-AllResponse($stream) {
    $ms = New-Object IO.MemoryStream
    $buf = New-Object byte[] 4096
    try { while (($n = $stream.Read($buf, 0, $buf.Length)) -gt 0) { $ms.Write($buf, 0, $n) } } catch { }
    return [Text.Encoding]::UTF8.GetString($ms.ToArray())
}

function Split-Response([string]$raw) {
    $code = 0
    $first = ($raw -split "`r`n", 2)[0]
    $parts = $first -split ' '
    if ($parts.Count -ge 2) { [int]::TryParse($parts[1], [ref]$code) | Out-Null }
    $body = ''
    $i = $raw.IndexOf("`r`n`r`n")
    if ($i -ge 0) { $body = $raw.Substring($i + 4) }
    return @{ Code = $code; Body = $body; Raw = $raw }
}

# fw + uptime_s. GET /api/ping is public on firmware with default-deny auth;
# older firmware only has the then-public /api/status.
function Get-BoardStatus {
    foreach ($path in '/api/ping', '/api/status') {
        try {
            $r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 5 "http://$Ip$path"
            return ($r.Content | ConvertFrom-Json)
        } catch { }
    }
    return $null
}

# ---- 1. Check the image ----
if (-not (Test-Path $BinPath)) { throw "Binary not found: $BinPath" }
$fw = [IO.File]::ReadAllBytes((Resolve-Path $BinPath))
$sha = (Get-FileHash $BinPath -Algorithm SHA256).Hash
if ($fw.Length -gt $MaxFlashBytes) { throw "Image is $($fw.Length) B, over the 224 KB staging limit." }
if ($ExpectedSha256 -and ($sha -ne $ExpectedSha256.ToUpper())) {
    throw "SHA256 mismatch: file $sha, expected $ExpectedSha256"
}

$before = Get-BoardStatus
if (-not $before) { throw "Controller $Ip is not answering /api/status." }

Write-Host ''
Write-Host "Target     : http://$Ip  (fw $($before.fw), uptime $($before.uptime_s) s)"
Write-Host "Image      : $BinPath"
Write-Host "Size       : $($fw.Length) bytes"
Write-Host "SHA256     : $sha"
Write-Host 'The controller resets after a successful upload; actuators re-initialise on boot.'
$answer = Read-Host 'Type FLASH to continue'
if ($answer -ne 'FLASH') { Write-Host 'Cancelled.'; exit 2 }

# ---- 2. Authenticate ----
$password = Read-Secret "Password for '$Username'"
$loginBody = [Text.Encoding]::UTF8.GetBytes((@{ username = $Username; password = $password } | ConvertTo-Json -Compress))
$r = Invoke-RawPost '/api/auth/login' @{ 'Content-Type' = 'application/json' } $loginBody 5000
$loginBody = $null
if ($r.Code -ne 200) { $password = $null; throw "Login failed (HTTP $($r.Code)): $($r.Body)" }
$login = $r.Body | ConvertFrom-Json
$token = $login.token
if (-not $token) { $password = $null; throw 'Login returned no session token.' }
if ($login.must_change) { $password = $null; throw 'Factory password still set: change it in the web UI, then retry.' }
$cookie = @{ 'Cookie' = "kx_session=$token" }
Write-Host '[+] Logged in.'

# ---- 3. OTA step-up ----
# Current firmware re-checks the admin password. Older firmware ignores it
# and answers 200 {"ok":false}; only then ask for its OTP.
$h = @{ 'Content-Type' = 'application/json'; 'Cookie' = "kx_session=$token" }
$vBody = [Text.Encoding]::UTF8.GetBytes((@{ password = $password } | ConvertTo-Json -Compress))
$password = $null
$r = Invoke-RawPost '/api/ota/verify' $h $vBody 4000
$vBody = $null
if ($r.Code -eq 200 -and $r.Body -match '"ok":false') {
    $otp = Read-Secret 'OTA OTP (older firmware)'
    $vBody = [Text.Encoding]::UTF8.GetBytes((@{ otp = $otp } | ConvertTo-Json -Compress))
    $otp = $null
    $r = Invoke-RawPost '/api/ota/verify' $h $vBody 4000
    $vBody = $null
}
if ($r.Code -ne 200 -or $r.Body -notmatch '"ok":true') { throw "OTA step-up rejected (HTTP $($r.Code)): $($r.Body)" }
Write-Host '[+] OTA step-up accepted.'

# ---- 4. Erase staging flash ----
$r = Invoke-RawPost '/api/ota/prepare' $cookie $null 8000
Write-Host "[+] Prepare sent (HTTP $($r.Code)); waiting for the staging erase..."
Start-Sleep -Milliseconds 3500

# ---- 5. Stream the image ----
$c = New-Object System.Net.Sockets.TcpClient
$c.SendTimeout = 30000; $c.ReceiveTimeout = 30000
$c.Connect($Ip, 80)
$s = $c.GetStream()
$hdr = "POST /update HTTP/1.1`r`nHost: $Ip`r`nContent-Type: application/octet-stream`r`n" +
       "Content-Length: $($fw.Length)`r`nCookie: kx_session=$token`r`nConnection: close`r`n`r`n"
$hb = [Text.Encoding]::ASCII.GetBytes($hdr)
$s.Write($hb, 0, $hb.Length)
$sent = 0; $chunk = 1024
while ($sent -lt $fw.Length) {
    $n = [Math]::Min($chunk, $fw.Length - $sent)
    $s.Write($fw, $sent, $n)
    $sent += $n
    if (($sent % 16384) -lt $chunk -or $sent -eq $fw.Length) {
        Write-Host ("`r[*] Upload {0}/{1} bytes ({2:N1}%)" -f $sent, $fw.Length, (100.0 * $sent / $fw.Length)) -NoNewline
    }
    Start-Sleep -Milliseconds 5
}
Write-Host ''
$resp = Split-Response (Read-AllResponse $s)
$c.Close()
if ($resp.Raw -notmatch '200 OK|OTA OK|Rebooting') { throw "Upload not acknowledged: $($resp.Raw)" }
Write-Host "[+] Upload acknowledged: $($resp.Body.Trim())"

# ---- 6. Wait for the reboot ----
Write-Host '[*] Waiting for the controller to come back...'
$deadline = (Get-Date).AddSeconds(120)
$after = $null
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    $st = Get-BoardStatus
    if ($st -and [int64]$st.uptime_s -lt [int64]$before.uptime_s -and [int64]$st.uptime_s -lt 600) { $after = $st; break }
}
if (-not $after) { throw 'Controller did not report a fresh boot within 120 s. Check it before retrying.' }
Write-Host "[SUCCESS] Controller back: fw $($after.fw), uptime $($after.uptime_s) s"
Write-Host "OTA_RESULT ok sha256=$sha"
