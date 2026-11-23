# OpenBF2 64-Player LoadBot & Route Recorder — Branch `feat/loadbot-64p` Handoff

> **Status:** Grana `feat/loadbot-64p` je presečena sa `feat/net-mt-32v32` (`f579a3c`).
> **Garancije:**
> - `hitreg-v1` (`9cd3527`) i `feat/net-mt-32v32` (`f579a3c`) su **potpuno netaknuti** — sva netcode i hitreg logika sa bazne grane je očuvana.
> - **100% retail BF2 v1.50 wire protokol** — `bf2_loadbot` komunicira sa neizmenjenim OpenBF2 serverom preko standardnog BF2 v1.50 UDP protokola (`protocolVersion = 4098` / `0x1002`, `buildVersion = 0x150C5100`).
> - **Nula ABI / layout / vtable promena** na postojećim engine klasama u `Code/BF2/` (svi novi loadbot kodovi žive izolovano u `Code/Tools/LoadBot/` bez ikakvog `#include` ka `Code/BF2/`).
> - **Svi novi CVar-ovi podrazumevano su isključeni (`sv-record-routes = 0`)**, tako da server sa default podešavanjima ima praktično nulti overhead i radi bit-identično baznoj grani.

---

## 1. Arhitektura komponenti

### Faza 1 — Server-side `RouteActionRecorder`
- **Fajlovi:** `Code/BF2/Game/GameServer/RouteActionRecorder.h`, `Code/BF2/Game/GameServer/RouteActionRecorder.cpp`
- **Šta radi:** Snima realne putanje i akcije živih igrača (pozicija, `yawDeg`, `pitchDeg`, `stance`, `actionFlags`, `moveForward`, `moveStrafe`) u kompaktni binarni fajl za kasniji replay od strane botova.
- **Binarni format (`OBR1` v1):**
  - `RouteActionHeader` (`32 B`): `magic[4]` = `"OBR1"`, `version = 1`, `sampleHz` (klampovan `1..30`), `recordSize = 36`, `mapName[16]`.
  - `RouteActionSample` (`36 B`): `tick`, `playerId`, `team`, `stance` (`0=Stand`, `1=Crouch`, `2=Prone`, `3=Swim`), `posX/Y/Z`, `yawDeg`, `pitchDeg`, `actionFlags` (bitmaska: fire, alt-fire, sprint, crouch, prone, jump, reload, kretanje, weapon switch), `moveForward`, `moveStrafe`.
  - Baferovani binarni I/O (`kFlushThreshold = 1024` uzoraka) minimizuje sistemske pozive tokom snimanja.

| CVar | Tip | Default | Siguran opseg | Opis |
| :--- | :--- | :--- | :--- | :--- |
| `sv-record-routes` | `int` | `0` | `0` / `1` | Uključuje binarno snimanje ruta i akcija igrača (`0` = isključeno, near-zero fast-path) |
| `sv-record-routes-file` | `string` | `"openbf2_routes.bin"` | Putanja fajla | Izlazna putanja za `OBR1` binarni fajl |
| `sv-record-routes-hz` | `int` | `5` | `1..30` | Frekvencija uzorkovanja po igraču u Hz |

---

### Faza 2 — Standalone headless 64-player klijent `bf2_loadbot`
Svi fajlovi se nalaze u `Code/Tools/LoadBot/` i kompajliraju se kao nezavisan C++14 Linux izvršni fajl `bf2_loadbot` (bez zavisnosti od `Code/BF2/` engine biblioteka):

- **Wire & transport sloj (`WireBitStream.{h,cpp}`, `SWTransport.{h,cpp}`, `Bf2Protocol.{h,cpp}`):**
  - Bit-level stream (`1472 B` max datagram, `1458 B` max payload, `9 B` data header).
  - Kompletan BF2 v1.50 UDP handshake i konekciona mašina: `UDP_CONNECT_REQUEST` → `UDP_CONNECT_ACCEPT` → `UDP_CONNECT_ACCEPT_ACK`, 6-bit sliding window seq/ack, `UDP_PING_REQUEST`/`RESPONSE` RTT merenje, `GE_CHALLENGE`/`GE_CHALLENGE_RESPONSE`, `GE_CONNECTION_TYPE`, `GE_DATA_BLOCK` / `GE_STRING_BLOCK`, `GE_BEGIN_ROUND`, dekodiranje `GhostManager` snapshotova i slanje `PlayerAction` inputa na `30 Hz`.
  - **LAN (`sv.internet 0`) handshake:** Omogućava direktnu konekciju do 64 bota lokalno ili preko LAN-a bez GameSpy autentikacije.
