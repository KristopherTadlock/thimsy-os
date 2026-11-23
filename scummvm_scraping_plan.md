# ScummVM Scraping Plan

Status: the listing is implemented (2026-11-16): with the ScummVM app as the player, Nano lists ScummVM's own game list, not files (Nano `docs/ARCHITECTURE.md`, "ScummVM is the exception"; `NanoScummvmList.cpp`). That replaces §R.2-R.3 and §3.1: each `scummvm.ini` game is one entry, launched by its target, named by its description, and scraped by its game ID (`romnom=<gameid>`). The scraping phases (§3.3 onwards, Phase 0 measurements) are still a plan.

Date: 2026-11-15, revised 2026-11-16 (§R: the game folder is the unit; it supersedes the parts of §0 and §3 it names).

Trees: AOSP `~/gammaos-src` (GammaOS Next 1.4.4 base, branch `nano-144`). Build repo `~/thimsy`.

Paths:
- `nano/` means `~/gammaos-src/frameworks/base/cmds/gammaos-nano/`.
- `mods/` means `~/thimsy/mods/`.
- Line numbers are for the current tree.

Research copies (downloaded as data, read only, nothing executed):
- `/tmp/svm-research-web/`: `scummvm-web` `data/en/*.yaml` and `include/DataUtils.php`.
- `/tmp/svm-research-icons/`: `scummvm-icons` `default/games.xml`, `companies.xml`, `LICENSE`.
- `/tmp/svm-research-src/`: ScummVM `master` sources (`engines/advancedDetector.*`, `base/commandLine.cpp`, `base/plugins.cpp`, a few `engines/*/detection.cpp`).
- `/tmp/svm-research-libretro/`: libretro-database `dat/ScummVM.dat` and `rdb/ScummVM.rdb`.
- `/tmp/svm-research-robloach/`: the generator of that DAT.
- `/tmp/svm-research-esde/`: ES-DE `USERGUIDE.md`, `ScreenScraper.cpp`, `GuiScraperSearch.cpp`, `FileData.h`.
- `/tmp/svm-research-skyscraper/`: Skyscraper docs and sources.
- `/tmp/svm-research-ss/`: ScreenScraper's public API page (`webapi2.php`) and public game pages. No API calls were made, and no credentials were used or read.

"Unverified" means it was read from source or docs, or inferred, and not tested against the live ScreenScraper API or on a device.

---

## R. Revision (2026-11-16): the top folder is the game

User: "The problem with scummvm is that it can't be scraped like other systems. Bins, cues, and iso are embedded all over the place." and "using the top most folder name for scraping probably makes more sense."

A real ScummVM library is one folder per game, holding `.bin`/`.cue` and `.iso` images, resource files, sub-folders and sometimes archives, at any depth. The original plan treated each `.scummvm` file as the game and only kept data folders out of the scan. This section makes the **top folder** the unit: it supersedes §0's "Recommended strategy", §3.1, the order in §3.2, §3.3 and §3.10. The rest (ini parsing, the ID table, variants, back-off, licence) still applies.

### R.1 New findings

1. **ScummVM's Android app can't start a game from a folder.** It passes the intent's data as a single argument (`ScummVMActivity.java:1106-1118`, and `:1217-1221` for a new intent, at tag `v2026.3.0`). That argument must be a target in `scummvm.ini` or a bare game ID (`base/commandLine.cpp:2270-2287`). A bare game ID makes a temporary target with no path, so it doesn't find the files. There is no second argument for `--path`, so `--auto-detect` can't be pointed at a folder. **A folder game is launchable only through a `scummvm.ini` target**: one the user added in ScummVM (Add Game or Mass Add), or one written by Thimsy (as `mods/46-scummvm-game.sh` does for Beneath a Steel Sky).
2. **RetroArch's ScummVM core isn't shipped** (`mods/10-update-emulators.sh:144`). The standalone app is the only player, so finding 1 applies to every launch.
3. **ScreenScraper supports folder games.** `jeuInfos.php` takes `romtype` = single ROM, single ISO or **folder**. For a folder, `romnom` is the folder name and `romtaille` the total size of the folder's contents. The CRC, MD5 and SHA1 are those of **the largest file in the folder** (`webapi2.txt:1109-1118`, `1394-1398`). So a folder has an identity ScreenScraper itself defines, and no data file is ever sent as a ROM. How many system 123 entries carry folder ROMs (the ROM tables seen so far are mostly `.svm` files and one `.zip`) is **unverified**. Phase 0 measures it.
4. **The stray entries now have a likely explanation.** "english", "language", "oak" and "zork0" fit files inside game folders (data-file stems, and a Glk game inside another folder) listed by the deep scan (§1.1). It is still **unverified** without the device's `.list` file (Q1).

### R.2 What counts as a game

For the ScummVM system (any system whose player launches with `{file.content}`), Nano looks **only at the direct children** of each source folder (`ROMs/scummvm/` and each folder the user added):

| Top-level entry | Becomes |
|---|---|
| A folder | One game, listed under the folder's name |
| A `.scummvm`/`.svm` file | One game, as today (its content is the target) |
| A file and a folder with the same stem (`Beneath a Steel Sky.scummvm` + `Beneath a Steel Sky/`, the shipped layout) | One game: the file gives the target, the folder gives the data and the size |
| An ES-DE folder-as-file (`Monkey Island.scummvm/`) | One game, named without the extension |
| Any other file (`.zip`, `.7z`, `.iso`, `.bin`, `.cue`, `.m3u`, `.txt` …) | Not a game |

- **Nothing inside a game folder is ever listed**, whatever its extension, whatever the "subfolders" setting of the source and the global recursion setting. The scan reads one directory per source, so a large library costs no more than its top level.
- **Hidden and system folders** (`.`-prefixed, `Android`, `LOST.DIR`) are skipped.
- **Grouping folders** (`ROMs/scummvm/LucasArts/Monkey Island/…`): the top folder is the game by default, as the user asked. Exception, only when ScummVM already knows: if `scummvm.ini` has targets in **two or more different sub-folders** of one top folder, each of those sub-folders becomes a game and the top folder is shown as a group. Without the ini, a grouping folder is one game with the group's name. Q8 asks whether to keep this exception.

