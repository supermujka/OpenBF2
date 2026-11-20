# OpenBF2 — Plan, telemetrija i uputstvo za testiranje (`feat/hitreg`)

Ovaj dokument opisuje implementaciju **Verzije 1** unapređenja hit detekcije i lag kompenzacije za **OpenBF2** (fokus na 4v4 / 5v5 mečeve na 16-player mapama, mešane pingove `10–100 ms` i povremeni packet loss / jitter), **Shadow A/B + `pingFactor` telemetriju (`openbf2_hitreg.csv`)**, i tačno uputstvo za build i testiranje u dvoje kod kuće.

> **Napomena o statusu:** Sve izmene su `[CHOSEN, not recovered]` (namerno odstupanje od originalnog BF2 1.50 ponašanja) i zaštićene su konzolnim prekidačima (`phy-latencyComp-*`). Nijedna izmena ne menja veličinu klasa, memorijski raspored (`x86` layout) niti `vtable` slotove.

---

## 1. Šta je urađeno u Verziji 1 (+ revizija nakon code audita)

| # | Problem u originalnom BF2 | Rešenje u Verziji 1 | Fajlovi |
| --- | --- | --- | --- |
| 1 | **Stajaći input red (`33–100 ms` skrivenog laga):** Posle jitter-a ili packet loss-a, `ActionBuffer` trajno zadrži 1–3 akcije u redu, a `getLatency()` to kašnjenje ne računa. | Meri se tick dolaska svake mrežne akcije (`notePlayerActionArrival`) i stvarno dodatno čekanje u redu (`setPlayerActiveQueueDelay`), uz `-1` tick korekciju jer `ServerGameLogic::uPlaying` uveća `WorldPref::mTickCount` pre `simulatePlayersUpdate`. | [LatencyCompensation.cpp](../Code/BF2/Physics/LatencyCompensation.cpp), [PlayerActionManager.cpp](../Code/BF2/Game/Common/PlayerActionManager.cpp), [GameServer.cpp](../Code/BF2/Game/GameServer/GameServer.cpp) |
| 2 | **Spawn / izlazak iz vozila („neuhvatljiv“ prvih ~150 ms):** `reset(true)` i `initLatencyCompensation()` pune ring bafer nulama `(0,0,0)` pa rewind vuče igrača ka centru mape. | Prvi validni tick (ili teleport > 3 m za vojnike / > 50 m za vozila) popunjava svih 16 slotova trenutnom pozicijom i rotacijom, i beleži tick reseed-a za telemetriju. | [SoldierPhysicsNode.cpp](../Code/BF2/Physics/SoldierPhysicsNode.cpp), [PhysicsNode.cpp](../Code/BF2/Physics/PhysicsNode.cpp) |
| 3 | **Prva kapsula umesto najbliže (`Skeleton::checkRayVsSkeleton`):** Petlja radi `break;` na prvoj kapsuli po indeksu u `m_vector24` (npr. ruka ume da „ukrade“ headshot). | Prolazi sve kolizione kapsule i bira presek sa strogo najmanjim vremenom duž putanje zrake `t`. | [Skeleton.cpp](../Code/BF2/Animation/BoneAnimation/Skeleton.cpp) |
| 4 | **Zamrzavanje kašnjenja u trenutku pucanja:** Rewind se u retail BF2 ponovo računa tokom svakog tick-a leta metka. | Zamrzava ukupno kašnjenje (`ping * pingFactor + interp + queueDelay`, limitirano sa `phy-latencyComp-max`) na projektilu; čisti se u `resetProjectile()`, destruktoru i pri `setPlayer(0)` (zaštita od `ObjectCache` recikliranja). | [GenericProjectile.cpp](../Code/BF2/Game/Objects/Projectiles/GenericProjectile.cpp), [SoldierResponse.cpp](../Code/BF2/Physics/SoldierResponse.cpp), [PointResponsePhysics.cpp](../Code/BF2/Physics/PointResponsePhysics.cpp), [ResponsePhysicsManager.cpp](../Code/BF2/Physics/ResponsePhysicsManager.cpp), [GameLogicExplosion.cpp](../Code/BF2/Game/GameLogic/GameLogicExplosion.cpp) |
| 5 | **Server-side limit za `interpolationTime`:** Klijent šalje 9 bita (`0–511 ms`); moguć backtrack exploit. | Ograničenje `phy-latencyComp-maxClientInterpMs` (default `133 ms`) primenjuje se u `LatencyCompensation::getLatency()`, dok `Player::setInterpolationTime()` čuva sirovu `0..512 ms` vrednost za telemetriju i trenutno reagovanje na cvar promene. | [Player.cpp](../Code/BF2/Game/GameLogic/Player.cpp), [LatencyCompensation.cpp](../Code/BF2/Physics/LatencyCompensation.cpp) |
| 6 | **Broadphase nestajanje vojnika pri stajanju:** Kad vojnik naglo stane (`speed ≈ 0`), `latencyDistance` padne na `0`. | Donji prag brzine `phy-latencyComp-broadphaseFloor` (default `8.0 m/s`) za vojnike u `latencyBroadphaseRadius`. | [ObjectManager.cpp](../Code/BF2/Scene/Object/ObjectManager.cpp) |
| 7 | **Hitreg Telemetrija, Shadow A/B i `pingFactor` sweep (`openbf2_hitreg.csv`):** Nemoguće je ponoviti 2 identične situacije uživo. | Na **svakom** pogotku i promašaju unutar `2.0 m` od vojnika beleži Shadow Vanilla rezultat, `pf00/pf05/pf075/pf10/raw05` distance do najbliže kosti, jitter, packet loss, input queue, pozu/brzinu mete, wall-override i konačni damage. | [HitregTelemetry.h](../Code/BF2/Physics/HitregTelemetry.h), [HitregTelemetry.cpp](../Code/BF2/Physics/HitregTelemetry.cpp) |

