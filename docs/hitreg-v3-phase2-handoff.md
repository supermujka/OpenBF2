# OpenBF2 Hitreg & Netcode — V3 Phase 2 Handoff (`feat/hitreg`)

> **Za AI agenta / developera na kućnom računaru — PROČITAJ OVO PRVO!**
> Ovaj fajl sadrži sve zaključke, simulacije, analize 27 realnih CSV fajlova (`125,943` redova), arhitektonske odluke i tačan plan za nastavak rada na grani `feat/hitreg`, tako da ne gubiš vreme na ponovno istraživanje ili vraćanje odbačenih ideja.

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

### 3) Forenzička telemetrija (`127 -> 143` kolona, 0 novih redova)
- **Fajlovi:** `Code/BF2/Physics/HitregTelemetry.h`, `Code/BF2/Physics/HitregTelemetry.cpp`, `Code/BF2/Game/Objects/Projectiles/GenericProjectile.cpp`, `Code/BF2/Game/Objects/Weapons/DeviationComponents/DeviationComp.cpp`, `Code/BF2/Game/Objects/Weapons/GenericFireArm.cpp`, `Code/BF2/Game/GameServer/GameServer.cpp`, `Code/Tests/runtime_pair.cpp`
- **Cilj:** Definitivno dijagnostikovati scenario *„dođeš čoveku iza leđa, ispališ 15 metaka iz blizine, on se okrene i ubije te“* — razdvajajući spread, server-odbijene hice, queue drop-ove, rani udar u zid, point-blank muzzle offset, stance/pitch mismatch i rewind grešku.
- **Trik koji garantuje tačnost:** `GenericProjectile::initOnLaunch()` snima tačan pre-deviation `aimDir` (`launchVel`), `DeviationComp::postFire()` odmah nakon `setPositionalSpeed(speed)` snima konačan `bulletDir`, `dev_deg` i tačan pre-shot `dev_cone_deg = atan(deviation / 100) * 180/pi`, a `GenericFireArm::postFire()` poziva `finalizeShotFired()` koji emituje `FIRE` red (sa `char line[1024]` + `appendCheckedLine` proverom truncation-a i tačnog broja zareza!).

#### Novih 16 kolona (indeksi `127..142`, postojeće `0..126` nisu pomerene!):
| Indeks | Kolona | Redovi | Značenje |
|---:|---|---|---|
| 127 | `dev_deg` | `FIRE` | Ugao (deg) između pre-deviation `aimDir` i konačnog `bulletDir` |
| 128 | `dev_cone_deg` | `FIRE` | Stvarni server-side 1-sigma konus devijacije oružja (deg) pre pucnja |
| 129 | `muzzle_off_m` | `FIRE` | `dot(startPos - eyePos, aimDir)` (point-blank spawn offset cevi) |
| 130 | `sh_held_cum` | `FIRE` (pucač), `DAMAGE` (žrtva) | Kumulativni prvokonzumirani tikovi sa držanim okidačem (`buttonMaskLow & 1`, bez replay-eva) |
| 131 | `sh_shots_cum` | `FIRE` (pucač), `DAMAGE` (žrtva) | Kumulativni broj `FIRE` metaka koje je server stvarno ispalio |
| 132 | `sh_fdrop_cum` | `FIRE` (pucač), `DAMAGE` (žrtva) | Kumulativni broj dropovanih inputa (`qDrain`/cap) sa držanim okidačem |
| 133 | `sh_early_np_cum` | `FIRE` (pucač), `DAMAGE` (žrtva) | Kumulativni broj projektila uništenih za `<= 1` tik bez ijednog `PROBE` reda (rani zid/prop) |
| 134 | `snd_gap_ticks` | `FIRE` | Broj tikova između poslednja 2 snapshot-a poslata ovom pucaču (Opcija A) |
| 135 | `q_drop_age_t` | `FIRE` | Tikovi od poslednjeg `qDrain` drop-a za ovog pucača (`-1` ako nije bilo) |
| 136 | `aim_hit` | `PROBE` | `0/1` shadow ray-test **čistog nišana bez spread-a** sa istog `shot.startPos` protiv V2 poze |
| 137 | `aim_miss_m` | `PROBE` | Distanca promašaja čistog nišana (`0.0000` ako `aim_hit=1`) |
| 138 | `miss_dy_m` | `PROBE` | Usmereni vertikalni promašaj (`+` iznad / `-` ispod najbliže kosti; prazno kad `v1_hit=1`) |
| 139 | `miss_lat_m` | `PROBE` | Usmereni bočni promašaj (`+` desno / `-` levo u BF2 left-handed sistemu; prazno kad `v1_hit=1`) |
| 140 | `tgt_back_dot` | `PROBE` | `dot(bulletDir, rewound_tgt_forward)` (`+1.0` = čist pucanj u leđa, `-1.0` = spreda) |
| 141 | `tgt_ticks_since_reseed` | `PROBE` | Tikovi od poslednjeg spawn/revive/vehicle reseed-a mete (Opcija C) |
| 142 | `ring_uf` | `PROBE` | Procenjeni retail 4-slot underflow (`1` ako `raw_interp_ms - 15 > (k_est + 4) * 33.333`, inače `0`) |

