# OpenBF2 Hitreg & Netcode — Clean Branch `hitreg-v1` Handoff

> **Za AI agenta / developera na kućnom računaru — PROČITAJ OVO PRVO!**
> Ova grana (**`hitreg-v1`**) je čista radna/produkcijska grana presečena sa `feat/hitreg` (`79e31f6`). Sadrži **sve finalne, dokazane optimizacije** (V1 + V1.1 + 30 Hz + V2 bone history + V3 drift-free tick & `queueDrain` + V3 Phase 2 Opcija A & C) i **pojednostavljenu, 5× bržu telemetriju od 82 kolone** koja poredi isključivo **Active (`act_*`, naše finalne optimizacije) vs Vanilla (`van_*`)** + forenziku (starih 11 V1/V2 `pf*`/`c17`/`c33`/`v11`/`v2a` sweep raycast-ova je uklonjeno; puna arhiva ostaje na grani `feat/hitreg`).

---

## 1. Cilj i tvrda pravila projekta (NE MENJATI bez odobrenja korisnika)
1. **Cilj:** Vrhunska hit detekcija i netcode za kompetitivne **4v4 / 5v5** pešadijske mečeve na 16-player mapama (pingovi `5–100 ms` + povremeni ISP jitter/packet loss).
2. **Server OS:** **Isključivo Linux dedicated server** (Windows-only fix-evi poput `timeBeginPeriod(1)`/`winmm` i `NetServer` `GetSystemTimeAsFileTime` nisu potrebni — Linux već koristi `clock_nanosleep` i `gettimeofday` sa mikrosekundnom preciznošću).
3. **Klijent kompatibilnost:** **100% kompatibilno sa neizmenjenim retail BF2 v1.50 Windows klijentima** (nikakve promene wire protokola/bitstream-a).
4. **Gameplay balans:** **`phy-latencyComp-closestBone = 0` MORA ostati vanilla** (korisnik je izričito odbio `closestBone = 1` jer menja balans oružja i TTK).
5. **CSV pravilo:** Nikad ne čitati sirove `openbf2_hitreg*.csv` fajlove u kontekst — uvek pokretati kratku Python skriptu koja ispisuje samo agregirane tabele.

---

## 2. Šta smo dokazali iz 27 realnih CSV fajlova (`69,943` FIRE+PROBE) i matematičkih simulacija

### A. Šta dokazano radi odlično (V1–V3 Phase 1.1)
- **V2 Root-Local Bone History:** Na V2 vatrenom oružju (`n=9,802`), V2 (`21.56%` hit / `1.43%` HS) nadmašuje V1.1 (`20.58%` / `1.38%`, `+96` pogodaka) i Vanillu (`18.95%` / `1.28%`, `+256` pogodaka). Na nišanjenim hicima (`flyby=0`, `n=1,074`), V2 ima **`+40.2%` više pogodaka od Vanille** i **`+83%` više headshot-ova od V1.1**. U 4v4 borbi `v2_head_shift_m > 10 cm` na **`53.3%`** provera (medijana `11.1 cm`, p90 `57.1 cm`).
- **V3 Drift-Free 30 Hz Tick na Linux-u:** `tick_us_p50` je zakucan na tačno **`33,333 us` u 100% redova**; `tick_us_p99` ima medijanu `33,542 us` (`+0.21 ms` jitter).
- **V3 Adaptive `ActionBuffer` Drain (`qDrain=1`):** Spustio je medijanu `q_depth` sa `4` (`84.3 ms`) na `2` (`36.5–47.9 ms`) i istovremeno **smanjio `replays_30t`** sa `2.22%` na `0.39%`.
- **`m_bandwidthChoke`:** Default je `0` (isključeno — ne guši 30 Hz).

### B. Ključno otkriće o retail `BF2.exe` v1.50 i ZAŠTO smo ODBACILI Opciju B (RTT `queueDrain` floor)
- U dekompajliranom OpenBF2 kodu (`ClientConnection.cpp:90,510`), `m_useAutoPingAdjust` je `false` (`[FRONTIER]`), zbog čega je prva analiza pretpostavila da retail `BF2.exe` uvek stoji zakucan na `interpolationTime = 200 ms`.
- **Dokazano na 18 pravih retail igrača u 27 CSV-ova:** Pravi retail `BF2.exe` v1.50 **IMA AKTIVAN `m_useAutoPingAdjust`**! Svakih ~256 primljenih paketa (`~8.5 s` na 30 Hz) automatski prilagođava `interpolationTime` u diskretnim `33 ms` koracima prema `q_depth`:
  - `qD=1` (`n=5,516`): `raw_interp_ms` medijana **`115 ms`** (`~100 ms` čisti interp + `~15 ms` lokalni lag).
  - `qD=2`: `126 ms`, `qD=3`: `161 ms`, `qD=4`: `215 ms`, `qD=5`: `224 ms`.
  - Isti igrač (`zzz Mujka[BIH]`, ping `23 ms`): u V2 (`qD=5`) imao je `227 ms`; u V3 (`qD=1..2`) retail klijent je sam spustio `interp` u koracima `302 -> 115 -> 98 -> 66 ms`.
