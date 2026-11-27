# Refresh matching for games (per-game monitor refresh rate)

Status: proposed 2026-11-27, revised the same day after two technical reviews. Planning only: no code was changed, nothing was built, and no device was touched. Every technical claim is marked **[C]** (confirmed from source, a binary, the Thimsy patch, a device log or arithmetic on sourced constants), **[I]** (inferred) or **[U]** (unverified).

Request (owner, 2026-11-27): "Write a plan for how to support refresh matching for games. Targets would be 50hz, 70hz, and a few others for pal, ms dos, wonderswan and amiga plus others. It would need to automatically detect the desired refresh rate and detect if the monitor can support it."

Background: `out/dp-evidence/test43/freesync/FREESYNC.md` (why true FreeSync is out, and option A, which this plan expands), `display_modes_plan.md` (the mode table, `try`/`keep`, the applying marker), `dp_modes_feasibility.md` (pixel clocks), `dp_dock_findings.md` (the owner's monitor and docks). Working files for this plan are in `out/dp-evidence/test43/refresh/`: `rates.py` / `rates_out.txt` (content rates and skews), `refresh_timings.py` / `refresh_timings.txt` (timings, a reference EDID parser and gate classifier run on the two real EDIDs), and `src/` (the emulator and MAME source copies cited below).

## 1. Summary for the owner

### What you get

- When you start a game with a monitor attached, Thimsy switches the monitor to a refresh rate that suits that game, then switches it back when you return to Nano.
  - **First (phases 1-4):** PAL games play at **50 Hz** (or 100 Hz) on monitors that list those rates, as your KTC monitor does. Today they judder on a 60 Hz or 120 Hz monitor. This covers the PAL consoles Thimsy ships (NES, SNES, Mega Drive, Master System, PlayStation, Saturn, N64, Dreamcast, GameCube, Wii, PS2, MSX, Atari 7800) and Amiga in PAL once its core is added.
  - **Later (phase 5), PC monitors only:** MS-DOS games in the usual 320x200 VGA mode at **70 Hz**; WonderSwan (75.47 Hz) and Lynx (75 Hz) at **75 Hz**; arcade boards with odd rates at their own modes: Mortal Kombat and R-Type at **55 Hz**, Toaplan, Cave, Double Dragon and Sega Model 2 at **57 Hz**, Neo Geo and Taito F3 at **59 Hz**. These are not TV timings, so they work only on monitors that declare a continuous range of rates (most PC and gaming monitors, not TVs).
  - Everything else (NTSC consoles, handhelds, CPS1/2/3) already looks right at 60 or 120 Hz, so nothing switches.
  - MS-DOS and Amiga need their emulators added first; Thimsy ships neither today. C64, Spectrum, CPC, Atari ST, X68000 and the other computers in the tables below also need a core and a system entry (decision 8).
- Thimsy works out the rate by itself. It looks at the system, the game's region (from the file name or the ROM header), an arcade rate table, and what the emulator reported last time you played that game. You can override it per game in Game Options: Auto, Off, or a fixed rate.
- Thimsy works out what the monitor can show. It uses the monitor's EDID (the list of modes and rates the monitor says it accepts), and checks the link, the adapter and the Cube's own pixel clock. The first time a monitor gets a new rate, you confirm it ("Can you see this?") or it switches back by itself after 15 s.
- RetroArch adjusts the game's speed by a fraction of a percent to fit the exact rate, so there is no judder. With an exact match (within 0.7%) the pitch change is inaudible; a near match (0.7-1.5%, for example X68000 at -1.05%) shifts the pitch by up to a quarter of a semitone.

### What it will not do

- **No true FreeSync / VRR.** The Cube's DP display controller runs on a fixed clock. Its graphics driver uses an interface for which Android turns VRR off. Both of your display paths convert DP to HDMI. Details in `out/dp-evidence/test43/freesync/FREESYNC.md` §1 and §3 **[C]**.
- **Each switch blanks the monitor for 1-3 s**, once when the game starts and once when you return. Thimsy switches only when it helps. It switches just before it starts the game, so the first blank happens while Nano shows "Switching the monitor to 50 Hz..." **[I]**.
- **No change in the middle of a game.** DOS games that change video mode during play stay at the rate they started with. Thimsy remembers the rate the game settled on and uses it next time.
- **TVs that don't list the rates get nothing new.** Your Hisense TV lists no 50 Hz mode and doesn't declare a continuous range, so it stays at 60 Hz **[C]**.
- **Matching only at 1080p for now.** If you chose 1440p or 4K in Output Resolution, games are not matched: the new modes are all 1920x1080, and a match never changes the resolution, since that can restart the emulator. Game Options says so. 1440p50 and 4K50 are an optional later phase (phase 10).
- **No 140 or 150 Hz** (2 x 70, 2 x 75). At the Cube's 297 MHz pixel clock limit, 1080p tops out at 120 Hz with TV blanking and about 132 Hz with the tightest blanking the display controller accepts (297 MHz / (2000 x 1121)); 140 Hz would need about 314 MHz **[C arithmetic]**.
- **Exact rates such as 49.70 or 59.94 are not separate modes.** The driver reports whole-number rates, so the plan uses one mode per whole number, and RetroArch absorbs the small difference (section 6.4).
- **Games started outside Nano** (ES-DE as the home app, the Android launcher) are not matched until optional phase 8.

### What it costs

About 3-4 weeks of work for phases 1-6, plus 1-2 weeks for the optional phases (section 8). The first useful part, PAL games at 50 or 100 Hz, needs phases 1-4: about 8-13 days plus device tests, including a soak of about 200 switches. Each new driver build means a vendor_dlkm write, which needs your approval each time.

## 2. Which rates, and which games use them

### 2.1 How a near-match plays

"Skew" below is how far the display rate is from the game's own rate.

- **RetroArch** compares the display rate with the core's rate. If they are within 5% (`DEFAULT_MAX_TIMING_SKEW` 0.05, `src/ra_config.def.h:1525-1527`), it runs the game at the display rate and resamples the audio. The game's speed and pitch change by the skew, and there is no judder (`audio_driver_monitor_adjust_system_rates`, `src/ra_retroarch.c:1543-1571`) **[C]**. Beyond 5% it gives up ("Timings deviate too much", `:1679-1682`), and the game judders **[C]**.
  - It handles integer multiples only through the swap interval: RetroArch divides the display rate by `video_swap_interval` (and by the black-frame-insertion and sub-frame factors) before it compares **[C, same function; the review checked it against upstream v1.22.2 retroarch.c]**. At 100 Hz with swap interval 2, a 50 Hz game counts as 50. At 100 Hz with swap interval 1 it is 100% off and RetroArch makes no adjustment. Nano writes no `video_swap_interval` today (no hit in the patch or in `mods/`) **[C]**, so section 5.9 adds it.
- **Pitch.** A skew of 0.5% is 8.6 cents; 1% is 17 cents; 1.5% is 26 cents; 5% is 85 cents, almost a semitone **[C arithmetic]**. The plan treats up to 0.7% as **exact** (inaudible to almost everyone), 0.7-1.5% as **near** (acceptable), and above 1.5% as no match **[I, from common pitch-discrimination figures of roughly 5-10 cents]**.
- **Standalone emulators** (Dolphin, PPSSPP, DuckStation, NetherSX2, Flycast...) pace themselves. They gain only when the game's rate and the display rate are about equal: PAL 50 at 50 Hz, for example. With a small mismatch they repeat or drop one frame every 1/|difference| seconds: 49.70 on 50 Hz is one frame every 3.3 s **[C arithmetic]**.

### 2.2 The display rates to support

All at 1920x1080. Section 4.4 has the exact timings.

| Display mode | Status | Why | Who can get it |
|---|---|---|---|
| 1080p60 | exists | NTSC and Japanese consoles, handhelds, DOS mode X | every monitor |
| 1080p120 | exists | 2 x 60 | monitors that list it |
| **1080p50** | new, CEA VIC 31 | everything PAL | monitors that list VIC 31, or declare a range that covers it |
| **1080p100** | new, CEA VIC 64 | 2 x 50 | monitors that list VIC 64, or a range |
| **1080p55** | new, "game rate" | Mortal Kombat 1-3, R-Type / Irem M72, Toaplan's 55 Hz boards | monitors with a continuous range only |
| **1080p57** | new, game rate | Toaplan 1, Cave, Double Dragon, Sega Model 2 and System 18 | same |
| **1080p59** | new, game rate | Neo Geo, Konami GX, Taito F3 | same |
| **1080p70** | new, game rate | MS-DOS VGA 320x200 and text modes | same, and a link that carries 297 MHz |
| **1080p75** | new, game rate | WonderSwan, Lynx | same |
| 1080p56, 1080p72 | optional, later | NMK16, Jaleco Mega System 1, PC-98; Pokémon Mini, Atari ST mono (near at 72) | on demand |

So the recommendation is **seven new modes**: 50, 55, 57, 59, 70, 75 and 100. 58 Hz is not needed: 57 and 59 cover it. 50 and 100 are standard TV timings and the low-risk start; the five game rates are non-standard timings that need the monitor's range and a confirm on first use **[I]**.

### 2.3 Target rate per system

Columns: the system's own rate and its source; the display mode the plan picks; whether that is exact (≤0.7%), near (0.7-1.5%) or a multiple; and the result in RetroArch, against today's result at 60 Hz. Skews were computed for the timings in section 4.4 **[C arithmetic]**. Full table: `out/dp-evidence/test43/refresh/rates_out.txt`.

Which of these Thimsy ships today **[C, `mods/nano-systems.txt`, `mods/retroarch-cores.txt`]**: NES (default core Mesen, not fceumm; the PAL rate is the same 50.007 **[I]**), SNES, Mega Drive, Master System, Game Gear, Sega CD, 32X, PlayStation (Beetle PSX HW), Saturn (default yabasanshiro, not Beetle Saturn: its PAL rate is **[U]**), N64, Dreamcast, GameCube, Wii, PS2, PSP, NDS, 3DS, PC Engine, Virtual Boy, MSX, Atari 2600/5200/7800, Lynx, WonderSwan, Neo Geo Pocket, 3DO, arcade (MAME 0.289 `mamearcade`), FBNeo and Neo Geo (both FBNeo). **Not shipped:** MS-DOS, Amiga, C64, ZX Spectrum, Amstrad CPC, Atari ST, Atari 8-bit, Jaguar, X68000, PC-98, Pokémon Mini. Their rows below apply once a core and a system entry are added (decision 8).

**PAL consoles and computers (target 50; 100 also works as 2 x 50).**

| System | Own rate (Hz) | Mode | Match | In RetroArch at the matched mode | Today at 60 Hz |
|---|---|---|---|---|---|
| NES PAL (incl. Dendy) / SNES PAL | 50.007 [C, fceumm libretro.c:138; snes9x libretro.cpp:1153; I for Mesen, the shipped NES default] | 50 | exact | -0.01% speed, no judder | 20% off: judders (10 repeated frames a second) |
| Mega Drive / Master System PAL | 49.70 [C, gpgx libretro.c:3162] | 50 | exact | +0.60% (10 cents) | judders |
| PlayStation PAL | 49.76 progressive, 50.00 interlaced [I; Beetle switches between them, bpsx.c:7064-7065, C] | 50 | exact | +0.48% | judders |
| Saturn PAL | 49.92 [C for Beetle Saturn, bss.c:1794; U for yabasanshiro, the shipped default] | 50 | exact | +0.16% | judders |
| N64 / Dreamcast / GameCube / Wii / PS2 PAL | 50.0 [C for mupen64plus-next and Flycast; I for Dolphin, PCSX2] | 50 | exact | 0%; standalones gain too | judders |
| **Amiga PAL** | 49.920 [C, puae libretro-core.h:281]; 50.000 interlaced [C arithmetic] | 50 | exact | +0.16% | judders |
| C64 PAL | 50.12 [C, vice c64.h:35-40] | 50 | exact | -0.25% | judders |
| ZX Spectrum, Amstrad CPC, MSX PAL, Atari 8-bit / 7800 PAL, Atari ST 50 Hz, Jaguar PAL | 49.86-50.16 [C, sources in rates_out.txt] | 50 | exact | within ±0.3% | judders |
| Virtual Boy (all regions) | 50.27 [C, beetle-vb libretro.cpp:716] | 50 | exact | -0.54% | judders |

**Already fine at 60 (or 120 as 2 x 60): no switch.**

| System | Own rate (Hz) | Result at 60 / 120 |
|---|---|---|
| NES / SNES NTSC | 60.10 [C] | -0.16% |
| Mega Drive / SMS / Game Gear NTSC, MSX NTSC | 59.92 [C] | +0.13% |
| Game Boy / GBC / GBA | 59.73 [C, gambatte :124, mgba :1356] | +0.46% |
| PlayStation, PC Engine, C64, Saturn NTSC, NDS, 3DS | 59.83 [C/I, rates_out.txt] | +0.29% |
| **Amiga NTSC** | 59.83 [C, puae libretro-core.h:282] | +0.29% |
| PS2 / GameCube / Wii / PSP / Dreamcast NTSC, **DOS mode X and 640x480** | 59.94 [C/I] | +0.10% |
| CPS1 / CPS2 / Toaplan 2 | 59.64 [C, mame capcom/cps1.h:39-45] | +0.61% |
| CPS3, Raiden | 59.60 [C, cps3.cpp:2489; raiden.cpp:743] | +0.67% |
| Pac-Man, Galaga, Donkey Kong | 60.61 [C, pacman.cpp:3734] | -1.0% (near) |

**Odd rates (new game-rate modes).**

| System | Own rate (Hz) | Mode | Match | Matched result | Today at 60 Hz |
|---|---|---|---|---|---|
| Neo Geo MVS/AES | 59.19 [C, 6 MHz / 384 / 264; clocks in mame snk/neogeo.h:32-36, totals in the driver's screen setup; U for FBNeo, the shipped Neo Geo core] | 59 | exact | +0.04% | +1.38% speed (24 cents), audible to some |
| Konami GX | 59.19 [C, konamigx.cpp:1484] | 59 | exact | +0.04% | +1.38% |
| Taito F3 | 58.97 [C, taito_f3.cpp:536] | 59 | exact | +0.41% | +1.75% (30 cents) |
| Mortal Kombat 1/2/3/UMK3 (Midway Y/T/W-unit) | 54.71 [C, midyunit.cpp:1306] | 55 | exact | +0.31% | 9.7% off: judders |
| R-Type / Irem M72 | 55.02 [C, m72.cpp:1706] | 55 | exact | -0.25% | judders |
| Toaplan 55 Hz boards (Rally Bike, Out Zone...) | 55.16 [C, toaplan1.cpp:1853-1864] | 55 | exact | -0.51% | judders |
| X68000 (31 kHz games) | 55.46 nominal [C, px68k libretro.c:84-88] | 55 | near | -1.05% | judders |
| Toaplan 1 (Zero Wing, Truxton) | 57.61 [C, toaplan1.cpp:1853] | 57 | exact | -0.20% | +4.1%: RetroArch absorbs it, at 70 cents of pitch |
| Double Dragon | 57.44 [C, ddragon.cpp:937-962] | 57 | exact | +0.10% | +4.5% |
| Sega Model 2 / System 18 | 57.52 / 57.23 [C, model2.cpp:2530; segas18.cpp:1406] | 57 | exact | -0.04% / +0.46% | +4.3% / +4.8% |
| Cave 68k (DoDonPachi, ESP Ra.De.) | ~57.55 [U] | 57 | exact | ~0% | ~+4.3% |
| NMK16, Jaleco Mega System 1, PC-98 | 56.2-56.4 [C, nmk16.cpp:4361; megasys1.cpp:1933; pc9801.cpp:2115] | 57 (or the optional 56) | none at 57 (+1.9-2.3%); exact at 56 | stays at 60 unless 56 is added | +6.4-6.8%: judders |
| **MS-DOS VGA 320x200 (mode 13h) and text** | 70.086 [I, 25.175 MHz / 800 / 449; DOSBox computes it from the CRTC, vga_draw.cpp:2295, C] | 70 | exact | +0.01% | 14% off: drops 10 frames a second |
| Atari ST monochrome | 71.2-71.5 [C constants, Hatari video.h:37, 43; rates_out.txt:41-42 gives 71.29 or 71.48; U for vtotal 500 vs 501] | the optional 72 only | none at 70 (-1.55% to -1.93%, over the 1.5% limit); near at 72 (+0.73% to +1.12%) | stays at 60 unless 72 is added | judders |
| **WonderSwan / Color** | 75.47 [C, beetle-wswan libretro.c:33] | 75 | exact | -0.35% | 20% off; the core's own "60 Hz mode" drops every 5th frame [C, :94] |
| Atari Lynx (Beetle) | 75.0 [C, blynx.cpp:59, 484-486] | 75 | exact | +0.28% | 20% off |
| Pokémon Mini | 72.0 [C, pokemini.c:51] | 70 (or the optional 72) | none (-2.65%) | stays at 60; the core's 5/6 mode [C, :102] | the core's 5/6 mode |

The 75 Hz mode is placed between Lynx (75.00) and WonderSwan (75.47), at 75.21 Hz, so that both are under 0.5%. The 55 Hz mode sits between Mortal Kombat and R-Type (54.88 Hz) for the same reason, and the 57 Hz mode at 57.50 Hz for the Toaplan, Cave and Model 2 group **[C arithmetic]**.

## 3. Working out the rate a game wants

### 3.1 What exists today

- **No emulator on Thimsy tells the system its rate before the game starts** **[C]**:
  - RetroArch 1.22.2 (README.md:25) cannot ask Android for a display mode. That code reached RetroArch master on 2026-08-16, after 1.22.2 (commits 7c65f1cbe4, 11570a8584, ba5319a5ed, d5bba5d455 in `gfx/display_servers/dispserv_android.c`) **[C]**.
  - The standalones vote a fixed number whatever the game: Mupen64Plus FZ 59.94 even for PAL games (`m64_GameSurface.java:194`), Azahar the first 60 Hz mode (`EmulationActivity.kt:104`), PPSSPP about 60 (`PpssppActivity.java:922-945`). Dolphin votes nothing in the files read **[C]**.
  - Thimsy's monitor hold discards every app vote anyway (`DesiredDisplayModeSpecs(held, false, [hz,hz], [hz,hz])`, patch ~169265-169274) **[C]**.
- So the first switch for a game has to be **predicted** before the game starts. After that, Thimsy can **learn** what the emulator really ran at and use it next time.
- Nano already knows, at launch, the system (`sys.gammaos.nano.launch_system`), the ROM path (`launch_game`), the RetroArch core (`launch_core`) or the emulator package (`setLaunchGame`, NanoMenuUtils.h:145-153; `launchXmbGame`, NanoMenuXmb.cpp:3776; AOSP tree) **[C]**.
- Nano already keeps a per-game map keyed by ROM path, for the emulator choice: `/data/system/nano_game_emulators.json` (patch:41034) **[C]**. The per-game refresh setting follows the same pattern.
- Nano never writes `video_refresh_rate` into RetroArch's config today (no hit in the patch) **[C]**.

### 3.2 Priority order

The resolver returns a content rate in exact Hz and the reason it chose it. The first rule that gives an answer wins.

1. **Per-game override** in Game Options: Off, or a fixed rate.
2. **Learned rate** for this game with this emulator (section 3.4), if the last session settled on one.
3. **Arcade table**, by romset name, for MAME and FBNeo games.
4. **Region**, for systems whose rate depends on it (section 3.3), mapped onto the system table's PAL or NTSC row.
5. **System table**: the system's default (Amiga, C64, Spectrum, CPC, Atari ST default to PAL; DOS defaults to 70; everything else to its NTSC rate), adjusted for core options that change the rate.
6. **Nothing known**: no switch.

Nano shows the result in Game Options, for example "Auto: 50 Hz (PAL, from the file name)".

### 3.3 Sources

- **System table** (`/system/etc/thimsy/refresh/systems.json`, built into the image). One row per Nano system id and per core: NTSC rate, PAL rate, region default, and the core options that change the rate. Seed values: section 2.3 and `rates_out.txt`.
  - Rate-changing core options, read from RetroArch's per-core `.opt` files or `retroarch-core-options.cfg` **[I]**: `wswan_60hz_mode` (WonderSwan 60 Hz mode), the Lynx 60 Hz option, Genesis Plus GX and snes9x region overrides, `dosbox_pure_forcefps` (`dosbox_pure_libretro.cpp:2383-2384`, C), PUAE's video standard.
- **Packages table** (`packages.json`): standalone package to system, and whether its own vote means anything. Mupen64Plus FZ, Azahar and PPSSPP are marked "vote is constant" **[C, section 3.1]**.
- **Arcade table** (`mame_refresh.bin`). Generated at build time on the host from the `<display refresh="...">` attribute in MAME's `-listxml` output for MAME 0.289, which the shipped `arcade` default (`mamearcade`) matches **[C that the attribute exists, MAME infoxml.cpp; C for the core, mods/retroarch-cores.txt:54]**. Store only sets whose rate is more than 0.7% from 60, which should be a few thousand rows **[I]**.
  - The `fbneo` and `neogeo` systems run FBNeo, which defines its own rate per driver, and `mame2003_plus` (optional) carries MAME 0.78 rates **[C for the cores; U for how far their rates differ from 0.289's]**. Keep one table per core family: MAME 0.289 for `mamearcade`; a FBNeo table generated from FBNeo's driver sources at build time **[U, extraction method to be worked out]**, falling back to MAME's rate for the same short name **[I]**; the learned rate (section 3.4) corrects any wrong entry from the second launch. A missing set falls back to "no switch".
- **Region from the file name** (cheap, first):
  - No-Intro / Redump: the first bracket group. Europe, Germany, France, Spain, Italy, UK, Australia, Scandinavia and the like mean PAL. USA, Japan, Korea mean NTSC. Brazil is PAL-M, which is 60 Hz. "World" and "USA, Europe" mean NTSC, unless the core picks otherwise **[I]**.
  - GoodTools: (E) is PAL; (U), (J) and (UE) are NTSC. TOSEC: explicit (PAL) / (NTSC), common in Amiga, C64 and ST sets. WHDLoad names ending `_NTSC` mean NTSC **[I]**.
- **Region from the ROM header** (cartridges only; cheap; used when the file name says nothing):
  - NES 2.0 byte 12 bits 0-1 (0 NTSC, 1 PAL, 2 multi, 3 Dendy). Dendy maps to 50 Hz.
  - SNES internal header region byte: bus address 0xFFD9, which is file offset 0x7FD9 for LoROM and 0xFFD9 for HiROM, plus 0x200 when the file has a copier header (file size mod 1024 = 512).
  - Mega Drive region field at 0x1F0: old-style letters ("E" alone means PAL) or the newer single hex digit (bit 3 = Europe; a digit with only bit 3 set means PAL).
  - N64 country byte 0x3E (D, F, I, P, S, U, X, Y mean PAL) in big-endian `.z64` order. `.v64` (16-bit byte-swapped) and `.n64` (32-bit little-endian) files must be un-swapped first, detected from the first word (0x80371240 for `.z64`).
  - [I] for the field meanings; disc serials (SLES/SCES for PS1/PS2, the GameCube disc id's 4th letter) are left for later, since reading a CHD is slow.
- **Fixed-rate systems** need no region: GB, GBC, GBA, WonderSwan, Neo Geo Pocket, Virtual Boy, Pokémon Mini, NDS, 3DS, PSP.

### 3.4 Learning what the emulator really ran at

This catches what prediction can't: DOS games in mode X, X68000 CRTC variants, PlayStation and Amiga interlace changes, a forced region, a "60 Hz mode" core option.

- **RetroArch log** (phase 6). RetroArch logs "[Core] Geometry: ..., FPS: x" once per load (1.22.2 runloop.c:4665 with %.2f; master :6964 with %.4f), and "[Environ] SET_SYSTEM_AV_INFO ... FPS" on every change (1.22.2 runloop.c:2697) **[C]**. These need `log_verbosity = "true"`, since RARCH_LOG returns early without it (verbosity.c v1.22.2:406-416) **[C]**. Nano would set it in the per-launch write of section 5.9 (new work) **[I]**. Nano runs as root, so it can read logcat tag RetroArch **[I]**. The parser accepts both formats.
- **SurfaceFlinger content vote** (later). SurfaceFlinger still records layer history while the monitor is held (`Scheduler::recordLayerHistory` gated on `canSwitch()`, Scheduler.cpp:757-761, true with more than one mode; `LayerHistory::summarize`, LayerHistory.cpp:191-235) **[C]**. A small hook can publish the focused layer's vote as `sys.gammaos.sf.content_vote`. It covers standalones. A cadence measurement is only meaningful when the display runs faster than the game, so trust it only from a 120 Hz base or when it is at least 3% below the active rate **[I]**.
  - Caveat: it needs content detection. Without `kContentDetection`, `getVoteType` gives every layer NoVote (LayerHistory.cpp:89-100), so `summarize` returns nothing **[C]**. The feature is off when `persist.gammaos.refresh.lock` is set or `ro.surface_flinger.use_content_detection_for_refresh_rate` is false (SurfaceFlinger.cpp:5802-5811) **[C]**; that property is set from `persist.sys.phh.dynamic_fps` (device/phh/treble/vndk.rc:127) **[C]**, otherwise it defaults to the FRO feature flag **[U]**. Phase 0 records these three props on the device.
- **Rule.** Watch the first 15-20 s of the session, take the dominant value, and store it (per ROM path, emulator and rate-affecting core options) only if it differs from the prediction by more than 0.5%. Store in `/data/system/nano_game_refresh.json` beside the override **[I]**.
- **No mid-game switch by default.** An opt-in "Follow the game's rate during play" could re-match after 5 s of a stable new rate, at the cost of an extra blank (decision 10).

### 3.5 Choosing the display mode

The mode side runs in system_server, so Nano and later the framework use the same rule.

- Candidates: the monitor's modes at the **current size** that the kernel listed (the kernel's EDID gate already decided which records the monitor may get, section 4.3), that are not marked failed for this monitor, and that this monitor has confirmed or that are being confirmed now. Phases 3-4 rely on the kernel gate alone and always confirm; the framework's own EDID labels (LISTED or RANGE) arrive in phase 5.
- For each candidate rate R and multiplier m in {1, 2}: skew = R / (m x content) - 1.
- **Only switch when it helps.** If the current mode's skew is within 0.7%, stay. Examples: a 59.94 Hz game on a monitor at 1080p120 stays at 120; a 50 Hz game on a monitor already at 100 stays.
- Otherwise pick the smallest |skew|. Prefer m = 1 on a tie, unless the m = 2 mode is the current one.
- Accept up to 1.5%. Above that, stay at the normal mode. That is today's behaviour, so a mismatch is never worse than now.
- Never change resolution for a match (decision 5). A resolution change is an Android configuration change and can restart the emulator **[I, display_modes_plan.md Step 4]**. So with Output Resolution at 1440p60, 4K30 or 4K60 there are no candidates and nothing is matched until phase 10 adds rates at those sizes.
- No match while RetroArch is still alive from an earlier session (Keep Running in Background, Quick Resume): it would not read the new `video_refresh_rate` (section 5.9), and it would run its audio timing for the old rate **[I]**.

### 3.6 Settings in Nano

- **Game Options > Monitor Refresh**: Auto (shows the resolved rate and its source) / Off / a fixed rate. The fixed rates offered are the confirmed rates of the monitor attached now, plus the rate stored for this game (if any) so it stays visible. With no monitor attached, the row shows Auto / Off and the stored choice; it takes effect next time a monitor is attached. Stored per game, keyed like `nano_game_emulators.json`.
- **Settings > Display Settings > External Display**:
  - **Match Game Refresh Rate**: Off / On (decision 3).
  - **Test Game Refresh Rates**: runs the first-use confirm for every rate the monitor offers, one after another (section 4.5).
  - Forget Monitor Resolutions also forgets the confirmed and failed rates.
- **All the new modes, 50 and 100 included, are hidden from Output Resolution and never chosen by Auto.** Otherwise Auto might settle on 59 Hz **[C that Auto ranks 1080p near 60 within 5 Hz first, patch ~168700-168760]**. Output Resolution already lists only its fixed entries (4K60 4:2:0, 4K30, 2560x1440, 1080p120, 1080p60, 720p60) plus a kept mode that is none of them (`monmode::entryChoices`, patch:60979-61000; menu comment at patch:35784-35788) **[C]**, so the Nano side is mostly done. The new work is in the framework's Auto and any other user of `offered()`. While a game holds a mode, Nano must treat the hold's REVERT mode as the current one, or `entryChoices` would list the held 1080p50 as a kept choice **[I, patch:35791-35793]**.

### 3.7 What Game Options shows

The Monitor Refresh row's value line, under Auto **[I, wording to be settled in Nano]**:

| Case | Text |
|---|---|
| Matched | "Auto: 50 Hz (PAL, from the file name)" |
| No switch needed | "Auto: 60 Hz, no switch needed" |
| Wanted rate not offered by this monitor | "Auto: 50 Hz wanted; this monitor can't show it, stays at 60" |
| Rate failed its confirm | "Auto: 50 Hz wanted; it failed on this monitor, stays at 60" |
| Not yet confirmed | "Auto: 50 Hz (asks to confirm the first time)" |
| Size without matching | "Auto: off at 1440p and 4K; choose 1080p in Output Resolution" |
| Mirror | "Auto: off in Mirror" |
| No monitor attached | "Auto: 50 Hz when a monitor is attached" |
| Nothing known | "Auto: no rate known for this game" |
| Match Game Refresh Rate off | "Off in Display Settings" |

## 4. Can the monitor show it

Four things must all hold: the Cube can make the pixel clock, the DP link carries it, the converter carries it, and the monitor accepts it.

### 4.1 The Cube's pixel clock

- The pixel clock is VCO / 9 / k with k from 1 to 16. Clocks that divide 297 MHz keep the pixel PLL at 2673 MHz, the only rate ever seen to work. 297, 148.5 and 74.25 MHz (k = 1, 2, 4) have shown a picture **[C, dp_modes_feasibility.md §2; dp_dock_findings.md:864]**.
- So every new mode keeps a proven clock (148.5 or 297 MHz) and changes only the totals. No PLL retune is needed **[C arithmetic]**.
  - 1080p50 and 1080p100 use the standard CEA VIC 31 and VIC 64 timings: htotal 2640, vtotal 1125, line rate 56.25 or 112.5 kHz.
  - The game-rate modes (55, 57, 59, 70, 75 and the optional 56, 72) keep the CEA 1080p60/120 horizontal timing (htotal 2200) and only lengthen the vertical front porch. That keeps the line rate at 67.5 kHz (148.5 MHz) or 135 kHz (297 MHz), the line rates the converters already carry for 1080p60 and 1080p120 **[C]**. FreeSync monitors expect rate changes made this way **[I]**.
- The limit is 297 MHz outside bypass (`dpu_clk_init` 0xbe94) **[C]**. Hence no 1080p140/144.
- Not used: the 178.2 MHz (70 Hz) and 190.93 MHz (75 Hz) clocks one research pass suggested. They are not 297/k, so they move the PLL to a rate that has never run **[C arithmetic]**.

### 4.2 Link and converter

- HBR2 x 2 lanes carries 360 MHz at 24 bpp. 297 MHz uses 82.5% and 148.5 MHz uses 41% **[C arithmetic]**.
- On a link that trains only HBR x 2 (180 MHz), the existing link check drops the 297 MHz modes (70, 75, 100) automatically; 50, 55, 57 and 59 remain **[C, display_modes_plan.md Step 1A]**.
- The Anker 310 has carried 297 MHz (1080p120 to the H27S17, 4K to the Hisense) **[C, dp_dock_findings.md:907, 918, 921]**. The VIA dock has only been shown at 148.5 MHz **[C]**; its limit above that is **[U]**. A converter's TMDS limit is in its DPCD (0x080 + 4n, byte 1), which the driver already caches but never logs; FREESYNC.md option C1's printk would show it **[I]**.
- A converter passes the monitor's own EDID through **[C for the Anker, dp_dock_findings.md:109-115; I, strong, for the dock]**.

### 4.3 The monitor: two EDID gates

- **LISTED**: the EDID lists that exact timing. 1080p50 is CEA VIC 31 and 1080p100 is VIC 64 (2640x1125, CEA horizontal porches 528/44/148) **[C, drm_edid.c android12-5.4]**.
- **RANGE**: the EDID's range-limits descriptor (tag 0xFD in the base block) covers the mode's vertical rate, line rate and pixel clock, **and** the EDID declares continuous frequency (feature byte 0x18, bit 0) **[I, EDID 1.3/1.4 semantics]**. Without bit 0 the range only describes the listed timings, so the gate stays closed.
  - In EDID 1.3 bit 0 is named "default GTF supported" and the range descriptor has no +255 offsets; in 1.4 it means "continuous frequency" and the offset flags (descriptor byte 4) apply. The gate treats bit 0 the same in both and applies offsets only for version 1.4 **[I]**. The H27S17's EDID is version 1.3 (real_edids_vrr.txt:2) **[C]**.
  - The H27S17's AMD FreeSync block declares 60-144 Hz, so 55, 57 and 59 sit below its VRR minimum but inside its range descriptor (48-144 Hz). Whether the monitor treats them differently is **[U]**; phase 5's device test answers it.
- Policy: the 50 and 100 Hz modes open on LISTED or RANGE. The game-rate modes (55, 57, 59, 70, 75) open on RANGE only. No EDID: unchanged (720p60 and 1080p60 only).

What that gives on the owner's displays **[C, refresh_timings.txt; real_edids_vrr.txt:6, 10, 33]**:

| Display | EDID facts | 50 | 100 | 55 / 57 / 59 | 70 / 75 |
|---|---|---|---|---|---|
| KTC H27S17 (HDMI input, through the Anker or the dock) | VIC 31, 64, 63; range V 48-144 Hz, H 30-240 kHz, 600 MHz; feature 0xaf (bit 0 set) | LISTED | LISTED | RANGE | RANGE (dock [U] at 297 MHz) |
| Hisense 6Series65 | no 50 Hz VIC; range V 23-76, H 15-81 kHz, 340 MHz; feature 0x0a (bit 0 clear) | no | no | no | no |

- A monitor's DisplayPort input often carries DisplayID instead of CEA VICs. Kernel 5.4 adds DisplayID detailed timings, but doesn't parse its range block **[I]**. On a native DP cable only RANGE gates may open **[U, FREESYNC.md §7 Q7]**.
- **The framework has no EDID today.** The vendor composer's `SprdDisplayClient::GET_DISPLAY_IDENTIFICATION_DATA` is `mov w0,#8; ret`, which is Unsupported (hwc.dis 0x4a7f0) **[C]**; `HWCDisplay` (0x5d210) and `SprdHWComposer2` (0x42830) forward to it, and a hidden subclass override through the table call at 0x428b0 is not ruled out **[I]**. Consistent with that, the H27S17 shows `key none` in `gammaos-monitor-mode list` (dp_dock_findings.md:910) **[C]**: `monitorKey` returns null without a PnP id and product id (patch:168853-168856) **[C]**, and those come from the same composer call. (SurfaceFlinger.cpp:7998 is only the dumpsys path that prints "no identification data" when the call fails.)
- So the framework uses two other sources **[I; the transport is the framework worker's choice]**:
  - **Which modes the monitor may get**: the kernel's mode list. The driver's gate already dropped every record the EDID doesn't allow (section 6.2), so phases 3-4 trust that list and need no parser.
  - **Labels and the monitor key** (phase 5): system_server reads the cached EDID from `/sys/class/drm/card0-DP-1/edid`. Upstream `drm_sysfs.c` `edid_show` only copies the connector's stored blob and does not probe, unlike `status`, which forces a probe **[I; check the android12-5.4 source before use]**. Whether system_server's SELinux domain may read that node is **[U]** (a sepolicy rule may be needed). A Java port of the parser in `refresh_timings.py`, host-tested against it, labels each mode LISTED or RANGE. Don't read `/sys/class/drm/*/status`.

### 4.4 The timings

One kernel record per whole-number rate. Horizontal: CEA 1920/2008/2052/2200 (front porch 88, sync 44, back porch 148) unless it says VIC. Vertical: sync 5, back porch 36, front porch as listed. Flags +hsync +vsync (0x5). **[C arithmetic, checked against refresh_timings.py]**

| Record | Clock (k) | Totals | V front porch | Real rate | Reports as | Line rate | Link (HBR2 x2) | Gate | Plays |
|---|---|---|---|---|---|---|---|---|---|
| 1080p50 (VIC 31) | 148.5 MHz (2) | 2640x1125, h 528/44/148 | 4 | 50.000 | 50 | 56.25 kHz | 41% | LISTED or RANGE | all PAL |
| 1080p55 | 148.5 MHz (2) | 2200x1230 | 109 | 54.878 | 55 | 67.5 kHz | 41% | RANGE | MK, R-Type |
| 1080p57 | 148.5 MHz (2) | 2200x1174 | 53 | 57.496 | 57 | 67.5 kHz | 41% | RANGE | Toaplan, Cave, Model 2 |
| 1080p59 | 148.5 MHz (2) | 2200x1140 | 19 | 59.211 | 59 | 67.5 kHz | 41% | RANGE | Neo Geo, Taito F3 |
| 1080p60 (exists) | 148.5 MHz (2) | 2200x1125 (VIC 16) | 4 | 60.000 | 60 | 67.5 kHz | 41% | exists | NTSC |
| 1080p70 | 297 MHz (1) | 2200x1926 | 805 | 70.094 | 70 | 135 kHz | 82.5% | RANGE | DOS VGA |
| 1080p75 | 297 MHz (1) | 2200x1795 | 674 | 75.209 | 75 | 135 kHz | 82.5% | RANGE | WonderSwan, Lynx |
| 1080p100 (VIC 64) | 297 MHz (1) | 2640x1125, h 528/44/148 | 4 | 100.000 | 100 | 112.5 kHz | 82.5% | LISTED or RANGE | 2 x PAL |
| 1080p120 (exists) | 297 MHz (1) | 2200x1125 (VIC 63) | 4 | 120.000 | 120 | 135 kHz | 82.5% | exists | 2 x NTSC |
| 1080p56 (optional) | 148.5 MHz (2) | 2200x1201 | 80 | 56.203 | 56 | 67.5 kHz | 41% | RANGE | NMK16, PC-98 |
| 1080p72 (optional) | 297 MHz (1) | 2200x1875 | 754 | 72.000 | 72 | 135 kHz | 82.5% | RANGE | Pokémon Mini, ST mono |

- Every row fits the DPU fields (sync ≤ 255, porches and totals ≤ 4095) and has vsync + back porch = 41, above the driver's `vsync+vbp < 32` underflow warning **[C arithmetic; dp_modes_feasibility.md:107]**.
- Every row passes the H27S17's range **[C]** and stays at or under 297 MHz, which the Anker has carried **[C]**.
- **Alternative for 70 and 75** (test module only): a long line instead of a long vertical blank. `refresh_timings.txt` (:290, :302) lists 3814x1111 and 3564x1111 with CVT-RB vertical timing 3/5/23, but that is vsync + back porch 28, which trips the underflow warning, so those rows are not used **[C]**. With the same 41-line blank as the table (vtotal 1121): about **3780x1121** at 297 MHz (70.09 Hz, 78.6 kHz) and **3523x1121** (75.20 Hz, 84.3 kHz) **[C arithmetic]**. It keeps the line rate nearer CVT-RB's, but no converter has carried such a line. Phase 5 tries both forms and ships the one the monitor and converters accept.
- **720p fallbacks** (optional, later): 720p50 (VIC 19, 74.25 MHz) and 720p100 (VIC 41, 148.5 MHz) for links or docks that can't carry 297 MHz **[C arithmetic]**. Their CEA vertical sync + back porch is 25, which trips the underflow warning like today's 720p60 does (dp_modes_feasibility.md:107) **[C]**. They would also need a "match at another size" rule, which decision 5 rules out for now.

### 4.5 First use, and when the monitor can't show it

- **The Cube cannot tell reliably that a monitor rejected a mode.** Link training reaches the converter, not the HDMI monitor. DPU1's vsync is free-running, so it keeps arriving with a black screen. A converter's HPD drop is a strong failure sign: the H27S17 through the Anker dropped HPD 3.8-10.3 s after unusable streams **[C, dp_dock_findings.md:376]**. But its absence proves nothing: a 640x480 failure held HPD for 33 s with no picture **[C, :424]**.
- So **the first time a monitor gets a rate, the user confirms it.** Nano switches with `match --confirm` (or `hold --confirm`), then shows the existing Keep/Revert dialog ("Can you see this? Press A") on the monitor, and on the Cube's screen when it is on. **system_server owns the 15 s timer**, as it does for `try`, so the screen comes back even if Nano dies during the confirm. Revert happens at 15 s, on B, on an HPD drop, or on a link-training failure.
  - Keep (Nano sends `confirm`) stores "ok" for this monitor and rate. No answer or a failure stores "failed", and that rate is not offered again until Forget Monitor Resolutions.
  - **Key.** Stored modes use `monitorKey`, a hash of PnP id, product id, year and name (patch:168853-168870) **[C]**, but it is null for the owner's H27S17 (section 4.3). Confirm results therefore fall back to a hash of the cached EDID blob from sysfs, or, until phase 5 reads it, a hash of the kernel's mode list for the connector **[I]**. A mode-list hash can't tell two monitors with the same list apart, and neither key tells the Anker from the dock behind one monitor **[I]**; a rate that fails through one path is then blocked on the other until Forget (open question 12).
  - Phases 3-4 always confirm, since the framework can't yet tell LISTED from RANGE. Decision 4 (whether LISTED modes, 50 and 100, may skip the confirm) applies from phase 5.
- **Fallback when the monitor can't show the wanted rate**: try the other multiple (100 for 50, 120 for 60); otherwise the nearest confirmed rate within 1.5%; otherwise stay at the normal mode. A rate that failed is treated as absent.

## 5. Switching and reverting

### 5.1 Today

- `cmd display gammaos-monitor-mode list|try W H HZ|keep|revert|auto|forget` (patch:168294, 168327-168370). Only root, shell and system may call it **[C]**. Nano runs it with `timeout -s KILL 10 cmd display ...` off the render thread (NanoMenuMonitorMode.cpp:89-93) **[C]**.
- `try` is a 15 s trial that verifies within 3 s and reverts by itself; `keep` stores the mode per monitor (patch:167600-167694) **[C]**. Neither fits a game: `try` reverts after 15 s and `keep` changes the monitor's default.
- The applying marker drops a stored mode that panicked at connect (patch:167388-167448) **[C]**.

### 5.2 New verbs

- `match [--confirm] CONTENT_HZ OWNER [SOURCE]`: system_server picks the mode by the rule in section 3.5 and holds it. It prints `held 1920x1080@50 50.000 x1 +0.60% REVERT 1920x1080@120`, `stay ...` or `nomatch REASON ...` (the reason feeds section 3.7's text). The exact rate in the reply comes from a table generated from `scripts/sprd_drm.py` MODES at build time, so Nano can give RetroArch the real rate and multiplier (section 5.9) **[I]**.
- `hold [--confirm] W H HZ OWNER`: an explicit hold, for the per-game fixed rate and for Test Game Refresh Rates. Same refusals as `try` (patch:168809-168821), plus Mirror.
- `--confirm`: a trial-style hold for a rate this monitor has not confirmed. system_server reverts it by itself after 15 s and marks the rate failed unless `confirm` arrives first.
- `release`: ends the hold. Idempotent.
- `confirm`: stores "ok" for the held rate on this monitor and ends the 15 s timer. It does not change the monitor's default.
- `list` gains `game MODE OWNER`, `ok MODE` and `failed MODE REASON` lines. (`game`, not `hold`, so it is not confused with the existing `held MODE REASON` line, patch:167563; Nano's `parseList` ignores unknown keys, patch:60890-60916 **[C]**.)
- Precedence becomes hold > stored > Auto, and `sys.gammaos.display.monitor_mode` gets the reason `game`. Nano's parser and labels assume "auto|user|trial" (patch:35564, 61075-61076) **[C]** and must learn `game`.
- **Output Resolution and a game hold don't meet.** Output Resolution is dimmed and does nothing while a game runs on the monitor (`NanoMenu::monitorModeLocked()` returns `gameOnMonitor()`, patch ~35616-35637; header comment ~35546-35547) **[C]**, so a trial can't start over a held game. `try` answers "error: game" during a hold, for any other caller. The one exemption is the new Quick Menu > Display > Normal Refresh Rate row (section 5.8), which only sends `release`.
- A hold is never written to disk and never applied at boot or at connect.

### 5.3 When to switch

- **Before the game starts.** Nano calls `match` on its command thread, showing "Switching the monitor to 50 Hz...", before `armAppLaunchTrigger()` in `launchXmbGame`, and in `overlayLaunchGame`, the drastic-nano intercept, search/recents and Quick Resume **[C for the launch functions, AOSP NanoMenuXmb.cpp:3776, NanoMenuOverlay.cpp:1780]**. The emulator then starts on the new mode, reads the right rate, opens its audio after the switch, and never sees a configuration change **[I]**. The first-use confirm also runs here, before launch.
- Nano stops its own music during the switch.
- Games started outside Nano (ES-DE as the home app, the Android launcher) get no matching in v1. Phase 8 adds a framework hook at activity start (`RootWindowContainer.java:2290-2445` builds the RetroArch and standalone intents) **[C for the hook point]**.

### 5.4 When to revert

Defence in depth **[I unless marked]**:
1. Nano calls `release` on every way back to Nano (`overlayQuitToHome`, `overlayKillAll`, `esdeReturnHome`, the drastic-nano session end) and unconditionally when Nano starts, which covers a Nano crash.
2. system_server watches the owner once a second, as `GammaosMonitorAudio` already does (patch:166824-166860) **[C for the pattern]**.
   - **Launch grace.** `match` runs before the launch, when `app_launched` and `launch_app` still describe nothing or the previous app; Nano sets them at launch (patch:16559-16560) **[C]**. So the watchdog starts in a "waiting" state and arms only when the owner's uid is first seen on top. If the owner never appears within 30 s, it releases. While a `--confirm` timer runs, the watchdog stays waiting; the confirm has its own timer.
   - **Armed.** It releases after two failed polls in a row: `sys.gammaos.nano.app_launched` is not 1, `launch_app` is not the owner, or the owner's uid is no longer top or gone (`mActivityManagerInternal.getUidProcessState`, AOSP DisplayManagerService.java:1359-1361, 3855-3858, C). "Not top" must stay one of the conditions, because a Keep Running in Background app keeps its uid after exit.
   - Pausing in Nano's Quick Menu keeps the hold, since the overlay is not an activity.
3. **Game crash**: covered by "uid gone" in item 2. (PhoneWindowManager's `nanoStopAppToHome`, patch ~169957-169980, sets `app_launched=0` on hotkey exits, not on crashes **[C]**.)
4. **Unplug**: clear the hold where the trial is cleared (`gammaosDisplayDeviceRemovedLocked`, patch:167460-167478) **[C for the trial]**. A replug while the game runs comes back at the normal mode; v1 doesn't re-apply the hold, since a modeset right after connect is the risky path from tests 41 and 42. An unplug within 15 s of a first-use switch marks the rate failed.
5. **Sleep and wake**: keep the hold. The composer re-enables at the active mode on wake, the same path as a stored 1080p120 today (fix E). Test it.
6. **Display mode changed to Mirror**: release.
7. **system_server restart or reboot**: the hold is lost by design. A "holding KEY MODE" marker, written before the switch and cleared 20 s after the mode is active, drops that rate from the confirmed list if a panic interrupts it, like the applying marker.

### 5.5 The vsync watchdog (prerequisite, FREESYNC.md A0, TODO.md "Display plan Step 3's SurfaceFlinger part")

- **It already trips after almost every connect and mode switch in the test 42 logs.** Example, B4: `setActiveConfig` at logcat:32369 (11:21:30.907), "no composer vsync from monitor 1" at :33408 (34.607), 3.03 s after the display was powered on again. The same about 2.5-3 s after connect in A1 :162087, A2 :28253, q1440 :39740 and T3 :226967 **[C]**. Whether vsync stops for good or only comes back after more than 3 s is **[U]**.
- Once tripped, the monitor runs on predicted vsync at the mode's nominal period until unplug (patch:178042-178130) **[C]**. SurfaceFlinger then never learns the real period (Scheduler.cpp:360-370) **[C]**. That undermines matching, and today it already affects most monitor sessions.
- Changes, in the SurfaceFlinger part of the patch:
  1. After a successful mode change on a monitor, and at monitor connect: set a 10 s grace, reset the enable time, clear the "broken" flag, and enable hardware vsync explicitly. The composer ignores an enable it thinks is already on (`HWCDisplay::set_vsync_enabled` 0x5c7b8-0x5c7cc) **[C]**, and so does HWComposer's own cache **[I]**.
  2. The 3 s check never trips during the grace; the first hardware vsync ends the grace early.
  3. When the grace ends without a sample: toggle vsync off and on, wait 3 s more, and only then mark it broken, as today.
  4. Log the time from `setActiveConfig` to the first vsync, so a device test can tell "slow" from "never".
- Risk: during the grace, SurfaceFlinger paces poorly while the monitor is blank anyway **[I]**.

### 5.6 Audio

- Monitor audio stays routed: sprd-drm changes the audio extcon only on HPD (`extcon_set_state_sync` once, at 0x37888 in `dptx_threaded_irq`), and the encoder's disable/enable has no audio calls (0x9188) **[C]**. So a mode switch shouldn't send sound to the speakers **[I]**, unless a converter pulses HPD on a timing change **[U]**.
- The DP audio stream after the encoder's disable/enable is the main audio risk **[U]**: if DPTX power-gates, its audio setup may be lost until the next stream opens. Switching before launch helps, since the game opens a fresh stream. Fallback: the audio broker disconnects and reconnects the HDMI device after a hold.
- HDMI sinks mute for about 1-2 s on any timing change **[I]**. Harmless before launch.

### 5.7 Display modes

- **Docked**: matching on. The monitor is display 0 and the vsync pacesetter **[C]**.
- **Dual Screen**: matching on for rates of 50 Hz and up, which is all of them (`kDualMinHz` 50, patch:61054) **[C]**. The monitor paces the Cube's screen too, so the second screen composes at the game rate **[I]**, and the Cube's 60.03 Hz panel then shows uneven motion at 50 Hz, as the `kDualMinHz` comment warns for low rates (patch:61051-61053) **[C for the comment]**. Dual-screen games (NDS, 3DS) run at about 60 Hz and never switch, so only Nano's companion screen is affected (decision 6) **[I]**.
- **Mirror**: matching off. The Cube's screen paces and the monitor only follows it (patch:168232-168261) **[C]**. `hold` and `match` answer "error: mirror".

### 5.8 Safe no-signal fallback

If a matched mode shows black:
1. First use of every rate on every monitor is a 15 s confirm that system_server reverts by itself, whatever Nano does.
2. An HPD drop within 15 s of a switch reverts and marks the rate failed. It is never re-applied on the replug, which would loop.
3. The user can always get out: quit the game with Nano's hotkey, which needs no picture **[I]**; unplug at the monitor end (the hold is cleared and the replug comes back at the normal mode); or choose a new Quick Menu > Display > Normal Refresh Rate row (`release`), which is exempt from the Output Resolution lock over a game.
4. The holding marker covers a panic during a switch.

### 5.9 RetroArch settings Nano writes at launch

- **This is new work.** Nano does not rewrite retroarch.cfg per launch today **[C]**: `raRewriteFileEx` runs only on storage-folder moves (NanoMenuPS3Folder.cpp:1169-1184; patch:42085-42100), and `nanohotkeys::provision` writes its keys on a detached thread at Nano start (NanoMenu.cpp:917) and on overlay pause (NanoMenuOverlay.cpp:534), skipping running emulators. The new write is synchronous, before the launch intent, under `kRaConfigLock` **[I]**.
- **Only for a launch where `match` replied `held`, or `stay` at a multiple (m = 2, for example a 59.94 Hz game at a 120 Hz base)**, Nano writes:
  - `video_refresh_rate` = the exact rate of the held mode (from the `match` reply, for example 54.878 or 100.000). RetroArch 1.22.2 uses the configured rate for its timing, not the display's real one **[I, strong; the live-rate code is in master only]**.
  - `video_swap_interval` = the multiplier m from the reply (1, or 2 for 100 Hz with a 50 Hz game) **[C that RetroArch divides by it, section 2.1]**. RetroArch's "0 = auto" is an alternative **[U for how 1.22.2 resolves auto on Android]**.
  - `log_verbosity = "true"` when learning wants it (section 3.4, decision 11).
- **Restore afterwards.** Before the write, Nano saves the user's own values of those keys (in a small file, so a Nano crash doesn't lose them); on return to Nano, once RetroArch is gone (it writes its config back on exit), Nano writes them back under the same lock **[I]**. So a user's tuned `video_refresh_rate` survives, and unmatched launches, including handheld play with no monitor (the panel's 60.03 Hz), are untouched, as today.
- **Warm RetroArch.** A RetroArch still alive from an earlier session ignores the file and writes its own values on exit, so Nano doesn't match it (section 3.5) **[I]**.
- Side benefit, to verify: at today's 1080p120 base with swap interval 1, RetroArch's skew check fails for every 60 fps core. Writing swap interval 2 for the `stay` x2 case fixes that too **[I]**; phase 0 checks today's behaviour.
- RetroArch's own automatic refresh-rate switch stays off, so two actors don't fight over the mode (`video_autoswitch_refresh_rate`) **[I for the key name]**. It can't work on Thimsy anyway: the hold blocks it, and it would only round to 50, 59.94 or 60 (`video_switch_refresh_rate_maybe`, src/ra_video_driver.c:3406-3433) **[C]**.
- "Sync to Exact Content Framerate" (`vrr_runloop_enable`) stays off. It is for VRR displays (src/ra_config.def.h:1813-1814) **[C]**.

## 6. Driver work

### 6.1 New kernel records (`scripts/sprd_drm.py`)

- Each mode is an 80-byte record in `MODES` (sprd_drm.py:490+): name, the twelve timing words, vrefresh, type, and an EDID gate **[C, sprd_drm.py:479-489]**. MODE_FILTER admits only modes that equal a record exactly, so an EDID override can open or close a gate but cannot bring in a new timing **[C, display_modes_plan.md §1; sprd_drm.py:49-50]**. Every new timing therefore means a new module build and a vendor_dlkm write.
- Records: section 4.4. Type 0x60 (driver and user-defined, so the composer lists them; never preferred) **[C]**.
- The kernel's link check drops the 297 MHz records on weak links by itself **[C]**.

### 6.2 The gate needs two new forms

Today the gate means "the EDID's list has a mode of this size (exact, or at least) at N Hz or more" (`b.hs` at 0x20354) **[C, sprd_drm.py:479-486, 572-603]**. A 1080p50 record gated that way would open on any monitor that lists 1080p60. So:
- **Bits 8-15: highest rate** (0 = no limit). That gives LISTED for VIC rates: "1920x1080 exactly, 50 to 50 Hz". It works because kernel 5.4's inferred 1080p and 720p modes are all 60 Hz **[I]**. About 3-4 words in the GET_MODES loop **[I]**.
- **Bit 30: RANGE**, as an alternative. Scan descriptors 54/72/90/108 of the base block for `00 00 00 FD`, require byte 24 bit 0, and check the record's own vertical rate, line rate and clock against the limits, with EDID 1.4's +255 offsets. The raw EDID pointer is in x20 at that point **[C, sprd_drm.py:531-533]**. About 35-45 words **[I]**.

### 6.3 Code space

- Today's caves are full **[C, sprd_drm.py:808-815; the Y420 note at :858-860]**. Seven records are 560 bytes, plus about 45 words for the gate. A survey for more dead per-SoC functions is needed **[U]**.
- Smaller alternative: "derived records" of 3 words each (base record, new vertical totals, vrefresh), expanded by both loops. More code, much less data **[I]**.

### 6.4 Whole-number rates

- The composer reports the record's integer vrefresh: `DrmMode::v_refresh` returns it when it is non-zero, and only otherwise computes clock x 1000 / (htotal x vtotal) (hwc 0x66000-0x66038) **[C]**. The framework matches modes within 0.5 Hz (`MonitorMode.matches`, patch:168613-168615) **[C]**.
- So: **one record per whole number**, and each record's real rate must round to its own number. The table in section 4.4 obeys that (57.496 rounds to 57) **[C arithmetic]**.
- What Android shows (55 Hz) differs a little from the truth (54.878 Hz). That is harmless:
  - RetroArch gets the exact rate from Nano (section 5.9).
  - SurfaceFlinger learns the real period from hardware vsync, once the watchdog fix keeps it on **[I]**.
- **Optional composer patch** (phase 7, `scripts/sprd_hwc.py`): `cbz w8,0x66010` at 0x66004 (0x34000068) to `b 0x66010` (0x14000003) makes the composer always report the exact rate, and the vsync period with it (GetConfigAttributes 0x5f180-0x5f198) **[C]**. Side effects: the panel reports 60.03 Hz and the 1440p mode 59.93 Hz; stored "@60" still matches within 0.5 Hz **[C arithmetic]**. Only needed for fractional variants such as 59.94 next to 60, which this plan doesn't add.

## 7. The built-in screen (later phase)

- The ST7703 panel (720x720, 50 MHz, 1096x760, 60.03 Hz) is in dtbo entry 2, which Thimsy already rewrites with `patch_dtbo.py` **[C, work/dtbo-a1/e2-stock.dts:9-16; live.dts:450-486]**.
- Adding a second display timing with a longer vertical front porch plus `sprd,vrr-enabled` gives discrete rates of about 26-60 Hz **[C arithmetic, FREESYNC.md option B]**. 50.02 Hz needs a front porch of 172 lines **[C arithmetic]**.
- Useful: 50 Hz for handheld PAL games. 55, 57 and 59 are possible, but the vendor DPU code matches panel modes by integer rate (realme source `sprd_dpu.c:180-183`) **[C for realme; I for the Cube's closed module]**. 70 and 75 Hz would mean overclocking the panel; the cores' frame-drop modes (WonderSwan 4/5, Pokémon Mini 5/6) stay the answer there.
- Risks: ST7703 flicker with a long blank **[U]**; `sprd,vrr-max-layers` behaviour **[U]**; SurfaceFlinger's idle timer switching a multi-mode panel by itself unless the panel gets a default hold too **[I]**; a dtbo partition write with a new `tested_sha256` and the owner's approval.

## 8. Phases

Effort is working days, not counting device time. Every phase keeps the device hazards in "Device test rules" below.

| Phase | What | Effort | Risk |
|---|---|---|---|
| 0 | Read-only owner checks on the current build | 0 | none |
| 1 | Vsync watchdog fix (A0) | 0.5-1 | low |
| 2 | Kernel: 1080p50 and 1080p100 with the LISTED gate | 1-2 | low-medium |
| 3 | Framework: `match`, `hold`, `release`, `confirm`, `--confirm` timer, owner watchdog with launch grace, confirm-result key, holding marker | 2.5-3.5 | low-medium |
| 4 | Nano: resolver v1, Game Options row and texts, switch before launch, release on return, RetroArch rate and swap interval with restore, first-use confirm, Normal Refresh Rate row; DOS and Amiga cores; 200-switch soak | 4-6 | low |
| 5 | Game rates 55, 57, 59, 70, 75: RANGE gate, test module, then shipping records; framework EDID labels from sysfs | 2-4 plus a cave survey | medium |
| 6 | Learning: RetroArch log; later the SurfaceFlinger content vote | 2-3 | low-medium |
| 7 | Optional: composer exact-rate patch | 0.5 | low-medium |
| 8 | Optional: games started outside Nano (ES-DE home, Android launcher) | 2-3 | medium |
| 9 | Optional: built-in screen 50 Hz (dtbo) | 2-4 | medium |
| 10 | Optional: matching at 1440p and 4K (1440p50, 4K50 4:2:0) | 1-2 | medium |

### Phase 0: owner checks (no new code)
- Docked at 1080p120 with a PAL game in RetroArch, then in Dolphin: `dumpsys SurfaceFlinger` frame-rate and LayerHistory lines. Does the heuristic see about 50 fps? (Answers open question 3.)
- `logcat -s RetroArch` during a launch, with and without `log_verbosity`: is the "[Core] Geometry ... FPS" line there?
- With the current Output Resolution, note how long the monitor stays black after a `try` to 1080p120, and whether monitor audio survives it with Nano's music playing.
- `getprop persist.gammaos.refresh.lock`, `persist.sys.phh.dynamic_fps` and `ro.surface_flinger.use_content_detection_for_refresh_rate` (section 3.4's content-detection caveat).
- At a 1080p120 base, an NTSC game in RetroArch: does the log show "Timings deviate too much", and what are `video_swap_interval` and `video_refresh_rate` in retroarch.cfg? (Section 5.9's side benefit.)
- With the monitor attached after boot, `xxd /sys/class/drm/card0-DP-1/edid | head` once, only after the `edid_show` source check in section 4.3 confirms it doesn't probe: does it return the H27S17's EDID? Can system_server's domain read it (`ls -Z`)?

### Phase 1: vsync watchdog
- Work: section 5.5.
- Unit tests: extend the SurfaceFlinger host test (the precedent is `tests/unit/test_surfaceflinger_yuv420.py`): grace set on a mode change and at connect; no trip during the grace; the toggle before "broken"; the first vsync ends the grace.
- Device test: plug in the H27S17 after boot; `try` 1080p120 and revert, ten times; `logcat` has no "no composer vsync from monitor", and the new first-vsync log line gives a number for each switch. `dumpsys SurfaceFlinger` shows a vsync period equal to the mode's.

### Phase 2: 1080p50 and 1080p100
- Work: two records (section 4.4), the highest-rate gate bits (section 6.2), code space (section 6.3).
- Unit tests (`tests/unit/test_vendor_patches.py`): every record's clock is 297/k MHz; each real rate rounds to its vrefresh and no two records at one size share a vrefresh; fields within the DPU limits; expected disassembly of the gate code (`tests/a64.py`); a Python model of the gate run on the two real EDIDs (H27S17: 50 and 100 open; Hisense: closed; no EDID: closed).
- Device test (needs the owner's approval for the vendor_dlkm write; read back and check the sha256): plug the H27S17 through the Anker after boot. `cmd display gammaos-monitor-mode list` shows 1920x1080@50 and @100. `try` each: picture, the monitor's OSD shows 50 and 100 Hz, audio, no HPD drop for 5 minutes. Repeat through the dock (100 Hz is the dock's first 297 MHz-class mode besides 120). Plug in the Hisense: neither mode listed.

### Phase 3: framework verbs
- Work: section 5.2 and 5.4 in `GammaosScreenArrangement.java`, `LocalDisplayAdapter.java`, `DisplayManagerService.java` and `DisplayManagerShellCommand.java`. Hide all the new modes (50 and 100 included) from Auto and `offered()`. Confirm results keyed as in section 4.5 (mode-list hash until phase 5).
- Unit tests (`GammaosScreenArrangementCheck.java`): precedence hold > stored > Auto; `try` refused during a hold; the match rule (49.70 to 50; 59.19 to 59; 59.94 at a 120 base stays x2; 75.47 to 75; 56.2 without a 56 mode gives no match; 71.2 at 70 gives no match; a 50 Hz game at a 100 base stays; 100 chosen when 50 failed; no candidates at 1440p); hold refused in Mirror; the `--confirm` timer reverts at 15 s and marks failed, and `confirm` stops it; the watchdog stays waiting until the owner is on top, releases if it never appears within 30 s, then releases on two failed polls; `ok`, `failed`, `game` and `holding` lines parsed and written; new modes never chosen by Auto.
- Device test (adb, monitor plugged after boot): `match 49.70 com.retroarch.aarch64` gives 1080p50; `release` returns to the stored mode. `match --confirm` with no `confirm`: reverts at 15 s, rate listed failed. Hold with no launch: released after 30 s. Hold, launch the owner, then `am force-stop` it: released within 2-3 s. Hold, then unplug at the monitor end: released; replug comes back at the normal mode. Hold, sleep and wake: still held. Hold, then switch to Mirror: released. No framework restart with a monitor attached; the holding marker is tested on the host only.

### Phase 4: Nano
- Work: the resolver (override, arcade table, region from file name and header, system table), section 3.6 settings and 3.7 texts, the Quick Menu's Normal Refresh Rate row exempt from the game lock (5.8), the `game` reason in Nano's prop parser and labels (5.2), the switch before launch and release paths (5.3, 5.4), the per-launch RetroArch write with restore (5.9), the first-use confirm (4.5). Build-time tables: `systems.json`, `packages.json`, `mame_refresh.bin`, and the exact-rate table generated from `sprd_drm.py`. Add `dosbox_pure` and `puae` to `mods/retroarch-cores.txt` and dos/amiga systems to `mods/nano-systems.txt`: none ship today **[C]** (decision 8).
- Unit tests (`tests/unit/test_nano_units.py` host suites): file-name region rules (No-Intro, Redump, GoodTools, TOSEC, WHDLoad, Brazil); NES 2.0, SNES, Mega Drive and N64 header regions; resolver order and the reason text; each section 3.7 text; arcade lookup; the retroarch.cfg write (exact rate, swap interval, the user's values saved and restored, nothing written for an unmatched launch or with no monitor); a test that the exact-rate table equals `sprd_drm.py` MODES.
- Device test: a PAL SNES game in RetroArch (switch card, monitor at 50, RetroArch log has no "Timings deviate too much", back to the normal mode on quit, the user's `video_refresh_rate` restored); the same at a 100 Hz override (swap interval 2, no "Timings deviate too much"); a PAL GameCube game in Dolphin; an NTSC game at a 120 base (no switch); a per-game override to Off and to 100; the first-use confirm left to time out (reverts, the rate is marked failed); Nano killed during a confirm (system_server reverts at 15 s); a game crash (the hold goes). An Amiga PAL and a DOS game once the cores are in (DOS needs phase 5's 70 Hz).
- **Soak (exit criterion):** about 200 match/release cycles on the H27S17 through the Anker, and a shorter run through the dock: no oops, no pstore entry, no `dpu_clk_disable` abort (TODO.md crash4), a picture after every switch. Matching doubles the number of modesets, so "on by default" (decision 3) also waits on crash4's status.

### Phase 5: game rates
- Work: the RANGE gate (section 6.2); a **test module** first with 55, 57, 59, 70 and 75, the latter two in both the long-blank and long-line forms (section 4.4, 41-line blank); then the shipping records. Port the EDID parser to Java in system_server, read the cached EDID from sysfs (section 4.3), label modes LISTED or RANGE, and switch the confirm-result key to the EDID hash. Decision 4 applies from here.
- Unit tests: the RANGE model on the real EDIDs (H27S17 opens all; Hisense none) plus synthetic ones (EDID 1.3 without offsets, EDID 1.4 offsets, bit 0 clear, a 48-75 Hz office monitor); the Java parser against `refresh_timings.py` on the same inputs.
- Device test (approval for each vendor_dlkm write): `try` each rate through the Anker and through the dock. Record the OSD rate, picture stability, any HPD drop, and audio. Ship one form of 70 and 75: the one both paths accept.

### Phase 6: learning
- Work: section 3.4. RetroArch log first; the SurfaceFlinger vote after phase 0 shows the heuristic works.
- Unit tests: log parser (%.2f and %.4f, SET_SYSTEM_AV_INFO), the settle rule, and the "store only if different by 0.5%" rule.
- Device test: a DOS game that runs in mode X (learned 59.94, no switch next time); a PlayStation PAL game (learned value near 49.76).

### Phase 7: composer exact-rate patch (optional)
- Work: the 0x66004 patch in `scripts/sprd_hwc.py` (section 6.4).
- Unit tests (`tests/unit/test_vendor_patches.py`, like the other hwc patches): the original word 0x34000068 is checked before patching, the patched word is 0x14000003 at 0x66004, and nothing else changes; a model check that stored "@60" still matches 60.03 and 59.93 within 0.5 Hz.
- Device test (approval for the vendor partition write; read back and check the sha256): `dumpsys SurfaceFlinger` shows 54.88 instead of 55 and 60.03 for the panel; a stored 1080p60 and 1440p60 still apply at connect.

### Phase 8: games started outside Nano (optional)
- A framework resolver at activity start (section 5.3), sharing the tables. Unit tests on the shared resolver; device test from ES-DE as the home app.

### Phase 9: built-in screen at 50 Hz (optional)
- Work: section 7, in `patch_dtbo.py`: a second display timing for dtbo entry 2 with a 172-line front porch, `sprd,vrr-enabled`, and a default hold at 60 Hz so the idle timer can't switch the panel.
- Unit tests: the patched dtbo entry decodes (dtc round trip) with both timings and the stock 60.03 Hz timing first and unchanged; the second timing computes to 50.02 Hz; every other entry is byte-identical to stock.
- Device test (owner approval for the dtbo write; read back and check the sha256 against the new `tested_sha256`): boot with no monitor; the panel comes up at 60 Hz; a PAL game switches the panel to 50 and back; look for flicker.
- **Way back:** keep a copy of the device's stock dtbo image off the device (`patch_dtbo.py` already checks its input against the stock sha256, scripts/patch_dtbo.py:30 **[C]**); flashing it from fastboot, with the owner's approval, restores the panel **[I]**. A bad dtbo affects the built-in screen at every boot, so the stock image and the fastboot command go in the test notes before the write.

### Phase 10: matching at 1440p and 4K (optional)
- Today a user at 1440p60 or 4K gets no matching (section 3.5). Two records would cover PAL there **[C arithmetic; U for acceptance]**:
  - **1440p50**: today's 2560x1440 record (297 MHz, 3346x1481, 59.93 Hz, sprd_drm.py:497-500) with vtotal 1775: 50.007 Hz, line rate 88.8 kHz unchanged. Inside the H27S17's range; gated RANGE, or LISTED if a monitor lists 2560x1440@50.
  - **4K50 as YCbCr 4:2:0**: CEA VIC 96 (5280x2250, 594 MHz, 297 MHz on the wire in 4:2:0), the same clock as today's 4K60 4:2:0 record, gated LISTED on VIC 96 in the 4:2:0 data block. The owner's Hisense lists no VIC 96 (dp_dock_findings.md:914) **[C]**, so it would not open there. The driver's Y420 gate today checks VIC 97 only (sprd_drm.py:38-39), so it needs a VIC 96 form.
- 4K25 (VIC 94) is not useful: a 50 fps game can't be shown at 25 Hz without dropping every other frame.
- Same unit and device test pattern as phase 2.

### Device test rules (every phase)
- Boot with **no monitor attached**: booting with one crash-loops. Plug it in once Nano is up.
- Before any reboot or display restart: unplug **at the monitor end** and wait for `card0-DP-1` to read `disconnected`.
- Never `stop; start` with a monitor attached.
- Never read `/sys/class/display/dispc1/*`. Never touch dwc3 debugfs.
- Every partition write (vendor_dlkm, vendor, dtbo) needs the owner's approval, with a read-back sha256 check.
- After a reboot, save `/sys/fs/pstore/*` before the next test.

## 9. Risks

- **Monitors may reject non-CEA timings or long vertical blanks**, including the converters inside the Anker and the dock **[U]**. Mitigated by the fixed line timing, the confirm and the HPD watchdog. 50 and 100 are CEA timings and carry the least risk.
- **More modesets.** Two full DPU1 disable/enable cycles per game. The crash4 `dpu_clk_disable` abort is still open (TODO.md) **[C]**. Phase 4's 200-switch soak is an exit criterion, and "on by default" waits on crash4.
- **DP audio after a switch** **[U]**.
- **The dock above 148.5 MHz** **[U]**: 70, 75 and 100 may not pass it.
- **Code space** in sprd-drm.ko **[U]**.
- **Wrong prediction**: a blank for nothing, and a 1-2 s mismatch. Mitigated by "only switch when it helps" and learning.
- **Standalones pace themselves**; they gain only when the game rate equals the display rate (PAL 50) **[I]**.
- **Multi-region carts**: the emulator's own region choice can differ from Nano's header reading **[I]**. The learned rate wins from the second launch.
- **Soft data**: PlayStation PAL 49.76, Neo Geo AES, Cave 57.55, Lynx per-game rates and Atari ST mono vtotal are **[U]**; 3DO, Dolphin and PCSX2 values are **[I]**.

## 10. Decisions for the owner

1. **The rate set.** 50 and 100, plus the game rates 55, 57, 59, 70 and 75 (recommended). Add 56 and 72 now or on demand?
2. **50 or 100 for PAL** when the monitor has both and is at 60: 50 (recommended: same pixel clock as 1080p60, works through the dock) or 100 (lower latency)?
3. **Match Game Refresh Rate on by default?** Recommended: on, once phase 4 has run on the device and passed the 200-switch soak, and the crash4 `dpu_clk_disable` abort is fixed or shown not to be made worse; using only confirmed rates. Until then, off.
4. **First-use confirm for LISTED rates** (50, 100), from phase 5 (phases 3-4 always confirm): keep it (recommended) or skip it?
5. **Same size only**: never change resolution to match (recommended)? Users at 1440p or 4K then get no matching unless phase 10 is done.
6. **Dual Screen**: match there too, or Docked only? Recommended: match there too. Dual-screen games (NDS, 3DS) never switch; for other games only Nano's companion screen on the Cube shows uneven motion at 50 Hz.
7. **Where the resolver lives**: Nano for v1 (recommended), the framework later for games started outside Nano (phase 8)?
8. **Add cores and systems** for the targets Thimsy doesn't ship: DOS (dosbox_pure) and Amiga (puae) (recommended, needed for the request), and optionally C64, ZX Spectrum, Amstrad CPC, Atari ST, Atari 8-bit, Jaguar, X68000, PC-98 and Pokémon Mini, which the rate tables already cover?
9. **RetroArch**: stay on 1.22.2 with Nano writing `video_refresh_rate` (recommended), or move to a nightly?
10. **Follow a game's rate during play** (DOS mode changes): off (recommended), or an opt-in setting?
11. **Learning through RetroArch's verbose log** (more logcat noise), or wait for the SurfaceFlinger vote? Recommended: the log, turned on only for launches that need learning (no learned rate yet, and a system whose rate varies: DOS, PlayStation, Amiga, X68000), and only for a matched launch, where Nano restores the setting afterwards.
12. **WonderSwan's "60 Hz mode" core option**: leave the user's setting alone and just read it (recommended), or have Nano turn it off when the monitor has 75 Hz?
13. **TVs like the Hisense**: offer nothing new (recommended, EDID policy), or a manual "try 50 Hz anyway"? The latter needs a gate override in the driver.
14. **Built-in screen 50 Hz** (phase 9): wanted? Recommended: not now; revisit after the monitor phases ship, since a bad dtbo affects the built-in screen at every boot.
15. **Composer exact-rate patch** (phase 7): wanted? Recommended: no. The plan adds no fractional variants, and RetroArch gets the exact rate from Nano (section 6.4).
16. **Matching at 1440p and 4K** (phase 10: 1440p50, 4K50 4:2:0): wanted? Recommended: on demand. Your H27S17 is a 1440p monitor, so 1440p50 is the one that would matter to you.

## 11. Open questions

1. After a modeset, does monitor vsync come back late (more than 3 s) or never? (Phase 1 log.)
2. Does the H27S17 accept 1080p at 55/57/59 with a long vertical blank at 148.5 MHz, and 70/75 at 297 MHz, through the Anker and through the dock? Long blank or long line? Does its OSD show the true rate?
3. Does SurfaceFlinger's heuristic see the right fps for emulators on the Cube? (Phase 0.)
4. Does monitor audio survive a switch? Do the converters pulse HPD on a timing change?
5. What are the dock's converter chip and TMDS limit? (DPCD 0x080-0x08F and 0x500, FREESYNC.md C1 printk.)
6. Does RetroArch 1.22.2 take `video_refresh_rate` from Nano's launch config as expected, and is `video_autoswitch_refresh_rate` the right key?
7. Does a held 1080p50/100 survive sleep and wake (fix E path)?
8. What does a native DP EDID contain (DisplayID, range, bit 0)? Needs a USB-C to DisplayPort cable.
9. Is there enough dead code in sprd-drm.ko for seven records and the range gate, or are derived records needed?
10. How well do FBNeo romset names match MAME's for the arcade table, and how should clones with their own rate be handled?
11. Does Beetle PSX still report 49.761 for PAL progressive? What do yabasanshiro (Saturn) and FBNeo (Neo Geo and other boards) report?
12. Can system_server tell the Anker from the dock behind one monitor (for example by the DP branch device's DPCD id), so a rate that fails through one path isn't blocked on the other?
13. Does system_server's SELinux domain allow reading `/sys/class/drm/card0-DP-1/edid`, and does the android12-5.4 `edid_show` avoid a probe?
14. How does RetroArch 1.22.2 on Android resolve `video_swap_interval = 0` (auto)?

## 12. Sources

- Research scratch: `out/dp-evidence/test43/refresh/` (`rates.py`, `rates_out.txt`, `refresh_timings.py`, `refresh_timings.txt`, `urls.txt`, `src/`).
- `out/dp-evidence/test43/freesync/FREESYNC.md` (§1, §3, §4 A0-A3, B, C1; §7), `real_edids_vrr.txt` (:6, :10, :33), `sinkdock/edid_audit.txt`.
- Repo: `scripts/sprd_drm.py` (:9-10, :49-50, :172-210, :479-501, :531-685, :808-815); `scripts/sprd_hwc.py`; `patches/thimsy/0001-thimsy.patch` (41034, 61054, 166824-166860, 167062-167753, 168232-168261, 168294-168370, 168555-168615, 168695-168821, 168853-168938, 169177-169274, 169960-169980, 177837-178169); `display_modes_plan.md`; `dp_modes_feasibility.md` (§2, :238); `dp_dock_findings.md` (:109-115, :376, :424, :864, :907, :918, :921, :932); `TODO.md` (Displays, "Still to do without a device"); `mods/retroarch-cores.txt`; `mods/nano-systems.txt`; README.md:25.
- Device logs: `out/dp-evidence/test42/B4/logcat.txt` (:32356, :32369, :33408), `A1/logcat.txt` (:162087), `A2/logcat.txt` (:28253), `q1440/logcat-run.txt` (:39740), `t3/T3-logcat-4k60.txt` (:226967).
- Binaries: `out/.dock/work/dpmodes-composer/hwc.dis` (0x66000, 0x5f180, 0x5e868, 0x5c7b8); `out/.dock/work/dp-driver/sprd-drm.dis` (0x37888, 0x9188); `out/.dock/work/dtbo-a1/e2-stock.dts`; `out/.dock/work/dp-driver/live.dts` (:450-486).
- AOSP `/aosp/gammaos`: `frameworks/base/cmds/gammaos-nano/` (NanoMenuXmb.cpp:233, 3776-4145; NanoMenuUtils.h:141-153; NanoMenuOverlay.cpp:534, 1780; NanoMenu.cpp:917; NanoMenuPS3Folder.cpp:1169-1184; NanoRaConfig.h:76; NanoMenuMonitorMode.cpp:89-93; LibretroRunner.cpp:276-296, 697-725); `device/phh/treble/vndk.rc:127`; `services/core/java/com/android/server/wm/RootWindowContainer.java` (:1856-1887, :2290-2445); `services/core/java/com/android/server/display/DisplayManagerService.java` (:1359-1361, :3855-3858); `frameworks/native/services/surfaceflinger/` (Scheduler.cpp:360-370, 757-787; LayerHistory.cpp:89-100, 191-235; SurfaceFlinger.cpp:5802-5811, 7998).
- Emulator sources (copies in `src/`): RetroArch config.def.h, retroarch.c, runloop.c, gfx/video_driver.c, dispserv_android.c (the `src/ra_*.c` copies are master-era code with no URL in `urls.txt`, so their line numbers in sections 2.1 and 5.9 are for master; the review compared the skew logic with upstream v1.22.2 `retroarch.c` and found it the same); the libretro cores listed in section 2.3; dosbox-pure; dosbox-staging vga_draw.cpp; MAME `src/mame/` drivers listed in section 2.3; PPSSPP, Mupen64Plus FZ and Azahar activity sources.
- Web: CTA-861 VIC timings via kernel `drm_edid.c` (android12-5.4); github.com/libretro/RetroArch commit history of `dispserv_android.c`; docs.libretro.com/library/handy/; ws.nesdev.org/wiki/Timing; VGA timing references (25.175 MHz / 800 / 449 = 70.086 Hz); shmups.system11.org (R-Type 55.0176 Hz); racketboy.com forum (Neo Geo 59.1856 Hz).