---

## 2. Konzolne promenljive (CVar-ovi)

### Lag kompenzacija & Hitreg (`LatencyCompensation.cpp`)
| CVar | Default | Opis |
| --- | --- | --- |
| `phy-latencyComp-improved` | `1` | Glavni prekidač za sve Verzija 1 izmene (`0` = 100% originalni BF2 1.50). |
| `phy-latencyComp-maxClientInterpMs` | `133` | Maksimalni dozvoljeni klijentov `interpolationTime` na serveru (ms). |
| `phy-latencyComp-queueDelay` | `1` | Kompenzacija čekanja u `ActionBuffer` redu (`0/1`). |
| `phy-latencyComp-maxQueueTicks` | `4` | Maksimalno dodatno kašnjenje iz reda koje se kompenzuje (u 30 Hz tick-ovima). |
| `phy-latencyComp-snapshotProjectile` | `1` | Zamrzava izračunati rewind na projektilu u trenutku ispaljivanja (`0/1`). |
| `phy-latencyComp-closestBone` | `1` | Bira najbližu kapsulu duž zrake umesto prve u nizu (`0/1`). |
| `phy-latencyComp-spawnRingFix` | `1` | Sprečava rewind ka `(0,0,0)` neposredno posle spawn-a / izlaska iz vozila (`0/1`). |
| `phy-latencyComp-broadphaseFloor` | `8.0` | Minimalna brzina (m/s) za širenje broadphase sfere vojnika. |
| `phy-latencyComp-debugLog` | `0` | Kada je `1`, ispisuje kratki `[HITREG]` red na `stderr`. |
| `phy-latencyComp-pingFactor` *(postojeći)* | `0.5` | Faktor množenja pinga (telemetrija automatski testira `0.0 / 0.5 / 0.75 / 1.0`). |
| `phy-latencyComp-max` *(postojeći)* | `0.3` | Gornji limit rewind-a u sekundama (`0.18`–`0.20` preporučeno za 5v5). |

### Telemetrija (`HitregTelemetry.cpp`)
| CVar | Default | Opis |
| --- | --- | --- |
| `phy-latencyComp-telemetry` | `0` | `0` = ugašeno (nula troška); `1` = `CONFIG + PROBE + DELIVER + DAMAGE`; `2` = sve + svaki `FIRE` (preporučeno za test u dvoje). |
| `phy-latencyComp-telemetryFile` | `"openbf2_hitreg.csv"` | Ime/putanja izlaznog CSV fajla (u radnom direktorijumu servera). |
| `phy-latencyComp-telemetryFlushTicks` | `30` | Na koliko tick-ova (`30` ≈ 1 s) se bafer prazni na disk. |

---

## 3. Build i igranje sa kolegom (šta kome treba?)

### Za Verziju 1 test: **NE šalješ nikakav `BF2.exe` kolegi!**
Sve Verzija 1 izmene i telemetrija žive **isključivo na dedicated serveru**.
- I ti i kolega se spajate sa **potpuno običnim, neizmenjenim retail Battlefield 2 v1.50 klijentom**.
- Na tvom računaru builduješ samo **`bf2_w64ded.exe`** (strict x64 dedicated server), koji stvara samo 2 fajla:
  - `out/build/local-x64/bin_w64ded/bf2_w64ded.exe`
  - `out/build/local-x64/bin_w64ded/dice_py.dll`

*(Napomena za budućnost: `bf2_client` / `BF2.exe` u ovom repou još nije spreman za igru — linkuje se sa `/FORCE` i ima nedovršene frontend/UI/sound granice. Ako jednog dana budemo pravili 60 Hz klijent, tada ćeš kolegi morati poslati ceo paket iz `bin_client/`: `BF2.exe`, `dice_py.dll`, `RendDX9.dll`, `SwiffPlayer.dll`, `BF2Audio.dll`, `TextureAtlasBuilder.dll` i `BF2VoipServer.dll`, jer se rebuilt i retail DLL-ovi ne smeju mešati.)*