### R.3 Launching a folder game

Targets are resolved by path. Nano reads `scummvm.ini` once per scan (read only, as before) and builds a map of normalised `path` to target. Normalising means `/sdcard`, `/storage/self/primary` and `/storage/emulated/0` are the same, trailing slashes are dropped, and the comparison ignores case for the FAT/exFAT card (`/storage/XXXX-XXXX`).

1. A same-stem `.scummvm` file's content wins, as today.
2. Otherwise, the target whose `path` is the game folder.
3. Otherwise, a target whose `path` lies inside it (`Monkey Island/CD`). If there are several (floppy and CD, two languages, a game and its add-on), Nano asks once at launch, listing the ini `description`s, and remembers the choice per folder. Add-ons (`-addon`) are never offered.
4. No target: the game is still listed and scraped. Launching shows "Add this game in ScummVM first: open ScummVM and choose Add Game, or Mass Add on the ScummVM folder", with an **Open ScummVM** button (launch with no data).

Nano doesn't write `scummvm.ini` (Q3 stands; Q9 asks again now that folder games depend on it). One small build-time help, if its key works on Android (**unverified**): seed `browser_lastpath=/storage/emulated/0/ROMs/scummvm` in the `[scummvm]` section `mods/46-scummvm-game.sh` already writes, so ScummVM's Mass Add opens on the right folder on a fresh install.

### R.4 Identity

Inputs: the top folder's **name**, a same-stem file's **target**, the ini **targets under the folder**, and (at scrape time only) the folder's **size** and **largest file**.

1. **Target known** (same-stem file, or one ini target for the folder): resolve the game ID as in §3.2 steps 1-3. Confidence: exact or high.
2. **Folder name.** Clean it into a title and variant hints:
   - Drop bracketed groups `(…)` and `[…]`, keeping recognised tags as hints: `CD`, `Floppy`, `Talkie`, `Demo`, `DOS`, `Windows`, `Amiga`, `Mac`, `FM-Towns`, `VGA`, `EGA`, `GOG`, `Steam`, `v1.2`, years, language names and codes.
   - Treat `_` and `.` as spaces (except in versions), collapse spaces, turn "Monkey Island, The" into "The Monkey Island".
   - Strip the same tags when they trail the name without brackets (`Monkey Island 2 CD`), only from the end.
   Confidence: medium.
3. **Folder name as an ID.** A folder named `sky` or `monkey2` (Batocera and some packs) is matched against the ID table (Phase 3). Confidence: high.
4. **Title table match** (Phase 3): the cleaned title against ScummVM's titles, unique exact match only. Gives the ID. Confidence: high.

The listed name is the folder's name as is, like file stems today. After a scrape, the title comes from ScreenScraper; with no match, from the table (Phase 3), else the cleaned folder name.

### R.5 Queries

ScreenScraper, system 123, stopping at the first match:
1. Cached ScreenScraper game id, as §3.3 step 1.
2. If the ID is known: `romnom=<gameid>.svm`, as §3.3 step 2.
3. **Folder ROM:** `jeuInfos.php` with `romtype=dossier`, `romnom=<top folder name>`, `romtaille=<total size>`, and the CRC, MD5 and SHA1 of the largest file, computed in one read. The walk for the size is bounded (depth 8, 20,000 files). The largest file can be a 700 MB disc image: hashing it costs seconds on the T820 and runs only on a scrape, once per folder, cached by the folder's path, size and the largest file's mtime. Phase 0 decides whether this step stays, and whether it goes before or after step 4. It spends a not-found on every miss.
4. `jeuRecherche.php` with the cleaned title, `systemeid=123`, then the original platform as §3.3 step 4.
5. TheGamesDB by the cleaned or canonical title, on PC.

Never: a job per file inside a folder, or any data file sent as a ROM.

Media and gamelist entries are named after the top folder (`Monkey Island 2 (CD).png`), or the same-stem file when there is one, so the shipped Beneath a Steel Sky keeps its current names.

### R.6 Tests (adds to §3.9)

- Scan fixture: a ScummVM tree with nested `.bin`/`.cue`/`.iso`/`.zip`/`.m3u`/`.scummvm` files several levels deep. Only the top folders and top-level `.scummvm` files are listed, with every source and recursion setting. A file+folder pair is one entry. Other systems are unchanged.
- Folder-name cleaning, table-driven, on real folder names (GOG, Steam, eXo-style and hand-made). This needs samples (Q10).
- ini path matching: `/sdcard` vs `/storage/emulated/0`, trailing slash, card case, a target in a sub-folder, two targets in one folder, an add-on.
- Folder ROM fingerprint: largest-file choice (ties broken by name), size total, the walk bounds, the cache key.

### R.7 Phases (replaces §3.10)

0. **Measure** (needs the user's ScreenScraper account). As §3.10, plus: for 10 folder games, `romtype=dossier` with the real folder name, size and largest-file hash; and `jeuRecherche` with cleaned folder names. Collect a sample of real top-level folder names (Q10).
1. **Top folder as the unit.** The scan rule (R.2), path-based launch with the "Add in ScummVM first" prompt (R.3), folder-name titles. This alone stops data files showing up as games.
2. **Queries.** ID-aware steps, cleaned-folder-name search, cached ids, the 30-day back-off; the folder ROM step if Phase 0 shows matches.
3. **Bundled table** (§3.6): folder-name-as-ID and title matching, canonical titles.
4. Optional: ScummVM detection fingerprints (§2) only if Phases 1-3 leave too many folders unidentified.

### R.8 New questions

8. **Grouping folders.** Keep the exception in R.2 (split a top folder when ScummVM has targets in several of its sub-folders), or always treat the top folder as one game?
9. **Folder games ScummVM doesn't know yet.** Leave adding them to ScummVM (recommended: Nano keeps never writing ScummVM's file), or let Nano add a target for a folder after a confirmation? That would need its own game detection (§2), since ScummVM can't be asked to detect a path from outside.
10. **Real folder names.** A listing of the top level of your ScummVM folder (`adb shell ls /sdcard/ROMs/scummvm`) would calibrate the cleaning rules and Phase 0's sample.

