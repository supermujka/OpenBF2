# OpenBF2 — Plan i dokumentacija za unapređenje hit detekcije (`feat/hitreg`)

Ovaj dokument opisuje implementaciju **Verzije 1** unapređenja hit detekcije i lag kompenzacije za **OpenBF2** (fokus na 4v4 / 5v5 mečeve na 16-player mapama, mešane pingove `10–100 ms` i povremeni packet loss / jitter), uz **100% kompatibilnost sa običnim (retail) BF2 1.50 klijentima**.

> **Napomena o statusu:** Sve izmene su `[CHOSEN, not recovered]` (namerno odstupanje od originalnog BF2 1.50 ponašanja) i zaštićene su konzolnim prekidačima (`phy-latencyComp-*`) kako bi se u svako doba moglo uraditi A/B testiranje (`0` = originalni BF2, `1` = poboljšani hitreg).
> Nijedna izmena ne menja veličinu klasa, memorijski raspored (`x86` layout) niti `vtable` slotove.

---

## 1. Šta je urađeno u Verziji 1

| # | Problem u originalnom BF2 | Rešenje u Verzij 1 | Fajlovi |
| --- | --- | --- | --- |
| 1 | **Stajaći input red (`33–100 ms` skrivenog laga):** Posle jitter-a ili packet loss-a, `ActionBuffer` trajno zadrži 1–3 akcije u redu, a `getLatency()` to kašnjenje ne računa. | Meri se tick dolaska svake akcije (`notePlayerActionArrival`) i stvarno dodatno čekanje u redu (`setPlayerActiveQueueDelay`), koje se sabira sa rewind-om (uz korekciju `-1` tick zbog toga što `uPlaying` uveća `mTickCount` pre `simulatePlayersUpdate`). | [LatencyCompensation.cpp](../Code/BF2/Physics/LatencyCompensation.cpp), [PlayerActionManager.cpp](../Code/BF2/Game/Common/PlayerActionManager.cpp), [GameServer.cpp](../Code/BF2/Game/GameServer/GameServer.cpp) |
| 2 | **Spawn / izlazak iz vozila („neuhvatljiv“ prvih ~150 ms):** `reset(true)` i `initLatencyCompensation()` pune ring bafer nulama `(0,0,0)` pa rewind vuče igrača ka centru mape. | Prvi validni tick (ili teleport > 3 m za vojnike / > 50 m za vozila) popunjava svih 16 slotova trenutnom pozicijom i rotacijom. | [SoldierPhysicsNode.cpp](../Code/BF2/Physics/SoldierPhysicsNode.cpp), [PhysicsNode.cpp](../Code/BF2/Physics/PhysicsNode.cpp) |
| 3 | **Prva kapsula umesto najbliže (`Skeleton::checkRayVsSkeleton`):** Petlja radi `break;` na prvoj kapsuli po indeksu u `m_vector24` (npr. ruka ume da „ukrade“ headshot). | Prolazi sve kolizione kapsule i bira presek sa strogo najmanjim vremenom duž putanje zrake `t`. | [Skeleton.cpp](../Code/BF2/Animation/BoneAnimation/Skeleton.cpp) |
| 4 | **Zamrzavanje kašnjenja u trenutku pucanja:** Rewind se u retail BF2 ponovo računa tokom svakog tick-a leta metka. | Zamrzava ukupno kašnjenje (`ping * pingFactor + interp + queueDelay`, limitirano sa `phy-latencyComp-max`) u trenutku aktiviranja/vezivanja igrača na projektilu, i čisti ga u `resetProjectile` i destruktoru. | [GenericProjectile.cpp](../Code/BF2/Game/Objects/Projectiles/GenericProjectile.cpp), [SoldierResponse.cpp](../Code/BF2/Physics/SoldierResponse.cpp), [PointResponsePhysics.cpp](../Code/BF2/Physics/PointResponsePhysics.cpp), [ResponsePhysicsManager.cpp](../Code/BF2/Physics/ResponsePhysicsManager.cpp), [GameLogicExplosion.cpp](../Code/BF2/Game/GameLogic/GameLogicExplosion.cpp) |
| 5 | **Server-side limit za `interpolationTime`:** Klijent šalje 9 bita (`0–511 ms`) direktno u `Player::setInterpolationTime` (moguć backtrack exploit). | Ograničava klijentov `interpolationTime` na serveru preko `phy-latencyComp-maxClientInterpMs` (default `133 ms`). | [Player.cpp](../Code/BF2/Game/GameLogic/Player.cpp), [LatencyCompensation.cpp](../Code/BF2/Physics/LatencyCompensation.cpp) |
| 6 | **Broadphase nestajanje vojnika pri stajanju:** Kad vojnik naglo stane (`speed ≈ 0`), `latencyDistance` u culler-u padne na `0`, pa metak promaši pretragu kandidata. | Donji prag brzine `phy-latencyComp-broadphaseFloor` (default `8.0 m/s`) za vojnike u `addObjectToCuller` i `updateObjectInCuller`. | [ObjectManager.cpp](../Code/BF2/Scene/Object/ObjectManager.cpp) |

---

## 2. Konzolne promenljive (CVar-ovi) za podešavanje i A/B testiranje

Svi novi cvar-ovi se podešavaju u server konzoli ili `.con` fajlu:

