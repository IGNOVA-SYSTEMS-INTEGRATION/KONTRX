# Dot-sourced by the read-only test scripts. Returns the request headers that
# carry the session saved by login.ps1, or an empty table when there is none
# (then gated routes answer 401 and only public checks pass).
function Get-AuthHeaders {
    $path = Join-Path $PSScriptRoot '.session'
    if (Test-Path $path) {
        $t = (Get-Content -Path $path -Raw).Trim()
        if ($t) { return @{ Cookie = "kx_session=$t" } }
    }
    Write-Host '[!] No .session: run login.ps1 first for authenticated routes.'
    return @{}
}