- **3-nivoa navigacija (`NavMesh.{h,cpp}`, `RouteLoader.{h,cpp}`, `BotBrain.{h,cpp}`):**
  1. **Nivo 1 — `Infantry.qtr` NavMesh + polygon-graph A\*:** Čita BF2 `AIPathFinding v3` `.qtr` (i opcioni `AerialHeighMap.ahm`), spušta se kroz QuadTree do poligona i traži putanju A\* pretragom preko susednih ivica (edge-midpoint waypointi).
  2. **Nivo 2 — `openbf2_routes.bin` (`OBR1`) replay:** Bira snimljenu trajektoriju pravog igrača za odgovarajući tim u blizini spawn pozicije i reprodukuje kretanje, stav i akcije.
  3. **Nivo 3 — `ControlPoint` / `SpawnGroup` fallback sa stuck recovery-jem:** Ako navmesh/ruta nisu dostupni ili zakažu, bot se kreće ka aktivnim zastavicama (`ControlPoint`) / spawn grupama (`SpawnGroup`), a ugrađeni stuck detektor (`m_stuckAnchorPos` / `m_stuckRecoverUntilUs`) vrši bočni strafe/skok oporavak kada se bot zaglavi.
- **Realistična 32v32 borba (`BotBrain.{h,cpp}`):**
  - Detektuje najbližeg živog neprijateljskog vojnika unutar borbenog radijusa (`50.0 m`), nišani (`facingDegrees`, `lookUpDownAngle`), puca uz periodičan reload, bočno strafe-uje i menja stav (`crouch`) radi realnog opterećenja `PlayerAction`, projektila i hitreg/ghost saobraćaja.
- **Mrežna emulacija (`NetImpairment.{h,cpp}`):**
  - Deterministički per-direction (`uplink` i `downlink`) priority-queue simulator kašnjenja (`--ping-ms`), uniformnog jitter-a (`--jitter-ms`) i gubitka paketa (`--loss-pct`), sa fiksnim RNG seed-om (`--seed`).
- **Klijentska telemetrija & sliding detektor (`Telemetry.{h,cpp}`, `LoadBotClient.{h,cpp}`):**
  - Meri `packetsRx`/`packetsTx`, `bytesRx`/`bytesTx` (Mbps), broj ghost-ova, `SnapHz` (`p50/p95/p99`) i među-paketni `gap` (`p50/p95/p99/max` u ms).
  - **Sliding/ekstrapolacioni detektor (`Slide`):** Prati ažuriranja bliskih neprijatelja (`< 50 m`, histereza čišćenja `> 60 m`) i broji intervale bez novog snapshot ažuriranja duže od **`> 100 ms`** (prag glatke interpolacije) i **`> 200 ms`** (vanilla prag gde klijent kreće u vidljivo klizanje/ekstrapolaciju), kao i `maxEnemyGapMs`.

---

## 2. Tabela svih CLI opcija za `bf2_loadbot`

| Opcija | Argument | Default | Opis |
| :--- | :--- | :--- | :--- |
| `--host` | `<ip\|hostname>` | `127.0.0.1` | IP adresa ili hostname ciljnog BF2 servera |
| `--port` | `<port>` | `16567` | UDP port servera |
| `--bots` | `<1..64>` | `64` | Broj istovremenih botova |
| `--ping-ms` | `<float>` | `35.0` | Simulirani RTT ping u ms (deli se 50/50 na uplink i downlink) |
| `--jitter-ms` | `<float>` | `8.0` | Simulirani RTT jitter u ms (deli se 50/50 po smeru) |
| `--loss-pct` | `<float>` | `1.0` | Simulirani gubitak paketa u % (primenjuje se nezavisno po smeru) |
| `--duration-sec` | `<int>` | `300` | Trajanje testa u sekundama (`0` = radi do `SIGINT`/`SIGTERM`) |
| `--navmesh` | `<dir_or_qtr>` | *(prazno)* | Putanja do `Infantry.qtr` fajla ili direktorijuma sa navmesh-om (Nivo 1 nav) |
| `--routes` | `<routes.bin>` | *(prazno)* | Putanja do snimljenog `OBR1` fajla ruta (Nivo 2 nav) |
| `--mod` | `<modName>` | `mods/bf2` | Putanja moda koja se šalje u `ConnectRequest` |
| `--password` | `<pw>` | *(prazno)* | Opciona lozinka servera |
| `--stagger-ms` | `<int>` | `150` | Pauza između uzastopnih konekcija botova u ms |
| `--world-size` | `<float>` | `1024.0` | Veličina mape u metrima (automatski se preuzima iz `--navmesh` ako je učitan) |
| `--seed` | `<uint32>` | `1337` | Deterministički RNG seed za botove i mrežnu emulaciju |
| `--report-sec` | `<int>` | `10` | Interval ispisa periodične telemetrije u sekundama |
| `--verbose` | *(flag)* | `false` | Detaljno logovanje handshake-a i diskonekcija po botu |
| `-h`, `--help` | *(flag)* | — | Prikazuje pomoć i izlazi |