| CVar | Default | Opis |
| --- | --- | --- |
| `phy-latencyComp-improved` | `1` | Glavni prekidač za sve Verzija 1 izmene (`0` = 100% originalni BF2 1.50). |
| `phy-latencyComp-maxClientInterpMs` | `133` | Maksimalni dozvoljeni klijentov `interpolationTime` na serveru (ms). |
| `phy-latencyComp-queueDelay` | `1` | Uključuje/isključuje kompenzaciju čekanja u `ActionBuffer` redu (`0/1`). |
| `phy-latencyComp-maxQueueTicks` | `4` | Maksimalno dodatno kašnjenje iz reda koje se kompenzuje (u 30 Hz tick-ovima). |
| `phy-latencyComp-snapshotProjectile` | `1` | Zamrzava izračunati rewind na projektilu u trenutku ispaljivanja (`0/1`). |
| `phy-latencyComp-closestBone` | `1` | Bira najbližu kapsulu duž zrake umesto prve u nizu (`0/1`). |
| `phy-latencyComp-spawnRingFix` | `1` | Sprečava rewind ka `(0,0,0)` neposredno posle spawn-a / izlaska iz vozila (`0/1`). |
| `phy-latencyComp-broadphaseFloor` | `8.0` | Minimalna brzina (m/s) za širenje broadphase sfere vojnika. |
| `phy-latencyComp-debugLog` | `0` | Kada je `1`, ispisuje `[HITREG]` liniju na `stderr` za svaki pogodak u vojnika. |
| `phy-latencyComp-pingFactor` *(postojeći)* | `0.5` | Faktor množenja pinga u formuli lag kompenzacije (za kalibraciju testirati `0.5` vs `0.75` vs `1.0`). |
| `phy-latencyComp-max` *(postojeći)* | `0.3` | Gornji limit rewind-a u sekundama (`0.15`–`0.20` preporučeno za 5v5 mečeve). |

---

## 3. Kako da pokreneš build i testiraš kod kuće

### Preduslovi na kućnom Windows računaru
1. **Visual Studio 2022 Community** (besplatan) sa instaliranim paketom **Desktop development with C++** (uključuje MSVC v143, Windows SDK, CMake i Ninja).
2. **Battlefield 2 v1.50** instalacija (za mape i podatke igre prilikom testiranja servera uživo).

### Kloniranje i prelazak na granu
```powershell
git clone https://github.com/supermujka/OpenBF2.git
cd OpenBF2
git checkout feat/hitreg
```

### Build i provera testova (iz **x64** ili **x86** Visual Studio Developer terminala)
1. **Brza provera kompajliranja i unit/physics testova (x86):**
   ```powershell
   cmake --preset msvc-x86 -B out/build/local-x86
   cmake --build out/build/local-x86 --target bf2_recovered bf2_run_checks
   ```
   *(Dodatno: `cmd /c Code\Tests\runtime_pair.cmd` i `cmd /c Code\Tests\syntaxcheck.cmd`.)*

2. **Build nativnog 64-bitnog Windows dedicated servera (`bf2_w64ded.exe`):**
   Iz **x64 Visual Studio Developer terminala**:
   ```powershell
   cmd /c Code\Build\python\build_python.cmd x64
   cmake --preset msvc-x64 -B out/build/local-x64 -DBF2_GAME_DIR="D:/Games/Battlefield 2"
   cmake --build out/build/local-x64 --target bf2_w64ded
   ```
   *(Zameni `D:/Games/Battlefield 2` putanjom do svoje BF2 1.50 instalacije.)*

### Testiranje uživo & kalibracija `pingFactor`-a
1. Pokreni server sa malom 16p mapom i botom koji se kreće (ili sa prijateljem), na portu različitom od `16567` ako na istoj mašini pokrećeš i klijent (npr. `+port 16568`).
2. Uključi dijagnostički log na serveru:
   ```text
   vars.set phy-latencyComp-debugLog 1
   ```
3. Pucaj u pokretnu metu sa simuliranim pingom (npr. `clumsy` / `netem` 50 ms i 100 ms) i uporedi osećaj i `[HITREG]` log sa:
   - `vars.set phy-latencyComp-improved 0` (originalni BF2) vs `1` (Verzija 1)
   - `vars.set phy-latencyComp-pingFactor 0.5` vs `0.85` vs `1.0` (da proverimo da li je retail `0.5` nedovoljan zbog `ClientConnection` low-biased EMA pinga).

---

## 4. Šta sledi posle testiranja Verzije 1

- **Verzija 2 (najveći sledeći skok za skakanje / čučanje / *dolphin diving*):**
  Čuvanje **root-local transformacija kolizionih kostiju** (`pos + quat` po kosti) u sidecar ring baferu od 16 stanja (sa čišćenjem i u `reset(true)` zbog `ObjectCache`-a), pa rekonstrukcija preko `getCollisionBoneTransform(rewoundRoot, ...)` uz SLERP rotacije tela. Ovo uklanja desync između trenutne animacije na serveru i istorijske poze koju je napadač video.
- **Verzija 3 (opciono za 5v5 custom klijent+server):**
  Povećanje tickrate-a sa `30 Hz` na `60 Hz` (zamena hardkodovanog `0.033333335f` dinamičkim `dt`-om i proširenje ring bafera sa 16 na 32+ unosa).
