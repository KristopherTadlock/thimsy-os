# ScummVM Auto-Add Plan

Status: plan only (2026-11-27; revised the same day after two reviews). Nothing is implemented. No code was changed, nothing was built, and the Cube was not touched.

Owner's report: "I am not seeing the playlist for scummvm persist in the way it should when stored on sd card."

Trees and sources:
- Nano code: `patches/thimsy/0001-thimsy.patch`, cited as `P:<line>`.
- ScummVM 2026.3.0 sources (tag `v2026.3.0`), saved as data in `out/dp-evidence/test43/scummvm/src-v2026.3.0/`, with `/` in the path replaced by `_`. Cited as `SV:<file>:<line>`. Nothing in that folder was run. `common/config-manager.cpp` and `engines/metaengine.h` of the same tag were read on GitHub.
- scummvm_scraping_plan.md (the "top folder is the game" rules, §R).
- The owner's device state, read earlier from the Cube, read-only.

Labels: **[C]** read in source or a log; **[I]** inferred; **[U]** unverified.

This plan builds on scummvm_scraping_plan.md §R and changes two of its rules: Nano will list game **folders**, not only ScummVM's list, and Nano will **add game entries** to ScummVM's `scummvm.ini`. Its Q3 and Q9 and docs/ARCHITECTURE.md say "Nano never writes ScummVM's file" (`P:73814`). That is already not quite true: the BIOS worker force-stops ScummVM and writes one setting, `extrapath`, into the same file (`P:9834-9840`). **[C]** The change asked for in decision D1 is about adding game sections.

---

## 1. Summary for the owner

### What you see today, and why

- Your microSD card has `Roms/scummvm/` with more than 40 game folders. ScummVM's own game list (`scummvm.ini`) holds only Beneath a Steel Sky (`[sky]`).
- When the ScummVM app is the player, Nano does not look in any `ROMs/scummvm` folder at all. It shows exactly ScummVM's game list (`P:59307`: `buildScanCandidates` returns only Nano's mirror folder). **[C]**
- So none of your card's folders can show. Before Nano read ScummVM's list (2026-11-16), the system was scanned for `.scummvm`/`.svm` files (`P:18535`), which plain game folders don't have, so these folders most likely never showed in Nano. **[I]**
- Nothing adds them to ScummVM by itself. ScummVM adds games only through its own Add Game or Mass Add buttons. **[C]** (`SV:gui_launcher.cpp:291-301`)
- A second, separate problem can make games you added in ScummVM vanish later. If emulator files are kept on the card (the Move Emulator Files to microSD setting), Nano copies `scummvm.ini` to and from the card, and the card's copy wins a conflict, dropping games that are only in ScummVM's copy (§2.2). Whether this happened to you is **[U]**; step 0.3 of §16 checks.

### What you will see after this plan

In two stages:

- **After Phase 1 (listing, no owner decision needed):** every folder directly inside `ROMs/scummvm` (or `Roms`, `roms`, any case) on internal storage and on the microSD card shows in the ScummVM list, named after the folder ("Day Of The Tentacle (DOS CD)"), within one background scan (30 s). It stays as long as the folder is there; a replaced `scummvm.ini` can no longer empty the list. A folder that ScummVM doesn't know yet shows with a "?" and **does not start from Nano yet**: launching it explains why, shows the folder's path, and offers **Open ScummVM** so you can add it there (Add Game, then browse to that path).
- **After Phases 2 and 3 (identification and registration, need D1 and D2; about 3-6 weeks of work):** Nano identifies each new folder in the background with ScummVM's own detection and adds it to ScummVM's list, so the games start from Nano without you opening ScummVM. A folder Nano can't identify keeps its "?", and a list "ScummVM: Not Recognised" in the system's options shows all such folders and why.
- In both stages: games you added in ScummVM yourself stay, with their names, saves and settings. With the card out, its games are hidden, not deleted; they come back with the same names, favourites, Recently Played and art.

### Until then: a stop-gap you can use today

In ScummVM, press **Mass Add** and choose the card's `Roms/scummvm` folder. ScummVM adds every game it recognises, and today's Nano lists them within 30 s. To keep them:
- if ScummVM shows the card only through Android's folder picker, its entries get `/saf/` paths: they work, on ScummVM's own grant from the picker, but Nano couldn't write such entries itself later without a folder grant of its own, which bug B1 breaks on the Cube today (step 0.4 of §16 checks which kind you get);
- if emulator files are kept on the card, Nano copies the new list to the card afterwards, and a later conflict can still drop it until bug B6 is fixed (Phase 0). Keep a copy of `scummvm.ini` (step 0.1).

### What changes from "add games in ScummVM"

| Today | After |
|---|---|
| Only games in ScummVM's list show | Every top folder in `ROMs/scummvm` shows, plus ScummVM's list |
| You add each game in ScummVM | Nano adds it for you (Phase 3); ScummVM's Add Game still works and isn't duplicated |
| A replaced `scummvm.ini` empties the list | The list comes from the folders; Nano re-adds its entries |
| Nano writes only ScummVM's `extrapath` setting | Nano also adds game entries, append only, when ScummVM isn't running (decision D1) |
| No Add Folder for ScummVM | Add Folder is back, for extra game folders (decision D6) |

### What you need to decide first

D1 (may Nano add game entries to `scummvm.ini`?) and D2 (how Nano identifies games: a separate GPL helper program, or a built-in table). Phase 1 assumes the recommended answers to D3, D6 and D7 unless you say otherwise. The full list is in §18.

---

## 2. Findings

### 2.1 The list is ScummVM's list only, by design

- `nanoscummvm::listsGames` is true for romDir `scummvm` with the ScummVM app as player (`P:65360`; tests `P:162168-162171`). **[C]**
- Then `buildScanCandidates` returns `{mirrorDir(pkg)}` and nothing else (`P:59307`). **[C]**
- `syncScummvmList` (`P:59331`) reads `/data/data/<pkg>/files/scummvm.ini` and writes one `<title>.scummvm` file per section into `/data/system/nano_scummvm/<pkg>/scummvm/`, holding `<target>\n<gameid>\n`. It runs before every scan: at boot and every 30 s at the home screen. **[C]**
- The ini's `path=` key is read but not used. **[C]**
- The Scan Folders screen shows "Games come from ScummVM's game list" and has no Add Folder (`P:41438`). ROM-folder auto-add skips the system (`P:59194`). **[C]**
- On card and USB volumes, Nano's other folder lookups already find `Roms` by a case-insensitive match on every `/storage/<id>`, whatever the id's length (`findCaseInsensitive(base, "ROMs")`, `P:58849`). On internal storage they use exact `ROMs` (`P:58841`). Neither is used for ScummVM. **[C]**

### 2.2 Why games added in ScummVM can still vanish

The list mirrors `scummvm.ini`, so anything that replaces the ini empties the list within 30 s.

1. **Card copy wins.** NanoEmuSync keeps ScummVM's `files/scummvm.ini` and `shared_prefs` in step with `<card>/emulator_config/scummvm/app/files/scummvm.ini` (`P:17330-17620`, driver `P:67148-67560`). **[C]**
   - It runs only when emulator files are kept on the card: `persist.gammaos.nano.emu_sd=1`, set by Move Emulator Files or setup, or a RetroArch folder already on a card (`emuSdMode`, `P:67163-67174`; checks at `P:67670`, `P:67790`), and only for the adopted card. With the setting unset, causes 1 and 2 don't apply. **[C]**
   - When both copies changed, or the pair was never synced on this card, the card's copy wins (`decide()`, `P:17415`). **[C]** Exceptions: "This Device" chosen for a foreign card (`appWins`, `P:67327`, `P:67759`), and a never-synced pair that is identical after `repoint()` needs nothing (`P:67423-67427`). **[C]**
   - When the card wins, the whole file is replaced, so games that were only in ScummVM's copy are dropped (bug B6). **[C]** mechanism.
