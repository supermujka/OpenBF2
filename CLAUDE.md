# BF2 Server Optimization — upute za Claude

## O projektu
Optimizacija Battlefield 2 dedicated servera (performanse, tick stabilnost, memorija, mrežni promet, server-side Python skripte).
Ranije optimizacije su rađene u više grana uz Gemini. Cilj sada: **pregledati, objediniti i doraditi** ih u jednu čistu, provjerenu liniju razvoja.

- Glavna grana: `main`  <!-- ispravi ako je drugačije -->
- Jezik/okruženje: <!-- npr. BF2 embedded Python 2.x (stara verzija: NEMA f-stringova, `with` itd. — provjeri tačnu verziju), C++ hook/DLL, config .con fajlovi -->
- Kako se pokreće/testira: <!-- npr. `./start_server.sh`, lokalni test server, bot test, skripta za benchmark -->
- Ključni folderi: <!-- npr. `python/bf2/`, `mods/`, `admin/`, `tools/` -->

## Uloge (ko šta radi)
- **Glavni (Opus) = šef.** Planira, odlučuje redoslijed, radi review i finalne izmjene na kritičnim mjestima. Ne čita cijeli repo sam — delegira.
- **`branch-auditor` (Haiku)** — jeftino čita diffove i kod, vraća SAMO sažetak. Koristi se za istraživanje i parsiranje.
- **`implementer` (Sonnet)** — piše kod po jasnom planu koji je šef odobrio.
- Za široku pretragu po repou koristi ugrađeni `Explore` agent.

## Pravila za štednju tokena
1. Prvo `git diff --stat` / `git log --oneline`, tek onda konkretni fajlovi. Nikad ne učitavaj cijeli repo.
2. Čitaj dijelove fajla (offset/limit) kad znaš gdje je kod; koristi grep umjesto čitanja naslijepo.
3. Teško čitanje (diffovi, logovi, veliki fajlovi) ide subagentima — oni vraćaju sažetak, ne sirovi kod.
4. Jedan zadatak = jedna sesija. Nakon završenog zadatka: commit, pa `/clear`.
5. Bilješke o stanju piši u `docs/BRANCHES.md` i `docs/DECISIONS.md`, ne drži ih u kontekstu.

## Pravila za kvalitet koda
- Svaka optimizacija mora imati **razlog i mjerenje** (prije/poslije: CPU %, tick time, RAM, broj poziva). Bez mjerenja = ne ulazi u `main`.
- Poštuj ograničenja BF2 runtime-a (stara verzija Pythona, nema vanjskih biblioteka). Ne uvodi moderne sintakse.
- Ne mijenjaj ponašanje igre (gameplay) bez eksplicitnog odobrenja — samo performanse.
- Male, fokusirane izmjene; jedan commit = jedna ideja; jasna commit poruka.
- Ako Gemini kod izgleda sumnjivo (mrtav kod, duplikati, "optimizacija" bez efekta) — označi i predloži uklanjanje, ne prepisuj slijepo.
- Prije izmjene u hot-pathu (event handleri koji se zovu svaki tick, `onPlayerKilled`, score update i sl.) objasni uticaj.

## Tok rada
1. Plan (šef) → 2. Istraživanje (auditor) → 3. Implementacija (implementer) → 4. Review i test (šef) → 5. Commit.
