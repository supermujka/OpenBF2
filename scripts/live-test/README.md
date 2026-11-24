# Live server test tools

`[CHOSEN, not recovered]` helpers used to tune hit registration and network settings on a running
Linux dedicated server with `bf2_loadbot` clients. None of them ship with the server; they read
the `openbf2_hitreg_*.csv` telemetry, the `[SV-PERF]` / `[SV-CPU]` console lines and the loadbot
log.

## Live console commands without a restart

The dedicated server reads no stdin when started with `+noStatusMonitor 1`, and on a server whose
embedded Python cannot load `pylib-2.3.4.zip` the stock `admin/default.py` (rcon) never loads.
`livecvar.py` is a dependency-free admin module:

1. Copy it to `<server>/admin/livecvar.py`.
2. Add `sv.adminScript "livecvar"` to the server config and restart once.
3. Write console commands, one per line, to `<server>/live_cvars.txt` (for example
   `vars.set phy-latencyComp-queueDelay 0` or `admin.restartMap`). About once a second, when the
   file content changes, every non-comment line is executed and logged to `live_cvars.log`. The
   content present at startup is not executed. Repeating an identical command needs a changed
   line, such as a `# <time>` comment.

## Soak run

`soak.sh` starts (if needed) the server in a `screen` session named `bf2`, applies the settings
listed at its top, starts 62 loadbots (31 v 31) in `screen` session `bots`, and then acts as a
watchdog: it restarts the server or bots when they exit, appends crash context to
`soak_crashes.log`, and writes a status line (RSS, tick time, CPU, bots, CSV size) to
`soak_status.log` every five minutes.

```bash
NAVMESH=/path/to/nav_karkand HOURS=8 LOADBOT=~/bf2_loadbot_aim bash soak.sh
```

Keep the controlling `wsl.exe` running and the host awake: WSL stops its VM, and the soak, when
no client process remains or the host sleeps. The server uses the config's `sv.serverPort`;
agent-run tests must not use the owner's port 16567.

## Analysis

| Script | Input | Output |
|---|---|---|
| `phases.py <server dir> [name]` | `exp_phases.log` written by a phase experiment | Active vs Vanilla per phase for one shooter |
| `qsum.py [name]` | same | input-queue depth, delay, replays and drops per queue-drain target |
| `netsum.py` | `net_phases.log`, console log, `~/bots.log` | tick phases, CPU, packets and bot snapshot rate per network preset |
| `noreg.py <server dir> [name]` | newest CSV (91+ columns) | input-index match check and "aim on target, server missed" breakdown |
| `misses.py <server dir>` | newest CSV | distance, bone, pose and burst breakdown of those misses |
| `devcomp.py <server dir> [name]` | newest CSV (102 columns) | deviation terms (min, speed, turn, misc, fire, pose, zoom) by outcome |

Phase experiments write lines `label|wall time|csv file|line count at switch` to
`exp_phases.log` (or `label|time|console lines|bot log lines` to `net_phases.log`) each time they
change a setting, so the scripts can split the telemetry by the setting that was live.