2. **Card identity is the mount index.** The Cube's vold names every public volume `00000000-0000-0000-0000-NNNNNNNNNNNN`, the lowest free index in mount order; a volume keeps its index across remounts, and the pool resets once no volume is left (linux_handheld_plan.md:751-752). Emusync keys its memory of what was synced by that name. **[C]** With one card and nothing else, the card is index 1 every time, so this bites only with a USB drive, a second volume on the card, or a volume that failed to mount taking an index **[I]**:
   - the same card at a new index looks new: never synced, the card's copy wins, or the "This microSD Card Has Emulator Files" question appears;
   - a different card at the same index looks like the same card: its different copy counts as "the card changed", and it replaces ScummVM's list without a question.
3. **ScummVM rewrites the whole file from memory.** It flushes its in-memory settings to `scummvm.ini` on every game launch (`SV:base_main.cpp:181-186`, `saveLastLaunchedTarget` → `flushToDisk`). Anything written to the file while a ScummVM process is alive is lost at its next flush. **[C]** for the flush, **[I]** for the loss.

Which of these hit you is **[U]**. Step 0.2 (the `emu_sd` settings) and step 0.3 (the log) of §16 tell.

### 2.3 ScummVM can't be asked to add games from outside

- The Android app takes **one** argument from its intent: `{"ScummVM", data.getSchemeSpecificPart()}` (`SV:backends_platform_android_org_scummvm_scummvm_ScummVMActivity.java:1106-1118`, and `onNewIntent` `1217-1221`). Extras are never read. The public entry, SplashActivity, forwards only action and data (`SV:..._SplashActivity.java:64-66`). **[C]**
- The command line is compiled in on Android (only n64 and dreamcast disable it, `SV:configure:3515`, `3776`), and `--add`, `--detect`, `--recursive` exist (`SV:base_commandLine.cpp:727-733`, `995`). But `--add` needs `--path` as a second argument (`2186-2189`), and one argument can't carry two (`607-615` matches the whole argument with `strcmp`). `scummvm:--add` alone scans the root node, a virtual drive list (`SV:backends_fs_android_android-fs-factory.cpp:40-51`), most likely adds nothing, and exits. **[C]** for the code path, **[I]** for "adds nothing".
- There is no service, content provider or broadcast receiver to call; ScummVMActivity is not exported. Mass Add runs only from the launcher button. **[C]**
- No intent can open ScummVM's Add Game browser either: the single argument is a target or an option. At most Nano opens ScummVM's launcher. **[C]**
- So Nano has to identify games itself and write the entries itself. This matches scummvm_scraping_plan.md R.1.

### 2.4 What an entry needs

- ScummVM's Mass Add writes `engineid`, `gameid`, `description`, `language`, `platform`, `path`, `extra`, `guioptions` and engine extras (`SV:base_plugins.cpp:736-760`, `createTargetForGame`). **[C]**
- To start, it needs `engineid` and `gameid` (`SV:base_main.cpp:125-170`). **[C]**
- ScummVM detects the game again on every launch: it reads `path`, scans the folder and picks the first match with the ini's `gameid`, using `language` and `platform` as filters, and `extra` only for engines with `kADFlagUseExtraAsHint` (`SV:engines_advancedDetector.cpp:360-440`). So `engineid` + `gameid` + `path` + `description` is enough to start; the variant is found at launch. **[C]**
- A wrong `gameid` gives ScummVM's "no game data found" error and a return to its launcher; the entry stays. A missing path (card out) gives "path does not exist"; the entry stays. **[C]** (`SV:base_main.cpp` runGame)
- ScummVM writes back every key of a domain, unknown ones included, and skips only keys with empty values (`common/config-manager.cpp` `writeDomain`). So unknown keys such as `thimsy_added` survive its flushes. **[C]** A section name with an invalid character, or a junk line, makes ScummVM stop loading the file (`warning()` and return false), and its next flush drops everything after it. **[C]** So Nano must only ever write valid names.
- ScummVM's run-time de-duplication compares the exact path. The `path=` Nano writes must byte-match what ScummVM's own file browser would return for that folder (§4.2), or a later Mass Add adds it again. **[C]** (`SV:gui_massadd.cpp:96-107`)

### 2.5 Can ScummVM read the card?

- ScummVM opens `/saf/...` paths through Android's storage access framework and every other path directly (`SV:backends_fs_android_android-fs-factory.cpp`, `android-saf-fs.cpp:264-290`). **[C]**
- On stock Android 14 it couldn't read shared storage by plain path (no all-files permission). On Thimsy, the patched MediaProvider lets apps read shared storage except denylisted folders on any volume (`P:183745+`, `P:169682-169705`). The shipped Beneath a Steel Sky entry already uses a plain internal path, `/storage/emulated/0/ROMs/scummvm/Beneath a Steel Sky` (`mods/46-scummvm-game.sh:89`). **[C]**
- Whether the same works for the card volume is **[U]**. ScummVM's own drive list comes from `/proc/mounts` lines starting `/dev/block/vold/` plus guesses in `findSdCardPath` (`SV:..._ExternalStorage.java:406-555`); whether `/storage/<36-character id>` appears there is **[I]**. If it doesn't, you would pick the card through Android's folder picker and get `/saf/` paths. Those work on ScummVM's own grant from the picker, but entries Nano writes for card folders would then need Nano to grant ScummVM the folder, which B1 currently breaks. Step 0.4 of §16 settles it.

### 2.6 Your card

- It is exFAT, serial `9602-C755`, mounted as `/storage/00000000-0000-0000-0000-000000000001` (linux_handheld_plan.md:751-752). **[C]** exFAT ignores case, so `Roms` and `ROMs` are the same folder there. The long id is vold's mount index, not a property of the filesystem. **[C]**
- The folder is `Roms/scummvm`, not `ROMs/scummvm`. Paths Nano writes must use the spelling on the card. **[C]**

---

## 3. Mechanism chosen

### 3.1 Options weighed

| Option | Result | Verdict |
|---|---|---|
| Ask ScummVM to add games (`--add` through the intent) | Impossible: one argument only (§2.3) | Rejected |
| A patched ScummVM APK that reads several arguments | Works, but needs re-signing and breaks upstream updates | Rejected; propose upstream later |
| Drive ScummVM's Mass Add screen by injected input | Fragile, visible to the user, breaks with any UI change | Rejected |
| Load the app's `libscummvm.so` inside Nano | GPL linking, private ABI, version coupling | Rejected |
| Compile ScummVM's detection into Nano | Makes Nano a GPLv3 combined work | Rejected |
| **Nano lists folders itself, and writes entries it identified** | Works without ScummVM's help | **Chosen** |
| Identify with a **separate GPL helper program**: a small driver built against ScummVM 2026.3.0 that calls its detection with Mass Add's arguments | The same detection results as Mass Add for one folder | **Chosen detector** (D2) |
| Identify with a **built-in table** made from ScummVM's detection data | Most AdvancedDetector engines, no extra program | Alternative if D2 says no helper |
| Name match against ScummVM's title list | Medium confidence only | Suggestion only, never written without asking |