---

## 4. Šta uraditi kod kuće (Korak po korak)

> **VAŽNA NAPOMENA:** Na računaru gde je ovaj commit napisan nije bilo C++ kompajlera, pa su izmene verifikovane statičkim auditom + Python CSV-schema/snprintf validatorom (`143/143` kolone, tačan broj argumenata na svih 5 redova). **Prvi korak kod kuće je kompajliranje!**

### Korak 1: Pull i Linux build dedicated servera
```bash
git checkout feat/hitreg
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
   - **Provera broja kolona:** `python` proverom potvrdi da svi redovi imaju tačno `143` kolone (`0` `[HITREG] telemetry column mismatch` poruka na `stderr`).

### Korak 3: Analiza prvog 4v4 meča (kriterijumi uspeha i dijagnostika)
Kada snimiš 4v4 meč na Linux-u, pokreni Python skriptu (nikad sirov CSV u kontekst!) da proveriš:
1. **Opcija A:** udeo `snd_gap_ticks > 1` treba da bude `~0%` (samo uz CPU hitch-eve).
2. **Opcija C:** za pogotke sa `tgt_ticks_since_reseed <= 15`, nema vertikalnog odstupanja posle revive-a/vozila.
3. **Retail ring transient:** ako je `ring_uf == 1` ukupno `< 2%` bez grupisanja posle `q_drop_age_t < 270`, **NE dirati `qDrain`**. Ako je `> 2%` grupisano odmah posle drop-ova, sledeći korak (Phase 3) je jednostavna vremenska pauza od `~9 s` (`270` tikova) posle drop-a koji sleti na `qD <= 2` (nikad trajni floor koji kvari input lag!).
4. **Dijagnoza „15 metaka u leđa iz blizine“** (filtriraj `shot_dist_m <= 12` i `tgt_back_dot >= 0.5`):
   - `sh_shots_cum` delta mnogo manji od `sh_held_cum * RoF / 30` (poređenjem `FIRE` i `DAMAGE` žrtve!) → **server odbio hice (sprint/reload/switch desync)**.
   - `sh_fdrop_cum` delta `> 0` → **`qDrain`/cap pojeo okidač**.
   - `sh_early_np_cum` delta skočio bez `PROBE` → **metak udario u zid/ivicu/prop pre mete**.
   - `v2_hit == 0 && aim_hit == 1 && dev_deg <= dev_cone_deg` → **čist BF2 RNG spread** (ako je pritom `dev_cone_deg` visok iako je igrač stajao → **movement/turn/stance desync pucača**).
   - `v2_hit == 0 && aim_hit == 0 && |miss_dy_m| >> |miss_lat_m| && dt_err ≈ 0` → **stance/crouch/prone/pitch desync**.
   - Velik `|miss_lat_m|` uz `dt_err != 0` → **rewind / timing greška**.
   - `muzzle_off_m >= shot_dist_m` → **point-blank spawn cevi prošao iza mete** (ako je p99 `muzzle_off_m < 0.3 m`, u Phase 3 ovu kolonu možemo obrisati).
