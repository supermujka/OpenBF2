# OpenBF2 32v32 MT Netcode — Branch `feat/net-mt-32v32` Handoff

> **Status:** Grana `feat/net-mt-32v32` je presečena sa `hitreg-v1` (`9cd3527`).
> **Garancije:**
> - `hitreg-v1` je **netaknut** (`phy-latencyComp-closestBone = 0`, Opcija A `g_currentUpdateSimSteps` / `forceFullRateSend` i 82-kolonska `HitregTelemetry` su očuvani).
> - **Nula ABI / layout / vtable promena** na postojećim engine klasama — 100% kompatibilno sa neizmenjenim retail BF2 v1.50 Windows klijentima.
> - **Svi novi feature CVar-ovi podrazumevano su `0` / `0.0f`** (off-by-default), tako da server sa default podešavanjima radi bit-identično `hitreg-v1` grani.

---

## 1. Pregled izmena po fazama

### Phase 0 — Instrumentacija (`ServerTickProfiler`)
- **Fajlovi:** `Code/BF2/Game/GameServer/ServerTickProfiler.h`, `Code/BF2/Game/GameServer/ServerTickProfiler.cpp`, `Code/BF2/Game/Main/BF2Engine.cpp`, `Code/BF2/Game/GameServer/GameServer.cpp`
- **Šta radi:** Kada je `sv-perf-profile 1`, meri mikrosekundno trajanje svake faze frejma (`recv`, `sim_players`, `sim_objects`, `sim_physics_coll`, `state_masks`, `ghost_scope`, `ghost_pack`, `net_send`, `python`, `total_tick`) i ispisuje `p50`/`p95`/`p99`/`max` izveštaj svakih `sv-perf-profile-interval` tikova (default `300` = `10 s` na 30 Hz).

### Phase 1 — Single-thread 32v32 uska grla (sva podrazumevano `0`)
1. **Zero-alloc `updateStateMasks` (`net-fast-statemasks`):** `Code/BF2/IO/Network/NetworkManager.{h,cpp}` — ravni `FastScopableRootBuffer` bez per-frame `std::map` + `std::list` alokacija.
2. **Catch-up cap (`sv-maxCatchUpTicks`):** `Code/BF2/IO/TickCalculator.cpp`, `Code/BF2/Game/GameServer/GameServer.cpp` — ograničava catch-up korake po frejmu (npr. `2` umesto `10`) da se spreči spirala smrti pri opterećenju.
3. **Adaptivni scope budget & 2D spatial grid (`net-scopeBudgetPerFrame`, `net-scopeDistance`, `net-scopeHysteresis`):** `Code/BF2/Game/GameServer/GameServer.cpp`, `Code/BF2/Game/Common/ServerConnection.cpp` — zamenjuje fiksni limit od 20 konekcija/frejmu i fiksni `500 m` minimum kada je uključeno.
4. **Ghost paketni budžet & resend prioritet (`net-maxPacketBytes`, `net-maxGhostsPerPacket`, `net-resendPriorityBoost`):** `Code/BF2/Game/Common/ClientConnection.cpp`, `Code/BF2/Game/Common/GhostManager.cpp`, `Code/BF2/Game/GameServer/GameServer.cpp`.

### Phase 2A / 2B — Paralelni Snapshot + Ghost Pack + Linux `sendmmsg`
- **Fajlovi:**
  - `Code/BF2/Game/GameServer/NetPipeline/WorldNetSnapshot.{h,cpp}` — read-only snapshot sveta za worker niti (sa 2D prostornom mrežom).
  - `Code/BF2/Game/GameServer/NetPipeline/ConstNetEncoders.{h,cpp}` — thread-safe bit-exact enkoderi za svih 9 aktivnih infantry tipova (`Soldier`, `Item`, `Kit`, `GenericProjectile`, `ControlPoint`, `SimpleObject`, `Player`, `ScoreManager`, `SpawnGroup`), uz čist serijski fallback za rijetke vehicle tipove.
  - `Code/BF2/Game/GameServer/NetPipeline/NetWorkerPool.{h,cpp}` — perzistentni `std::thread` worker pool (`net-mt-enable`, `net-mt-threads`, `net-mt-verify`).
  - `Code/BF2/Game/GameServer/NetPipeline/LinuxBatchSender.{h,cpp}` — grupno slanje pod jednim `NetServer::m_mutex` lock-om koristeći Linux `sendmmsg()` (`net-batch-send`).
  - `Code/Tests/net_pipeline_golden_test.cpp` — standalone bit-exact test suite za enkodere, 2D prostornu mrežu i profiler.

---

## 2. Build & Golden Test komande na Linux-u

> **Napomena:** `CMakeLists.txt` koristi `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` nad `Code/BF2/*`, tako da su svi novi `.cpp` fajlovi u `Code/BF2/Game/GameServer/` automatski uključeni u `bf2` target.

### A) CMake build (`bf2` dedicated server + `bf2_net_pipeline_golden_test`)
```bash
cmake --preset linux-x64 -DBUILD_TESTING=ON
cmake --build --preset linux-x64 --target bf2 bf2_net_pipeline_golden_test -j$(nproc)
ctest --test-dir out/build/linux-x64 -R bf2_net_pipeline_golden_test --output-on-failure
```