### 3.2 The chosen design in one paragraph

Nano finds game folders in every `ROMs/scummvm` folder and lists each one, whether or not ScummVM knows it (§4). It matches folders to ScummVM's existing entries by path, so nothing is listed twice (§5). For each folder ScummVM doesn't know, Nano identifies the game in the background with ScummVM's own detection, run as a separate helper program, and caches the answer (§6). When no live ScummVM game could be lost, Nano appends the found entries to `scummvm.ini`, marked as its own (§9). Launching works as today, through the target. A folder that can't be identified stays listed, marked, and explains itself (§7).

### 3.3 Fallbacks, in order

1. The helper is missing, fails, or times out → the built-in table, if shipped (D2).
2. Nothing identifies the folder → listed with "?". Launch shows the folder's path and **Open ScummVM**, which starts ScummVM's launcher; you press Add Game and browse there. No ini write.
3. With D1 = yes only: Nano also seeds ScummVM's `browser_lastpath` to that folder, under the §9.1 write conditions, so the Add Game browser starts there. Android sets the key to `/` only when it is missing (`SV:backends_platform_android_android.cpp:406-407`). **[C]** for the key, **[I]** that a seeded value opens there.
4. A folder-name match against ScummVM's titles (scummvm_scraping_plan.md §R.4) is offered as a suggestion in "ScummVM: Not Recognised" ("Is this Day of the Tentacle?"). It is written only after the owner confirms (needs D1).
5. If D1 says Nano may not write `scummvm.ini`: Phase 1 still lists every folder and keeps them listed; unregistered ones use fallback 2.

---

## 4. Finding game folders

### 4.1 Where Nano looks

- **Internal storage:** `/storage/emulated/0/<roms>/<scummvm>`, where `<roms>` and `<scummvm>` match `ROMs` and `scummvm` ignoring case. Today's code tries only exact `ROMs` on internal storage; this lookup must ignore case too.
- **Each microSD card and USB drive:** chosen the way `sdCardRoot` chooses volumes (`/storage/<id>` with `/mnt/media_rw/<id>` present, not `emulated`, `self` or network shares), with the same lookups as the generic code: `<base>/<roms>/<scummvm>` and `<base>/<scummvm>`, ignoring case. Any id format, the Cube's 36-character form included. Your `Roms/scummvm` is found.
- **Folders you add** (decision D6): Add Folder comes back on the ScummVM Scan Folders screen. A folder you add is a source of game folders, like `ROMs/scummvm`. Network shares are refused: ScummVM can't read `/mnt/shares` **[I]**.
- **ES-DE store mode:** the store's ROM directory plus `scummvm`.

If you have added your own sources, they replace the default folders, as for every other system.

### 4.2 Paths as ScummVM sees them

Nano writes and compares paths in ScummVM's form, byte for byte what ScummVM's POSIX file node returns (§2.4):
- internal: `/storage/emulated/0/...`, never `/sdcard` or `/data/media/0`;
- cards: `/storage/<id>/...`, never `/mnt/media_rw/<id>`;
- the folder names as spelled on disk (`Roms`, not `ROMs`);
- no trailing `/`.

Two spellings of the same folder (`/storage/<id>` and `/mnt/media_rw/<id>`) are one source.

### 4.3 What counts as a game

The rules of scummvm_scraping_plan.md §R.2, unchanged. Nano reads one directory per source, direct children only:

| Entry | Becomes |
|---|---|
| A folder | One game, named after the folder |
| `X.scummvm` / `X.svm` file | One game; its first line is the target, as today |
| A file and a folder with the same stem (the shipped `Beneath a Steel Sky.scummvm` + `Beneath a Steel Sky/`) | One game |
| An ES-DE folder-as-file (`Monkey Island.scummvm/`) | One game, named without the extension |
| Any other file | Not a game |
| `.`-names, `Android`, `LOST.DIR` | Skipped |

Nothing inside a game folder is ever listed. Grouping folders (`LucasArts/Monkey Island/`) are one game by default (D7); scummvm_scraping_plan.md Q8 still applies.

### 4.4 Cost

One `readdir` per source every 30 s, on the background scan thread. Negligible, even with hundreds of folders. Identification (§6) runs only for new or changed folders, within the budget of §6.4.

---

## 5. Matching folders to ScummVM's list

### 5.1 Normalising paths

`normalize(p)`:
- drops a trailing `/`;
- maps `/sdcard`, `/storage/self/primary`, `/data/media/0` to `/storage/emulated/0`;
- maps `/mnt/media_rw/<v>` to `/storage/<v>`;
- decodes `/saf/<tree>/<rest>` to `/storage/<vol>/<rel>/<rest>` (emusync already decodes tree URIs);
- compares ignoring case on cards (exFAT, FAT). Internal storage: decision on case pending (Q3).

### 5.2 Rules

- A folder F is **registered** when some `scummvm.ini` section's normalised `path` equals F or lies inside F. Mass Add often stores a sub-folder (`.../Game/CD`). **[C]** (`SV:gui_massadd.cpp:99-107`, `219-230`, `257-263`)
- Several sections inside one folder (CD and floppy, language variants): one list entry per section, named as in §12.3. Add-ons (`-addon`) are not listed on their own.
- A section whose path is in none of the sources (a game added in ScummVM from elsewhere) is listed as today, named after its description.
- A folder with no section is listed as **unregistered** until §6 and §9 register it.

### 5.3 A card at a new index

A section whose path is on `/storage/<A>/rel`, where volume A is absent and `/storage/<B>/rel` exists on a present volume: Nano treats the folder on B as registered and, at its next safe write, changes only that section's `path=` line to B. This covers the Cube's index changes, which emusync's `repoint()` doesn't (it only repoints `emulator_config` paths, and only for ids it recognises; bug B2). For your own entries (not added by Nano), this rewrite is decision D5.

### 5.4 A renamed or moved folder

Without a rule, a renamed folder, or a game moved from internal storage to the card, would leave its old section pointing nowhere, get a second target (`tentacle-1`, because `tentacle` is taken), lose its saves as far as you can see (many engines name saves by target **[I]**), and get a new list name, breaking its favourite and Recently Played.

Rule: when a Nano-added section's path is missing (its volume present, or another volume holding the game) and a new unregistered folder detects as the same `engineid`, `gameid`, `language` and `platform`, and exactly one such pair exists, Nano changes only that section's `path=` to the new folder and keeps its target. The list entry keeps its old name (§12.2). Two or more candidates: nothing is changed, and both show in "ScummVM: Not Recognised" as "Possibly moved from <old path>". Your own sections are repointed this way only with D5.

---

## 6. Detection

### 6.1 The helper (recommended; decision D2)

- **What:** `thimsy-scummvm-detect`, a small GPLv3 command-line driver linked against ScummVM 2026.3.0's detection code. It does not use ScummVM's `--add`, because `--add` differs from Mass Add **[C]**:
  - `--add` calls `detectGames(files)` with the defaults, no skipped flags and incomplete matches kept (`SV:base_commandLine.cpp:1653`; `engines/metaengine.h`), so it also adds unsupported and warning variants (which then fail at launch, `SV:engines_advancedDetector.cpp:1210`), add-ons as their own targets, and incomplete matches;
  - Mass Add calls `detectGames(files, ADGF_WARNING | ADGF_UNSUPPORTED | ADGF_ADDON, true)` (`SV:gui_massadd.cpp:202`);
  - `--add` skips a game whose preferred target already exists (`SV:base_commandLine.cpp:1720-1723`), so within one recursive run a multi-CD game's later CD folders are dropped; Mass Add de-duplicates by path and keys instead.
