<#
.SYNOPSIS
  Changes the controller's admin password over the API (hidden prompts).
  Use it when the web UI cannot, e.g. to leave the factory password so
  that OTA and the API unlock (SEC-01). Nothing is stored. Every session
  is logged out afterwards; run login.ps1 again for the test scripts.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File change_password.ps1
#>
param(
    [string]$Ip = '192.168.1.200',
    [string]$Username = 'admin'
)
$ErrorActionPreference = 'Stop'

function Read-Secret([string]$prompt) {
    $sec = Read-Host -Prompt $prompt -AsSecureString
    $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
}

$cur = Read-Secret "Current password for $Username@$Ip"
$new = Read-Secret 'New password (8+ characters)'
$rep = Read-Secret 'Repeat new password'
if ($new -ne $rep) { throw 'New passwords do not match.' }
if ($new.Length -lt 8) { throw 'New password must be at least 8 characters.' }
$rep = $null

try {
    $body = @{ username = $Username; password = $cur } | ConvertTo-Json -Compress
    $r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 5 -Method Post -ContentType 'application/json' -Body $body "http://$Ip/api/auth/login"
    $token = ($r.Content | ConvertFrom-Json).token
    if (-not $token) { throw 'Login failed.' }
    $body = @{ current_password = $cur; new_password = $new } | ConvertTo-Json -Compress
    $r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 8 -Method Post -ContentType 'application/json' `
        -Headers @{ Authorization = "Bearer $token" } -Body $body "http://$Ip/api/auth/change_password"
    $d = $r.Content | ConvertFrom-Json
    if (-not $d.ok) { throw "Change refused: $($d.error)" }
    Write-Host '[+] Admin password changed. All sessions were logged out.'
} catch {
    $resp = $_.Exception.Response
    if ($resp) {
        $msg = (New-Object IO.StreamReader($resp.GetResponseStream())).ReadToEnd()
        throw "HTTP $([int]$resp.StatusCode): $msg"
    }
    throw
} finally {
    $cur = $null; $new = $null; $body = $null
}
