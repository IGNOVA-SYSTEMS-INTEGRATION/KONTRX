<#
.SYNOPSIS
  Logs in to the controller and saves ONLY the session token (never the
  password) to .session next to this script, readable by the current user
  only. The read-only test scripts send it as a cookie.
  Run it yourself after each reboot: sessions live in the controller's RAM.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File login.ps1
#>
param(
    [string]$Ip = '192.168.1.200',
    [string]$Username = 'admin'
)
$ErrorActionPreference = 'Stop'

$sec = Read-Host -Prompt "Admin password for $Username@$Ip" -AsSecureString
$ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
try { $password = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) }
finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }

$body = @{ username = $Username; password = $password } | ConvertTo-Json -Compress
$password = $null
try {
    $r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 5 -Method Post -ContentType 'application/json' `
        -Body $body "http://$Ip/api/auth/login"
} finally { $body = $null }
$token = ($r.Content | ConvertFrom-Json).token
if (-not $token) { throw 'Login failed: no token in response.' }

$path = Join-Path $PSScriptRoot '.session'
Set-Content -Path $path -Value $token -NoNewline -Encoding ASCII
# Owner-only ACL: drop inherited rules, grant the current user full control.
# icacls edits only the DACL; Set-Acl also rewrites the SACL and needs
# SeSecurityPrivilege, which a normal shell does not hold.
$me = [Security.Principal.WindowsIdentity]::GetCurrent().Name
& icacls $path /inheritance:r /grant:r "${me}:F" | Out-Null
if ($LASTEXITCODE -ne 0) { Remove-Item $path -Force; throw "Could not restrict $path; token not kept." }
Write-Host "[+] Session saved to $path (token only). It is lost when the controller reboots."
