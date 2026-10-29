# Dot-sourced by the read-only test scripts. Returns the request headers that
# carry the session saved by login.ps1, or an empty table when there is none
# (then gated routes answer 401 and only public checks pass).
function Get-AuthHeaders {
    $path = Join-Path $PSScriptRoot '.session'
    if (Test-Path $path) {
        $t = (Get-Content -Path $path -Raw).Trim()
        # Bearer, not Cookie: PowerShell 5.1 Invoke-WebRequest drops a
        # Cookie passed through -Headers.
        if ($t) { return @{ Authorization = "Bearer $t" } }
    }
    Write-Host '[!] No .session: run login.ps1 first for authenticated routes.'
    return @{}
}