---

## 3. Linux Build & Test komande

### A) CMake `linux-x64` preset build (`bf2`, `bf2_loadbot`, `bf2_loadbot_tests`) + `ctest`
```bash
cmake --preset linux-x64 -DBUILD_TESTING=ON
cmake --build --preset linux-x64 --target bf2 bf2_loadbot bf2_loadbot_tests -j$(nproc)
ctest --test-dir out/build/linux-x64 -R bf2_loadbot_tests --output-on-failure
```

### B) Standalone `g++` one-liner za `bf2_loadbot_tests`
```bash
g++ -std=c++14 -fno-strict-aliasing -pthread -I Code/BF2 -I Code/Tools/LoadBot \
    Code/Tests/bf2_loadbot_tests.cpp \
    Code/Tools/LoadBot/WireBitStream.cpp \
    Code/Tools/LoadBot/SWTransport.cpp \
    Code/Tools/LoadBot/Bf2Protocol.cpp \
    Code/Tools/LoadBot/NavMesh.cpp \
    Code/Tools/LoadBot/RouteLoader.cpp \
    Code/Tools/LoadBot/NetImpairment.cpp \
    Code/Tools/LoadBot/Telemetry.cpp \
    Code/Tools/LoadBot/BotBrain.cpp \
    -o /tmp/bf2_loadbot_tests && /tmp/bf2_loadbot_tests
```

### C) Python statička verifikacija (`verify_loadbot_64p.py`)
Proverava granu, nultu ABI/layout/vtable promenu u `Code/BF2/`, default `sv-record-routes = 0`, strogi C++14 standard, izolaciju `LoadBot` include-ova, binarnu parnost `RouteFormat.h` vs `RouteActionRecorder.h` (`32 B` / `36 B`) i CMake targete:
```bash
python3 scripts/verify_loadbot_64p.py
```

---

## 4. Kako snimiti rute pravih igrača (`RouteActionRecorder`) i izvući `Infantry.qtr`

### Snimanje ruta na serveru (5–10 minuta na mapi npr. Strike at Karkand ili Road to Jalalabad)
Na aktivnom serveru sa pravim igračima uključite snimanje preko konzole/rcon-a, pustite partiju da traje 5–10 minuta kako bi se sakupilo dovoljno različitih ruta oba tima, a zatim ugasite snimanje da bi se bafer bezbedno flush-ovao i fajl zatvorio:
```text
vars.set sv-record-routes-file openbf2_routes.bin
vars.set sv-record-routes-hz 5
vars.set sv-record-routes 1
# ... sačekati 5-10 minuta aktivne igre na Karkand / Jalalabad ...
vars.set sv-record-routes 0
```
*(Napomena: Zaustavljanje sa `sv-record-routes 0` ili uredno gašenje servera zatvara fajl i flush-uje preostale uzorke iz memorije.)*

### Izvlačenje `Infantry.qtr` (i `AerialHeighMap.ahm`) iz `Levels/<mapa>/server.zip`
`bf2_loadbot --navmesh` prihvata direktorijum sa `Infantry.qtr` (i opcionim `AerialHeighMap.ahm`) ili direktnu putanju do `.qtr` fajla. Iz server arhive mape možete ih raspakovati npr. ovako:
```bash
mkdir -p /tmp/nav_karkand
unzip -j mods/bf2/Levels/Strike_at_Karkand/server.zip \
    "AIPathFinding/Infantry.qtr" \
    "AIPathFinding/AerialHeighMap.ahm" \
    -d /tmp/nav_karkand
```
*(Za drugu mapu zamenite `Strike_at_Karkand` npr. sa `Road_to_Jalalabad`.)*

---