---

## 0. Summary

ScummVM games are a **closed catalogue**: every game ScummVM can run has an ID (`engine:gameid`) in its detection tables. Thimsy already stores that ID in each game's `.scummvm` file, so the scraper can know the game **exactly**. Today it throws that away: it hashes the 4-byte text file and sends its file name, which ScreenScraper can't match.

What was found:

1. **Nano scrapes ScummVM like a cartridge.** It sends `romnom=Beneath a Steel Sky.scummvm`, the CRC of the file (`sky\n`: `0264df0e`, 4 bytes) and `systemeid=123` to `jeuInfos.php` (`nano/NanoScraper.cpp:589-611`). It never reads the ID inside the file, nor ScummVM's `scummvm.ini`.
2. **ScreenScraper keys its ScummVM ROMs by game ID.** Its public ROM tables for system 123 list `sky.svm` (19 bytes), `sky.scummvm` (20 and 21 bytes) for Beneath a Steel Sky, `gk2.svm`/`gk2.scummvm` for Gabriel Knight 2 and `toltecs.svm`/`toltecs.scummvm` for 3 Skulls of the Toltecs. The sizes fit a file holding the game's *description* (RetroPie's `.svm` style), not its ID. So the **name** `<gameid>.svm` is what can match. A CRC of Thimsy's `sky\n` file can't (§1.3).
3. **ScreenScraper covers 508 ScummVM games (1,387 ROMs).** ScummVM's site lists 1,223 games, and its detection tables know about 12,158 IDs (mostly AGS and Glk fan and IF games). Most of the long tail will never match on system 123 (§1.3).
4. **`jeuInfos.php` takes `gameid=<ScreenScraper id>`** to fetch one game exactly, with no ROM data. Once a game has matched, it can be fetched again by that ID without guessing (§1.3).
5. **ScummVM publishes machine-readable game lists.** These are `scummvm-web` `data/en/games.yaml` (1,223 games: id, name, engine, company, year, MobyGames id; GPLv3 repo), `scummvm-icons` `default/games.xml` (the same data, GPLv2), `scummvm --list-all-games` (every detectable `engine:gameid` and title) and `scummvm --dump-all-detection-entries` (a DAT of every detection entry with file names, sizes and MD5s) (§1.2).
6. **libretro's `ScummVM.dat` is the ID-in-a-file convention RetroArch scans.** It has 12,158 game IDs, each as `<id>.scummvm` with the CRC of `<id>`, `<id>\n` and `<id>\r\n`. Thimsy's `sky\n` file is in it (`crc 0264df0e`). It is CC BY-SA 4.0, and its titles are partly matched from a third-party XML (§1.4).
7. **What counts as a ScummVM game today** is any `.scummvm` or `.svm` file, but also **any `.zip`, `.7z` or `.m3u`**, because Nano adds those to every system (`nano/NanoMenuXmb.cpp:1873-1874`, `1720`). Added folders are scanned 6 levels deep (`:1834-1840`). The stray "english", "language", "oak" and "zork0" entries were not reproduced from the build. These are the likely ways in (§1.1, Q1).

Recommended strategy (§3):
- A game is a `.scummvm` or `.svm` file with a valid ID, or an ES-DE "folder as file". Archives, playlists and game data folders are not games.
- Identify it by its ID, through `scummvm.ini`, then a bundled ID table, then the name.
- Query ScreenScraper by its cached game ID, then `romnom=<gameid>.svm`, then by title on system 123, then by title on the variant's original platform. Query TheGamesDB by title only, on PC.
- Bundle one small generated table (ID → title, engine, year, company) from ScummVM's own data. Folder fingerprinting is optional and comes later.

Phases (§3.10): 0 measure, 1 listing hygiene, 2 ID-aware queries (no bundled data), 3 the bundled table, 4 optional folder fingerprints and a ".scummvm creator".

---

## 1. Findings

### 1.1 Nano today

**What is listed.** The ScummVM system's player is `scummvm.org.scummvm.scummvm` (`mods/nano-systems.txt:62`). Its catalog entry `nano/ps3xmb/daijishou/ScummVM.json` has:
- a platform regex `^(?!(?:\._|\.).*).*$` (every file not starting with a dot). Nano's `extsFromRegex` (`nano/NanoMenuPS3EmuCatalog.cpp:62-80`) skips groups with backslashes, so Nano never uses this regex. Daijisho itself would accept every file with it.
- a player regex `^(.*)\.(?:scummvm|svm)$`, so `acceptExts` = `.scummvm,.svm` (`:597-599`). The RetroArch core's players add `.sh`.
- `amStartArguments … -d {file.content}`. The launch passes the first line of the file, checked by `launchFileId` (1-64 of `[A-Za-z0-9_-]`, no leading `-`; `nano/NanoLaunch.cpp:163-178`).

On top of `acceptExts`, `scanRomPaths`/`scanOneSystemAsync` **always add `.zip` and `.7z`** (`nano/NanoMenuXmb.cpp:1873-1874`, `1979-1980`, `2144-2145`). `scanSystemDir` **always accepts `.m3u`/`.m3u8`** (`:1720`). Folders are entered only with `persist.gammaos.nano.rom.recursive=1` (`:1825-1831`, default off). A folder the user or auto-add added as a source is scanned 6 levels deep unless it was added "This Folder Only" (`:1834-1840`; auto-add sources default to subfolders, `:1408`). So in a ScummVM folder that is scanned deeply, any archive or playlist inside a game's data folder becomes a game.

