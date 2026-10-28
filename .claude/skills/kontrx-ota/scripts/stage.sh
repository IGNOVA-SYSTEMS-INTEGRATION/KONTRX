#!/usr/bin/env bash
# Build KontrxRTOS.bin and stage it, with the OTA tools, on the remote LAN host.
# Usage: stage.sh [ssh-host] [remote-dir]
# Prints the exact command the user runs there (it prompts for credentials).
set -euo pipefail

HOST="${1:-kontrx-remote}"
RDIR="${2:-Desktop/kontrx-ota}"
REPO="$(git rev-parse --show-toplevel)"
SKILL="$REPO/.claude/skills/kontrx-ota/scripts"
BIN="$REPO/build/KontrxRTOS.bin"
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10)
quiet() { grep -v "post-quantum\|store now\|openssh.com/pq" || true; }

export PATH="/c/Program Files (x86)/GNU Arm Embedded Toolchain/10 2021.10/bin:$PATH"

echo "[*] Building..."
if ! cmake --build "$REPO/build" >"$REPO/build/ota_stage_build.log" 2>&1; then
    grep -E "error|FAILED" "$REPO/build/ota_stage_build.log" | head -20
    echo "[-] Build failed; nothing staged."
    exit 1
fi

SIZE=$(stat -c %s "$BIN")
if [ "$SIZE" -gt $((224 * 1024)) ]; then
    echo "[-] $BIN is $SIZE B, over the 224 KB staging limit."
    exit 1
fi
SHA=$(sha256sum "$BIN" | cut -d' ' -f1 | tr 'a-f' 'A-F')
BRANCH=$(git -C "$REPO" branch --show-current)
DIRTY=$(git -C "$REPO" status --porcelain -- APP HAL MCAL CMakeLists.txt web | wc -l)
echo "[+] Built $BIN ($SIZE B) from branch $BRANCH ($DIRTY uncommitted source files)"
echo "[+] SHA256 $SHA"

ssh "${SSH_OPTS[@]}" "$HOST" "powershell -NoProfile -Command \"New-Item -ItemType Directory -Force '$RDIR' | Out-Null\"" 2>&1 | quiet
scp -q "${SSH_OPTS[@]}" "$BIN" "$HOST:$RDIR/KontrxRTOS.bin" 2>&1 | quiet
scp -q "${SSH_OPTS[@]}" "$SKILL/ota_upload.ps1" "$SKILL/lat_test.ps1" "$SKILL/login.ps1" "$SKILL/session.ps1" \
    "$SKILL/health.ps1" "$SKILL/smoke_get.ps1" "$HOST:$RDIR/" 2>&1 | quiet

RSHA=$(ssh "${SSH_OPTS[@]}" "$HOST" "powershell -NoProfile -Command \"(Get-FileHash '$RDIR/KontrxRTOS.bin' -Algorithm SHA256).Hash\"" 2>&1 | quiet | tr -d '\r\n ')
if [ "$RSHA" != "$SHA" ]; then
    echo "[-] Remote hash $RSHA does not match $SHA."
    exit 1
fi
echo "[+] Staged on $HOST:$RDIR (hash verified)"

WINDIR="${RDIR//\//\\}"
echo
echo "USER_COMMAND:"
echo "cd \"\$HOME\\$WINDIR\"; powershell -NoProfile -ExecutionPolicy Bypass -File .\\ota_upload.ps1 -BinPath .\\KontrxRTOS.bin -ExpectedSha256 $SHA"