- **Tačna IEEE-754 float32 simulacija (`60 min` / `108,000` tikova):**
  - Pri `interp = 100 ms` (gde retail klijent sam siđe kad je `qD = 1`), **`K = 0` i `K = 1` (ping `5–33 ms`) imaju `0.00%` underflow-a i `0.00%` ekstrapolacije!**
  - Stvarni udeo underflow-a u V3 CSV-u bio je samo **`3.48%`** (kratak prelaz od ~8.5 s dok klijentov auto-adjust sustigne `qDrain`).
  - **Zato je Opcija B (permanentni RTT floor `qD=3`) ODBAČENA:** dodala bi stalnih `+66.7 ms` input lag-a low-ping igračima, blokirala bi retail auto-adjust da spusti `interp` na `100 ms`, a pritom na `200 ms` granica `K = 2` ionako ima `13.70%` IEEE-754 float32 ULP underflow-a.
  - **Odbačeno je i ubrzavanje drain-a (`-1` pomeraj prstena / `0.5 s` po koraku):** simulacija je pokazala `0` dobitka u steady-state `qD`/replay-evima, a brži drop-ovi bi još više bežali klijentovom `~8.5 s` auto-adjust-u. **Zadržati `depthRingCount = 0` reset (`LatencyCompensation.cpp:550`) i `win = 60` (`2.0 s`)** — reset je namerna zaštita od duplog drain-a!

### C. Odložene stavke (NE ubacivati bez novih 4v4 podataka)
- `#6 getRawCollisionSkeleton()` (`SoldierResponse.cpp:1224`): odloženo za Phase 3 (prvo izmeriti da li pravi realan problem uz `closestBone=0`).
- `#4` Ghost resend prioritet `10000.0f` (`GhostManager.cpp:1606`): rizik od izgladnjivanja ostalih ghost-ova pri zasićenju heap-a; na ≤16 igrača heap se retko puni.
- `#5` Bitstream compression vector (`GhostManager.cpp`): visok rizik od desync/crash-a na retail v1.50 klijentima.
- `#7` Pomeranje `updateAnimationSystems` pre `postFrameUpdate` (`BF2Engine.cpp`): ogroman blast radius po celom engine-u.
- `#9` Child delovi vozila (`PointResponsePhysics.cpp`) / brzina putnika (`ObjectManager.cpp`): nizak ROI za 4v4 pešadiju.
- `pf` retuning (`pf075`/`pf10`/`pf125`): ostaviti `pf = 1.0` fiksno u ovom testu radi čiste atribucije.

---

## 3. Šta je implementirano u V3 Phase 2 (ovaj commit)

Sve izmene su **server-only**, bez promene ABI/layout-a/vtable-a i 100% kompatibilne sa retail BF2 v1.50 klijentima:

### 1) Opcija A — 30 Hz Snapshot Cadence Hardening
- **Fajl:** `Code/BF2/Game/GameServer/GameServer.cpp`
- **Šta radi:** Prati `g_currentUpdateSimSteps` u `GameServer::update()`. U `processGameStateAndSendPackets`, kada je `!recording && effectiveServerSendRate(conn) >= 30 && g_currentUpdateSimSteps >= 1 && LatencyCompensation::isImprovedEnabled()`, konekcija se uvek šalje jednom u tom simulacionom frejmu i `conn->m_timer1C` se resetuje na `0.0f` (eliminiše retke post-hitch `66.7 ms` rupe i sprečava catch-up burst). Za `sendRate < 30`, `dt < 0` (loading), `steps == 0` i demo snimanje ponašanje je bit-identično starom.
- **Verifikacija:** `HitregTelemetry::noteSnapshotSent()` beleži `snd_gap_ticks`.