**Stray entries ("english", "language", "oak", "zork0").** None of these is in the build:
- the ROM pack ships only `scummvm/Beneath a Steel Sky.scummvm` and the game's folder (`mods/bundled-roms.txt:52-53`);
- the folder holds `sky.dnr`, `sky.dsk`, `sky.cpt`, `readme.txt` (`work/cache/scummvm/bass-cd-1.2.zip`).

`zork0` is a real ScummVM ID (Zork Zero, Glk), and `english` and `language` look like data-file stems. So they most likely came from user files on the card ("card wins"), picked up as `.zip`/`.7z`/`.m3u`/`.svm` inside game folders through a deep scan. **Unverified.** `/data/system/nano_xmb_cache/<system id>.list` on that device holds the full paths (`nano/NanoMenuXmb.cpp:1931-1944`) and would settle it (Q1).

**What is scraped.** `scrapeSystemsAsync` makes one job per listed file (`nano/NanoMenuScraper.cpp:1260-1316`). `platformForSystem` maps `scummvm` to ScreenScraper 123 and TheGamesDB 1 (PC) (`nano/NanoScraper.cpp:413`). `scrapeScreenScraper` (`:568`):
- sets `romnom` to the file name with its extension, or the user's title override (`:589`);
- adds the CRC of the file (up to 128 MB), `romtaille` and `systemeid`, and calls `jeuInfos.php` (`:594-611`);
- uses `jeuRecherche.php` only for Android `.app` files or files with neither a CRC nor a system (`:593-596`);
- if nothing matches, takes the title from the file name (`:713`).

TheGamesDB searches by the cleaned file stem, with `filter[platform]=1` (`:920-934`). For `Beneath a Steel Sky.scummvm` that stem is fine by luck. For an ES-DE-style `sky.scummvm` it is `sky`.

**What is stored.** Covers and fan art go to the scrape index or, in ES-DE store mode, to `ES-DE/downloaded_media/scummvm/...`. Titles and metadata go to `gamelists/scummvm/gamelist.xml` (`nano/NanoEsdeStore.cpp:16-24`). Media names follow the game file's stem. ES-DE keeps the extension for a "folder interpreted as file" (`dig.scummvm.png`, ES-DE `USERGUIDE.md:3741-3750`).

**ScummVM's own list.** Nano already reads `/data/data/org.scummvm.scummvm/files/scummvm.ini` (`appdef::kScummvmIni`, `nano/NanoAppFiles.h:41`; read in `nano/NanoButtonMapReaders.cpp:1724`). Thimsy's shipped entry is `[sky]` with `gameid=sky`, `engineid=sky`, `description=Beneath a Steel Sky (CD/DOS/English)`, `extra=CD`, `language=en`, `platform=pc` and `path=…` (`mods/46-scummvm-game.sh:44-55`).

### 1.2 ScummVM's canonical data

| Source | What it holds | Format | Licence | URL |
|---|---|---|---|---|
| `scummvm-web` `data/en/games.yaml` | 1,223 games: `id` (`engine:gameid`), `name`, `engine_id`, `company_id`, `year`, `moby_id`, `steam_id`, `gog_id`, `series_id` | YAML, "generated file" from a public Google Sheet (`include/DataUtils.php:26`) | Repo is GPLv3 (`LICENSE`) | https://github.com/scummvm/scummvm-web/tree/master/data/en |
| same: `compatibility.yaml`, `engines.yaml`, `companies.yaml`, `platforms.yaml`, `series.yaml` | 1,041 support records (`support`, `stable_platforms`), 127 engines (`enabled`), 467 companies, platforms with MobyGames ids | YAML | GPLv3 | same |
| `scummvm-icons` `default/games.xml` (+ `companies.xml`, `series.xml`) | The same games sheet, `id` without the engine prefix | XML | GPLv2 | https://github.com/scummvm/scummvm-icons |
| `scummvm --list-all-games` | Every ID any detection plugin knows: `engine:gameid` + title, fixed-width text (`base/commandLine.cpp:1136-1158`) | Text | Output of GPLv3 code (titles are facts from the detection tables) | https://docs.scummvm.org/en/latest/advanced_topics/command_line.html |
| `scummvm --list-games[-json]` | The same for the engines built into that binary; `--list-games-json` gives `{"engine:gameid": "Title"}` (`:1084-1131`) | Text / JSON | same | same |
| `scummvm --dump-all-detection-entries` | One `game ( name, title, extra, language, platform, engine, rom ( name size md5-N md5 ) )` per detection entry (`base/commandLine.cpp:1636-1648`, `engines/advancedDetector.cpp:687-721`) | clrmamepro-style DAT | same | source only |
| `scummvm --detect [--recursive] -p DIR` | Detects games in a folder and prints them with their IDs, without adding them | Text | n/a (runs ScummVM) | docs above |
| Detection tables (`engines/*/detection_tables.h`) | `ADGameDescription`: `gameId`, `extra`, up to 13 files (`fileName`, `md5` of the first 5000 bytes by default, size), language, platform, flags (`engines/advancedDetector.h:78-90`, `164-204`; `_md5Bytes = 5000`, `.cpp:994`). Prefixes `t:` (tail), `r:`/`d:` (Mac forks), `A:` (archive) (`.cpp:495-514`) | C++ | GPLv3+ | https://github.com/scummvm/scummvm |