- **What the driver does:** for one game folder, list the folder and call `EngineMan.detectGames(files, ADGF_WARNING | ADGF_UNSUPPORTED | ADGF_ADDON, true)`, exactly as Mass Add does; print each recognised game's keys (what `createTargetForGame` would write, `SV:base_plugins.cpp:736-760`) and its preferred target. If nothing is found at the top, walk sub-folders (depth 4) and keep only the shallowest level where games were found. This last rule departs from Mass Add on purpose (Mass Add adds every level, `SV:gui_massadd.cpp:257-263`), so a CD sub-folder doesn't become a second game. Within the kept level, results are kept as Mass Add keeps them. A step-0.5 Mass Add of your card is then a valid test oracle for every folder whose games sit at one level.
- **Engines, and size:** some engines detect unknown variants on the engine side (`fallbackDetectExtern`, `SV:engines_advancedDetector.h:671-687`, "only meant to be used if fallback detection heavily depends on engine resources"; Wintermute is the example given, SCI is **[I]**). That needs the engine itself. So the helper is built with the release's engines enabled, which makes it about as large as the APK's `libscummvm.so` (tens of MB **[U]**), not a small detection-only program. A detection-only build (engines disabled, full detection on, `SV:configure:289`) would be smaller but lose those fallbacks; it is the fallback plan if size matters more. Either way the helper answers only with engines the shipped APK contains: an allowlist made from the release's configuration filters `engineid`. Whether unstable engines such as Dungeon Master (`dm`) are in the 2026.3.0 release is **[U]**.
- **Version lock:** the build checks that the helper's version equals the APK's (`mods/46-scummvm-game.sh` already reads it from the APK name). Nano checks the helper's `--version` at run time and turns it off on a mismatch.
- **How it's built:** outside AOSP, in the Docker build image with the NDK, as a prebuilt installed to `/system/bin/`. Not a configure flag away: with `--host=android-*`, ScummVM's configure forces the Android backend, `-shared` and a `.so` output (`SV:configure:3687-3696`). **[C]** A null-backend executable needs a non-`android-*` triple with the NDK's clang (for example `aarch64-linux-android`, which takes the generic Linux path) or a configure patch. Whether the generic path builds as is, and the effort, are **[U]**.
- **How it runs:** never as root, never as ScummVM's uid (that would give a process parsing untrusted files write access to ScummVM's settings and saves). Preferred: an isolated uid with read-only access to the game folders, no network, its own SELinux domain entered by an explicit transition from Nano, a time limit per folder (20 s) and a cap on output size. Whether a package-less isolated uid can read `/storage/<id>` through FUSE, or must read the card's lower filesystem, is **[U]**.
- **Licence [I, not legal advice]:** a separate program Nano starts and talks to only through arguments, files and stdout is "mere aggregation" under the GPL FAQ, so Nano stays Apache-2.0. Thimsy must ship the helper's complete source (tag, driver, build script, patches) and GPLv3 notices in ATTRIBUTION.md. Thimsy already ships the GPL ScummVM APK, so a source offer is owed anyway.

### 6.2 The built-in table (alternative)