### 2) Opcija C — Medic Revive & Vehicle Latency-Ring Reseed
- **Fajlovi:** `Code/BF2/Physics/SoldierPhysicsNode.cpp`, `Code/BF2/Game/Objects/Game/Soldier.h`
- **Problem koji rešava:** Dok je igrač man-down (`!m_enabled` ili `m_ragDoll != 0`), `updateLatencyState()` je nastavljao da upisuje `m_ragDoll->m_position` (`~1.0 m` niže po Y). Pri oživljavanju (`dy = +1.0 m < 3.0 m` teleport praga), rewind je potapao hitbox do `1.0 m` pod zemlju narednih `500 ms` (slično i kod ulaska/izlaska iz vozila unutar `3.0 m`).
- **Šta radi (iza `isSpawnRingFixEnabled()`):**
  - Preskače uzorkovanje dok je `!m_enabled || !soldier || soldier->m_ragDoll != 0` i resetuje `validSamples = 0`.
  - Prati `lastTick` i `lastParent` u `SoldierLatencyMeta`; na promenu `parent`-a ili `tickGap > 1` odmah radi pun reseed svih 16 slotova + `boneFrames` živom pozom.
- **Verifikacija:** `tgt_ticks_since_reseed` u CSV-u.

### 3) Čista `hitreg-v1` telemetrija (**82 kolone**, Active vs Vanilla + Forenzika, ~5× manji CPU trošak)
- **Fajlovi:** `Code/BF2/Physics/HitregTelemetry.h`, `Code/BF2/Physics/HitregTelemetry.cpp`, `Code/BF2/Game/Objects/Projectiles/GenericProjectile.cpp`, `Code/BF2/Game/Objects/Weapons/DeviationComponents/DeviationComp.cpp`, `Code/BF2/Game/Objects/Weapons/GenericFireArm.cpp`, `Code/BF2/Game/GameServer/GameServer.cpp`, `Code/Tests/runtime_pair.cpp`
- **Šta je uklonjeno u odnosu na `feat/hitreg` (143 -> 82 kolone):** svih 11 starih V1/V2 kalibracionih shadow sweep-ova (`pf00..pf125`, `raw05`, `uncl10`, `v11`, `nodedup`, `v2a`, `c17`, `c33`) — sada na svaki `PROBE` idu samo **3 raycast-a** (`Active`, `Vanilla` i `Zero-Deviation Aim`) umesto 14!
- **Struktura 82 kolone (`0..81`):**
  - `0..8`: `event,tick,time_s,shot_id,weapon,shooter_id,shooter,tgt_id,tgt`
  - `9..22`: `ping_ms,raw_ping_ms,rtt_jitter_ms,pkt_loss_dn_pct,raw_interp_ms,interp_ms,q_depth,q_delay_ms,replay_cnt,replays_30t,q_target,q_drain_drops,tick_us_p50,tick_us_p99`
  - `23..34`: `act_rewind_ms,van_rewind_ms,shot_dist_m,flight_ticks,coarse_dist_m,flyby,tgt_alive,tgt_mandown,tgt_live_pose,tgt_act_pose,tgt_speed,tgt_ticks_since_reseed`
  - `35..43` (**Active — naše finalne optimizacije**): `act_bone_hist,act_head_shift_m,act_hit,act_bone,act_bone_name,act_part,act_miss_m,act_near_bone,act_dt_err_ms`
  - `44..48` (**Vanilla baseline**): `van_hit,van_bone,van_part,van_miss_m,van_ring_unfilled`
  - `49..57` (**Forenzika `FIRE` + `DAMAGE` žrtva**): `dev_deg,dev_cone_deg,muzzle_off_m,sh_held_cum,sh_shots_cum,sh_fdrop_cum,sh_early_np_cum,snd_gap_ticks,q_drop_age_t`
  - `58..63` (**Forenzika `PROBE`**): `aim_hit,aim_miss_m,miss_dy_m,miss_lat_m,tgt_back_dot,ring_uf`
  - `64..71` (**`DELIVER`**): `outcome,blocker,blocker_mat,override_margin_m,t_wall_m,t_soldier_m,obj_class,eye_clear`
  - `72..81` (**`DAMAGE`**): `mat_a,mat_b,base_dmg,mat_mod,angle_mod,final_dmg,hp_before,hp_after,killed,hitmarker`
- **Cilj forenzike:** Definitivno dijagnostikovati scenario *„dođeš čoveku iza leđa, ispališ 15 metaka iz blizine, on se okrene i ubije te“* (`act_hit=0 && aim_hit=1` = čist RNG spread; `sh_shots_cum` mnogo manji od `sh_held_cum` na `DAMAGE` redu žrtve = server odbio hice; `sh_fdrop_cum > 0` = queue pojeo okidač; `sh_early_np_cum` = zid/prop pre mete; `miss_dy_m` vs `miss_lat_m` = stance/pitch vs rewind).