Points that matter for identification:
- **IDs collide across engines.** The docs say `atlantis` alone is ambiguous (`scumm:atlantis` vs `cryomni3d:atlantis`). In `games.yaml`, `carnival` and `driller` each appear under two engines. libretro's DAT keeps the engine prefix on 224 of its IDs for this reason.
- **A target is not a game ID.** ScummVM names a new entry `generatePreferredTarget()`: the game ID (or the sanitised `extra` with `ADGF_AUTOGENTARGET`), then `-demo`, `-cd`, `-dvd`, `-remastered`, `-<platform>` (unless DOS or unknown), `-<language>` (unless English or unknown), `-addon` (`engines/advancedDetector.cpp:130-165`). A clash then adds `-1`, `-2`… (`base/plugins.cpp:729-736`). Users can rename targets too. ES-DE warns about `sky-1` (`USERGUIDE.md:3049`). SCUMM, Sky and some others build targets in their own code. Whether they add the same suffixes is **unverified**.
- **Not every engine dumps its detection data.** Sky's `dumpDetectionEntries()` is empty. It detects by `sky.dnr`'s entry count and `sky.dsk`'s size (`engines/sky/detection.cpp:80`, `150-171`). SCUMM uses its own MD5 table over the first 1 MB (`engines/scumm/detection_internal.h:46`). Glk has its own detectors.

**Licence for a Thimsy table.** Titles, IDs and years are facts, but the lists are GPL. A table derived from them, shipped as a separate data file under `/system/etc/` with its licence and an `ATTRIBUTION.md` row, sits beside Apache-2.0 Nano the way the CC BY-SA console pictures do (`ATTRIBUTION.md:33-51`). That reading is mine, not legal advice (Q6).

### 1.3 ScreenScraper and ScummVM (system 123)

