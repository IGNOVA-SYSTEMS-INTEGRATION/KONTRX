---
name: kontrx-ota
description: Build the Kontrx RTOS firmware, stage it over SSH on the remote LAN host, have the user flash it by OTA, then wait for the reboot and run latency/health tests against the saved baseline. Use when asked to "ارفع بال OTA", flash/OTA the controller, deploy firmware to the board, or test a new build on the board.
---

# Kontrx OTA deploy and test

The controller (default `192.168.1.200`) is only reachable from the remote LAN
host `kontrx-remote` (SSH alias in `~/.ssh/config`, Tailscale). That host has
Windows PowerShell 5.1 and no Python.

## Rule: the user authenticates, not Claude

The OTA needs the admin password and the OTA OTP. Claude never types, reads,
stores or sends them, and never runs `tools/ota_upload.py` (it hard-codes
them). The user runs `scripts/ota_upload.ps1` themselves; it prompts for both
with hidden input. Everything before and after that step is automated.

Per project rules, show the diff and get the user's go-ahead before an OTA of
code that has not been reviewed.

## Steps

1. **Preflight.** Confirm the user wants this build flashed. Note the branch
   and whether source files are uncommitted (stage.sh prints both).

2. **Build and stage** (Bash):
   ```bash
   bash .claude/skills/kontrx-ota/scripts/stage.sh kontrx-remote Desktop/kontrx-ota
   ```
   It builds, checks the 224 KB limit, copies `KontrxRTOS.bin`,
   `ota_upload.ps1` and `lat_test.ps1` to the remote folder, verifies the
   SHA256 there, and prints a `USER_COMMAND:` line. Stop if it fails.

3. **Start the reboot watcher** (Bash, `run_in_background: true`):
   ```bash
   bash .claude/skills/kontrx-ota/scripts/wait_reboot.sh kontrx-remote 192.168.1.200 60
   ```

4. **Hand off.** Give the user the `USER_COMMAND` line in a `powershell`
   block, to run in their own SSH session on the remote host
   (`ssh kontrx-remote`) or on its desktop. The script shows target, size and
   hash, asks them to type `FLASH`, prompts for password and OTP, uploads,
   and waits for the reboot. End the turn; do not poll.

5. **After `REBOOTED`** (the watcher's notification), run the tests from the
   remote host so the tunnel's latency does not skew them:
   ```bash
   ssh -o BatchMode=yes kontrx-remote "powershell -NoProfile -ExecutionPolicy Bypass -File Desktop/kontrx-ota/lat_test.ps1 -N 300"
   ```
   Also read `/api/status` (`sys.heap_free`, `cpu_pct`, `fw`, `uptime_s`) and
   `/api/logs?limit=20` (new entries after boot prove the log path works).
   Tests are read-only. Toggling any actuator needs the user's explicit OK.

6. **Report** each figure next to `baseline.md`, flag regressions (heap down,
   errors > 0, p95 up), and leave updating `baseline.md` to the user's call.

## Optional: local tunnel

For browsing or tests from this machine (ports 8200-8203 are reserved by
Windows here):
```bash
ssh -o BatchMode=yes -o ExitOnForwardFailure=yes -N -L 18080:192.168.1.200:80 -L 15020:192.168.1.200:502 kontrx-remote
```
Run it in the background; `http://localhost:18080`, Modbus TCP on `localhost:15020`.

## Files

- `scripts/stage.sh` — build, size/hash check, copy to remote, print user command.
- `scripts/ota_upload.ps1` — user-run uploader (login, OTP, prepare, `/update`, reboot wait).
- `scripts/wait_reboot.sh` — background watcher; prints `REBOOTED` or `TIMEOUT`.
- `scripts/lat_test.ps1` — Modbus TCP FC03 idle and under log-page load, HTTP status, log pages.
- `baseline.md` — last accepted measurements.