## 5. Kompletan 32v32 A/B Benchmark vodič (`hitreg-v1` vs `Preset B` vs `Preset C`)

### Korak 1 — Postavke servera za lokalni/LAN 64-player test
U `serversettings.con` ili preko serverske konzole podesite LAN režim bez PunkBuster-a i auto-balansa (kako bi botovi slobodno popunili 32v32), uz uključen `ServerTickProfiler`:
```text
sv.internet 0
sv.maxPlayers 64
sv.punkBuster 0
sv.autoBalanceTeam 0
vars.set sv-perf-profile 1
vars.set sv-perf-profile-interval 300
```

Zatim za svaki od tri A/B prolaza primenite odgovarajući preset iz `docs/feat-net-mt-32v32-handoff.md`:
- **Preset A (`hitreg-v1` baseline):** svi `net-*` i `sv-maxCatchUpTicks` CVar-ovi ostaju na `0` (samo `sv-perf-profile 1`).
- **Preset B (Phase 1 single-thread optimizacije):**
  ```text
  vars.set net-fast-statemasks 1
  vars.set net-maxPacketBytes 1400
  vars.set net-maxGhostsPerPacket 80
  vars.set net-scopeBudgetPerFrame 64
  vars.set net-scopeDistance 300
  vars.set net-scopeHysteresis 40
  vars.set net-resendPriorityBoost 5.0
  vars.set sv-maxCatchUpTicks 2
  ```
- **Preset C (Phase 2 MT Worker Pool + Linux `sendmmsg`):** sve iz Preset B +
  ```text
  vars.set net-mt-enable 1
  vars.set net-mt-threads 4
  vars.set net-batch-send 1
  ```

### Korak 2 — Reproducibilna `bf2_loadbot` komanda (32v32, 5 minuta, deterministički seed)
Pokrenite identičan profil opterećenja na sva tri preseta (fiksni `--seed 1337` garantuje identične gubitke/jitter i ponašanje botova):
```bash
./bf2_loadbot \
    --host 127.0.0.1 \
    --port 16567 \
    --bots 64 \
    --ping-ms 35 \
    --jitter-ms 8 \
    --loss-pct 1.0 \
    --duration-sec 300 \
    --navmesh /tmp/nav_karkand \
    --routes openbf2_routes.bin \
    --seed 1337
```

### Korak 3 — Kako porediti rezultate
1. **Serverska strana (`ServerTickProfiler`):**
   - Pratite `total_tick` **`p99 < 33,333 us`** (`33.3 ms` budžet za stabilnih 30 Hz). Ako `total_tick p99` prelazi `33,333 us`, server kasni sa tikovima.
   - Uporedite pojedinačne mrežne faze (`state_masks`, `ghost_scope`, `ghost_pack`, `net_send`) između Preseta A, B i C.
2. **Klijentska strana (`FINAL A/B SUMMARY` iz `bf2_loadbot`):**
   - **`SnapHz` (`p50 / p95 / p99`) i `max_gap_ms` (`gap ... max=... ms`):** viši `p95`/`p99` SnapHz (bliže 30 Hz) i niži `max_gap_ms` znače manje zastoja u isporuci snapshotova.
   - **Sliding/ekstrapolacioni detektor (`Slide`):** poredite procenat i apsolutni broj bliskih ažuriranja neprijatelja (`< 50 m`) koji kasne **`> 100 ms`** i **`> 200 ms`**, kao i `maxEnemyGapMs`. Cilj Preseta B i C je drastično smanjenje `> 100 ms` i eliminacija `> 200 ms` skokova u odnosu na `hitreg-v1` baseline (Preset A), bez `disc` (diskonekcija) i `protoErr` grešaka.

---

## 6. Praktični scenariji pokretanja (drugi Linux server, WSL2, igranje uživo sa `--bots 62`)

### A) Zlatni standard: Drugi Linux server za botove + pravi `BF2.exe` od kuće (PREPORUČENO)
- **Linux Server #1 (BF2 server):** vrti samo `bf2` dedicated server (`0%` CPU opterećenja od botova, pravi NIC/kernel UDP/`sendmmsg`, čisti `ServerTickProfiler` brojevi).
- **Linux Server #2 (`bf2_loadbot`):** ne treba ni puni CMake build ni BF2 fajlove — samo `g++`, `git` i opciono prekopiran `Infantry.qtr` (par stotina KB).
- **Tvoj Windows PC (`BF2.exe`):** ostaviš 2 slobodna slota (`--bots 62`, tj. `31 vs 31`), spojiš se preko **Multiplayer → Connect to IP** (`<IP_SERVERA_1>:16567`) i uživo provjeravaš hitreg i "klizanje".