API (public page https://www.screenscraper.fr/webapi2.php, `/tmp/svm-research-ss/webapi2.txt:1057-1135`):
- `jeuInfos.php` takes `crc`/`md5`/`sha1` "du fichier rom/iso/dossier", `systemeid`, `romtype` (single ROM / single ISO / **folder**), `romnom` ("nom du fichier (avec extension) ou nom du dossier"), `romtaille`, `serialnum`, and **`gameid`** ("Forcer la recherche du jeu avec son identifiant numérique"; with it "Aucune information sur la rom n'est envoyée").
- At least one hash plus the size is required "unless exempted".
- `jeuRecherche.php` takes `recherche` (+ optional `systemeid`) and returns up to 30 games "classés par probabilité", without ROM data.
- Not-found answers count against a separate daily limit (`maxrequestskoperday`). Nano already reports it ("faite du tri", `nano/NanoScraper.cpp:553`).

Public site data (fetched 2026-11-15, no login):
- System 123 has **508 games, 1,387 ROMs**.
- ROM tables seen (`/tmp/svm-research-ss/roms_*.html`):

| Game (ScreenScraper id) | ROM names, sizes |
|---|---|
| Beneath a Steel Sky (99499) | `sky.svm` 19 B (CRC `D4348EA3`; "35" file-name aliases), `sky.scummvm` 21 B (`9B881095`), `sky.scummvm` 20 B |
| The Beast Within: A Gabriel Knight Mystery (155473) | `gk2.svm` 44 B, `gk2.scummvm` 20 B |
| 3 Skulls of the Toltecs (99478) | `toltecs.svm` 23 B, `toltecs.scummvm` 25 B and 36 B, and a 101 MB `3 Skulls Of The Toltects (1996)(Warner Interactive).zip` |

"Beneath a Steel Sky" is 19 characters, the size of `sky.svm`. RetroPie's script writes `<target>.svm` files holding the game's description (https://retropie.org.uk/forum/topic/34663.rss).

**So ScreenScraper's ScummVM ROMs are named by game ID, and their hashes are of description files.** A Thimsy-style file (`sky\n`, 4 B) hashes to nothing they hold. ES-DE says "ScreenScraper does natively support ScummVM short names and you should get very accurate results" for `sky.scummvm` files (`USERGUIDE.md:3059`). ES-DE sends `romnom=<stem without extension>` (here `sky`), with the MD5 when hashing is on (`ScreenScraper.cpp:215-225`, `818-877`; `FileData.h:43-46`). Which `romnom` spelling matches best (`sky`, `sky.svm`, `sky.scummvm`, or the 35 aliases), and whether a non-matching hash hurts, is **unverified**. This needs authenticated calls (Phase 0).

Cross-references to the original DOS or Amiga games: ScreenScraper's ScummVM games are their own entries (99499 is "Beneath A Steel Sky - ScummVM"). Whether they link to the DOS entry is **unverified**. The pages list per-system games, not links. Media and text are licensed **CC BY-NC-SA 4.0** (footer of every game page). That rules out bundling anything derived from ScreenScraper in a commercial-friendly way, including an ID map, unless the user accepts NC terms (Q5).

### 1.4 Other frontends and databases

| Frontend / DB | Layout | How it identifies / scrapes | Source |
|---|---|---|---|
| **ES-DE** | `ROMs/scummvm/<Game>/<id>.scummvm` or `ROMs/scummvm/<id>.scummvm/<id>.scummvm` (folder as file). The file must contain the ID. Android: games must be added in the app first | ScreenScraper: `jeuInfos` with `romnom=<stem>` (+MD5 in automatic mode, size cap); `jeuRecherche` for interactive. TheGamesDB "very inaccurate" with short names | `USERGUIDE.md:3030-3075`, `ScreenScraper.cpp` |
| **Batocera / RetroBat** | One folder per game. The `.scummvm` file holds the ID (v42+), or is named by it, or ScummVM autodetects. `-cd`, `-win`, `-amiga`, `-demo` and `-fr` suffixes pick variants | "This system scrapes metadata for the 'pc' group" | https://wiki.batocera.org/systems:scummvm |
| **RetroPie** | Generated `<target>.svm` files holding the description | ES scraper on the `.svm` names | forum link above |
| **Skyscraper** | Accepts folders for `scummvm` | For name-based modules, turns `monkey2` into `description=` from `scummvm.ini` (`scummIni`); `searchStem` sends ScreenScraper the stem | `docs/CONFIGINI.md:752-764`, `1153-1159`; `src/abstractscraper.cpp:511-514` |
| **RetroArch / libretro-database** | `<id>.scummvm` files | Scanner matches the file's CRC in `ScummVM.dat` (12,158 IDs × 3 line-ending variants) | `dat/ScummVM.dat` header; generator https://github.com/RobLoach/libretro-database-scummvm (MIT code) |
| **Daijisho** | catalog `ScummVM.json` | `scraperSourceList: LIBRETRO:ScummVM`, the same DAT | `nano/ps3xmb/daijishou/ScummVM.json` |
| LaunchBox, Pegasus | n/a | Not researched in depth. LaunchBox treats ScummVM as an emulator for games of their original platform (**unverified**) | — |

libretro's DAT is built from `scummvm --list-all-games`, `scummvm-icons` metadata and two `ScummVM.xml` files matched by title. Its titles, descriptions and developers therefore partly come from a third party. libretro-database is CC BY-SA 4.0. It is a good **cross-check**, but a poor source to ship.

Fallback databases:
- **TheGamesDB** has no ScummVM platform, so search by the real title on PC (1). Nano already does this with the file stem.
- **MobyGames**: `games.yaml` carries `moby_id` for most games (275 of 1,223 are empty), so a MobyGames lookup would be exact. Its API terms couldn't be fetched (403), so they are **unverified**. It is not proposed here.
- **IGDB**: not researched.

---

## 2. Identification options weighed

| Method | Accuracy | Data to ship | Maintenance | Covers |
|---|---|---|---|---|
| ID in the `.scummvm` file → `scummvm.ini` section | Exact (what ScummVM will launch: engine, gameid, variant) | none | none | App users whose games are in ScummVM's list (all Thimsy installs) |
| ID in the file, no ini entry (RetroArch core, copied files) → strip target suffixes → bundled ID table | High; ambiguous for colliding IDs | ID table, ~60 KB for the 1,223 curated games, ~0.5 MB for all 12k IDs (estimates) | Regenerate when the ScummVM APK is bumped | All valid files |
| File stem as ID (`sky.scummvm` with no content) | High when the stem is a known ID | ID table | same | ES-DE/Batocera-style files. Not launchable in Nano today |
| Folder fingerprint: file names + sizes, MD5 of the first 5000 bytes, against a detection subset | High for supported variants; needs per-engine rules (Sky, SCUMM, Glk) | Unknown size: measure `--dump-all-detection-entries` minus AGS/Glk/Wintermute (Phase 4) | High: tables change every release | Plain folders with no `.scummvm` file |
| Name (file stem, folder name, ini `description`) → fuzzy match to table titles | Medium to low | ID table | low | Everything else |

Nano must not start ScummVM to detect games. That fits the "no needless subprocesses" rule. Android's ScummVM has no headless `--detect` either.

---

## 3. Recommended strategy (pipeline)

### 3.1 What counts as a game

For the ScummVM system (and any system whose player launches with `{file.content}`):
1. A `.scummvm` or `.svm` file. Its first line, read with `readLaunchFileId` (`nano/NanoLaunch.cpp:180`), is its **target**. A file with no valid line is still listed (the launch already explains the problem, `nano/NanoMenuXmb.cpp:3008`), but it is scraped only by name (§3.2 step 4).
2. An ES-DE folder-as-file: a folder named `*.scummvm`/`*.svm` directly in the system folder. The game is the file inside with the same name. It is listed once, under the folder's name, without turning recursion on.
3. **Not games** in this system:
   - `.zip`/`.7z`. ScummVM's `{file.content}` launch can't open an archive. Don't add them for content-launched systems (`nano/NanoMenuXmb.cpp:1873`).
   - `.m3u`. ScummVM games are folders and need no playlists.
   - Anything inside a game's data folder. A data folder is the folder beside a `.scummvm` file with the same stem, or the ini `path`. Never recurse into it, whatever the source's subfolder setting.
4. Entries in `scummvm.ini` without a `.scummvm` file are not listed (Q3 offers a creator).

### 3.2 Identity, in order of confidence

Inputs: `target` (file content), `stem` (file name), `folder` (data folder), `ini` (parsed `scummvm.ini`, read once per scrape run, never written).

1. **ini hit.** `ini[target]` exists → `engineid`, `gameid`, `language`, `platform`, `extra`, `description`. Confidence: exact.
2. **Qualified or plain ID.** `target` is `engine:gameid`, or is itself a table ID → that ID. If the ID is ambiguous, use the engine from step 1 or 5. Otherwise prefer the entry in the curated 1,223, else mark it ambiguous. Confidence: high. (Nano's `launchFileId` rejects `:`, so qualified IDs only appear through the ini.)
3. **Target suffixes.** Repeatedly strip, from the end: `-<n>`, `-<language code>`, `-<platform abbrev>`, `-remastered`, `-dvd`, `-cd`, `-demo`, `-addon`, until the rest is a table ID. Keep what was stripped as variant hints (`monkey2-cd-fr` → `monkey2`, CD, French). Confidence: high.
4. **Stem as ID.** `stem` lowercased is a table ID (`sky.scummvm`). Confidence: high. Used only when the content gives nothing.
5. **Folder fingerprint** (Phase 4, optional). Confidence: high to medium.
6. **Name.** Clean `ini.description` (drop the trailing `(CD/DOS/English)`), else the stem, else the folder name. Match to table titles (normalised, articles moved, "The" handled). Accept only a unique exact match, else keep it as a search string. Confidence: medium or low.

The result is `{id, engine, title, variant{cd, demo, platform, language, extra}, confidence}`, cached per game file in the scrape index (path + file mtime as the key).

### 3.3 From identity to queries

ScreenScraper, system 123, stopping at the first match:
1. **Cached ScreenScraper game id.** Earlier matches are remembered per `engine:gameid` (a small map in the scrape index; it is per device, not shipped) → `jeuInfos.php?gameid=<n>`. Exact, and no ROM data is sent.
2. **`jeuInfos.php` with `romnom=<gameid>.svm`** (the name ScreenScraper's ROMs carry), `systemeid=123`, `romtype=rom`, with the local file's CRC, MD5, SHA1 and size, since the API asks for a hash. If that misses, try `romnom=<gameid>.scummvm`, then `romnom=<gameid>` (ES-DE's form). Phase 0 decides which one or two spellings to keep (**unverified**). Each miss costs a not-found from the daily allowance, so keep this to at most two calls.
3. **`jeuRecherche.php`** `systemeid=123`, `recherche=<canonical title>`, with Nano's existing best-name check (`nano/NanoScraper.cpp:640-664`). This catches games ScreenScraper has under another ROM name.
4. **Original platform** (Phase 3). The variant platform (`pc`→DOS 135, `amiga`→64, `windows`→138; others **unverified**: `macintosh`, `fmtowns`, `pce`) → `jeuRecherche` by title on that system, accepting only an exact name. This gets art for the long tail (Glk/Infocom, AGS commercial games) where system 123 has no entry.
5. **TheGamesDB** (when it is the engine): `ByGameName` with the **canonical title**, `filter[platform]=1`. Never the ID.

A user title override or a one-off search name replaces the title in steps 3-5. It never replaces the ID in steps 1-2, unless the identity confidence is "name".

Writing results: the title comes from ScreenScraper's region names as today. If there is no match, use the **canonical title** from §3.2, not the file stem (`nano/NanoScraper.cpp:713`). Then a `sky.scummvm` file shows "Beneath a Steel Sky" even offline.

### 3.4 Variants and language

- One ScreenScraper game per `gameid`. CD and floppy, platform and language variants share its art and text.
- Synopsis and genre language: keep Nano's English-first rule (`nano/NanoScraper.cpp:716-725`). Optionally prefer the game's `language` from the ini when it matches the UI language (Q4).
- Two variants of one game in a library (for example `monkey` EGA and `monkey-cd`): give both the art, and add a variant tag to the shown title **only when names would collide** ("The Secret of Monkey Island (CD)"). Never write the tag into ScreenScraper queries.
- Distinct releases with distinct IDs (`sky` vs `ibass` remastered, `sword1` vs `sword1-remastered`?) are different games. The table decides.

### 3.5 What not to scrape

- Files with no valid ID whose name doesn't give a unique table match. List them as unidentified, and scrape them only through "Scrape This Game" with a typed name.
- Repeat misses. A not-found is remembered per ID for 30 days, like the extras' recheck (`nano/NanoMenuScraper.cpp:1362-1364`), so a library of 300 AGS freeware games doesn't burn the daily not-found allowance every run. ScreenScraper's 508 games against ScummVM's 12k IDs means most AGS, Glk and Wintermute games will miss on step 2.
- Fallback-detected targets (`*-fallback`, `engines/advancedDetector.cpp:302`) and add-ons (`-addon`): name search only.
- Archives, playlists and data files (§3.1).

### 3.6 Data files to bundle

| File (on the device: `/system/etc/nano_xmb/scummvm/`) | Contents | Source | Size (estimate) | Licence |
|---|---|---|---|---|
| `games.tsv` | `engine`, `gameid`, `title`, `year`, `company`, `moby_id`, `curated` (in the site list) | `scummvm-icons/default/games.xml` (or `scummvm-web` `games.yaml`) + `scummvm --list-all-games` of the **shipped** ScummVM version (2026.3.0, `mods/46-scummvm-game.sh:39-41`) | ~60 KB curated only; ~0.5 MB with all IDs, ~150-200 KB compressed | GPL (v2 for icons data, v3 for site/program output), with a `LICENSE` beside it and an `ATTRIBUTION.md` row |
| `platforms.tsv` | ScummVM platform code → ScreenScraper system, TheGamesDB platform | hand-written | <1 KB | Thimsy's Apache-2.0 |
| `signatures.bin` (Phase 4, optional) | Per entry: engine, gameid, variant, file names, sizes, MD5-5000 | `scummvm --dump-all-detection-entries`, minus AGS/Glk/Wintermute/Director fan entries, plus hand rules for Sky and SCUMM | measure first | GPLv3 |

Generation: `scripts/scummvm_games.py`, run on the build host with:
- the `scummvm-icons` or `scummvm-web` file at a pinned commit;
- the `--list-all-games` text from a ScummVM **built or packaged by the maintainer** at the same version as the APK. The script parses the text and never runs anything it downloaded.

The build checks that the table's recorded ScummVM version matches the APK in `mods/added-apps.txt`, and fails with "regenerate" when it doesn't. So the table is updated with each ScummVM bump, and nowhere else.

Not shipped: any ScreenScraper-derived data (CC BY-NC-SA), libretro's DAT titles (third-party XML provenance), or MobyGames data.

### 3.7 Edge cases

- **Multi-disc / CD vs floppy.** One folder per game. ScummVM wants all discs' files in it. Never group or split. `-cd` stays a variant of the same ID.
- **Fan translations.** These are usually the base `gameid` with an unknown or other language, or an `extra` note. Use the same art and the English title.
- **Freeware pack-ins.** Beneath a Steel Sky ships with Thimsy. Add it to `mods/pack-in-media.txt` with art made from the game itself (`tools/pack-in-art`), like the other pack-ins. Then it has a cover with no account and no network. ScummVM's other freeware games (Flight of the Amazon Queen, Lure of the Temptress, Drascula, Dreamweb) match on step 2 if users add them (**unverified**).
- **Huge engine catalogues.**
  - AGS (396 on the site, thousands detectable), Glk (Infocom and IF), Wintermute and SCI/AGI fan games are identified fine, but rarely on system 123.
  - AGI fan games share `gameid=agi-fanmade`. The title comes from the ini `description` or `extra`, never the ID.
  - Glk games: step 4 on DOS finds Infocom's commercial titles. Fan IF has no art anywhere, so it gets the one search and the 30-day back-off.
- **Ambiguous IDs** (`atlantis`, `carnival`, `driller`): need the engine from the ini, or stay "ambiguous" and fall to the title search.
- **Renamed targets** ("My Monkey"): only the ini resolves them; otherwise a name search.
- **RetroArch's ScummVM core**: the file holds a game ID that the core autodetects in the folder, with no app ini entry. Step 2/3 covers it.

### 3.8 UI implications

- The scrape progress line names what was identified: "Beneath a Steel Sky (sky · CD · DOS)".
- "Scrape This Game" prefills the canonical title. Info shows the ScummVM ID and variant.
- An unidentified file (no valid ID) gets a small "?" badge or an Info line pointing to the fix (Q2).
- An option "Show ScummVM titles" would turn `sky.scummvm` into "Beneath a Steel Sky" in the list before any scrape (Q2).
- Phase 4, optional: "Create ScummVM entries", which writes `.scummvm` files for games already in `scummvm.ini` (and only those), named by description. It never edits ScummVM's ini (Q3).

### 3.9 Tests

Host unit tests (`nano/test/unit/`, new `NanoScummvmId_test.cpp`; the scraper has none today):
- ini parsing: case, BOM, CRLF, sections with `path=` holding spaces, missing keys, a huge file cap.
- Target resolution, table-driven: `sky`, `sky-1`, `monkey2-cd-fr`, `indy3-fmtowns-jp`, `ft-demo`, `atlantis` (ambiguous, with and without the ini engine), `agi-fanmade`, renamed `My Monkey` with the ini, an empty file with `sky.scummvm` as the stem, garbage content.
- Query building: the exact parameters for steps 1-5, and that an override changes only the title steps.
- Table loader: rejects a malformed or old-version table and falls back to steps 1-2 only.
- Scan filter: in the ScummVM system, `.zip`/`.7z`/`.m3u` aren't listed, data folders aren't entered at any source depth, and a `*.scummvm/` folder is one game. Other systems are unchanged.
- Generator: `scripts/scummvm_games.py` on a fixture slice of `games.xml` and `--list-all-games` text gives a stable sorted TSV.

Device:
- Beneath a Steel Sky with the shipped file, with the ini entry removed, and with the file renamed `sky.scummvm`.
- A Mass-Added library of 5 games with variants.
- A folder with stray zips (the Q1 repro).
- ES-DE reading the gamelist and media afterwards.

### 3.10 Phases

0. **Measure** (needs the user's own ScreenScraper account; not done here).
   - For about 10 games (sky, queen, monkey, monkey2, tentacle, gk2, toltecs, an AGS freeware game, a Glk Infocom game, an AGI fan game), record what `jeuInfos` returns for `romnom` = `sky` / `sky.svm` / `sky.scummvm` / `Beneath a Steel Sky.scummvm`, with and without a hash.
   - Record what `jeuRecherche` returns on 123 and on 135.
   - Get the `.list` file from the device with the stray entries (Q1).
1. **Listing hygiene.** No `.zip`/`.7z`/`.m3u` for `{file.content}` systems. No recursion into data folders. ES-DE folder-as-file. This fixes the stray entries if Q1 confirms the cause.
2. **ID-aware queries, no bundled data.**
   - Read the target and the ini.
   - Query steps 1-3, using the ini `description` as the title.
   - Cache ScreenScraper ids and the 30-day not-found back-off.
   - Fall back to the description instead of the file stem.
   This gets most of the benefit for Thimsy's own installs.
3. **Bundled table.** `games.tsv` + generator + version check. Suffix stripping, stem-as-ID, canonical titles, TheGamesDB by title, the original-platform fallback, the optional "Show ScummVM titles". Beneath a Steel Sky in `pack-in-media.txt`.
4. **Optional.** Folder fingerprints (measure size first) and "Create ScummVM entries".

---

## 4. Risks and open questions

Risks:
- **ScreenScraper's matching rules are not documented.** The `<gameid>.svm` finding rests on public ROM tables and ES-DE's claim. If ScreenScraper insists on a hash match, steps 1 and 3 still work, and step 2 becomes one wasted not-found per new game. Phase 0 tells which.
- **Not-found quota.** A large AGS or Glk library can exhaust the daily KO allowance. The back-off and the "at most two step-2 calls" cap matter.
- **Target suffixes for non-AdvancedDetector engines** (SCUMM, Sky, Sword) are unverified. A wrong strip falls through to the name step, never to a wrong exact match, as long as only known table IDs are accepted.
- **Table drift.** A ScummVM APK bump without regenerating loses new IDs. The build check prevents that.
- **The ini is ScummVM's file.** Read it only, tolerate any content, and never write it from the scraper.

Questions for the user:
1. **Stray entries.** Can you share `/data/system/nano_xmb_cache/<scummvm system id>.list`, or the paths of "english", "language", "oak" and "zork0", from that install? Was the card from an earlier setup, and does the ScummVM system have an added folder with subfolders on?
2. **Unidentified files and titles.** Show a "?" for `.scummvm` files with no valid ID? Add "Show ScummVM titles" (canonical titles before scraping)?
3. **Games only in ScummVM's list.** Should Nano offer to create `.scummvm` files for games that are in `scummvm.ini` but have no file (Phase 4), or leave that to the user?
4. **Language.** Prefer synopsis and genre in the game's own language (from the ini) when it matches the UI language, or always English-first as today?
5. **ScreenScraper id map.** Learn ScreenScraper game ids on each device only (recommended), or also ship a map? A map would mean mass API calls and CC BY-NC-SA terms on the shipped file.
6. **Licence.** Is a GPL-licensed data file (`games.tsv`, generated from ScummVM's GPL lists) acceptable beside Apache-2.0 Nano, shipped as a separate file with its licence and credit? If not, the fallback is to ship nothing and rely on the ini `description` only (Phase 2).
7. **Scope of the table.** Ship only the 1,223 curated games (~60 KB), or all ~12k detectable IDs (~0.5 MB, mostly AGS/Glk fan games)?
