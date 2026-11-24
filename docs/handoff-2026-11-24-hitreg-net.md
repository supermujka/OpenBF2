# Handoff 2026-11-24: hitreg, deviation bug, network presets, loadbot

Sažetak jedne duge sesije. Novi agent: pročitaj ovo, pa `AGENTS.md`.

## Grane (sve pushane na origin)

| Grana | Šta sadrži |
|---|---|
| `feat/hitreg` @ `de6bdb4` | hitreg V3 + **ispravka x64 bug-a poze** (cherry-pick). Ovo je grana koju vlasnik koristi u produkciji. |
| `feat/net-mt-hitreg-fixes` | `feat/net-mt-32v32` + analyzer CSV-a + telemetrija devijacije (102 kolone) + `[SV-CPU]` + ista ispravka poze + `scripts/live-test/` (live CVar hook, soak watchdog, analize) + ovaj dokument |
| `feat/loadbot-aim-teams` | loadbot: ping po botu, ispravka timova, "ljudsko" nišanjenje (još ne radi dobro) |
| `feat/net-mt-32v32` (origin) | `.claude/agents/` (haiku auditor, sonnet implementer) |

## Potvrđeni nalazi

1. **Bug x64 (ispravljen, `de6bdb4` / `056e28e`)**: `SoldierDeviationComp::updateDeviation` čitao je
   sirovi x86 offset `response[0xFA]` (ground contact). Na x64 to je drugi član → uvijek 0 → poza
   forsirana na Stand → `devModStand` (1.3) i kad čučiš/ležiš. Server je pucao šire od retail klijenta
   → klijent crta pogodak, server promaši, nema hitmarkera. Ispravka: `SoldierResponsePhysics::m_byteFA`
   (friend). Potvrđeno telemetrijom: prije 1.3/1.3/1.3, poslije 1.3/1.2/1.0 = `RURIF_AK101.tweak`.
   Pogađa samo native x64 servere (Linux, `bf2_w64ded`).
2. **Input index**: server koristi isti `currentInputIndex` kao klijentova akcija na 100% svježih hitaca.
   Nije uzrok.
3. **`updateDeviation` se NE poziva dvaput za pješadiju**: `PlayerControlObject.cpp:1583` je samo za
   vozila; `Soldier` nije PCO. Hipoteza odbačena, kod ne mijenjati.
4. **Preostali "nišan na meti, server promašio"** = retail rasipanje (minDev 0.45, okrznuća 5–15 cm),
   ne hitreg. Active i Vanilla se tu slažu.
5. **`queueDelay`**: default `phy-latencyComp-queueDelay 1` dodaje ~45–70 ms rewinda; na metama koje
   trče Active gubi vs Vanilla. `queueDelay 0` (C) izjednačava. Pri pingu ~25 ms nijedna opcija nije
   statistički bolja od vanile; prava razlika se vidi tek na 80–150 ms.
   **Ali na pravom internetu (Oracle, 2 prava igrača, 8.6 min, default qDelay=1)** red je bio 2–3,
   i live (v1) je registrovao više od vanile: Mujka 43 vs 26, SYNKRONIZED 38 vs 25 (v1-only vs
   van-only). Zaključak: lokalni test bez jittera nije reprezentativan; `queueDelay 1` ostaviti dok
   gather CSV ne pokaže suprotno. Tamo je i `OVERRIDE_WALL` 7.7% isporuka (lokalno ~2%), kandidat
   za "pogodak bez štete".
   **`maxClientInterpMs` na pravom serveru** (default u kodu 133, retail samo 512 tehnički):
   133 → live vs vanilla +1.5/100; 250 → ~0; **50 → −0.7 (lošije, rewind premali)**; 450 → +1.3.
   133 i 450/350 su statistički isti. Vlasnik i drugi igrač subjektivno osjećaju 350 kao
   "ogromno bolje" (u istom periodu: restart mape, friendly fire 0, manje rafala), pa je
   **350 upisan trajno** u `karkand_inf64.con`, uz `queueDrainMaxTarget 4` i friendly fire 0.
   Promjene na serveru se bilježe u `/home/bf2/server/live_changes.log`.
6. **`queueDrainMaxTarget`**: kod stabilne veze red je uvijek 1, delay 0 → postavka nema efekta.
   Vlasnik subjektivno preferira 1; za igrače s jitterom 2 je kompromis.
7. **Kapacitet 64 igrača (Karkand, loadbotovi)**: tik p50 ~5.7 ms / p99 ~7.5 ms od 33.3 ms, glavna nit
   ~22% jezgra, RSS ~190 MB, 0 overruna. Mrežni preseti nisu potrebni:
   - Preset B: duplo veći paketi (~1338 B), ~20 Mbit/s upload na 64 igrača, retail klijent nije provjeren.
   - Preset C (MT): ušteda ~0.8 ms, `net-mt-verify` 0 mismatcheva.
