#!/usr/bin/env bash
# Wait until the controller reports a fresh boot, polling from the remote host.
# Usage: wait_reboot.sh [ssh-host] [board-ip] [timeout-minutes]
# Prints REBOOTED <uptime> <fw> or TIMEOUT. Run it in the background.
# An uptime under 600 s on the first poll counts as already rebooted.
set -uo pipefail

HOST="${1:-kontrx-remote}"
IP="${2:-192.168.1.200}"
MINUTES="${3:-60}"
PS='try { $j = (Invoke-WebRequest -UseBasicParsing -TimeoutSec 5 http://'"$IP"'/api/status).Content | ConvertFrom-Json; "$($j.uptime_s) $($j.fw)" } catch { "down" }'
# PowerShell -EncodedCommand wants base64 of UTF-16LE (Git Bash has no iconv).
ENC=$(printf %s "$PS" | python -I -c "import sys,base64;print(base64.b64encode(sys.stdin.read().encode('utf-16-le')).decode())")

first=""
out=""
end=$(( $(date +%s) + MINUTES * 60 ))
while [ "$(date +%s)" -lt "$end" ]; do
    out=$(ssh -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "powershell -NoProfile -EncodedCommand $ENC" 2>/dev/null | tr -d '\r' | tail -1)
    up=${out%% *}
    if [[ "$up" =~ ^[0-9]+$ ]]; then
        [ -z "$first" ] && first=$up
        if [ "$up" -lt 600 ] && { [ "$up" -lt "$first" ] || [ "$first" -lt 600 ]; }; then
            echo "REBOOTED uptime=$up fw=${out#* } at $(date +%T)"
            exit 0
        fi
    fi
    sleep 15
done
echo "TIMEOUT last=$out"
exit 1
