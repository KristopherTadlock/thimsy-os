# Arcade and MAME controls plan (per-game layouts and the Button Map overlay)

Status: proposed 2026-11-27, revised the same day after a technical review and a completeness review. Planning only: no code was changed, nothing was built, and no device was touched. Technical claims are marked **[C]** (confirmed in source, a data file, the Thimsy patch or the repo), **[I]** (inferred) or **[U]** (unverified).

Request (owner, 2026-11-27): "For arcade games the inputs vary by game. Can we come up with a strategy for the controller overlay for arcade and MAME."

Background: `button_map_overlay_plan.md` (the Button Map overlay, phases 1-5 built; §3.1 has the RetroArch reader and the per-core RetroPad table), `controller_layout_plan.md` (layouts by position, §10 Neo Geo and CPS rows, §11 the ownership sidecar), `HOTKEYS.md`, `user_emulators_plan.md`, `plan_remove_unlicensed_files.md`, `ATTRIBUTION.md`.

Paths:
- `nano/` means `frameworks/base/cmds/gammaos-nano/` in the AOSP tree. Its files are read from `patches/thimsy/0001-thimsy.patch`; "P:n" is a line of that patch.
- Core sources were read from copies under `out/dp-evidence/test43/arcade/` (data only, nothing run): `fbneo/` and `src-fbneo/` (FBNeo master, `src/burner/libretro/`), `mame/` and `src-mame-libretro/` (libretro/mame master, `src/osd/modules/input/` and `src/osd/libretro/`), `mame2003p/` and `src-mame2003plus/` (mame2003-plus master), `mame-listxml/` (official `mame0289lx.zip`, SHA-256 checked), `batocera/` (reference only). These are current masters, not the exact commits of the 2026-11-24 nightlies Thimsy ships, so line numbers may move a little **[U]**.
- Survey scripts (host Python, data only) are in `out/dp-evidence/test43/arcade/survey-scripts/` (`fbneo_inputs.py`, `fbneo_size.py`, `listxml_stats.py`, `listxml_size.py`, `lx_stats.py`, the review's `rv_lx.py` and `rv_profiles.py`, and `count6.py` for the 6-button count in §2.2). They move to `tools/arcade-inputs/` in phase 1.
- RetroArch v1.22.2 sources: `out/dp-evidence/test43/arcade/ra1222-overlay/` (`command.c`, `runloop.c`, `verbosity.c`, `config.def.h`) and `review-ra1222/configuration.c`.

Positions in this plan: **S** bottom, **E** right, **N** top, **W** left. On the Cube those are the buttons printed **B**, **A**, **X**, **Y**, which are also RetroArch's RetroPad B, A, X, Y with Thimsy's pinned binds (`button_map_overlay_plan.md` §3.1) **[C]**.

---

## 0. Summary for the owner

### The problem

Arcade games don't share one controller. Street Fighter II has six buttons named punches and kicks, Neo Geo games have A-B-C-D, Pac-Man has a 4-way stick and no buttons, Robotron has two sticks, Arkanoid has a spinner, Operation Wolf has a gun. Today the overlay shows the same generic row for every arcade game ("Btn 1 … Btn 6", "Fire 1 …") with a "Per-game layout" badge (P:14933) **[C]**. For many games that row is wrong, not just vague: under FinalBurn Neo, Street Fighter II's light punch is on the left button, not the bottom one the row shows **[C]** (§1.3).

RetroArch itself knows each game's button names only while the core runs, and keeps them in memory. Thimsy ships the unmodified RetroArch 1.22.2 APK (`mods/10-update-emulators.sh:17`), so Nano can't read them **[C]**.

### The strategy in one paragraph

Work out each game's controls **on the build host**, from the cores' own sources and MAME's official game list, for the exact core versions Thimsy ships. Ship that as small per-core data files beside the overlay's other data. When the overlay opens over an arcade game, Nano looks the game up by its zip name and puts the game's own layer on top of what the overlay already reads (RetroArch binds, remap files, core options). The result: each Cube button is labelled with what it does in this game ("Jab Punch", "C", "Bomb"), on a drawing that matches the game's panel (6 buttons, Neo Geo row, twin stick, spinner, gun…). Where data is missing, today's generic row stays. Separately, a small number of settings make the cores' layouts better and consistent, so the map has a good layout to show.

### Decisions (recommended defaults; §13 has the full list)

| # | Decision | Recommended | Why |
|---|---|---|---|
| D1 | Turn on MAME's per-game button profiles (a MAME core option, off by default) | **Yes**, in the shipped MAME options file, and switched on once for existing installs (§6.3) | With it off, MAME puts Street Fighter's Jab on the bottom button and a kick on L1. With it on, a fixed list of 47 fighting games (Street Fighter, Darkstalkers, Marvel, Killer Instinct…) gets the same layout as FinalBurn Neo, Mortal Kombat gets the FBNeo layout too, and Tekken gets a 4-limb diamond. The other ~300 six-button games stay numbered **[C]**. MAME's Neo Geo layout changes too (D2) |
| D2 | Neo Geo face layout | **NEOGEO mini pad: A = Y, B = B, C = X, D = A** (left, bottom, top, right), in both cores | SNK's own pad, MAME's profile and FBNeo's "Modern" mode all use it. Today FBNeo uses A = B, B = A, C = Y, D = X (bottom, right, left, top), which `controller_layout_plan.md` §10 says to keep. One must give way. Either way, 11 Neo Geo games (Samurai Shodown, Last Blade…) get a different order from MAME's profile, and Thimsy's own per-game remap file evens them out (phase 5). **Decide before phase 1**, because D1 moves MAME's Neo Geo games to the mini layout straight away |
| D3 | Where per-game data comes from | **Generated at build time** from FBNeo source, libretro/mame source and MAME's `-listxml`; nothing read at run time from the internet | Exact for the shipped versions; works offline; licences are clear (§3) |
| D4 | Ship FBNeo-derived data | **Yes**, under FBNeo's licence, as its own file with the licence text beside it | The FBNeo core already ships under the same licence. The image must stay free and ask for no donations (it does today) **[C]** |
| D5 | Button names for MAME 2003-Plus games | **Use MAME's own names (the phase 3 parser) for the same set name; no copy of MAME 2003-Plus's `controls.c`**. Copy `controls.c` names only if the owner accepts the licence risk | The core's licence (MAME 0.78) says "You are not allowed to distribute a modified version", and the names came from the old controls.dat project, whose licence is unknown. Copying them into a separate file is weaker ground than "they already ship inside the core" (§3.2) |
| D6 | Never ship controls.ini / controls.xml / colors.ini, ArcadeDB, ScreenScraper or LaunchBox data | **Agreed** | No redistribution licence (§3.2) |
| D7 | Thimsy writes per-game RetroArch remap files at launch | **Only where the cores need evening out**: a device type (FBNeo Neo Geo games if D2 = mini; MAME 2003-Plus 6-button fighters) or button keys (MAME 2003-Plus Neo Geo and Mortal Kombat; MAME's 11 odd Neo Geo games; MAME's Neo Geo games if D2 = keep Classic). Never over a user's file | Most games need nothing written: the cores already do the right thing. The map says when a layout comes from Thimsy's file, not the user's |
| D8 | Second player | **Player 1 only, plus one note** about other pads' Coin and Start | Keeps the panel readable |
| D9 | Mahjong, hanafuda, gambling, keypad games | **Table only, with "needs a keyboard"** | 20+ keys; not playable on face buttons |
| D10 | Light guns on the touch screen | **Later phase** (phase 6) | Needs device checks of touch coordinates and gesture clashes |

### What changes for a player

- Opening the map over Street Fighter II shows the three punches on Y / X / L1 and the three kicks on B / A / R1, the same layout under FinalBurn Neo and MAME. The names are each core's own: "Weak / Medium / Strong Punch" in FinalBurn Neo, "Jab / Strong / Fierce Punch" in MAME (from phase 3).
- Over King of Fighters it shows the Neo Geo A-B-C-D row on the Cube's diamond, plus the AB/CD/ABC/BCD macro buttons FBNeo puts on the shoulders.
- Over Pac-Man it says "4-way stick". Over Robotron it shows two sticks. Over Arkanoid it says the spinner is the left stick.
- The panel always says how to insert a coin (Select) and start (Start), and how to reach the service menu where the core has one.
- If the game is unknown (homebrew, a core Thimsy has no data for), the panel is as today.
- The map always shows what the buttons do now. Where the two cores lay a game out differently (until phase 5 evens them out), each core's map is still right for that core.

### Phases (§10)

| Phase | What | Effort | Risk |
|---|---|---|---|
| 0 | Fix wrong rows and docs (FBNeo Neo Geo shoulders, badge order, device ids, the CPS HP/HK swap in `controller_layout_plan.md`); owner decides D1, D2, D5 | 1 day | low |
| 1 | Data format and lookup; MAME classes from `-listxml`; MAME profile table; MAME 2003-Plus layouts; D1 | 4-5 days | low |
| 2 | FBNeo per-game table from a host harness (no ROMs), with names; drift guard | 4-5 days | medium |
| 3 | Button names for MAME and MAME 2003-Plus; translated vocabulary | 3-4 days | medium |
| 4 | Panel drawings per class, header notes, Coin/Start/Service rows, P2 note | 3-4 days | low |
| 5 | Thimsy-owned per-game remaps that even out the cores (D2, D7), Game Options "Arcade layout", reading MAME's own input settings | 4-5 days | medium |
| 6 | Optional: touch light guns, turbo, mahjong touch panel, MAME4droid, Flycast NAOMI, an upstream libretro/mame change | 6-10 days | — |

Total: about 20-24 days for phases 0-5, plus 6-10 days for the optional phase 6 **[I]**.

Generators run on the host, never during an AOSP build (the WSL resource rule). Only phase 2 compiles anything: FBNeo's drivers and input code, thousands of files, so up to tens of minutes per core refresh **[I]**.

---

## 1. What exists today

### 1.1 How arcade games launch

| Nano system | Default player | Core |
|---|---|---|
| `arcade` | `mame.ra64.mamearcade` | libretro MAME, "MAME 0.289 sets" |
| `fbneo` | `fbneo.ra64.fbneo` | FinalBurn Neo |
| `neogeo` | `neogeo.ra64.fbneo` | FinalBurn Neo |

Sources: `mods/nano-systems.txt:64-66`, `mods/retroarch-cores.txt:52-55` **[C]**.

- Shipped arcade cores: `fbneo`, `mamearcade` (the default), `mame2003_plus` (MAME 0.78 sets, shipped but no system default). All are buildbot nightlies of 2026-11-24, pinned by SHA-256 in `mods/retroarch-cores.lock` **[C]**.
- Nano's per-game emulator list also offers MAME 2010/2003/2000, HBMAME, FB Alpha 2012, Geolith, Flycast, DICE, Supermodel and standalones (MAME4droid, NEO.emu, Flycast, SUPER3) for `arcade`, `mame`, `cps1-3`, `neogeo` (P:17677-18100) **[C]**. None of those cores or apps ship; they work only if the user installs them.
- Nano sets `sys.gammaos.nano.launch_core`, `launch_rom` and `launch_time`; the reader falls back to RetroArch's `content_history.lpl` when it is newer (`button_map_overlay_plan.md` §3.1, "Which core is running") **[C]**.
- **No remap files ship**: the remaps folder is empty and the core overrides have no `input_` keys (`button_map_overlay_plan.md` §3.1) **[C]**. **Arcade core options probably don't ship either [I]**: whether the RetroArch payload (`/system/etc/retroarch.tar.zst`) holds a `MAME/MAME.opt` or `FinalBurn Neo/FinalBurn Neo.opt` is not checked **[U]**. Phase 0 reads the tar; until then this plan assumes it doesn't.
- So every arcade core runs on its defaults: FBNeo device **Classic** (RetroArch's plain RetroPad is forced to Classic, `retro_input.cpp:3564-3578`), MAME button profiles **off** (`libretro_core_options.h:388-400`) **[C]**.

### 1.2 What the overlay does today

- `retropad.json` gives `fbneo`, `mamearcade` and `mame2003_plus` `"perGame": true` and a generic map (P:75143-75255) **[C]**:
  - fbneo: B→btn-1, A→btn-2, Y→btn-3, X→btn-4, L→btn-6, R→btn-5, L2 "Fire 8", R2 "Fire 7", L3 "Fire 10", R3 "Fire 9", Select→Coin; a `neogeo`/`neogeocd` variant B→A, A→B, Y→C, X→D with the shoulders blank.
  - mamearcade: B, A, Y, X, L, R → btn-1..6, L3 "Button 7", R3 "Button 8"; a `neogeo` variant like FBNeo's.
  - mame2003_plus: B, A, Y, X, L, R → btn-1..6, L2/R2/L3/R3 → Button 7-10.
- The reader adds the note "Per-game layout" (P:14933). The badge shows only the first note, so an override or remap note hides it (P:12684-12685, `r.notes[0]`) **[C]**.
- Drawings: `arcade6` (stick, btn-1..6, Coin, Start) and `neogeo` (btn-a..d, Select, Start), CC BY-NC 4.0 project art (`ATTRIBUTION.md`) **[C]**.

### 1.3 Is today's generic row right?

| Core, game kind | Today's row | Reality | Verdict |
|---|---|---|---|
| mamearcade, any game, profiles off (the shipped state) | B1..B6 = B, A, Y, X, L, R; B7 = L3, B8 = R3 | Same: `button_mapping[]` default (`input_retro.cpp:207-231`), L3/R3 (`:1393-1406`) **[C]** | **Right order, generic names.** Street Fighter's layout is poor, but the map is truthful |
| mamearcade, Neo Geo in the `neogeo` folder, profiles off | A = S, B = E, C = W, D = N | Same **[C]** | Right. Becomes wrong the moment D1 turns profiles on (A = W, B = S, C = N, D = E, `input_retro.cpp:529-548`; 11 titles get layout B or C instead, §2.2) **[C]** |
| mamearcade, Neo Geo in the `arcade` folder | generic btn-1..4 | right buttons, wrong drawing | Drawing only |
| fbneo, games with no special handling | Fire 1 = B, 2 = A, 3 = Y, 4 = X, 5 = R, 6 = L, 7 = R2, 8 = L2 | Same (Classic, `retro_input.cpp:76-85`) **[C]** | Right, generic names |
| fbneo, Street Fighter-style 6-button (named punches/kicks, or CPS-2 with 5+ buttons) | Fire 1 on B | LP = Y, MP = X, HP = L1, LK = B, MK = A, HK = R1; 3x Punch = L2, 3x Kick = R2 (`:396-401`, `:2023-2027`, `:2374-2393`) **[C]** | **Wrong** |
| fbneo, Neo Geo | A-D right; shoulders blank | AB = R1, CD = L1, ABC = R2, BCD = L2 macros (`:2029-2038`) **[C]** | **Incomplete** |
| fbneo, Mortal Kombat 1-3, twin-stick, driving, ~150 other special cases (`:801-2330`) | generic | per game **[C]** | **Wrong** |
| mame2003_plus, RetroPad device (default) | B, A, Y, X, L, R, L2, R2, L3, R3 = B1..B10 | Same (`mame2003.c:~1100-1168`) **[C]** | Right order; the core itself shows per-game names |

So today the map is right in order for MAME and wrong for many FBNeo games. Turning on MAME profiles (D1) without updating the overlay would make MAME wrong too, so **D1 and the overlay's per-game layer ship together** (phase 1).

---

## 2. What the three cores do

### 2.1 FinalBurn Neo

- Each driver lists its inputs with names (`BurnInputInfo`, e.g. "P1 Weak Punch"). The libretro layer maps them to RetroPad buttons by name and by a table of ~150 per-game special cases, then sends those names to RetroArch as descriptors with "P1 " stripped (`retro_input.cpp:692-724`, `:2830-2840`, `:3188-3205`) **[C]**.
- The mapping depends on the **device type** chosen in RetroArch (Quick Menu > Controls > Device Type, saved only in a `.rmp`):

| FBNeo device | Id in `.rmp` (`input_libretro_device_p1`) | Column 3 (HP / HK) | Column 4 (3xP / 3xK) | Neo Geo A-B-C-D |
|---|---|---|---|---|
| Classic (default) | 5 | L1 / R1 | L2 / R2 | S, E, W, N; AB = R1, CD = L1, ABC = R2, BCD = L2 |
| Modern | 517 | R1 / R2 | L1 / L2 | W, S, N, E (NEOGEO mini); AB = R2, CD = R1, ABC = L2, BCD = L1 |
| 6-Button Panel | 261 | SF games as Classic (the column macros switch only for Modern). Numbered games in straight rows: Fire 1-6 = Y X L1 / B A R1 | L2 / R2, as Classic | W, N, L1, S; AB = E, CD = R1, ABC = R2, BCD = L2 |
| Mouse (ball only) / Mouse (full) | 773 / 514 | — | — | — |
| Lightgun / Analog Arcade Gun | 4 / 1029 | — | — | — |
| Pointer / Touchscreen | 6 / 262 | — | — | — |

  Ids from `retro_input.h:35-41` (`RETRO_DEVICE_SUBCLASS(base, n) = ((n+1) << 8) | base`) **[C]**. Column and FIRE macros `retro_input.cpp:59-85`, Neo Geo face buttons `:2356-2372` (Modern) and the generic FIRE path otherwise, Neo Geo macros `:2029-2038` (first bind wins) **[C]**. `button_map_overlay_plan.md` §3.1 lists "Modern 261", which is the 6-Button Panel id; writing it would select the wrong device **[C]**.
- The Street Fighter layout is not a list of games: FBNeo uses it for any game whose inputs include 3x Punch and 3x Kick macros (named punches and kicks) and for any CPS-2 game with 5 or more buttons (`:396-401`) **[C]**. Games with plain "Button 1-6" names, such as Killer Instinct (`src/burn/drv/midway/d_kinst.cpp`: `kinst`, `kinst2`, `kinst2uk`, all working), stay numbered: B, A, Y, X, R1, L1 **[C]**.
- Other per-game rules: Mortal Kombat HP = Y, LP = B, HK = X, LK = A, Block = L1, Run = R1 (`:1762-1793`); fire directions and "Right Stick" inputs go to the right stick (`:1658-1676`, `:2249-2287`); Rotate → L1/R1; Accelerator → R2, Brake → L2, Gear → R1 (`:2289-2310`); mahjong inputs go to keyboard keys (`:177`, `:2444+`) **[C]**.
- Service menu: core option `fbneo-diagnostic-input`, default "Hold Start" (`retro_common.cpp:230-251`) **[C]**.

### 2.2 libretro MAME (`mamearcade`, MAME 0.289)

- Sends RetroArch **only fixed, generic descriptors** ("A, B, X, Y, L, R …") for every game (`src/osd/libretro/libretro-internal/libretro.cpp:816-851`) **[C]**. RetroArch's Controls menu shows no game names for it.
- Game button k goes to `button_mapping[k-1]`: default B, A, Y, X, L, R; button 7 = L3, button 8 = R3; L2/R2 have no button (`input_retro.cpp:207-231`, `:1393-1406`) **[C]**.
- Pedals: R2 = gas (`IPT_PEDAL`), L2 = brake (`IPT_PEDAL2`); game buttons 1 and 2 also ramp them (`:1482-1500`). Twin sticks: the right stick, and also game buttons 1-4 (`:1460-1474`) **[C]**. These use `switch_ids[SWITCH_B/A/Y/X]`, which are the items built from `button_mapping[0..3]` (`:1351-1360`), so they are B/A and Y/A/X/B only with profiles off; with a profile they follow it **[C]**.
- Driving buttons are whatever `BUTTONn` the driver uses: OutRun's gear is `IPT_BUTTON3 PORT_NAME("Gear Shift") PORT_TOGGLE` (`mame/sega/segaorun.cpp:1257`), so it lands on **Y** **[C]**.
- **Profiles** (`Input_Binding`, `:217-~700`) apply only when `mame_buttons_profiles = enabled` (`:1547`; option default `disabled`, `libretro_core_options.h:388-400`; read at `libretro.cpp:422-430`) **[C]**. It is an if / else-if chain, so **the first match wins**: the name lists come before the Neo Geo source-file test at `:529` **[C]**. About 104 set names, each matched by name or parent:

| Profile (in chain order) | Game buttons 1-6 → RetroPad | Games |
|---|---|---|
| Four-button layout B | Y, X, B, A | Tekken 1-3, Samurai Shodown 64 and these **Neo Geo** titles: `kabukikl`, `lastblad`, `lastbld2`, `ninjamas`, `samsho`, `samsho2`, `samsho5`, `samsh5sp`, `wakuwak7` (Neo Geo A = W, B = N, C = S, D = E) (`:233-302`) |
| Four-button layout C | Y, X, A, B | 15 sets: `blandia`, `cybots`, `doa`, `doapp`, `jojo`, `jojoba`, `mmaulers`, `mtlchamp`, `plsmaswd`, `samsho3`, `samsho4`, `souledge`, `soulclbr`, `svg`, `vfremix` (`samsho3`/`samsho4`: A = W, B = N, C = E, D = S) (`:304-349`) |
| Tech Romancer | per game | `techromn` (`:351`) |
| 6-button fighters | Y, X, L, B, A, R | a fixed list of 47 parents: SF, SF Alpha/III/EX, Darkstalkers, Marvel/X-Men, Killer Instinct 1-2, Red Earth, Battle K-Road… (`:424-527`) |
| Neo Geo (source `neogeo`/`neopcb`) | Y, B, X, A, L, R | MVS games not in an earlier list (`:529-548`) |
| Mortal Kombat 1-3, UMK3, WWF | Y, L, X, B, A, R | (`:615-621`) |
| Others | per game | MK4, Tekken Tag, Virtua Fighter, Dark Edge, Slam Masters II, Toshinden 2, Ehrgeiz (`:352-600`) |

  MAME 0.289 has about 350 parent games with an 8-way stick and 6 buttons (`count6.py`: 349) **[C]**; outside the 47 listed they stay numbered B, A, Y, X, L, R even with profiles on. So FBNeo and MAME agree on the SF layout only for the listed sets.

- Other options: `mame_mame_4way_enable` (default disabled), `mame_lightgun_mode` (none/lightgun/touchscreen, default none) (`libretro_core_options.h:372-429`) **[C]**.
- Remaps made in MAME's own menu are saved to `<savefile_directory>/mame/cfg/<set>.cfg` (`retro_init.cpp:108-153`, `:555-575`); format at https://docs.mamedev.org/advanced/ctrlr_config.html **[C]**. On the Cube that is probably `RetroArch/saves/mame/cfg/` **[U]**, depending on save-folder sorting.

### 2.3 MAME 2003-Plus

- Device types: RetroPad 1 (numbered B, A, Y, X, L, R, L2, R2, L3, R3), **Fightstick** 257 (B1 = Y, B2 = X, B3 = R, B4 = B, B5 = A, B6 = R2), 8-Button 513, **6-Button** 769 (Y X L / B A R) (`mame2003.c:101-111`, `:735-790`, `:~1100-1168`) **[C]**. `button_map_overlay_plan.md` §3.1 calls 257 "Modern"; the core calls it "Fightstick" **[C]**.
- It sends per-game descriptors from `src/controls.c` (918 `ControlInfo` entries: 917 games plus `generic_ctrl`), "Originally derived from controls.dat Version=0.141.1" (`controls.c:1-6`; `retro_describe_controls()`, `mame2003.c:824-930`) **[C]**. The entries also hold long `control_details` prose from controls.dat, which Thimsy never ships in any case.
- Its own input remaps are saved in MAME 0.78's binary cfg format, which the overlay won't read **[I]**.

---

## 3. Data sources and licences

### 3.1 What ships, what is generated, what is read at run time

| When | What | From |
|---|---|---|
| **Build time (generated on the host, shipped as data)** | Per-game panel class (stick ways, buttons, players, analog controls) | MAME `-listxml` for the shipped MAME version (`mame0289lx.zip`, official release asset). Its `buttons` attribute is the highest `BUTTONn` index for `joy`/`stick` controls but a plain **count** for every other type (`infoxml.cpp:1792-1815`) **[C]**: outrun reports `paddle buttons="1"` though its one button is BUTTON3. So listxml alone places buttons only for stick games; dial, paddle, pedal, gun and trackball games need the phase 3 port parser for button positions |
| | MAME per-game RetroPad order, profiles on and off | Parsed from libretro/mame `input_retro.cpp` at the shipped commit |
| | MAME button names | Parsed from MAME driver `INPUT_PORTS` blocks (`PORT_NAME`) at the shipped commit (phase 3) |
| | FBNeo per-game RetroPad map and names, per device type | A host harness built from FBNeo's libretro input code at the shipped commit (phase 2) |
| | MAME 2003-Plus per-game button counts and device tables | Parsed from its `controls.c` and `mame2003.c`; names from MAME's `PORT_NAME` for the same set name (D5); `controls.c` names only if the owner accepts that risk |
| | Clone → parent map | `-listxml` (`cloneof`), FBNeo's dat, mame2003-plus's `metadata/mame2003-plus.xml` |
| **Run time (read by Nano when the map opens)** | Which game and core | `launch_rom`/`launch_core`, or `content_history.lpl` (existing) |
| | Device type, button remaps | the effective `.rmp` (existing reader) |
| | MAME profiles on or off | the `.opt` chain (existing reader; add the key) |
| | User remaps made inside MAME | `saves/mame/cfg/<set>.cfg`, then `default.cfg` (phase 5) |
| **Run time (written by Nano at launch, phase 5)** | Device type for the few classes that need it | Thimsy-owned per-game `.rmp`, only where no user file exists (§6) |
| **Never** | Querying any website from the device; reading RetroArch's memory or log; a patched RetroArch | — |

Rejected ways to get the running core's labels **[C]**:
- RetroArch network commands: `GET_STATUS` gives only the state (PAUSED or PLAYING), system, content base name and `crc32`; no descriptors; `network_cmd_enable` is off and would open UDP 55355 (`command.c:424-488`, `:1034-1072`, status line `:1052-1066`; `config.def.h:1466`).
- RetroArch's log prints descriptors only at debug level with verbose logging (`runloop.c:2117-2160`, `verbosity.c:390-397`). The descriptor text is the core's own English; only the RetroPad button names are localised. Diagnostics only, and useless for MAME, which sends only generic names.
- A patched RetroArch: Thimsy ships the upstream signed APK; a fork means its own signing and upkeep.
- A Nano helper that loads the core with the user's ROM to capture descriptors: exact, but costs RAM and seconds, needs BIOS paths, and SELinux access to RetroArch's core folder is uncertain **[U]**. Kept as an idea, not planned.

### 3.2 Licence verdicts

| Source | Licence | Ship? |
|---|---|---|
| MAME `-listxml` summary (classes, buttons, players) | Generated by MAME (GPL-2.0+, many files BSD-3-Clause, e.g. `infoxml.cpp`); factual data | **Yes**, with a MAME credit in `NOTICE` **[I, legal judgement]** |
| MAME `PORT_NAME` button names | same | **Yes** (generated) |
| libretro/mame profile table | MAME fork; `input_retro.cpp` has no licence header, so the repo's GPL-2.0+ applies **[C]** | **Yes** (generated) |
| FBNeo-derived names and maps | FBNeo licence (`src/license.txt`): no monetary gain, "You must make public any changes you make to the source code", include the text verbatim, no donation requests **[C]** | **Yes**, separate data file with `LICENSE.fbneo.txt` beside it; free image only; the harness source is published with the build repo (it is built from FBNeo code). README, NOTICE and ATTRIBUTION have no donation links today **[C]** |
| MAME 2003-Plus `controls.c` names | the repo's `LICENSE.md` is the MAME 0.78 licence: non-commercial, and "You are not allowed to distribute a modified version" (https://github.com/libretro/mame2003-plus-libretro/blob/master/LICENSE.md) **[C]**; content from controls.dat, whose own licence is unknown | **Not by default (D5).** Names come from MAME 0.289's `PORT_NAME` for the same set name instead. Copying `controls.c` names needs the owner to accept the risk; fallback is counts only |
| controls.ini / controls.xml / controls.json, colors.ini | the original files are "devoid of any license"; only changes made in the kpshek repo are Apache-2.0 (https://github.com/kpshek/ledblinky-controls-colors); project dead since 0.141.1 (2011) **[C]** | **No** (build-time cross-check at most) |
| ArcadeDB (adb.arcadeitalia.net) | site terms: attribution, one connection per IP, no redistribution grant (https://adb.arcadeitalia.net/service_scraper.php) **[C]** | **No**; manual QA reference only |
| ScreenScraper, LaunchBox, HyperSpin | site terms; no per-button data found **[U]** | **No** |
| Batocera `roms.toml`, `mame_controls.toml` | LGPL-3.0+ from the repo licence; the `.toml` files have no header (the `.py` files do) **[C]** | **No**; test cross-check only |
| libretro-database (CC-BY-SA-4.0), RetroArch common-overlays (CC-BY-4.0) | free | Not needed |

Records to add (phase 1-3, in the same change as the data):
- `NOTICE`: MAME data (GPL-2.0+/BSD-3-Clause); FBNeo's licence text verbatim, for the data **and** for the `fbneo` core that already ships. Neither `NOTICE`, `ATTRIBUTION.md` nor `README.md` mentions FBNeo or MAME today **[C]**; `plan_remove_unlicensed_files.md` §1.4 only says non-commercial cores are "fine for a free image" **[C]**. That gap exists without this plan.
- `NOTICE`: mame2003-plus and the controls.dat project, only if the owner chooses to copy `controls.c` names (D5).
- `ATTRIBUTION.md`: the new panel drawings (project art, CC BY-NC 4.0 like the existing ones).

---

## 4. Identifying the game

1. **Key:** the lower-cased file name of the ROM, without folder and without `.zip`/`.7z`, from `launch_rom` or the newest `content_history.lpl` entry. Look it up **in the running core's own table**, so FBNeo names, MAME 0.289 names and MAME 0.78 names never need converting.
2. **The name is reliable for arcade:** FBNeo and MAME find the driver by the zip's name, so a renamed zip doesn't run anyway **[I]**. No CRC matching is needed. (That would mean shipping ROM CRCs, 15-300 MB per core.)
3. **Clones:** a clone whose controls equal its parent's is stored as `=parent`; the lookup follows one step. A clone that differs (some regional sets change buttons) has its own line.
4. **Not found:** FBNeo's console sets (`md_`, `cv_`… prefixes and folders), homebrew, or a core with no table. The panel keeps today's generic row. For the `neogeo` system the Neo Geo class still applies (it comes from the system, not the game). Nano has no Neo Geo CD system (`mods/nano-systems.txt:64-66` has only `arcade`, `fbneo` and `neogeo`) **[C]**; `retropad.json`'s `neogeocd` variant is unused.
5. **Standalone MAME (MAME4droid, user-installed):** the same MAME table applies when its MAME version is close; game name from Nano's launch (phase 6) **[I]**.

---

## 5. Layout classes and the default Cube mapping

### 5.1 How common each class is (MAME 0.289, parent machines with coin slots)

| Class | Games | Class | Games |
|---|---|---|---|
| 8-way stick, 1-2 buttons | 1,081 | Twin stick | 64 |
| 8-way stick, 3-4 buttons | 1,014 | Dial / paddle | 385 |
| 8-way stick, 5-6 buttons | 372 | Trackball | 132 |
| 8-way stick, 7-9 buttons | 41 | Light gun | 169 |
| 4-way stick | 262 | Analog stick (flight) | 190 |
| 2-way stick | 222 | Pedal (driving) | ~29 + wheel games counted as dial |
| Neo Geo (source file) | 162 | Mahjong / hanafuda | 857 |
| Keypad | 742 | Gambling | 1,305 |
| Buttons only | 1,520 | Positional / rotary | 27 |

From `lx_stats.py` over `mame0289lx.zip` **[C]**. `-listxml` gives control types, stick ways and button counts (a highest index for sticks, a count otherwise, §3.1), but **no button names and no genre** (`infoxml.cpp:240-256`) **[C]**. Players: 1 player 4,746, 2 players 3,529, 3-4 players 483 **[C]**.

### 5.2 Default Cube layout per class

"Core" means the core already does this with Thimsy's defaults (plus D1 for MAME); nothing is written. Detection order in the generator: light gun → driving (pedal, or paddle + pedal) → dial/paddle → trackball → twin stick (`doublejoy` or two joysticks for P1) → analog stick → keypad/mahjong/gambling → stick + buttons (Neo Geo by source file or FBNeo hardware). A small hand-kept override list fixes misclassified games.

| Class | Cube layout | FBNeo | mamearcade (D1 on) | mame2003_plus | Drawing |
|---|---|---|---|---|---|
| 1-4 buttons, 8-way | Stick or D-pad; B1 = S, B2 = E, B3 = W, B4 = N; Start; Select = Coin | core | core | core | `arcade1`..`arcade4` |
| Buttons only (no stick; quiz, some sports and puzzle games: 1,520) | As 1-4 buttons without the stick; 5-6 buttons as "Other 5-6 buttons"; more than 6 go to the table strip | core | core | core | `arcade1`..`arcade4` / `arcade6` with the stick marked unused |
| **6-button fighter** (SF style) | LP = W, MP = N, HP = L1; LK = S, MK = E, HK = R1. FBNeo adds 3x Punch = L2, 3x Kick = R2 | core (Classic, 6-Panel; any game with punch/kick inputs or CPS-2 with 5+ buttons) | core only for the 47 listed sets (no 3x macros); the other ~300 six-button games stay numbered | `.rmp` device 769 (phase 5) | `arcade6` with names |
| **Neo Geo** (D2 = mini) | A = W, B = S, C = N, D = E; FBNeo macros AB = R2, CD = R1, ABC = L2, BCD = L1 (Modern columns) | `.rmp` device 517 (phase 5) | core (profile), except the 11 layout B/C titles (§2.2): `.rmp` button remap (phase 5) | `.rmp` button remap (phase 5) | `neogeo` |
| Neo Geo (D2 = keep FBNeo Classic) | A = S, B = E, C = W, D = N; AB = R1, CD = L1, ABC = R2, BCD = L2 | core | `.rmp` button remap per Neo Geo game (phase 5), e.g. for the mini profile: Cube B → RetroPad Y, A → B, Y → X, X → A **[I]**; the 11 layout B/C titles get their own remap | core | `neogeo` |
| **Mortal Kombat** 1-3, UMK3, WWF | HP = W, LP = S, HK = N, LK = E, Block = L1, Run / Block 2 = R1 | core | core | `.rmp` remap (phase 5) | `arcade6` with names |
| Killer Instinct 1-2 | as the 6-button fighter row (MAME profile) | core: numbered B, A, Y, X, R1, L1, generic names (no special case) **[C]**; a Thimsy remap to match MAME is open question 1 | core | — | `arcade6` |
| **Tekken** 1-3 (4 limbs) | LP = W, RP = N, LK = S, RK = E | — (not in FBNeo) | core | — | `arcade4` diamond |
| Other 5-6 buttons (not fighters) | numbered; FBNeo B5 = R1, B6 = L1 but MAME B5 = L1, B6 = R1 | core | core | core | `arcade6` numbered |
| 7-10 buttons | FBNeo B7 = R2, B8 = L2; mame2003_plus B7 = L2, B8 = R2; MAME B7 = L3, B8 = R3 | core | core | core | `arcade6` + table strip |
| **4-way** (Pac-Man, Donkey Kong) / **2-way** (Galaga) | D-pad first; left stick works too. Header note "4-way stick" | core | core. `mame_mame_4way_enable` would be per game, but a per-game `.opt` replaces `MAME.opt` entirely, so it must copy every key (including D1's) **[I]**; not planned | core | stick + note |
| **Twin stick** (Robotron, Smash TV) | Left stick or D-pad moves; right stick fires/aims. MAME also fires with game buttons 1-4 (Y/A/X/B with profiles off) | core | core | **[U]** | `twinstick` |
| **Driving** | Steer with left stick X; gas R2, brake L2. Gear: FBNeo R1 (`retro_input.cpp:2306`); MAME the driver's own button (OutRun: button 3 = Y). L2/R2 are digital on the Cube (on/off throttle); MAME also ramps the pedals with game buttons 1 and 2 (B/A with no profile) | core | core (from the per-game data, not a fixed row) | **[U]** | `driving` |
| **Dial / paddle / spinner** (Arkanoid, Tempest) | Left stick X (rate-based), D-pad left/right; fire = S | core (`fbneo-analog-speed`) | core | core | `dial` |
| **Trackball** (Centipede) | Left stick as a 2D rate; fire = S | core | core | core | `trackball` |
| Positional / rotary (Ikari Warriors) | Right stick aims where the core supports it; L1/R1 rotate | core | **[U]** | **[U]** | stick + note |
| **Light gun** | Today: stick aims (core default). Phase 6: touch screen (tap = trigger) only when the game is on the Cube's own screen. When the game is on a monitor (External Only, Mirror's TV, or Dual Screen with the monitor as main screen), touch guns are not supported: the stick aims | core / `.rmp` 262 or 1029 (phase 6) | `.opt` `mame_lightgun_mode` (phase 6) | **[U]** | `lightgun` |
| Analog stick (flight) | Left stick; buttons numbered | core | core | core | `flight` |
| Mahjong / hanafuda / keypad | Not playable on the pad. Table plus "needs a keyboard" | keyboard keys | MAME keyboard defaults | — | table only |
| Gambling / mechanical | "Not supported on a controller" | — | — | — | table only |

Evidence: §2.1-2.3 **[C]**; Batocera's schemes (`batocera_launch_libretro_mame`, `mame_controls.toml`: `sfsnes`, `mksnes`, `neomini`, `twinstick`, `qbert`) agree on the SF, MK and NEOGEO mini layouts **[C]**, so the recommended rows are the community consensus, not a Thimsy invention.

ABXY Swap is not involved: arcade layouts are by position, and the overlay's `PadModel` already handles the swap and remaps when it labels positions (`controller_layout_plan.md` §0, `button_map_overlay_plan.md` §4.2) **[C]**. The map is drawn for the pad that pressed the chord (`HOTKEYS.md:68`, `bmap_dev`) **[C]**, so an external pad gets its own positions; §10.1 tests both.

Limits:
- **Rotated games.** The map assumes the game's stick is not rotated. FBNeo's "Vertical mode" (`fbneo-vertical-mode`) rotates only the picture; `retro_input.cpp` has no rotation handling **[C]**. Whether MAME's `mame_rotation_mode` also turns the stick directions is **[U]**; if it does, the panel adds a "Rotated" note.
- **3-4 player games** (483 in MAME, e.g. TMNT, Gauntlet): the panel shows player 1 with the P2 note (D8); each extra pad is its own player **[I]**.

---

## 6. Per-game remaps and user overrides

### 6.1 What decides the layout, in order

1. **The user's own files always win.** A RetroArch game, content-folder or core remap (`<remapdir>/<library>/<set>.rmp`, `<folder>.rmp`, `<library>.rmp`; they don't stack, `button_map_overlay_plan.md` §3.1 item 4) **[C]**, and MAME's own `cfg/<set>.cfg`.
2. **Thimsy-owned per-game remaps** written at launch (phase 5), only for: FBNeo Neo Geo games when D2 = mini (`input_libretro_device_p1 = "517"`), MAME 2003-Plus 6-button fighters (`"769"`), MK and Neo Geo button remaps for MAME 2003-Plus, button remaps for mamearcade's 11 layout B/C Neo Geo titles, and for all mamearcade Neo Geo games if D2 = keep Classic. Later, touch guns and turbo (phase 6).
3. **Core options:** `mame_buttons_profiles = "enabled"` in `MAME/MAME.opt` (D1).
4. **The core's built-in per-game logic** (FBNeo special cases, MAME profiles).

### 6.2 Rules for anything Thimsy writes

- Nano writes only when RetroArch is not running, just before the launch, and only if **no** user remap exists at any of the three levels for that core. Otherwise a Thimsy game file would silently hide a user's folder or core remap **[C, remaps don't stack]**.
- At every launch Nano also checks for the reverse case: a Thimsy-owned game file left from an earlier launch while the user has since saved a folder or core remap. Then Nano deletes its own game file (it still matches the sidecar record), so the user's file takes effect.
- Each write is recorded in the ownership sidecar `/data/system/nano_pads_owned.json` (file, keys, hash), as `controller_layout_plan.md` §11 proposes. Nano changes or deletes only a file whose content still matches its record. A file the user saved over is theirs.
- If Merge Controllers' core `.rmp` (`controller_layout_plan.md` §9.3) lands, one writer must compose merge keys and arcade keys into the same per-game file.
- A remap with only the device key and no button keys leaves the buttons unchanged: `input_remapping_load_file` first calls `input_remapping_set_defaults(false)` and then changes a button only when its key is present (`configuration.c:6162`, `:6191-6198` at v1.22.2) **[C]**.
- A Thimsy `.rmp` may also carry turbo: 1.22.2 reads `input_turbo_enable`, `_mode`, `_bind`, `_button`, `_period` and `_duty_cycle` from the remap file itself (`configuration.c:6259-6265`) **[C]**.
- Thimsy never writes a per-game `.opt`: it replaces `MAME.opt` entirely, so it would drop D1's key unless every key were copied **[I]**.
- **Not written:** thousands of pre-made `.rmp` files in the payload, and MAME `.cfg` files (Batocera writes them at every launch, `controllers.py:234-276` **[C]**; MAME rewrites the cfg on exit together with DIP switches, which makes ownership hard **[I]**). Kept as a later option for 3x Punch on L2 in MAME and an SNES-style Killer Instinct layout.

### 6.3 D1: how profiles get turned on

- New images: `mame_buttons_profiles = "enabled"` in the payload's `RetroArch/config/MAME/MAME.opt`, added by a `mods/` step like the other RetroArch provisioning **[I]**.
- Existing installs: **"add only if absent" would do nothing.** At core deinit RetroArch writes every option's current value to the active `.opt` (`runloop_deinit_core_options`: `core_option_manager_flush` then `config_file_write`, `runloop.c:1033-1080`; also `core_options_flush`, `:5297`) **[C]**. So on any install where mamearcade has run once, `mame_buttons_profiles = "disabled"` is already in the file, and "disabled" can't be told apart from "never chosen" **[C/I]**.
- So the migration **overwrites `disabled` with `enabled` once**, when RetroArch is not running, in the release that ships D1, and records that in the sidecar. It never runs again: a player who turns profiles off afterwards keeps that. A per-game or folder `.opt` the user saved is not touched. The release notes say so. The owner accepts this as part of D1 (the alternative, new images only, leaves existing installs numbered).
- **The overlay always reads the effective value** from the `.opt` chain (game `.opt` → folder `.opt` → core `.opt`, P:14917-14920) **[C]**, so the map is right either way. A per-game `.opt` replaces the core `.opt` rather than merging **[I]**; the reader already follows that.

**D1 and D2 in time.** D2 must be decided before phase 1 ships, because D1 (phase 1) moves MAME's Neo Geo games to the mini layout straight away, while the Thimsy remap files that even the cores out come in phase 5:
- D2 = mini: from phase 1 to phase 5, MAME uses mini (except the 11 layout B/C titles) and FBNeo stays on Classic.
- D2 = keep Classic: from phase 1 to phase 5, MAME's Neo Geo games leave Classic; phase 5 brings them back with owned button remaps.

In both cases the map is right for each core throughout (it reads the per-game data and the effective files). To close the gap sooner, the Neo Geo part of the phase 5 writer can move into phase 1 (about +2 days) **[I]**.

### 6.4 Settings

- Game Options (per game, phase 5): **Arcade layout**: Auto (the class default) / Core default (numbered) / Fight pad (FBNeo Modern, MAME 2003-Plus Fightstick). Choosing anything writes or removes only Thimsy-owned files.
- Settings → Gamepad Settings: **Neo Geo layout**: NEOGEO mini / Classic (FBNeo's default), defaulting to D2. It changes which owned `.rmp` files phase 5 writes for both cores.
- A global "Fight pad" option for players with 6-button external pads is an open question (§13 Q5).

---

## 7. Overlay changes

### 7.1 Data files

- New prebuilt `nano_xmb_buttonmap_arcade` → `/system/etc/nano_xmb/buttonmap/arcade/{fbneo,mame,mame2003_plus}.tsv`, with `/data/system/nano_xmb/buttonmap/arcade/` checked first like the other `nano_xmb` assets (`button_map_overlay_plan.md` §6.2) **[C pattern]**.
- One game per line, **sorted by set name**, so Nano finds a line by binary search with `pread` and keeps nothing in memory.
- Header lines: `#core`, `#lockSha256` (the `.so.zip` hash from `mods/retroarch-cores.lock`, for the build check), `#soSize` and `#soSha256` (the unzipped `.so` the image ships, for the device check), `#upstream` (commit), `#mame` (listxml version), `#generator`, `#licence`.
- Named layouts: `#layout sf6 y,x,l,b,a,r`, `#layout neo4 y,b,x,a`… (game button k → RetroPad id, `-` for none, plus macros such as `r2=3xP`).
- Game line: `set ⇥ parent ⇥ class ⇥ players ⇥ ways ⇥ buttons ⇥ layout ⇥ labels ⇥ extras`.
  - FBNeo: `layout` per device, `c:<name>;m:<name>;6:<name>`.
  - MAME: `on:<name>;off:<name>` (profiles on/off).
  - MAME 2003-Plus: per device 1/257/513/769 (these are fixed tables, so the game line needs only `buttons` and `labels`).
  - `labels` = `Jab Punch|Strong Punch|…` in game-button order; empty = generic.
  - `extras` = `l3=Service`, `pedal1=r2`, `dial`, `gun`…
  - A clone equal to its parent: `set ⇥ =parent`.
- Size: MAME's class summary alone is 903 KB JSON / 102 KB gzip for 19,224 games (`listxml_size.py`) **[C]**; FBNeo P1 names 297 KB JSON / 49 KB gzip for 9,753 games with 1,808 unique input lists (`fbneo_size.py`) **[C]**. TSV with shared layouts should total about 0.6-1.5 MB **[I]**. No JSON DOM is built.

### 7.2 How the reader composes the map

Over a `perGame` core with a known ROM, `readRetroArch` (P:~14900-15000) does:

1. Cube control → RetroPad: unchanged (binds, autoconfig, `.rmp` button keys).
2. RetroPad → game button: from the game's line, for the effective FBNeo device (`.rmp`, default Classic), MAME profile state (`.opt`), or MAME 2003-Plus device. Then MAME `cfg` remaps on top (phase 5).
3. Game button → name: the game's label if any, else the class vocabulary, else "Button n".
4. Drawing: the game's class drawing (§7.4); `neogeo` for Neo Geo hardware.

New pure module `nano/NanoButtonMapArcade.h/.cpp` (host-tested): `lookupArcadeGame(env, dir, core, set, *out)` (follows the parent), `layoutFor(game, device, profiles)`, `mameCfgRemap(text, layout)`, `fbneoDevice(id)`. Game labels go into `EmuBind`'s existing `name` field, "the console's name for it when not the art's" (P:16250), as the reader already does for text rows with `bindFor(&res.binds, "", name)` (P:14978) **[C]**; no new field.

### 7.3 Labels

- Game labels as the core names them, "P1 " stripped: "Jab Punch", "Weak Kick", "Shot", "Bomb", "Button A".
- MAME 2003-Plus games use MAME 0.289's names for the same set name (D5) **[I: button numbers rarely change between versions; the generator flags sets whose button count differs and leaves those generic]**. Only if the owner chooses `controls.c` names: drop the core's "B1: " prefix.
- Labels longer than the art's label box go to the table strip, as today (layout tests cover it).
- MAME labels never borrowed from another core by default (open question Q3).

### 7.4 Drawings per panel class

New project drawings (CC BY-NC 4.0, generated with `work/controller-art`, following the anchor contract in `button_map_overlay_plan.md` §6.3). Class names follow ES-DE's controller badges (`joystick_arcade_no_buttons`, `joystick_arcade_no_buttons_twin`, `joystick_arcade_1_button` … `joystick_arcade_6_buttons`, `lightgun_generic`, `steering_wheel_generic`, `spinner_generic`, `trackball_generic`, `flight_stick_generic`; ES-DE `BadgeComponent.cpp`) **[C]**.

| Drawing | Anchors (new ones in bold) |
|---|---|
| `arcade1`..`arcade4` | `stick-l`, `btn-1`..`btn-4`, `btn-coin`, `btn-start` |
| `arcade6` (existing) | as today |
| `neogeo` (existing) | as today |
| `twinstick` | `stick-l`, `stick-r`, `btn-coin`, `btn-start` |
| `dial` | **`dial`**, `btn-1`, `btn-2` |
| `trackball` | **`trackball`**, `btn-1`..`btn-3` |
| `lightgun` | **`gun`**, `btn-1`, `btn-2` |
| `driving` | **`wheel`**, **`pedal-gas`**, **`pedal-brake`**, **`btn-gear`** |
| `flight` | `stick-l`, **`throttle`**, `btn-1`..`btn-4` |
| mahjong, keypad, gambling | table only |

- The reader's `anchorLike` prefixes (`:693`) gain `dial`, `trackball`, `gun`, `wheel`, `pedal`, `throttle`.
- Header notes: "4-way stick", "2-way stick", "Spinner: left stick", "Aim: right stick".
- If the ES-DE gamelist has a `<controller>` for the game (parsed by `NanoThemeEngine.cpp`, P:~71076 **[I]**), it may override the class (open question Q6).

### 7.5 Remaps shown

- `.rmp` button remaps and device: already read; now they also select the game layout.
- MAME `cfg/<set>.cfg`, then `cfg/default.cfg` (phase 5): each `<port type="P1_BUTTONk"><newseq type="standard">JOYCODE_1_BUTTONj</newseq>` is inverted through the active layout. Read capped at 256 KB. Badge note "MAME input settings: sf2.cfg".
- MAME 2003-Plus binary cfg: not read; noted as a limit.
- Unsaved changes made in RetroArch's menu are invisible (as for every core, `button_map_overlay_plan.md` §3.1 item 8).

### 7.6 Players, Coin, Start, Service

- The panel labels the pad in hand as player 1 (the reader uses `input_player1_*`).
- Select is **Coin** in all three cores (MAME COIN1 default includes `JOYCODE_SELECT_INDEXED(0)`, `inpttype.ipp`; FBNeo maps coin/select to Select; MAME 2003-Plus names it "Coin") **[C]**. New hint line: "Insert a coin with %s, then press %s".
- With two or more pads: "Each pad is its own player; its Select and Start are that player's Coin and Start" **[C for MAME]**. A P2 page is open question Q4.
- Service menu row in the hotkeys section: FBNeo "Hold Start" (from `fbneo-diagnostic-input`'s effective value). MAME's Service/Test are keyboard keys (F2 clashes with Thimsy's RetroArch save-state key, `HOTKEYS.md:219`) **[C]**: hidden for now, open question Q7.
- RetroArch's Select hotkey-enable chords (Select + L2/R2/R1) already appear in the hotkeys section. Select also inserts a coin when used that way; harmless **[I]**.

### 7.7 When data is missing

| Case | Panel |
|---|---|
| Game not in the table | Today's generic row; "Per-game layout" moved to the front of the notes (fixes the hidden-badge bug) |
| Game found, no names | Correct buttons, class words or "Button n" |
| No ROM path (launched outside Nano, no history) | Generic |
| Core has no table (MAME 2010, HBMAME, Geolith…) | No arcade layer; the existing "no map" path |
| Table built for a different core version | Shown, with the note "Made for an earlier version of this core" when the installed core's file size differs from the `#soSize` in the table header (§7.1). Nano only `stat`s the file, never hashes it. If Nano can't read RetroArch's core folder, no note **[I]** |

Badge sources replace "Per-game layout": "FinalBurn Neo's layout for this game", "MAME button profile", "Thimsy's layout for this game" (the effective `.rmp` matches its record in the ownership sidecar), "From your remap file" (any other `.rmp`), "Generic (game not in Thimsy's list)".

### 7.7a Which screen

Arcade adds nothing new here: the map opens where the existing overlay opens and is drawn at the same sizes (720x720 panel; centred on a monitor in External Only and Mirror, `button_map_overlay_plan.md:1018`). In Dual Screen it follows the controller's main screen once `dual_screen_43_plan.md` lands **[I]**. Phase 4's new drawings get the same size tests, and §10.1 has a monitor row.

### 7.8 Translation

- Game labels stay in English. A vocabulary of frequent terms is translated: Coin, Start, Service, Test, Weak/Medium/Strong Punch/Kick, Jab, Punch, Kick, Jump, Shot, Fire, Bomb, Attack, Special, Guard, Block, Run, Accelerate, Brake, Gear, Trigger, Reload, Pedal, Spinner, Trackball, Steering wheel, Stick.
- The generator counts label frequency and writes `buttonmap/arcade_terms.txt`; the terms join `uiStrings()` (P:12053) in a new arcade group; `i18n_check` enforces all 21 translation files (P: 21 `ps3xmb/i18n/*.json`) **[C]**; a unit test checks that `arcade_terms.txt` ⊆ `uiStrings()`.
- `bmap::translate` matches whole strings only; anything else stays as is. RTL goes through NanoBidi as today.
- New UI strings: the coin hint, "Player %s", "4-way stick", "2-way stick", "Spinner: left stick", "Aim: right stick", "Needs a keyboard", "Not supported on a controller", "MAME input settings: %s", "Button profiles off (core option)", and the badge sources above.

### 7.9 Cost on the device

- Lookup: about 20 `pread`s of 4 KB, under 2 ms; a MAME cfg read under 1 ms **[I]**. Nothing stays in memory.
- New SVGs 1-2 KB each; `controllers.json` grows about 30 KB **[I]**. Rendering unchanged (one raster per open).

---

## 8. Cores and emulators covered

| Core / emulator | Shipped | Coverage | How |
|---|---|---|---|
| FinalBurn Neo (`fbneo`) | yes | full, phase 2 | Harness table per device; names from the drivers |
| libretro MAME (`mamearcade`) | yes (default) | layouts phase 1, names phase 3 | Profile table + `-listxml`; `PORT_NAME` parser; MAME cfg reader |
| MAME 2003-Plus (`mame2003_plus`) | yes | phase 1 (layouts), phase 3 (names, D5) | `controls.c` counts + device tables; MAME 0.289 names |
| Flycast NAOMI / Atomiswave | `flycast` core ships (`retroarch-cores.txt:44`); NAOMI/AW aren't Nano systems | existing fixed row (`button_map_overlay_plan.md` §3.1) | Same pattern later, from Flycast's NAOMI input tables (phase 6) **[I]** |
| MAME4droid 2024 (standalone) | no (user-installed; offered per game, P:17677-18100; BIOS slot, P:5690-5712) | none until phase 6 ("no map", as today) | Phase 6: the MAME table plus its MAME XML cfg with the same parser; paths **[U]** |
| MAME 2010/2003/2000, HBMAME, FB Alpha 2012, Geolith, DICE, Supermodel, NEO.emu, SUPER3 | no | none | "No map" or the system's generic row |

---

## 9. Generators and keeping them in sync

- Location: `tools/arcade-inputs/` in the build repo: `mame_listxml.py`, `mame_profiles.py` (parses `input_retro.cpp`), `mame_portnames.py` (phase 3), `fbneo_dump.cpp` + `build_fbneo_dump.sh` (phase 2), `mame2003p.py`, `merge.py` (writes the TSVs).
- Run from `scripts/refresh_cores.sh` when `mods/retroarch-cores.lock` changes, on the host, never while an AOSP build runs.
- **FBNeo harness** (phase 2): compile FBNeo's `burn` drivers and `src/burner/libretro/` at the shipped commit, without the `link.T` export list (tens of minutes; host only, never during an AOSP build). No ROMs are needed: `retro_load_game` builds the inputs before any ROM is touched (`nMaxPlayers = BurnDrvGetMaxPlayers(); SetDefaultDeviceTypes(); InputInit(); SetControllerInfo();`, `libretro.cpp:2263-2272`; `BurnDrvInit` comes only at `:2417`) **[C]**. For each arcade driver, with a stub `environ_cb` capturing `RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS`:
  1. `nBurnDrvActive = i; nMaxPlayers = BurnDrvGetMaxPlayers(); SetDefaultDeviceTypes(); InputInit(); SetControllerInfo();` (`SetControllerInfo` sets `nMaxControllers`; without it `retro_set_controller_port_device` ignores the port, `retro_input.cpp:3582`) **[C]**.
  2. For Classic, Modern and 6-Panel: `retro_set_controller_port_device(0, id)`, then call `RefreshControllers()` directly. It is static, and the port call runs it only once `bControllersSetOnce` is set (`:3248-3257`, `:3586`) **[C]**, so the harness is compiled into the same unit or the function is exposed for it.
  3. Skip the load path's `BurnDrvIsWorking` check (`libretro.cpp:2240`), so non-working sets are covered too **[C]**.
  Fork per driver so a crash costs one game. Gun games come out wrong without a running driver (`BurnGunIsActive`); class them from their input names instead **[I]**. The harness source is published with the build repo, as FBNeo's licence requires for changes to its code **[C]**.
- **MAME names** (phase 3): a Python parser of `INPUT_PORTS_START` blocks (`PORT_START`, `PORT_INCLUDE`, `PORT_MODIFY`, `PORT_BIT`, `PORT_NAME`, `PORT_PLAYER`). 7,643 of 17,507 `IPT_BUTTONn` lines carry a `PORT_NAME` across 595 files **[C]**; the parser should be 85-90% right and misses device-slot ports **[I]**. It is checked against FBNeo's names for sets both have (test only). An exact alternative is a ~30-line patch to MAME's `infoxml.cpp` emitting `<field name type player>`, which needs a full MAME compile (hours) once per MAME version **[I]**; open question Q2.
- **Which upstream commit a core is:** read from the core zip as strings, never run: FBNeo `library_version` ("v%x.%x.%x.%02x GIT_DATE GIT_VERSION", `libretro.cpp:464-477`), MAME `build_version`, mame2003-plus `GIT_VERSION` (`mame2003.c:177-180`) **[C source]**. Whether buildbot nightlies fill these in is unknown **[U]**.
- **Drift guard:** each TSV header carries the lock SHA-256 (of the `.so.zip`) and the size and SHA-256 of the unzipped `.so`. `mods/10-update-emulators.sh` fails when a shipped core's lock hash and its table's header differ, with an `ALLOW_STALE=1` override. On the device, Nano compares only the installed `.so`'s size with `#soSize` (§7.7). FBNeo's special cases change often, so the tables are regenerated with every lock refresh **[I]**.

---

## 10. Phases

### Phase 0: fix what is wrong now (1 day, low risk)

- `retropad.json` fbneo Neo Geo variant: R1 "AB", L1 "CD", R2 "ABC", L2 "BCD" (Classic) **[C]**.
- Badge: show "Per-game layout" even when an override or remap note exists (join two notes, or put it first).
- `button_map_overlay_plan.md` §3.1: FBNeo Modern = 517 (261 is 6-Button Panel); MAME 2003-Plus 257 is "Fightstick".
- `controller_layout_plan.md` §10 CPS row: "HP R1, HK L1" is wrong; FBNeo Classic and the MAME profile give HP = L1, HK = R1 (`retro_input.cpp:65-66`, `input_retro.cpp:521-527`) **[C]**. The Neo Geo row needs updating after D2.
- Read the payload's `retroarch.tar.zst` for existing arcade `.opt`/`.rmp` files (read only).
- Owner decides D2 (and D1, D5) before phase 1 starts (§6.3).
- Tests: the arcade golden scene updated for the Neo Geo shoulders; a badge test with an override note.

### Phase 1: data, lookup, MAME layouts, D1 (4-5 days, low risk)

- Generators: `mame_listxml.py`, `mame_profiles.py` (the whole if / else-if chain in order, first match wins, name and parent), `mame2003p.py` (device tables, counts), `merge.py`; TSV format; licence headers. Phase 1 places buttons only for stick and buttons-only games; dial, paddle, pedal, gun and trackball games get their class and drawing but numbered buttons stay generic until phase 3's port parser (listxml's `buttons` is a count for them, §3.1).
- Nano: `NanoButtonMapArcade` lookup; reader composition (§7.2) with class words and "Button n"; `mame_buttons_profiles` in `optionKeys`; data dir in `ReaderEnv`.
- D1: `MAME.opt` in the payload and the one-time overwrite migration (§6.3).
- Tests (host): lookup (hit, clone → parent, miss, malformed line, unsorted file rejected); MAME profile on/off for `sf2`, `kof98`, `samsho2` (layout B), `samsho4` (layout C), `mk2`, `tekken3`, `pacman`; migration (no `.opt`, `.opt` with `disabled`, already migrated, user's per-game `.opt` untouched); generator unit tests on a small listxml and an `input_retro.cpp` excerpt; T2 extended: every `perGame` core has a table; goldens: `sf2` under MAME with profiles on and off, `kof98` under MAME.

### Phase 2: FBNeo table (4-5 days, medium risk: harness)

- `fbneo_dump.cpp` harness; Classic/Modern/6-Panel layouts and names; drift guard in `10-update-emulators.sh`.
- `NOTICE`: FBNeo licence text (for the core too); `LICENSE.fbneo.txt` beside the data.
- Tests: harness output compared with hand-checked games (`sf2`, `ssf2t`, `kof98`, `mslug`, `mk`, `kinst`, `robotron`, `outrun`); device switching from a fixture `.rmp` (5/261/517), including Neo Geo under 6-Panel; golden `sf2` under FBNeo Classic and Modern.

### Phase 3: names for MAME and MAME 2003-Plus; vocabulary (3-4 days, medium risk: parser accuracy)

- `mame_portnames.py` with a cross-check report against FBNeo names; it also gives button positions for dial, paddle, pedal, gun and trackball games (e.g. OutRun's gear on button 3). MAME 2003-Plus names from the same parser by set name (D5).
- `arcade_terms.txt` and the translations; `i18n_check`.
- Tests: parser fixtures (`PORT_INCLUDE`, `PORT_MODIFY`, missing names); label clean-up; long-label layout test.

### Phase 4: drawings and panel text (3-4 days, low risk)

- New SVGs (§7.4) and `controllers.json` entries; header notes; coin hint; P2 note; FBNeo service row.
- Tests: layout tests at all sizes for each new drawing; goldens for twin stick, dial, driving, light gun and a mahjong table.

### Phase 5: Thimsy-owned remaps, settings, MAME cfg (4-5 days, medium risk: user files)

- Launch-time writer (§6.2) with the sidecar; Neo Geo layout setting; Game Options "Arcade layout".
- MAME `cfg` reader (§7.5).
- Tests: writer never touches a user file at any of the three levels; an owned game file is deleted when a user folder or core remap appears; reset deletes only owned files; content-hash mismatch leaves the file alone; badge "Thimsy's layout" vs "From your remap file"; cfg inversion through each profile; a 256 KB cap test.

### Phase 6 (optional, 6-10 days)

- Light gun on the touch screen (FBNeo 262 / MAME `touchscreen`) only when the game is on the Cube's screen; on a monitor (any mode, Dual Screen included) touch guns are unsupported and the stick aims. Check Nano's two-finger gestures don't clash with FBNeo's two-finger "button 2".
- Turbo on R1 per game, in the Thimsy-owned per-game `.rmp` (1.22.2 reads the turbo keys from it, §6.2) **[C]**; no override `.cfg` needed.
- Mahjong touch panel (a Thimsy-drawn RetroArch overlay sending the cores' keyboard keys).
- MAME4droid cfg reader; Flycast NAOMI tables.
- An upstream libretro/mame change so the core sends real descriptors (then RetroArch's own Controls menu shows names too).

### 10.1 On-device test steps

For each game: launch it from Nano, open the map (Anbernic + bottom button), compare with the game's own behaviour, and for FBNeo and MAME 2003-Plus with RetroArch Quick Menu > Controls; for MAME with its own input menu (Tab via the keyboard or MAME's menu).

FBNeo rows launched from the `arcade` folder need FBNeo picked as that game's emulator (the folder defaults to mamearcade). "From" is the first phase whose build can run the row.

| From | Class | Games (set names) | Cores | Check |
|---|---|---|---|---|
| 0 | Neo Geo shoulders, badge | `kof98` with a RetroArch override saved | fbneo | AB R1, CD L1, ABC R2, BCD L2; "Per-game layout" still in the badge |
| 1 | 1-4 buttons, 8-way | `ffight` (listxml says 3 buttons, the `cps1_3b` ports), `1942` | fbneo, mamearcade | Attack S, Jump E |
| 1 | 6-button fighter | `sf2`, `ssf2t`, `sfa3`, `sfiii3` | all three (mame2003_plus from phase 5) | LP W, MP N, HP L1, LK S, MK E, HK R1; FBNeo 3xP L2 |
| 1 | 6-button fighter not in MAME's lists | `taotaido` (FBNeo names its inputs Weak … Strong Punch/Kick, `d_taotaido.cpp:90-95`; in no MAME profile) **[C]**; MAME's button count **[U]** | fbneo, mamearcade | FBNeo SF layout; MAME numbered; each map matches its core |
| 1 | Neo Geo | `kof98`, `mslug` | fbneo (neogeo folder, and arcade folder with FBNeo picked), mamearcade | D2 layout (FBNeo from phase 5 if mini); FBNeo macros on the shoulders |
| 1 | Neo Geo, MAME layout B/C | `samsho2`, `samsho4` | mamearcade | map shows the layout B / C order; after phase 5, the D2 layout |
| 1 | Mortal Kombat | `mk2`, `umk3` | fbneo, mamearcade | Block L1, Run R1 |
| 1 / 2 | Killer Instinct | `kinst` | mamearcade (1), fbneo (2) | MAME profile layout; FBNeo numbered B A Y X R1 L1 |
| 1 | Tekken | `tekken3` | mamearcade | 4-limb diamond |
| 1 | 4-way / 2-way | `pacman`, `dkong`, `qbert`, `galaga` | fbneo, mamearcade | "4-way stick" / "2-way stick" note (from phase 4) |
| 1 | Twin stick | `robotron`, `smashtv` | fbneo, mamearcade | right stick fires |
| 2 / 3 | Driving | `outrun` | fbneo (2), mamearcade (3) | R2 gas, L2 brake; gear R1 under FBNeo, Y under MAME |
| 1 | Dial | `arkanoid`, `tempest` | fbneo, mamearcade | left stick spins |
| 1 | Trackball | `centiped` | fbneo, mamearcade | left stick rolls |
| 1 | Light gun | `opwolf`, `ptblank` | fbneo, mamearcade | stick aim; touch in phase 6 |
| 2 | Shmup | `ddonpach`, `batsugun` | fbneo | Shot S, Bomb E |
| 1 | 3-4 players | `tmnt`, `gauntlet` | fbneo, mamearcade | player 1 panel; P2 note (from phase 4); a second pad's Coin/Start work |
| 1 | Mahjong | `mjsister` | mamearcade | table + "needs a keyboard" |
| 1 | Not in the table | a homebrew zip in `fbneo` | fbneo | generic row, badge first |
| 1 | D1 migration | `sf2` on an install with no `MAME.opt`, one with `disabled`, and one with a user per-game `sf2.opt` | mamearcade | first two: profiles on after the update; the per-game `.opt` wins and the map says profiles off |
| 1 | User remap | `sf2` with a RetroArch game remap saved | fbneo | map follows the remap; Thimsy writes nothing |
| 1 | Device type | `sf2` with Device Type = Modern saved | fbneo | HP R1, HK R2 |
| 1 | External pad, ABXY Swap | `sf2` with an Xbox pad (chord pressed on it); then the Cube with ABXY Swap on | fbneo | map drawn for the pad that pressed the chord; positions, not letters, keep the layout |
| 1 | 6-button external pad | `sf2` with a 6-button pad (e.g. a Saturn-style USB pad) | fbneo | map follows that pad's positions (Q5) |
| 4 | Monitor | `sf2` and `robotron` in External Only, Mirror and Dual Screen | fbneo | map on the controller's screen, centred, nothing clipped |
| 5 | Owned remap lifecycle | `kof98` with D2 = mini: launch (owned 517 file written); save a core remap in RetroArch; relaunch; Game Options "Arcade layout" reset | fbneo | owned file written, badge "Thimsy's layout"; user's core remap wins and the owned file is gone; reset removes only owned files |
| 5 | MAME cfg | `sf2` with a button changed in MAME's menu | mamearcade | map follows `sf2.cfg` |

---

## 11. Corrections to other documents (to apply in phase 0)

| Document | Line / section | Correction |
|---|---|---|
| `controller_layout_plan.md` | §10, CPS row (`:558`) | HP = L1, HK = R1 (not R1/L1) **[C]** |
| `controller_layout_plan.md` | §10, Neo Geo row (`:557`) | Update after D2 |
| `button_map_overlay_plan.md` | §3.1, device subclass list | FBNeo Modern 517, 6-Panel 261; MAME 2003-Plus Fightstick 257 **[C]** |
| `button_map_overlay_plan.md` | §3.1, "Which core is running" | `GET_STATUS` does carry the content's base name (`command.c:1034-1072`); still off and without descriptors **[C]** |
| `button_map_overlay_plan.md` | §3.1, fbneo evidence bullet | Neo Geo macros on the shoulders; MAME profiles (D1) |
| `retropad.json` | fbneo `neogeo` variant | Shoulder macros (phase 0) |

---

## 12. Risks

1. **Nightly drift.** The tables are right only for the pinned commits. Mitigation: lock hash in each header, the build check, regeneration with every refresh. Users who update cores in RetroArch get the "earlier version" note when the installed `.so`'s size differs from the table's `#soSize` and Nano can read RetroArch's core folder (§7.7); otherwise no note **[I]**.
2. **FBNeo harness feasibility.** Some drivers may expect init state; fork per driver and fall back to generic for failures. Gun detection needs class data **[I]**.
3. **MAME name parser accuracy** (~85-90%) **[I]**. Mitigation: FBNeo cross-check; generic names when unsure.
4. **D1 changes gameplay** for every MAME player, not only the overlay. Players used to today's numbered layout will notice; the one-time migration overwrites a `disabled` value RetroArch wrote itself (§6.3).
5. **D2 changes FBNeo's Neo Geo layout** for existing players if mini is chosen.
6. **User files.** A Thimsy per-game `.rmp` must never hide a user's folder or core remap (§6.2).
7. **Licences.** FBNeo's and MAME 2003-Plus's non-commercial terms bind the whole image (already true for the cores). FBNeo's "publish changes" covers the harness. D5 avoids copying `controls.c`, whose licence forbids distributing a modified version.
8. **Invisible state.** Unsaved RetroArch menu changes, MAME 2003-Plus binary cfg remaps.
9. **Long labels** push more rows into the table strip; layout tests must cover them.
10. **Digital L2/R2** give on/off throttle in driving games; nothing the overlay can fix.
11. **Compiles in the refresh pipeline** (phase 2, optional MAME) must stay out of AOSP build windows.

---

## 13. Owner decisions and open questions

### Decisions

| # | Question | Options | Recommended |
|---|---|---|---|
| D1 | Enable MAME button profiles? | Yes in `MAME.opt`, with a one-time overwrite of `disabled` on existing installs / yes for new images only / keep off / Batocera-style MAME cfg per launch | **Yes, with the one-time overwrite** (§6.3) |
| D2 | Neo Geo layout (decide before phase 1) | NEOGEO mini (A = Y, B = B, C = X, D = A) everywhere / keep FBNeo Classic (A = B, B = A, C = Y, D = X) | **NEOGEO mini**. Either way Thimsy-owned `.rmp` files even out the cores in phase 5: FBNeo device 517 if mini, MAME button remaps if Classic, plus the 11 MAME layout B/C titles in both cases |
| D3 | Build-time generated data as the source | Yes / no per-game data | **Yes** |
| D4 | Ship FBNeo-derived data under FBNeo's licence | Yes / names only for MAME | **Yes**, and add FBNeo's licence to `NOTICE` regardless |
| D5 | MAME 2003-Plus names | MAME 0.289 `PORT_NAME` names by set name / copy `controls.c` names (MAME 0.78 licence: no modified versions; controls.dat licence unknown) / counts only | **MAME 0.289 names by set name** |
| D6 | Never ship controls.ini, ArcadeDB, ScreenScraper, LaunchBox data | — | **Agreed** |
| D7 | Thimsy-owned per-game `.rmp` at launch | Only where the cores need evening out (device types and button keys, §6.1) / never (overlay only) | **Only where needed**, with its own badge "Thimsy's layout for this game" |
| D8 | P2 on the panel | Note only / a P2 page with two pads | **Note only** |
| D9 | Mahjong and similar | Table + "needs a keyboard" / touch panel (phase 6) | **Table now** |
| D10 | Touch light guns | Phase 6 / never | **Phase 6** |

### Open questions

1. Killer Instinct: MAME's SF-style profile (nothing to do) or Batocera's SNES-port layout (Quick P/K on the shoulders)? And should a Thimsy `.rmp` give FBNeo's numbered `kinst` the same layout as MAME?
2. MAME names: Python parser (recommended) or a patched host MAME compile (exact, hours per MAME version)?
3. May MAME 2003-Plus or FBNeo names label `mamearcade` games when MAME has no `PORT_NAME` (marked "names from FinalBurn Neo")?
4. A P2 page when a second pad is connected?
5. A global "Fight pad" option (FBNeo Modern, MAME 2003-Plus Fightstick), possibly automatic for 6-button external pads?
6. May an ES-DE `<controller>` entry override the panel class?
7. MAME service/test: which key should a Nano "Service" action send, given F2 is RetroArch's save-state key?
8. Turbo: off with a per-game toggle, or on for shmups?
9. Should the default arcade core move to FBNeo for the games it covers? Not the overlay's call, but FBNeo gives RetroArch real names and MAME doesn't.
10. Offer libretro/mame an upstream change so the core sends descriptors?
11. To verify in source or on the device: per-game `.opt` replaces the core `.opt`; the MAME cfg path on the Cube; whether nightlies embed `GIT_VERSION`; whether the payload already has arcade `.opt` files; whether Nano can `stat` RetroArch's installed cores; whether `mame_rotation_mode` turns stick directions; how MAME 2003-Plus handles twin sticks, driving and guns. (Settled: a device-only `.rmp` leaves the buttons alone, §6.2 **[C]**.)

---

## Sources

- FBNeo: https://github.com/finalburnneo/FBNeo (`src/burner/libretro/retro_input.cpp`, `retro_input.h`, `retro_common.cpp`, `src/license.txt`); docs https://docs.libretro.com/library/fbneo/
- libretro MAME: https://github.com/libretro/mame (`src/osd/modules/input/input_retro.cpp`, `src/osd/libretro/libretro-internal/libretro.cpp`, `libretro_core_options.h`, `retro_init.cpp`)
- MAME: https://github.com/mamedev/mame (`src/frontend/mame/infoxml.cpp`, `src/emu/inpttype.ipp`), `mame0289lx.zip` from the 0.289 release; cfg format https://docs.mamedev.org/advanced/ctrlr_config.html
- MAME 2003-Plus: https://github.com/libretro/mame2003-plus-libretro (`src/controls.c`, `src/mame2003/mame2003.c`); docs https://docs.libretro.com/development/cores/core-specific/mame-2003-plus/
- RetroArch v1.22.2: `command.c`, `runloop.c`, `verbosity.c`, `config.def.h`, `configuration.c`
- MAME 2003-Plus licence: https://github.com/libretro/mame2003-plus-libretro/blob/master/LICENSE.md
- FBNeo Killer Instinct driver: `src/burn/drv/midway/d_kinst.cpp`; MAME OutRun ports: `src/mame/sega/segaorun.cpp`
- ES-DE controller badges: https://gitlab.com/es-de/emulationstation-de/-/raw/master/es-core/src/components/BadgeComponent.cpp
- Batocera (reference only, LGPL-3.0+): `batocera_launch_libretro_mame`, `mame_controls.toml`, `roms.toml`
- controls.dat lineage: https://github.com/kpshek/ledblinky-controls-colors; ArcadeDB terms https://adb.arcadeitalia.net/service_scraper.php
- Community practice: https://knulli.org/configure/retroarch/controls/, https://github.com/christianhaitian/arkos/wiki/ArkOS-Emulators-and-Ports-information, https://retropie.org.uk/forum/post/276034