### B) Standalone `g++` one-liner za golden test
```bash
g++ -std=c++14 -fno-strict-aliasing -pthread -I Code/BF2 \
    Code/Tests/net_pipeline_golden_test.cpp -o /tmp/net_pipeline_golden_test \
    && /tmp/net_pipeline_golden_test
```

### C) Statička verifikacija (Python)
```bash
python3 scripts/verify_net_mt_32v32.py
```

---

## 3. Tabela novih CVar-ova

| CVar | Tip | Default | Siguran opseg | Opis |
| :--- | :--- | :--- | :--- | :--- |
| `sv-perf-profile` | `int` | `0` | `0` / `1` | Uključuje `ServerTickProfiler` po fazama frejma |
| `sv-perf-profile-interval` | `int` | `300` | `30..3600` | Interval ispisa profiler tabele u tikovima (`300` = 10 s) |
| `sv-maxCatchUpTicks` | `int` | `0` | `0` (vanilla 10), `1..4` | Maksimalan broj sim catch-up tikova po frejmu |
| `net-fast-statemasks` | `int` | `0` | `0` / `1` | Zero-alloc `FastScopableRootBuffer` u `NetworkManager` |
| `net-scopeBudgetPerFrame` | `int` | `0` | `0` (vanilla 20), `32..128` | Broj konekcija za ghost scope po frejmu |
| `net-scopeDistance` | `float` | `0.0f` | `0.0f` (vanilla ≥500 m), `200..600` | Maksimalni radijus ghost scoping-a (m) |
| `net-scopeHysteresis` | `float` | `0.0f` | `0.0f`, `20..80` | Histereza izlaska iz scope-a (m) protiv treperenja |
| `net-maxPacketBytes` | `int` | `0` | `0` (vanilla), `320..1400` | Maksimalna veličina snapshot paketa u bajtovima (unutar `SWConnection` `1458 B` limita) |
| `net-maxGhostsPerPacket` | `int` | `0` | `0` (vanilla `40`), `40..255` | Maksimalan broj ghost-ova po paketu (preporučeno `80` za 32v32) |
| `net-resendPriorityBoost` | `float` | `0.0f` | `0.0f`, `1.0..10.0` | Prioritetni boost za resend ghost-ove |
| `net-mt-enable` | `int` | `0` | `0` / `1` | Uključuje paralelni scope + ghost pack worker pool |
| `net-mt-threads` | `int` | `0` | `0` (auto), `2..8` | Broj worker niti u `NetWorkerPool` |
| `net-mt-verify` | `int` | `0` | `0` / `1` | Poredi MT vs ST bit-za-bit (samo za kratak sanity test!) |
| `net-batch-send` | `int` | `0` | `0` / `1` | Grupno slanje preko Linux `sendmmsg()` |

---

## 4. Preporučeni A/B test preseti (32v32 progresija)

### Preset A — Baseline (`hitreg-v1` paritet + profiler)
```text
vars.set sv-perf-profile 1
vars.set sv-perf-profile-interval 300
```
*(Svi ostali novi CVar-ovi ostaju na `0`.)*

### Preset B — Phase 1 (Single-thread 32v32 optimizacije)
```text
vars.set sv-perf-profile 1
vars.set net-fast-statemasks 1
vars.set net-maxPacketBytes 1400
vars.set net-maxGhostsPerPacket 80
vars.set net-scopeBudgetPerFrame 64
vars.set net-scopeDistance 300
vars.set net-scopeHysteresis 40
vars.set net-resendPriorityBoost 5.0
vars.set sv-maxCatchUpTicks 2
```

### Preset C — Phase 2 (MT Worker Pool + Linux `sendmmsg`)
1. Prvo uraditi kratak **2-minutni sanity test** sa `vars.set net-mt-verify 1` (proveriti da nema mismatch logova na konzoli, pa vratiti `net-mt-verify 0` jer duplira posao).
2. Produkcijski Preset C:
```text
# Sve iz Preset B +
vars.set net-mt-enable 1
vars.set net-mt-threads 4
vars.set net-batch-send 1
```

---

## 5. Tumačenje `ServerTickProfiler` izlaza i Phase 3 pravilo

Svakih `sv-perf-profile-interval` tikova server ispisuje tabelu po fazama (`p50 / p95 / p99 / max` u mikrosekundama):
- **Budžet za 30 Hz je `33,333 us` (`33.3 ms`) po tiku.** Ako `total_tick` `p99` pređe `33,333 us`, dolazi do kašnjenja tikova i jitter-a.
- **Mrežna uska grla (`state_masks`, `ghost_scope`, `ghost_pack`, `net_send`):** direktno se adresiraju Preset-om B i C.
- **Pravilo za pokretanje Phase 3 (paralelizacija simulacije):**
  Tek ako na realnom **64-player (32v32)** meču sa aktivnim Preset-om C zbir:
  $$\text{sim\_players} + \text{sim\_objects} + \text{sim\_physics\_coll} > 15\text{ ms}\;(15,000\text{ us})$$
  na `p95`/`p99`, onda ima smisla otvarati Phase 3 (paralelizaciju fizike/simulacije). Dok je taj zbir ispod `15 ms`, Phase 1 + Phase 2 su dovoljni za stabilan 30 Hz server.