8. **30 Hz vs 20 Hz send rate**: +1 pp CPU, +50% uploada (~15 vs ~10 Mbit/s na 64), razmak snapshota
   p95 44 ms vs 73 ms. Zadržati 30 Hz.
9. **Python admin na Linux serveru ne radi** (`pylib-2.3.4.zip` se ne učitava) → stock `default.py`
   (rcon) i `standard_admin/autobalance` nikad ne rade. Rješenje za live komande:
   `scripts/live-test/livecvar.py` + `sv.adminScript "livecvar"` + `live_cvars.txt`.

## Loadbot

- Server svakog igrača kreira u timu 2; loadbot je to preuzimao prije svog JoinTeam → svi USMC. Ispravljeno.
- Server vidi ping bota ~51 ms veći od podešenog (bot ackuje samo u svojoj 30 Hz petlji).
- Novo nišanjenje: botovi i dalje rijetko pucaju i promašuju bočno, a borbe su na 1–5 m.
  **Sljedeći sumnjivac**: skaliranje yaw inputa. Server: `yawDelta = mouseLookX * phy-soldier-look-factor-x
  * mouseSpeedMod + recoil` (`Soldier.cpp:3821`). Bot šalje `deg / 5`; provjeriti jedinice ugla
  (deg vs rad) i mouseSpeedMod.

## Okruženja

- **Lokalno WSL** (`~/bf2server`, port 16567 je vlasnikov): `~/bf2-linux-claude` (net-mt build),
  `~/bf2_loadbot_aim`, `~/bf2_loadbot_pingdist` (wrapper `--aim-skill 1.0`). WSL se restartuje ako
  Windows spava; dugi testovi traže "Sleep: Never".
- **Udaljeni server** (Oracle, x86_64, 2 jezgra, 956 MB RAM + 4 GB swap; PuTTY sesija `bf2`,
  korisnik `ubuntu`, `plink -batch -load bf2 -l ubuntu`):
  - repo `/home/ubuntu/OpenBF2` na `feat/hitreg`, build dir `/home/ubuntu/openbf2-build`
    (build obavezno `-j1`).
  - server dir `/home/bf2/server`, pokreće se kao `bf2`: screen `bf2server`,
    `./start.sh +modPath mods/bf2 +config mods/bf2/settings/karkand_inf64.con
    +mapList mods/bf2/settings/karkand_maplist.con +maxPlayers 64 +noStatusMonitor 1`.
  - `karkand_inf64.con` = kopija `serversettings.con` + 64 igrača, `sv.noVehicles 1`,
    `sv.autoBalanceTeam 0`.
  - Stari binarni: `bin/amd-64/bf2.pre-posefix`. Log: `/home/bf2/server/screen_bf2server.log`,
    stderr `/home/bf2/bf2_stderr.log`. Telemetrija: `/home/bf2/hitreg_logs/`.
  - Očišćeno: bf2.top node handler, BF2CC autostart, bf2.top env i profili (karantena
    `/home/bf2/_bf2top_removed_20261124_205534/`, backup `/home/ubuntu/bf2top-backup-*.tar.gz`),
    root auto-reboot 03:00/14:00, nginx. **Nema autostarta servera nakon reboota.**
  - Rcon lozinka je bila u čistom tekstu u `.bashrc`/profilu: promijeniti.
  - `std_archive.md5` preimenovan u `.bak` (inače server izbacuje loadbotove na provjeri arhiva).
  - **Kapacitet Oracle Micro**: 62 bota = bf2 ~94% CPU + ~31% steal, tik p99 do 190 ms,
    snapshot p95 5.7 Hz (teleportovanje). 30 botova = ~28% CPU, snapshot p95 ~27 Hz, uz povremene
    skokove. Za 32v32 treba jača mašina. `feat/hitreg` build nema `[SV-PERF]`/`[SV-CPU]`.
  - Loadbotovi prema serveru idu iz lokalnog WSL-a (`~/bf2_loadbot`, svi USMC, `--password`).

## Otvoreno

1. Udaljeni server je gore od 2026-11-24 21:03 s buildom `de6bdb4` (screen `bf2server`, UDP 16567 i
   29900, Karkand učitan). Dodati `@reboot` autostart ako treba; testirati igru s klijentom.
2. Odlučiti trajno: `queueDelay 0`, `queueDrainMaxTarget` (1 ili 2), eventualno `phy-latencyComp-max` 0.20–0.25.
3. Prikupiti CSV s pravog gathera i napraviti "duel" analizu (zadnje 3 s prije svake smrti).
4. Popraviti yaw skaliranje loadbota, zatim noćni soak 31v31 (`scripts/live-test/soak.sh`).
5. IDA/Ghidra potvrde po AGENTS.md za sve što se tvrdi kao retail ponašanje.