Brzi build na **Linux Serveru #2** (~10 sekundi):
```bash
sudo apt update && sudo apt install -y g++ git
git clone https://github.com/supermujka/OpenBF2.git && cd OpenBF2
git checkout feat/loadbot-64p

# Unit testovi
g++ -std=c++14 -O2 -fno-strict-aliasing -pthread -I Code/BF2 -I Code/Tools/LoadBot \
    Code/Tests/bf2_loadbot_tests.cpp \
    Code/Tools/LoadBot/WireBitStream.cpp \
    Code/Tools/LoadBot/SWTransport.cpp \
    Code/Tools/LoadBot/Bf2Protocol.cpp \
    Code/Tools/LoadBot/NavMesh.cpp \
    Code/Tools/LoadBot/RouteLoader.cpp \
    Code/Tools/LoadBot/NetImpairment.cpp \
    Code/Tools/LoadBot/Telemetry.cpp \
    Code/Tools/LoadBot/BotBrain.cpp \
    -o ./bf2_loadbot_tests && ./bf2_loadbot_tests

# Standalone bf2_loadbot binarni fajl
g++ -std=c++14 -O2 -fno-strict-aliasing -pthread -Wall -Wextra -I Code/Tools/LoadBot \
    Code/Tools/LoadBot/*.cpp -o ./bf2_loadbot
```

Pokretanje sa **Linux Servera #2** prema **Serveru #1** (sa `--bots 62` i `--duration-sec 0` da radi do `Ctrl+C`):
```bash
# Udaljeni serveri (već postoji realan ping/jitter/loss — isključi vještačku simulaciju nulama):
./bf2_loadbot --host <IP_SERVERA_1> --port 16567 \
    --bots 62 --ping-ms 0 --jitter-ms 0 --loss-pct 0 \
    --duration-sec 0 --navmesh ~/bf2_nav/karkand

# Isti datacentar (ping < 2 ms — uključi determinističku simulaciju igrača):
./bf2_loadbot --host <IP_SERVERA_1> --port 16567 \
    --bots 62 --ping-ms 35 --jitter-ms 8 --loss-pct 1.0 \
    --duration-sec 0 --navmesh ~/bf2_nav/karkand --seed 1337
```
> **Napomena za firewall / DDoS filter na Serveru #1:** Svih 62 botova dolazi sa iste IP adrese (Servera #2) preko 62 različita UDP porta (~1,860 paketa/s). Ako na Serveru #1 postoji `iptables` rate-limit ili VPS anti-DDoS zaštita, whitelist-uj IP adresu Servera #2 za UDP port `16567`.

### B) Pokretanje na istom Linux serveru (`127.0.0.1`) uz izolaciju CPU jezgra
Ako nemaš drugi server, pokreni `bf2_loadbot` na istom serveru zakucan na zadnje CPU jezgro (`taskset`) da ne utiče na `bf2` simulaciju i `ServerTickProfiler`, a ti uđi od kuće sa `BF2.exe`:
```bash
taskset -c 3 ./bf2_loadbot --host 127.0.0.1 --port 16567 \
    --bots 62 --ping-ms 35 --jitter-ms 8 --loss-pct 1.0 \
    --duration-sec 0 --navmesh ~/bf2_nav/karkand --seed 1337
```

### C) Pokretanje sa Windows PC-a kroz WSL2 (Ubuntu)
`bf2_loadbot` koristi Linux/POSIX UDP sokete, ali se na Windowsu može pokrenuti direktno kroz WSL2:
1. U PowerShell-u (Admin): `wsl --install -d Ubuntu` (jednokratno + restart).
2. U Ubuntu terminalu: `sudo apt update && sudo apt install -y build-essential cmake ninja-build git python3 unzip`.
3. Kloniraj u Linux home (`~/OpenBF2`, ne pod `/mnt/c/` radi brzine kompajliranja) i izgradi po gornjim `g++` ili `cmake` komandama.
4. Za spajanje na udaljeni Linux server koristi `--ping-ms 0 --jitter-ms 0 --loss-pct 0` (vlastiti internet već dodaje realan ping). Pazi da 62 bota troše ~1–2 Mbps upload-a i ~10–25 Mbps download-a kroz kućni ruter.