- Built at Thimsy build time from `scummvm --dump-all-detection-entries` of the same tag (a host build): per entry, file names, sizes and an MD5 over a number of bytes that each engine sets (default 5000, `SV:engines_advancedDetector.cpp:1005`; the dump prints `md5-<N>` per entry, `:717`). The table stores N per entry, and tail MD5s where the dump has them. **[C]**
- Nano walks the folder (depth 4, names folded to lower case, ScummVM's punycode for odd names), hashes only files whose names are in the table, and requires every file of an entry to match. Milliseconds per folder. **[I]**
- Size: about 40-70 thousand file rows; 1-1.5 MB compressed. **[I]**
- Misses: fallback detectors (unknown AGS games, engine-side fallbacks), Glk by extension, Mac resource forks, engines whose dump is empty (Sky's is reported empty: it detects by `sky.dnr`'s entry count and `sky.dsk`'s size), and multi-CD layouts. **[I]** (Sky's detection source is not in the saved set.)
- Shipped as a separate GPLv3 data file with credit. Nano reading data does not make Nano a derived work. **[I]**

### 6.3 What Nano writes for a found game

- With the helper: the keys the helper printed, plus `thimsy_added=1`, under a target name Nano chooses against the real ini (the helper doesn't see it, so it always reports preferred targets).
- With the table: `engineid`, `gameid`, `description` and `path`; plus `language`, `platform` and `extra` when exactly one entry matched; plus `thimsy_added=1`.
- **Target name:** ScummVM's rule (`SV:base_plugins.cpp:727-734`, `generateUniqueDomain`), applied to the real ini at write time: the preferred target, then `-1`, `-2`... It must match `[A-Za-z0-9_-]`, 1-64 characters, no leading `-` (Nano's `validId`).
- **Path:** the folder in ScummVM's form (§4.2), or the sub-folder the helper reported.

### 6.4 Cache and timing

- Results are cached in `/data/system/nano_scummvm/<pkg>/detect.txt`, keyed by normalised path, the folder's mtime and a hash of its top-level listing. A folder is identified again only when it changes.
- Identification runs on the background scan thread, one helper run at a time, with a budget of 10 s per 30 s pass at the home screen and none while a game runs. A folder that takes 20 s (the per-folder limit) uses that pass and the next.
- Worst case: every folder hitting the limit, 40 folders take about 40 passes (20 minutes); a typical folder takes well under a second **[I]**, so your 40 folders should take a few minutes. Hundreds of folders work the same way, only longer; the folders show at once and become launchable as they are identified, newest first.
- A folder that timed out or failed is retried with a back-off (next pass, then 10 minutes, 1 hour, 1 day), and not again until it changes after three failures.
- "Identify Again" in a game's options clears its cache entry and back-off.

---

## 7. Wrong and unknown folders

### 7.1 Unknown folders: shown, flagged, listed

- **Shown:** an unidentified folder stays in the ScummVM list under its folder name (decision D3; recommended).
- **Flagged:** a small "?" on its entry, and an Info line: "ScummVM doesn't recognise this folder yet".
- **Launch:** a dialog with the reason, the folder's path, and three choices:
  - **Open ScummVM**: starts ScummVM's launcher; press Add Game and browse to the path shown (with D1, the browser starts there **[I]**; §3.3);
  - **Identify Again**;
  - **Hide Game** (Nano's usual hide).
- **"ScummVM: Not Recognised" list** (in the system's options, next to Scan Folders): every unidentified folder with its reason:
  - "No ScummVM game found in this folder";
  - "Found <engine>:<game>, which this ScummVM can't play" (an engine not in the APK);
  - "Can't read this folder";
  - "Identification timed out";
  - "Possibly moved from <old path>" (§5.4).
  Each row offers Identify Again, Open ScummVM, and a title suggestion when the name matches (§3.3, fallback 4).
- Once you add the game in ScummVM yourself, its section matches the folder by path and the "?" goes away at the next scan.

### 7.2 Wrong identification

- ScummVM checks the game again at every launch (§2.4). A wrong `gameid` can't start the wrong game; it shows ScummVM's "no game data found" message. **[C]**
- Nano can't see that message. For entries Nano added, the game's options get **Forget ScummVM Entry** (removes the Nano section; the folder stays listed, unregistered) and **Identify Again**.
- A helper result is ScummVM's own detection with Mass Add's arguments, so a wrong answer would be wrong in Mass Add too. A table result is written only on a full match of every file of one entry; several matching entries of different games count as unknown.

### 7.3 Expected results on your folders [I]

| Folder | Expected engine:game | Notes |
|---|---|---|
| 7th Guest, The (DOS) | groovie:t7g | |
| Blade Runner (CD Windows) | bladerunner | All CDs' files must be in one folder; see Q7 |
| Day Of The Tentacle (DOS CD) | scumm:tentacle | SCUMM's own detector |
| Gabriel Knight - Sins Of The Fathers (DOS CD) | sci:gk1 | |
| King's Quest 3 - To Heir is Human AGD Interactive 2011 remake | ags:kq3agdi | §8 |
| Eye of the Beholder (Amiga) | kyra:eob | |
| Dungeon Master (Amiga) | dm | **[U]** likely not in the release build: listed as "can't play" |
| Indiana Jones and the Call of Thunder (ScummGEN) | **[U]** | Possibly not a ScummVM game: listed as unknown |

A Mass Add of your card in ScummVM (§16, step 0.5) gives the real answers and becomes the test's expected results.

---

## 8. AGS games

- AGS is a ScummVM engine in the app. Detection has three steps, all in the detection code (`SV:engines_ags_detection.cpp:74-195`): **[C]**
  1. the MD5 of the first 5000 bytes plus the size of the game's `.exe`, `.ags` or `ac2game.dat`, against about 5,400 known entries;
  2. any such file with the AGS signature, looked up in the table under another file name;
  3. an unknown AGS game becomes `gameid=ags` with the file name as `extra`, target `<id>-fallback` (`:117`). The engine allows unknown variants (`kADFlagCanPlayUnknownVariants`, `:74-76`), so it still plays.
- Your "King's Quest 3 - To Heir is Human AGD Interactive 2011 remake" should be `ags:kq3agdi` ("King's Quest III Redux"): `kq3redux.exe`, MD5 `4c2ea3f7b4974509c59546ca8761b040` / 11890197 bytes (v1.0) or `e569fb2ceabdc4a1609348c23ebc0821` / 11986266 (v1.1) (`SV:engines_ags_detection_tables.h:454`, `5675-5676`). **[C]**
- The helper handles all three AGS steps, because AGS's fallback is in its detection code, not the engine. With Mass Add's arguments step 2's incomplete matches are skipped, and step 3 then catches the game **[I]**. The table handles step 1 only, so unknown AGS fan games stay "Not Recognised" without the helper. **[I]**
- AGS games in `ROMs/ags` (the separate AGS system) are unchanged: that system still scans files (`P:162170`).

---

## 9. Writing `scummvm.ini` safely

### 9.1 When Nano writes

All on the background scan thread. Nano skips the round, and tries again at the next 30 s scan, unless every condition holds:
1. boot is complete and at least 60 s old (cards and partial mounts settle), and no emusync question for this card ("This microSD Card Has Emulator Files") is waiting for an answer, and emusync has finished its first pass on an adopted card since boot;
2. no `org.scummvm.scummvm` process exists (`sdPkgRunning`, `P:65883`). ScummVM's `onDestroy` stops its game thread, which flushes, but leaves the process cached (`SV:..._ScummVMActivity.java:1273-1308`), so Android may keep it for hours. At the home screen, Nano force-stops ScummVM when it has no activity left (a cached, empty process: its game thread has ended and flushed), as the BIOS worker already does before writing `extrapath` (`P:9834-9840`). It never force-stops a ScummVM that still has an activity, since you may have pressed Home in the middle of a game; then the write waits **[I]** for how to tell the two apart (the activity stack in `dumpsys activity`);
3. no app launch is in progress (`sys.gammaos.nano.app_launched` is false);
4. the BIOS worker isn't busy (it writes `extrapath` into the same file);
5. no Move Emulator Files is running (the move lock, shared);
6. the emusync locks are free: its in-process mutex (to be exposed from NanoSdMove.cpp) and the `/data/system/nano_emu_sync.lock` file lock, which cover both Nano processes;
7. the ini is settled (mtime at least 3 s old) and complete (it has a `[scummvm]` section).

**No settings file yet** (first boot without the shipped Beneath a Steel Sky: `mods/46-scummvm-game.sh` adds the file only when the ROM pack has the game, lines 66-86): background registration waits until ScummVM has made its own file, and Nano never creates it in the background. At a launch (§9.3), Nano creates a minimal file: a `[scummvm]` section with `versioninfo` and the game's section. ScummVM fills every missing setting with its default at start (`android.cpp:406-407` is one such case) **[I]**; the Phase 3 test checks it.

### 9.2 How Nano writes

- Re-read the ini under the locks.
- Edit it as text: append new sections at the end, change `path=` lines in place (§5.3, §5.4), keep every other byte unchanged. Never reorder, never rewrite other sections.
- Validate: every new section name valid, no duplicate names, no line breaks in values.
- Keep one backup, `scummvm.ini.nano-bak`, beside it.
- Write with `nanobios::appFileWrite` (temporary file and rename, the app's owner, mode and SELinux label), as emusync already does.
- Remember the hash of what was written, so the next pass can tell Nano's writes from others.

### 9.3 Just before launching an unregistered folder

At the hand-off where `emuSettingsSyncLaunch` runs: identify the folder now (with a spinner, within the time limit), write the entry under the same locks, update the mirror file's first line, then launch. The three launch sites (the XMB, Recently Played, the overlay) call this before `launchFileIdFor` refuses an empty target.

ScummVM must not be running for the write. If it has no activity, Nano force-stops it as in §9.1. If it still has an activity (a game left in the background), force-stopping would lose that game's unsaved progress, so Nano asks first: "ScummVM is still running a game. Close it and lose unsaved progress?" Saying no cancels the launch.

### 9.4 NanoEmuSync

- Nano's write is an ordinary "app side changed" edit, so emusync's next pass copies it to the card (`ToCard`). **[C]**
- If the card wins a conflict and drops Nano's sections, the folders stay listed (§4), and Nano adds the same sections again at its next safe pass. Writes are idempotent, keyed by path, so this settles in one cycle. **[I]**
- Nano doesn't write in the same pass as an emusync card-to-app copy of `scummvm.ini`. Emusync records when it replaced the file from the card, so Nano doesn't read the missing sections as "the user removed them" (§10).
- Your own sections are protected by the B6 fix (Phase 0), not by this plan's re-adding: when the card's copy would win for `scummvm.ini` while the app's copy also changed or was never synced, emusync merges by section instead (the card's file, plus every game section only the app's copy has). A game you removed on one side may then come back once; that is the safe side.

---

## 10. Keeping manual additions

- Sections you added in ScummVM are never removed or changed by Nano. The exceptions, only if you agree (D5), are the `path=` repoints of §5.3 and §5.4.
- Your existing sections keep their list names. A folder entry is named after the folder; for a folder that already had a description-named entry, the existing file name is kept, so favourites, Recently Played, collections and art keep working (§12).
- **You remove a Nano-added game in ScummVM** (Remove Game): Nano must not add it back. It sees "section gone, folder still there, and the last writer was neither Nano nor an emusync card copy", and records the folder in `/data/system/nano_scummvm/<pkg>/dismissed.txt`. The folder stays listed as unregistered, with **Add to ScummVM Again** in its options. Whether ScummVM's Edit Game dialog keeps the unknown `thimsy_added` key is **[U]** (its flushes do keep it, §2.4); if the dialog drops it, the section counts as yours from then on, which is the safe side.
- ScummVM's own Mass Add won't duplicate a Nano entry when the keys are complete and the path is byte-identical: it skips games whose path, engine, game, language and platform are already listed (`SV:gui_massadd.cpp:96-107`, `219-245`). **[C]** Table entries with fewer keys may be duplicated by a later Mass Add **[I]**; the path match (§5.2) still shows one entry per section, so a duplicate shows twice until removed.

---

## 11. Removed folders and the card out and in

- **Card out:** nothing in the ini changes. Its games are hidden from the list, as for other systems. Their mirror names are kept (§12), so they come back unchanged.
- **Card in again:** its folders show again within one scan. If the card came back at another index, §5.3 repoints Nano's sections.
- **A folder deleted on a present volume:** hidden at once. Nano's own section for it is removed only by **Clean Up Missing ScummVM Games** (an action in the system's options), never on its own (decision D4; recommended). Saves stay: ScummVM keeps them by target name, and removing a section doesn't delete them. **[I]**
- **A folder renamed or moved:** §5.4.
- **Your own sections for missing folders:** listed as today (ScummVM shows its own error at launch). Clean Up offers them too, unticked by default.
- **A different card at the same index:** its folders are listed; Nano's sections for the other card's folders stay but are hidden (their paths don't exist). Fixing card identity (bug B3) makes this exact.

---

## 12. Mirror files and names

The mirror folder stays (`/data/system/nano_scummvm/<pkg>/scummvm/`), so the scan cache, favourites, Recently Played, collections, hidden games, scrape data and ES-DE store mode keep working unchanged.

### 12.1 Format (backward compatible)

- line 1: the target, or empty while unregistered;
- line 2: the game id, or empty;
- line 3 (new): the folder path, in ScummVM's form.

`readLaunchFileId` and `mirrorGameId` read lines 1 and 2 as today.

### 12.2 Names

- A folder entry is named after the folder, whether registered or not, so registering never renames it. The shipped `Beneath a Steel Sky` keeps its current name (its description without "(CD/DOS/English)" equals the folder name).
- An ini-only entry keeps its description name.
- An existing file name is kept while it still fits (today's rule, extended to folder names), and a section repointed by §5.3 or §5.4 keeps its entry's name.
- Names are worked out over all entries, present and absent, and only present ones are written. A game that comes back gets its old name.

### 12.3 Two entries with the same name

The mirror is one folder, so names must be unique. Collisions, in order of the suffix added:
- **The same folder name in two sources** (the card also holds `Beneath a Steel Sky` next to the shipped internal copy): the internal one keeps the plain name; a card copy becomes "Beneath a Steel Sky (microSD)", a USB copy "(USB)", a folder from an added source "(<source folder name>)".
- **Several sections from one folder** (CD and floppy, several languages): the first section (by its order in the ini) keeps the folder's name; the others add what tells them apart: platform, then language, then `extra` ("Day Of The Tentacle (DOS CD) (German)"), and a number when nothing does.
- Names, once given, stay with their entry (§12.2), so a second copy appearing later never takes the first one's name.

### 12.4 Why it matters

Every link to a game keys on the mirror file's path. A rename breaks its favourite, Recently Played, collection, scrape and art links (nano report §1.6). **[C]**

---

## 13. RetroArch's ScummVM core as the player

Everything above is for the ScummVM app. With RetroArch's ScummVM core as the system's player, `listsGames` is false (`P:162171`) and the system does a normal file scan for `.scummvm`/`.svm` files (`P:18535`; docs/ARCHITECTURE at `P:73813`). Your 40 folders have no such files, so nothing useful lists then either. **[C]**

The core reads a `.scummvm` file and plays the game in the folder it sits in, which is how the shipped Beneath a Steel Sky works (`mods/46-scummvm-game.sh:3-7`). **[C]** for the shipped layout; **[I]** for the core's exact rules (a game id in the file, or detection in the file's folder) and whether it accepts a folder directly **[U]**.

Phase 1's folder listing could apply with the core too, launching each folder through a hook file Nano generates (in its own data folder, never in your game folder) that names the folder and, when known, the game id. That needs the core's rules confirmed and your decision D9. Until then the core keeps today's file scan.

---

## 14. Bugs found

| # | Bug | Evidence | Severity | Fix |
|---|---|---|---|---|
| B1 | **Folder grants to the card can't be sent with a 36-character volume id.** `sdUriCommand` refuses when `"<pkg>\|<volume>\|<rel>"` plus 25 reaches 92 characters (`PROPERTY_VALUE_MAX`). Logged for Move Emulator Files to microSD for Azahar, whose move then stops (`P:68041`). By the same arithmetic it also fails for SeedlessDS (82 + 25 = 107, `P:68054`), emusync's re-grants for the ScummVM, hakuX and WatermelonDS card folders, and hakuX's card `ROMs/xbox` (exactly 92). | `P:66701-66706`; device log `out/dp-evidence/dock-tv-run/logcat.txt:14655` and 20 more lines, all "uri_grant for org.azahar_emu.azahar too long to send (volume 00000000-0000-0000-0000-000000000001)" **[C]** for Azahar; **[I]** for the others (length arithmetic, not logged) | **High**: a user-visible feature fails on every Cube with a card | Carry the argument outside the property (a root-written file named by the sequence number, or a short volume alias). A system_server (NanoNetBridge) change. |
| B2 | **`emusync::isVolumeId` rejects the Cube's ids** (it accepts only 9 or 32 characters), and `repoint()` stops reading an id after 32 characters. So emulator settings that name a card at an old index are never repointed. | `P:17466-17475`, `P:17486` (`end - at < 32`); tests `P:158288-158293` **[C]** | **Medium**: dormant until the card's index changes, then emulator paths (RetroArch, DraStic, PPSSPP, Flycast, Dolphin, hakuX, ScummVM's savepath) point at a missing volume | Also accept 8-4-4-4-12 hex; read up to 36 characters; unit tests with the Cube form. Small. |
| B6 | **A card-wins sync drops your ScummVM games.** For `scummvm.ini`, when the card's copy wins (both changed, or never synced on this card), the whole file is replaced, so game sections only in ScummVM's copy are lost and leave Nano's list. | `decide()` `P:17415`; driver `P:67415-67430` **[C]** mechanism; **[I]** that it caused your `[sky]`-only list (Q5) | **Medium**: loses your own entries, only with emulator files kept on the card | For `scummvm.ini` only, a card win while the app's copy also changed or has no base becomes a merge: the card's file plus every game section only the app's copy has. Host tests. 1-2 d. |
| B3 | **Card identity is the mount index on the Cube.** Emusync's base and `persist.gammaos.nano.emu_sd_card` key on it, so the same card at a new index looks new, and another card at the same index looks the same. Either can make the card's `scummvm.ini` replace ScummVM's list. | NanoSdMove.cpp card id = folder name (nano report §3.3); linux_handheld_plan.md:751-752, decision 20 **[C]** mechanism, **[I]** effect | **Low-Medium**: possible, not likely: one card alone is index 1 every time; it bites with a USB drive or a second volume | A stable card id: a random id in `<card>/emulator_config/.thimsy-card` written at adoption, or the exFAT serial read as root. Key emusync on it; keep the vold id as the mount path only. |
| B4 | **Paths Nano writes into ScummVM's ini name the index id.** They break when the index changes. | §5.3 **[I]** | Low, once §5.3 exists | §5.3 repoint now; B3 makes it rarer. |
| B5 | Cosmetic: the folder browser labels the card "SD: 00000000-0000-0000-0000-000000000001". | NanoMenuPS3Folder.cpp (nano report §3.4) **[I]** (not re-checked) | Low | Show "microSD card". |
| B7 | Docs: docs/ARCHITECTURE.md says Nano never writes ScummVM's ini, but the BIOS worker writes `extrapath`. | `P:73814` vs `P:9834-9840` **[C]** | Low | Fixed by Phase 3's doc rewrite. |

B1, B2 and B6 are separate from the ScummVM listing and can ship first (Phase 0).

---

## 15. Host unit tests

In `NanoScummvmList_test.cpp` (`P:162068`) unless noted.

- **Sources:** `ROMs`, `Roms`, `roms` and `ROMS` on a card; internal `Roms`; `/storage/<id>` and `/mnt/media_rw/<id>` as one source; the 36-character id, `XXXX-XXXX` and 32-hex ids; a USB drive; user sources replacing the defaults; network shares refused.
- **Game rules (§4.3):** folders, `.scummvm` files, a file + folder pair as one game, an ES-DE folder-as-file, skipped names, nothing listed from inside a game folder.
- **Matching (§5):** `/sdcard` against `/storage/emulated/0`; trailing `/`; card case; a section in a sub-folder; two sections in one folder; an add-on; a `/saf/` path; a section outside every source.
- **Card at a new index:** the section on absent A, the folder on present B → registered, `path=` rewrite planned only when allowed (D5).
- **Renamed or moved folder (§5.4):** one candidate repointed with its target and name kept; two candidates left alone and flagged; your own section repointed only with D5.
- **Ini editor:** appends at the end; changes `path=` in place; keeps comments, unknown keys, CRLF, BOM and order byte for byte; refuses invalid names; unique `-1`/`-2` suffixes against the real ini; never writes a duplicate section; round-trips a real ScummVM file; creates the minimal file of §9.1 only on the launch path.
- **Helper output parser:** driver output (fixture text, not a run), several games, the shallowest-level rule, an engine outside the allowlist, a version mismatch, an empty result, oversized output.
- **Table matcher** (if D2 picks it): full match, a missing file, a size mismatch, `AD_NO_SIZE`, an `md5-<N>` other than 5000, a tail MD5, two entries matching (unknown), punycode names.
- **Names (§12):** folder names stable across registration; the shipped Beneath a Steel Sky name unchanged; an absent card's games keep their names on return; existing description names kept; the same folder name on internal and the card; two sections from one folder; a later copy never takes an earlier name.
- **Dismissed and clean-up:** a removed Nano section not re-added; Add to ScummVM Again; Clean Up lists only missing folders, yours unticked.
- **Write conditions:** each of §9.1's seven conditions blocks the write (a fake process list, activity state and clock); a cached ScummVM with no activity is force-stopped, one with an activity is not.
- **Back-off (§6.4):** a timed-out folder is retried on the schedule and stops after three failures until it changes.
- **Emusync** (`NanoEmuSync` tests, `P:158288` area): `isVolumeId` with `00000000-0000-0000-0000-000000000001`; `repoint()` over a 36-character id; B6's section merge (app-only sections kept, the card's main section and shared sections taken, other files unchanged); the stable card id once B3 lands.
- **B1:** an argument longer than the property limit is sent the new way.

Host tests are built with g++ by `tests/unit/test_nano_units.py`.

---

## 16. On-device steps (with your card)

Respect the Cube hazards (no dwc3 debugfs, no dispc1 sysfs, no stop/start). Steps 0.1-0.3 only read. Steps 0.4 and 0.5 are your own use of ScummVM and **change** `scummvm.ini` (Add Game and Mass Add write it).

**Phase 0, before any code:**
1. Back up ScummVM's settings: `adb shell su -c 'cat /data/data/org.scummvm.scummvm/files/scummvm.ini' > scummvm.ini.phone` and `adb shell cat "/storage/00000000-0000-0000-0000-000000000001/emulator_config/scummvm/app/files/scummvm.ini" > scummvm.ini.card`.
2. `adb shell mount | grep 00000000` (filesystem and options) and `adb shell getprop persist.gammaos.nano.emu_sd`, `persist.gammaos.nano.emu_sd_card` and `persist.gammaos.nano.emu_sd_app_wins`. With `emu_sd` unset, emusync never ran and §2.2's causes 1 and 2 are ruled out.
3. `adb logcat -d | grep -iE "emusync|scummvm|foreign|too long"` (no tag filter: emusync's driver and sdmove log under `NanoMenu`, `P:65689`, and NanoEmuSync.cpp under `GammaOSNano`, `P:17356`). Look for "emusync: scummvm files/scummvm.ini from the card" (the card copy replaced ScummVM's list) and the foreign-card question. `logcat -d` keeps only recent history, so an old event has likely rotated out; nothing found doesn't rule it out.
4. In ScummVM: Add Game. Check whether the card appears in ScummVM's drive list by its `/storage/00000000-...` path (§2.5). Browse to `.../Roms/scummvm/Day Of The Tentacle (DOS CD)` and add it. If ScummVM lists and adds it with a plain path, plain card paths work. If you had to use Android's folder picker, note the `path=` it writes (`/saf/...`): that answers Q1.
5. In ScummVM: Mass Add on `Roms/scummvm` on the card. Save the resulting `scummvm.ini`: it is the expected answer for every folder (§7.3) and the fixture for the parser tests. It is also the stop-gap of §1.
6. Tell us whether you ever Mass Added these games before (Q5).

**After Phase 1 (listing):**
- All top folders of the card's `Roms/scummvm` and internal `ROMs/scummvm` show, Beneath a Steel Sky once, no data files.
- Card out: its games hidden within 30 s. Card in: back, same names, favourites and Recently Played intact.
- Replace ScummVM's `scummvm.ini` with a `[sky]`-only one: the folders stay listed.

**After Phase 3 (registration):** first restore the step-0.1 backup (the `[sky]`-only ini), or the step 0.4/0.5 sections hide what Nano adds.
- From an ini with only `[sky]`, within a few minutes every identifiable folder has a section marked `thimsy_added=1`, and starting it from Nano opens the game.
- Launch an unregistered folder at once after inserting the card: spinner, then the game.
- ScummVM quit and cached: Nano force-stops it and writes. ScummVM with a game left in the background: no write, and a launch of an unregistered folder asks first.
- Remove a Nano-added game in ScummVM: it isn't re-added; Add to ScummVM Again works.
- Edit the card's copy on a PC (both sides changed): after one emusync and one Nano pass, Nano's and your sections are all there.
- Rename a registered folder: same target, same list name, saves still there.
- A USB drive mounted before the card (card at index 2): the games still start (§5.3, with B2 and B3).
- Mass Add in ScummVM afterwards: no duplicates for helper-made entries.
- Clear ScummVM's data, then launch an unregistered folder: Nano's minimal file works and ScummVM keeps its defaults.

**After Phase 0 fixes:** Move Emulator Files to microSD for Azahar succeeds, and the "too long to send" line is gone from the log. Both sides of `scummvm.ini` edited: your sections survive the sync.

---

## 17. Phases

Efforts are guesses for one developer.

| Phase | Content | Effort | Risk |
|---|---|---|---|
| 0. Facts and card fixes | Owner's §16 steps 0.1-0.6. B2 (`isVolumeId`, 36 characters, tests). B6 (section merge for `scummvm.ini` on a card win). B1 (grant argument out of band). | 0.5 d for B2; 1-2 d for B6; 2-3 d for B1 | Low (B2, B6); medium (B1 touches system_server) |
| 1. Folders listed | §4 sources and rules, §5.1-5.2 matching, §12 mirror format, names and collisions, card out and in, Add Folder for ScummVM, "?" mark, launch dialog with the folder path and Open ScummVM (launcher only). No ini writes, no `browser_lastpath`. **Stops the list disappearing; games without a ScummVM entry show but don't start yet.** | 4-6 d | Low-medium: name changes would break links, so §12's rules and tests come first |
| 2. Detector | D2: the GPL helper (driver, NDK build outside configure's Android path, engine allowlist, version check, isolated uid and SELinux, licence and source) or the table (generator, matcher). Cache, time budget and back-off, Not Recognised list. | Helper 2-4 weeks **[U]**; table 1-2 weeks | High for the helper (build path, size and sandbox unknown); medium for the table (lower coverage) |
| 3. Registration | §9 write conditions and locks (force-stop of a cached ScummVM, first-boot minimal file), text editor, backup, pre-launch path at the three launch sites, emusync note of card copies, §10 dismissed list, §5.3 and §5.4 repoints, `browser_lastpath` seeding. Rewrite docs/ARCHITECTURE.md "ScummVM is the exception" (B7) and scummvm_scraping_plan.md Q3/Q9. | 5-7 d | Medium-high: Nano becomes a second writer of ScummVM's file |
| 4. Card identity and polish | B3 stable card id for emusync; Clean Up Missing ScummVM Games; title suggestions; B5; D9 (RetroArch core), if chosen. | 1-1.5 weeks | Medium: emusync migration of existing bases |

Decisions each phase needs:
- Phase 0: D8 (order), recommended yes.
- Phase 1: none of D1/D2. It assumes the recommended D3 (show "?"), D6 (Add Folder back) and D7 (top folder is one game); a different answer changes only small parts.
- Phase 2: D2. Phase 3: D1 (and D5 for repointing your own sections). Phase 4: D9 for the RetroArch part.

---

## 18. Decisions for the owner

- **D1. May Nano add game entries to ScummVM's `scummvm.ini`?** Nano already writes one setting there (`extrapath`, for BIOS files); this adds game sections. Append only, marked `thimsy_added=1`, only when no ScummVM game could be lost (§9.1), with a backup; also lets Nano seed the Add Game browser's start folder. Recommended: **yes**. Without it, games are listed (Phase 1) but each one must be added once in ScummVM before it starts.
- **D2. How should Nano identify games?** (a) A separate GPLv3 helper program built from ScummVM 2026.3.0 (best: the same detection as Mass Add, AGS unknowns and engine-side fallbacks included; but a program of tens of MB **[U]** on the system image, with its source shipped, and the most build work). (b) A built-in table from ScummVM's detection data (about 1.5 MB, no extra program; misses fallback detection, Sky, unknown AGS games and some engines). Recommended: **(a)**, with (b) if the helper's size or build turns out too costly.
- **D3. Unrecognised folders:** shown with "?" (recommended) or hidden?
- **D4. Deleted folders:** Nano's sections removed only through Clean Up Missing ScummVM Games (recommended), or automatically after a grace period?
- **D5. Your own entries whose folder moved** (a card at another index, §5.3, or a renamed or moved folder, §5.4): may Nano change their `path=`? Recommended: yes, only the path, only when the old path is gone and exactly one new folder matches.
- **D6. Add Folder for ScummVM:** bring it back so extra folders are scanned for game folders? Recommended: yes, without network shares.
- **D7. Grouping folders** (scummvm_scraping_plan.md Q8): keep the top folder as one game by default? Recommended: **yes**; a grouping folder then shows as one "?" entry, and you can add its games in ScummVM. Treating sub-folders as games can wait for scummvm_scraping_plan.md Q8.
- **D8. Order:** ship the card fixes B1, B2 and B6 first, ahead of the ScummVM work? Recommended: yes.
- **D9. RetroArch's ScummVM core as the player** (§13): should folders list and launch with the core too, through hook files Nano generates? Recommended: later (Phase 4), once the core's rules are confirmed; the ScummVM app is the default player.

---

## 19. Open questions

1. Do plain paths to the card work for ScummVM on Thimsy, and does the card appear in its drive list (§16, step 0.4)? If not, the card games Nano registers need `/saf/` paths and a folder grant from Nano, which B1 currently blocks on the Cube.
2. Is Dungeon Master's `dm` engine in the 2026.3.0 Android release? What is "Indiana Jones and the Call of Thunder (ScummGEN)"? Step 0.5 answers both.
3. Is internal storage case-folded on this build? That decides whether internal paths compare ignoring case.
4. Does ScummVM's Edit Game dialog keep unknown keys such as `thimsy_added`? (Its flushes do.)
5. Did you ever Mass Add these games? If so, the `[sky]`-only ini points to a card copy winning (§2.2, B6), and the logcat lines in step 0.3 might show when.
6. The helper's real size, and what an NDK build of ScummVM's null backend outside configure's Android path needs. Can an isolated uid read the card through FUSE?
7. Multi-CD games split over several folders (Blade Runner): merge them by hand, or should Nano say so in the Not Recognised list?
8. How RetroArch's ScummVM core finds the game from a `.scummvm` file, and whether it takes a folder directly (D9).

---

## 20. Evidence index

- Nano: `P:59298-59345` (`buildScanCandidates`, `syncScummvmList`), `P:65196-65650` (`NanoScummvmList.cpp/.h`), `P:73797-73860` (ARCHITECTURE "ScummVM is the exception"), `P:17330-17620` and `P:67148-67560` (NanoEmuSync and driver), `P:67163-67174` (`emuSdMode`), `P:66701-66706` (B1), `P:17466-17486` (B2), `P:9834-9840` (BIOS worker writes `extrapath`), `P:58841-58849` (internal `ROMs` and card `Roms` lookups), `P:65883` (`sdPkgRunning`), `P:18535` (`.scummvm .svm`), `P:162068` (tests).
- ScummVM 2026.3.0: `out/dp-evidence/test43/scummvm/src-v2026.3.0/` (`base_commandLine.cpp`, `base_main.cpp`, `base_plugins.cpp`, `engines_advancedDetector.{cpp,h}`, `engines_ags_detection*.{cpp,h}`, `gui_massadd.cpp`, `gui_launcher.cpp`, `backends_*`, `dists_android_*`, `configure`); on GitHub, tag `v2026.3.0`: `common/config-manager.cpp`, `engines/metaengine.h`.
- Device log: `out/dp-evidence/dock-tv-run/logcat.txt:14655` ff.
- Docs: https://docs.scummvm.org/en/latest/other_platforms/android.html (UI steps only, no command-line or intent options); https://lists.scummvm.org/pipermail/scummvm-git-logs/2025-April/110950.html (SCUMM and Glk detection dumps).
- Related plans: scummvm_scraping_plan.md (§R), linux_handheld_plan.md (card identity, decision 20), usb_storage_plan.md (the drive model, which B3 should share).