---

## 4. Šta uraditi kod kuće (Korak po korak)

> **VAŽNA NAPOMENA:** Na računaru gde je ovaj commit napisan nije bilo C++ kompajlera, pa su izmene verifikovane statičkim auditom + Python CSV-schema/snprintf validatorom (**`82/82` kolone**, tačan broj argumenata na svih 5 redova). **Prvi korak kod kuće je kompajliranje!**

### Korak 1: Pull i Linux build dedicated servera
```bash
git fetch origin
git checkout hitreg-v1
git pull
# Pokreni svoj standardni Linux dedicated CMake build za bf2_ded / bf2_l64ded
# (i po želji x86/tests: bf2_recovered, bf2_run_checks, runtime_pair)
```
Ako se pojavi bilo kakav compiler warning/error (npr. strict GCC `-Wformat` ili nedostajući include), popravi ga prvo pre test meča.

### Korak 2: Brzi 2-minutni sanity test pre 4v4 meča
1. Pokreni Linux server sa default V3 configom (`phy-latencyComp-telemetry 2`, `phy-latencyComp-closestBone 0`, `net-serverSendRate 30`).
2. Uradi 3 kratke provere:
   - **Stojeći ADS prvi metak:** u CSV-u mora biti `dev_deg <= dev_cone_deg` (provera da `aimDir` i `bulletDir` nemaju ofset osa).
   - **Medic revive:** ubij kolegu, oživi ga, pucaj u prvih 15 tikova → `tgt_ticks_since_reseed` treba da bude `0..15`, a hitbox na visini tela (ne pod zemljom).
   - **Provera broja kolona:** `python` proverom potvrdi da svi redovi imaju tačno **`82` kolone** (`0` `[HITREG] telemetry column mismatch` poruka na `stderr`).

### Korak 3: Analiza prvog 4v4 meča (Active vs Vanilla + Forenzika)
Kada snimiš 4v4 meč na Linux-u, pokreni Python skriptu (nikad sirov CSV u kontekst!) da proveriš:
1. **Active vs Vanilla:** `act_hit` / headshot (`act_bone==47`) vs `van_hit` / headshot (`van_bone==47`) — ukupno, samo vatreno oružje i `flyby==0`.
2. **Opcija A:** udeo `snd_gap_ticks > 1` treba da bude `~0%` (samo uz CPU hitch-eve).
3. **Opcija C:** za pogotke sa `tgt_ticks_since_reseed <= 15`, nema vertikalnog odstupanja posle revive-a/vozila.
4. **Retail ring transient:** ako je `ring_uf == 1` ukupno `< 2%` bez grupisanja posle `q_drop_age_t < 270`, **NE dirati `qDrain`**. Ako je `> 2%` grupisano odmah posle drop-ova, sledeći korak je jednostavna vremenska pauza od `~9 s` (`270` tikova) posle drop-a koji sleti na `qD <= 2` (nikad trajni floor koji kvari input lag!).
5. **Dijagnoza „15 metaka u leđa iz blizine“** (filtriraj `shot_dist_m <= 12` i `tgt_back_dot >= 0.5`):
   - `sh_shots_cum` delta mnogo manji od `sh_held_cum * RoF / 30` (poređenjem `FIRE` i `DAMAGE` žrtve!) → **server odbio hice (sprint/reload/switch desync)**.
   - `sh_fdrop_cum` delta `> 0` → **`qDrain`/cap pojeo okidač**.
   - `sh_early_np_cum` delta skočio bez `PROBE` → **metak udario u zid/ivicu/prop pre mete**.
   - `act_hit == 0 && aim_hit == 1 && dev_deg <= dev_cone_deg` → **čist BF2 RNG spread** (ako je pritom `dev_cone_deg` visok iako je igrač stajao → **movement/turn/stance desync pucača**).
   - `act_hit == 0 && aim_hit == 0 && |miss_dy_m| >> |miss_lat_m| && act_dt_err_ms ≈ 0` → **stance/crouch/prone/pitch desync**.
   - Velik `|miss_lat_m|` uz `act_dt_err_ms != 0` → **rewind / timing greška**.
   - `muzzle_off_m >= shot_dist_m` → **point-blank spawn cevi prošao iza mete**.