### Korak 1: Kloniranje i build na kućnom Windows računaru
Preduslov: **Visual Studio 2022 Community** (besplatan) sa paketom **Desktop development with C++**, plus tvoja instalacija **Battlefield 2 v1.50**.

1. Kloniraj svoj fork i pređi na granu:
   ```powershell
   git clone https://github.com/supermujka/OpenBF2.git
   cd OpenBF2
   git checkout feat/hitreg
   ```
2. **Brza provera kompajliranja i testova** (iz **x86** Visual Studio Developer terminala):
   ```powershell
   cmake --preset msvc-x86 -B out/build/local-x86
   cmake --build out/build/local-x86 --target bf2_recovered bf2_run_checks
   ```
   *(Ako želiš dodatno: `cmd /c Code\Tests\runtime_pair.cmd` i `cmd /c Code\Tests\syntaxcheck.cmd`.)*
3. **Build dedicated servera** (iz **x64** Visual Studio Developer terminala):
   ```powershell
   cmd /c Code\Build\python\build_python.cmd x64
   cmake --preset msvc-x64 -B out/build/local-x64 -DBF2_GAME_DIR="D:/Games/Battlefield 2"
   cmake --build out/build/local-x64 --target bf2_w64ded
   ```
   *(Zameni `D:/Games/Battlefield 2` putanjom do tvoje BF2 1.50 instalacije.)*

### Korak 2: Pokretanje servera i spajanje sa kolegom
**Nikada ne kopiraj `bf2_w64ded.exe` u originalni BF2 folder!** Ostavi ga u `bin_w64ded/`, a pokreni ga iz svog retail BF2 foldera (kao working directory) na portu `16568` (da izbegneš koliziju sa klijentskim portom `16567`):

1. Napravi mali test config (npr. `D:\bf2_test_server.con`):
   ```text
   sv.serverName "OpenBF2 Hitreg Test"
   sv.internet 0
   sv.punkBuster 0
   sv.serverPort 16568
   sv.maxPlayers 16
   sv.friendlyFireWithSoldier 0
   vars.set phy-latencyComp-telemetry 2
   ```
   *(Sa `sv.internet 0` zaobilazi se ugašeni GameSpy — i ti i kolega možete ući sa običnim **Offline/Local** profilom u BF2!)*
2. Pokreni server (iz BF2 1.50 foldera kao CWD):
   ```powershell
   cd "D:\Games\Battlefield 2"
   & "C:\putanja\do\OpenBF2\out\build\local-x64\bin_w64ded\bf2_w64ded.exe" +modPath mods/bf2 +config "D:\bf2_test_server.con" +loadLevel strike_at_karkand +gameMode gpm_cq +maxPlayers 16 +port 16568
   ```
   *(Za spajanje kolege preko interneta: prosledi **UDP port 16568** na ruteru i dozvoli u Windows Firewall-u, ili upali Tailscale / ZeroTier / Radmin VPN pa neka se kolega spoji na tvoj VPN IP.)*
3. Ti i kolega pokrenite običan `BF2.exe` → **Multiplayer → Connect to IP** (`127.0.0.1:16568` za tebe, `<TVOJ_IP>:16568` za kolegu), ili prečicom:
   ```powershell
   BF2.exe +modPath mods/bf2 +joinServer <IP_SERVERA> +port 16568 +fullscreen 0 +restart 1
   ```

### Korak 3: Kako snimiti savršen test log za analizu (5–10 min)
1. Proverite u server konzoli da je uključeno: `vars.set phy-latencyComp-telemetry 2`.
2. Odigrajte nekoliko konkretnih scenarija (posebno sa mešanim pingom ili `clumsy` 50–100 ms + 2% packet loss):
   - Trčanje levo-desno (*strafe*) i gađanje u telo i glavu (snajper + puška).
   - Naglo zaustavljanje / čučanje / ležanje (*dolphin dive*) i skakanje tokom paljbe.
   - Pucanje u metu neposredno nakon spawn-a ili izlaska iz vozila.
3. Kad završite, u server konzoli ukucaj:
   ```text
   vars.set phy-latencyComp-telemetry 0
   ```
   To odmah zatvara i snima ceo **`openbf2_hitreg.csv`** fajl u `D:\Games\Battlefield 2\openbf2_hitreg.csv` (bez potrebe da gasiš server).
4. Pošalji mi **`openbf2_hitreg.csv`** — na osnovu njega ćemo odmah izračunati:
   - koliko je tačno pogodaka i headshot-ova V1 spasao u odnosu na Vanilla BF2 (`v1_hit` vs `van_hit`),
   - koja vrednost `phy-latencyComp-pingFactor` (`0.5` vs `0.75` vs `1.0`) daje najmanju grešku u metrima (`*_miss_m`),
   - koliki % promašaja dolazi od skakanja/animacije (`tgt_in_air`, `tgt_pose_diff`) za **Verziju 2** (čuvanje kompletnih kostiju skeleta u istoriji).
