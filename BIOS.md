# BIOS and system files in Thimsy OS

Some emulators in Thimsy OS need files from the original consoles (BIOS, boot ROMs, firmware) to run, or run better with them. Thimsy OS does not include, download or link to any of them (apart from the free C-BIOS for MSX, see [Bundled](#bundled)): you provide your own, dumped from consoles you own, and Thimsy OS copies them to the emulators that need them. Adding them is optional; most systems work without any. This page lists every file Thimsy OS can install, how it recognises them and where each one goes.

## Installing

Put the files in a folder (any folder, any names, subfolders are fine), then either:

- in the first-boot setup, at the optional BIOS step (shown when a microSD card is in), choose **Select Folder** and pick the folder (a folder named `bios` on the microSD card is offered first), or **Skip** to do it later, or
- any time later: **Game Settings > Install Your Own BIOS Files**.

Thimsy OS looks through the whole folder, copies everything it recognises to the emulators that need it and shows a short report: installed, already installed, different file already there (kept), not recognised. If nothing in the folder is recognised, the report says so and reminds you that you need to provide your own files.

## How files are recognised

- **By content, not by name.** Every file whose size matches a known file is checked by its SHA-1 (or MD5 where no SHA-1 is known). The name in your folder doesn't matter: each file is copied under the name its emulators need, to every emulator that uses it (one PlayStation BIOS, for example, serves all three PlayStation cores).
- **Arcade BIOS zips** (`neogeo.zip`, `pgm.zip`, `qsound.zip`, `stvbios.zip`) and **MAME's system zips for MAME4droid 2024** (`apple2gs.zip`, `ti99_4a.zip`, `coco3.zip`, `adam.zip` and the others listed below) are recognised by the files inside them (name, size and CRC32, then each one's SHA-1) and installed as the whole zip under its proper name. Other zips are ignored: Thimsy OS never takes files out of a zip. 7z and rar files are ignored.
- **Any PS2 BIOS** of 4 MB with a valid ROMDIR and ROMVER is accepted (ARMSX2 checks it the same way), even if it isn't in the list below; it keeps its own name.
- **PS Vita system software** is recognised by its update package header, any version.
- **Files marked "name and size"** are console-unique or come in many versions, so they are matched by their exact file name (and size where shown). They must keep that name. They are only copied where no such file exists yet.
- A DS `firmware.bin` must also carry the firmware's "MAC" mark, and an Xbox BIOS (`Complex_4627.bin`, `bios.bin` or `flash.bin`, installed as hakuX's `bios.bin`) must be exactly 256 KB or 1 MB.
- Files larger than 1 GB are never looked at, and links leading outside the chosen folder are not followed.

## Installer behaviour

- Nothing is ever overwritten. If the same file is already there it counts as already installed; if a different file is there it is kept and reported.
- Each file is written to a temporary file next to its destination, synced, then renamed into place, and given the owner, permissions and SELinux label the emulator's folder needs.
- Thimsy OS only touches an emulator when at least one file for it was newly installed in this run: only then does it stop the app, change its settings (ARMSX2's BIOS choice, M64Plus FZ's 64DD IPL, ScummVM's extra path, Vita3K's first-run flag), set ownership or labels, or create its folders. A folder with nothing recognised changes nothing at all.
- An emulator app that isn't installed is skipped and reported as not installed.
- Before copying, the free space on each drive is checked (each file rounded up to the drive's block size, plus 8 MB spare). Files for a drive without room are skipped with a message; files for other drives still install.
- Where an emulator uses one file of several (ARMSX2's BIOS, YabaSanshiro's `saturn_bios.bin`), the version for your region is chosen from the system language: Japanese selects Japan; English (US) and other languages of the Americas select USA; European languages select Europe; others select USA. All matched versions are installed anyway. ARMSX2's choice is only set when it has none or its file is gone.
- **PS Vita:** Vita3K installs its firmware itself. Thimsy OS copies the package into Vita3K's folder, marks Vita3K's first-run setup as done, starts Vita3K and asks it to install the package (about a minute; Vita3K's window shows meanwhile), then closes it. This is skipped when the firmware is already installed, or when there isn't room for about three times the package size.
- What was installed where is remembered in `/data/system/nano_bios/installed.json`. After **Move Emulator Files to microSD**, files that are now missing where an emulator looks are copied again from a copy that is still there (most move with their emulator's folder anyway).

## Bundled

- **C-BIOS 0.29a** for MSX (free, 2-clause BSD licence; the licence is in each machine folder and in NOTICE) is shipped in RetroArch's `system/Machines` (`MSX - C-BIOS`, `MSX2 - C-BIOS`, `MSX2+ - C-BIOS`). It runs MSX cartridge games. The real blueMSX `Machines` and `Databases` folders add disk support; installing them keeps the C-BIOS machines as they are.

## Never installed

- **Games, and files that belong to a game**: for example Sonic & Knuckles' `sk.bin` / `sk2chip.bin`, the KOF95 and Ultraman Saturn cartridges, CPS-3 game sets, per-game BIOS files such as `3do_arcade_saot.bin`. These are games, not system files.
- **Cheat cartridges** (Game Genie, Action Replay ROMs): not system files.
- **Flycast's arcade BIOS sets** (`naomi.zip`, `naomi2.zip`, `awbios.zip`, `hod2bios.zip`, `f355bios.zip`, `f355dlx.zip`, `airlbios.zip`): left out on purpose.
- **WatermelonDS** keeps its built-in free BIOS: its custom BIOS mode also needs the console-unique DS firmware, so Thimsy OS doesn't switch it. DSi files are not installed for the same reason.

These systems need no BIOS: PSP, Xbox, 3DS (decrypted games), N64 cartridges, Virtual Boy, Atari 2600, WonderSwan, Neo Geo Pocket, PICO-8, 32X, and CPS-1/CPS-2 in FBNeo.

## Listed in setup

The setup step and Game Settings > Install Your Own BIOS Files list these (the files for your region are shown; the others are under Alternates). Everything in the next section is installed, listed or not.

| | System | Emulators | Files |
|---|---|---|---|
| Required | PlayStation 2 | ARMSX2 | `ps2-0230a-20080220.bin`, `ps2-0230e-20080220.bin`, `ps2-0230j-20080220.bin`, `ps2-0220a-20060210.bin`, `ps2-0200a-20040614.bin`, `ps2-0200e-20040614.bin`, `ps2-0160a-20020207.bin`, `ps2-0100j-20000117.bin` |
| Required | PS Vita | Vita3K | `PSVUPDAT.PUP`, `PSP2UPDAT.PUP` |
| Required | Saturn | YabaSanshiro, Beetle Saturn | `mpr-17933.bin`, `sega_101.bin`, `saturn_bios.bin` |
| Required | 3DO | Opera | `panafz10.bin`, `panafz1.bin`, `panafz1j.bin`, `goldstar.bin`, `sanyotry.bin`, `panafz10e-anvil.bin` |
| Required for Sega CD games | Sega CD | Genesis Plus GX, PicoDrive | `bios_CD_U.bin`, `bios_CD_E.bin`, `bios_CD_J.bin` |
| Required for CD games | TurboGrafx-16 / PC Engine | Beetle PCE Fast | `syscard3.pce`, `syscard2.pce`, `syscard1.pce`, `gexpress.pce` |
| Required for Neo Geo games | Neo Geo | FBNeo, MAME, MAME 2003-Plus | `neogeo.zip` |
| Required for PGM games | PGM | FBNeo, MAME, MAME 2003-Plus | `pgm.zip` |
| Required for CPS-2 games in MAME | CPS-2 | MAME | `qsound.zip` |
| Optional | PlayStation | Beetle PSX HW, SwanStation, PCSX ReARMed | `scph5501.bin`, `scph5502.bin`, `scph5500.bin`, `scph101.bin`, `scph7001.bin`, `scph1001.bin`, `psxonpsp660.bin` |
| Optional | Dreamcast | Flycast | `dc_boot.bin` |

## All files by system

"Listed" means shown in the setup dialog. "How" is how the file must be in your folder.

### 3DO

Emulators: Opera. Required. Required for Japanese games that use Kanji fonts (not listed in setup). Optional (not listed in setup). BIOS variants without the RSA check.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `panafz10.bin` Panasonic FZ-10 | 1 MB | `3c912300775d1ad730dc35757e279c274c0acaad` | `51f2f43ae2f3508a14d9f56597e2d3ce` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/panafz10.bin` |
| `panafz1.bin` Panasonic FZ-1 | 1 MB | `34bf189111295f74d7b7dfc1f304d98b8d36325a` | `f47264dd47fe30f73ab3c010015c155b` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/panafz1.bin` |
| `panafz1j.bin` Panasonic FZ-1J (Japan) | 1 MB | `ec7ec62d60ec0459a14ed56ebc66761ef3c80efc` | `a496cfdded3da562759be3561317b605` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/panafz1j.bin` |
| `goldstar.bin` GoldStar GDO-101M | 1 MB | `c4a2e5336f77fb5f743de1eea2cda43675ee2de7` | `8639fd5e549bd6238cfee79e3e749114` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/goldstar.bin` |
| `sanyotry.bin` Sanyo IMP-21J TRY | 1 MB | `b01c53da256dde43ffec4ad3fc3adfa8d635e943` | `35fa1a1ebaaeea286dc5cd15487c13ea` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/sanyotry.bin` |
| `panafz10e-anvil.bin` Panasonic FZ-10-E (Anvil) | 1 MB | `a900371f0cdcdc03f79557f11d406fd71251a5fd` | `a48e6746bd7edec0f40cff078f0bb19f` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/panafz10e-anvil.bin` |
| `panafz1-kanji.bin` Panasonic FZ-1 Kanji ROM | 933636 bytes | `acd39a8fee1b9d2950d5ab447846c11fb31af63e` | `b8dc97f778a6245c58e064b0312e8281` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/panafz1-kanji.bin` |
| `panafz1j-kanji.bin` Panasonic FZ-1J Kanji ROM | 1 MB | `884515605ee243577ab20767ef8c1a7368e4e407` | `c23fb5d5e6bb1c240d02cf968972be37` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/panafz1j-kanji.bin` |
| `panafz10ja-anvil-kanji.bin` Panasonic FZ-10JA Kanji ROM | 1 MB | `2e857b957803d0331fd229328df01f3ffab69eee` | `428577250f43edc902ea239c50d2240d` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/panafz10ja-anvil-kanji.bin` |
| `panafz10-norsa.bin` Panasonic FZ-10 | 1 MB | `f05e642322c03694f06a809c0b90fc27ac73c002` | `1477bda80dc33731a65468c1f5bcbee9` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/panafz10-norsa.bin` |
| `panafz10e-anvil-norsa.bin` Panasonic FZ-10-E (Anvil) | 1 MB | `2765c7b4557cc838b32567d2428d088980295159` | `cf11bbb5a16d7af9875cca9de9a15e09` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/panafz10e-anvil-norsa.bin` |
| `panafz1j-norsa.bin` Panasonic FZ-1J | 1 MB | `a417587ae3b0b8ef00c830920c21af8bee88e419` | `f6c71de7470d16abe4f71b1444883dc8` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/panafz1j-norsa.bin` |

### Acorn Archimedes

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `aa310.zip` Archimedes 310 | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/aa310.zip` |
| `aa3000.zip` A3000, A440/1 | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/aa3000.zip`, `files/roms/aa4401.zip` |

`aa310.zip` is recognised when it holds `0277,022-02.rom`, `0277,023-02.rom`, `0277,024-02.rom`, `0277,025-02.rom`, `cmos_arthur.bin` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `0277,022-02.rom` | 131072 | `03bfe550` | `e4f3f1e37b84e716d75a32aa291a9189371daa1c` |
| `0277,023-02.rom` | 131072 | `89ece77c` | `e1979a8d3586c006e3837ff721cfa5439e6394bc` |
| `0277,024-02.rom` | 131072 | `2302db86` | `9fb3761571141bd51936daf03fa4f94fca177356` |
| `0277,025-02.rom` | 131072 | `1546a0da` | `078ef64aa8a7196d0ee4bca9926a74ade0a36173` |
| `0276,322-01.rom` | 131072 | `e6862d4c` | `13d8470f1cb2c1d15530bc7fa8a95ecc4a371cf3` |
| `0276,323-01.rom` | 131072 | `a9aeb4cf` | `f37e744ba0e48861815683b24612b0dd69d6ea8b` |
| `0276,324-01.rom` | 131072 | `7a175186` | `efdee41d9811b05a726af9ba135e0178bb1749e5` |
| `0276,325-01.rom` | 131072 | `7a8811b8` | `7a6b5d7bb94ff5e2a31469ab024c8b2d4d295709` |
| `0283,022-01.rom` | 131072 | `24291ebf` | `758adaf6f73b4041a680cdf9a0b2107da12ca5a0` |
| `0283,023-01.rom` | 131072 | `44a134f1` | `2db7f06e692c3191b2e131d55a1cf997e226c7c6` |
| `0283,024-01.rom` | 131072 | `997f42b6` | `779fcb13ce4107c27a003cdc848e8a5b7e039268` |
| `0283,025-01.rom` | 131072 | `6335dba2` | `0e0631ee43acf3d005f24a4d5c11c5c60e6f29a2` |
| `0270,601-01.rom` | 131072 | `29e2890b` | `2ccdbda7494824180426d66cd38659f6ee55a045` |
| `0270,602-01.rom` | 131072 | `dd1e4893` | `2d39a5027fd164fd9409e38074c68731622406bb` |
| `0270,603-01.rom` | 131072 | `985a8703` | `87376fba36757b311f6c4178c2ac04d8a4ad063a` |
| `0270,604-01.rom` | 131072 | `f23e4c8d` | `27595da90d76d3b25b55e176e0688740c5ce39de` |
| `0270,251-01.rom` | 524288 | `023115a9` | `d3233f76d5750e04ef2bc39d5b2dfd96e6a03c45` |
| `0270,252-01.rom` | 524288 | `6db01129` | `4b801dcce4d268d5e4c2680efa23acb29e4f907f` |
| `0270,253-01.rom` | 524288 | `d749a9f2` | `c53c35b847d300989163f9e779590d1853e8adaf` |
| `0270,254-01.rom` | 524288 | `5b13c523` | `b815bdf31dd99e5b4f2d99790cc28ee8cd907b43` |
| `0296,041-01.rom` | 524288 | `b7499ef8` | `4ab53a53c531bfbecdd441c82d9f4c0517682dde` |
| `0296,042-01.rom` | 524288 | `d55a854c` | `c9308cee92cca2a626d8577ec99485ad58b8da2a` |
| `0296,043-01.rom` | 524288 | `19bc549a` | `88b02bd3df94b56284ffad1c24fa140249b7cb63` |
| `0296,044-01.rom` | 524288 | `bf86f497` | `a200dca6dbee7c0be25a7e5363a6a3e6455a3bf3` |
| `0296,041-02.rom` | 524288 | `84185879` | `2740312b32e9cb8ca6cba9f7b33b68dc0dfab810` |
| `0296,042-02.rom` | 524288 | `c7584553` | `144f8f55f06d6d0752f2f989f4f5c7cec38a43ea` |
| `0296,043-02.rom` | 524288 | `ff5acf17` | `f9c9d4eb2f465b44353257594e631d0e3706f651` |
| `0296,044-02.rom` | 524288 | `e2a3480e` | `5b48e8b66ba86568e2225d60f34e201dd5f5d52a` |
| `0276,146-01.rom` | 65536 | `9c45283c` | `9eb5bd7ad0958f194a3416d79d7e01e4c45741e1` |
| `0276,147-01.rom` | 65536 | `ad94e17f` | `1c8e39c69d4ae1b674e0f732aaa62a4403998f41` |
| `0276,148-01.rom` | 65536 | `1ab02f2d` | `dd7d216967524e64d1a03076a6081461ec8528c3` |
| `0276,149-01.rom` | 65536 | `5fd6a406` | `790af8a4c74d0f6714d528f7502443ce5898a618` |
| `cmos_arthur.bin` | 256 | `4fc66ddc` | `f0eae9a535505d82ba3488ddb7895434df940d73` |
| `cmos_riscos2.bin` | 256 | `1ecf3369` | `96163285797e0d54017d8d4ae87835328a4658bd` |
| `cmos_riscos3.bin` | 256 | `96ed59b2` | `9dab30b4c3305e1142819687889fca334b532679` |

`aa3000.zip` is recognised when it holds `0283,022-01.rom`, `0283,023-01.rom`, `0283,024-01.rom`, `0283,025-01.rom`, `cmos_riscos2.bin` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `0283,022-01.rom` | 131072 | `24291ebf` | `758adaf6f73b4041a680cdf9a0b2107da12ca5a0` |
| `0283,023-01.rom` | 131072 | `44a134f1` | `2db7f06e692c3191b2e131d55a1cf997e226c7c6` |
| `0283,024-01.rom` | 131072 | `997f42b6` | `779fcb13ce4107c27a003cdc848e8a5b7e039268` |
| `0283,025-01.rom` | 131072 | `6335dba2` | `0e0631ee43acf3d005f24a4d5c11c5c60e6f29a2` |
| `0270,601-01.rom` | 131072 | `29e2890b` | `2ccdbda7494824180426d66cd38659f6ee55a045` |
| `0270,602-01.rom` | 131072 | `dd1e4893` | `2d39a5027fd164fd9409e38074c68731622406bb` |
| `0270,603-01.rom` | 131072 | `985a8703` | `87376fba36757b311f6c4178c2ac04d8a4ad063a` |
| `0270,604-01.rom` | 131072 | `f23e4c8d` | `27595da90d76d3b25b55e176e0688740c5ce39de` |
| `0270,251-01.rom` | 524288 | `023115a9` | `d3233f76d5750e04ef2bc39d5b2dfd96e6a03c45` |
| `0270,252-01.rom` | 524288 | `6db01129` | `4b801dcce4d268d5e4c2680efa23acb29e4f907f` |
| `0270,253-01.rom` | 524288 | `d749a9f2` | `c53c35b847d300989163f9e779590d1853e8adaf` |
| `0270,254-01.rom` | 524288 | `5b13c523` | `b815bdf31dd99e5b4f2d99790cc28ee8cd907b43` |
| `0296,041-01.rom` | 524288 | `b7499ef8` | `4ab53a53c531bfbecdd441c82d9f4c0517682dde` |
| `0296,042-01.rom` | 524288 | `d55a854c` | `c9308cee92cca2a626d8577ec99485ad58b8da2a` |
| `0296,043-01.rom` | 524288 | `19bc549a` | `88b02bd3df94b56284ffad1c24fa140249b7cb63` |
| `0296,044-01.rom` | 524288 | `bf86f497` | `a200dca6dbee7c0be25a7e5363a6a3e6455a3bf3` |
| `0296,041-02.rom` | 524288 | `84185879` | `2740312b32e9cb8ca6cba9f7b33b68dc0dfab810` |
| `0296,042-02.rom` | 524288 | `c7584553` | `144f8f55f06d6d0752f2f989f4f5c7cec38a43ea` |
| `0296,043-02.rom` | 524288 | `ff5acf17` | `f9c9d4eb2f465b44353257594e631d0e3706f651` |
| `0296,044-02.rom` | 524288 | `e2a3480e` | `5b48e8b66ba86568e2225d60f34e201dd5f5d52a` |
| `cmos_riscos2.bin` | 256 | `1ecf3369` | `96163285797e0d54017d8d4ae87835328a4658bd` |
| `cmos_riscos3.bin` | 256 | `96ed59b2` | `9dab30b4c3305e1142819687889fca334b532679` |

### Acorn Atom

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `atom.zip` | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/atom.zip` |
| `atom_discpack.zip` Disc Pack | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/atom_discpack.zip` |

`atom.zip` is recognised when it holds `abasic.ic20`, `afloat.ic21` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `abasic.ic20` | 8192 | `289b7791` | `0072c83458a9690a3ea1f6094f0f38cf8e96a445` |
| `afloat.ic21` | 4096 | `81d86af7` | `ebcde5b36cb3a3344567cbba4c7b9fde015f4802` |

`atom_discpack.zip` is recognised when it holds `dosrom.ic15` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `dosrom.ic15` | 4096 | `c431a9b7` | `71ea0a4b8d9c3caf9718fc7cc279f4306a23b39c` |

### Acorn Electron

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `electron64.zip` Electron 64K | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/electron64.zip` |
| `electron_plus3.zip` Plus 3 disc interface | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/electron_plus3.zip` |

`electron64.zip` is recognised when it holds `basic.rom`, `os_300.rom` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `basic.rom` | 16384 | `79434781` | `4a7393f3a45ea309f744441c16723e2ef447a281` |
| `os_300.rom` | 16384 | `f80a0cea` | `165e42ff4164a842e56f08ebd420d5027af99fdd` |

`electron_plus3.zip` is recognised when it holds `adfs.rom` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `adfs.rom` | 16384 | `3289bdc6` | `e7c7a1094d50a3579751df2007269067c8ff6812` |
| `pres_adfs_113.rom` | 16384 | `f06ca04a` | `3c8221d63457c552aa2c9a502db632ce1dea66b4` |
| `pres_adfs_115.rom` | 16384 | `8f81edc3` | `32007425058a7b0f8bd5c17b3c22552aa3a03eca` |
| `acp_dfs1770_200.rom` | 16384 | `5a3a13c7` | `d5dad7ab5a0237c44d0426cd85a8ec86545747e0` |
| `acp_dfs1770_210.rom` | 16384 | `b0661992` | `c62f1290f689788dad6a2df30ace083eb827cffe` |

### Adventure Vision

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `advision.zip` | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/advision.zip` |

`advision.zip` is recognised when it holds `ins8048-11kdp_n.u5`, `cop411l-kcn_n.u8` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `ins8048-11kdp_n.u5` | 1024 | `279e33d1` | `bf7b0663e9125c9bfb950232eab627d9dbda8460` |
| `cop411l-kcn_n.u8` | 512 | `81e95975` | `8b6f8c30dd3e9d8e43f1ea20fba2361b383790eb` |

### Amiga

Emulators: PUAE. Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `kick34005.A500` Kickstart 1.3 (A500) | 256 KB | `891e9a547772fe0c6c19b610baf8bc4ea7fcb785` | `82a21c1890cae844b3df741f2762d48d` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/kick34005.A500` |
| `kick40063.A600` Kickstart 3.1 (A600) | 512 KB | `3b7f1493b27e212830f989f26ca76c02049f09ca` | `e40a5dfb3d017ba8779faba30cbd1c8e` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/kick40063.A600` |
| `kick40068.A1200` Kickstart 3.1 (A1200) | 512 KB | `e21545723fe8374e91342617604f1b3d703094f1` | `646773759326fbac3b2311fd8c8793ee` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/kick40068.A1200` |

### Amiga CD32 / CDTV

Emulators: PUAE. Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `kick40060.CD32` CD32 Kickstart | 512 KB | `3525be8887f79b5929e017b42380a79edfee542d` | `5f8924d013dd57a89cf349f4cdedc6b1` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/kick40060.CD32` |
| `kick40060.CD32.ext` CD32 extended ROM | 512 KB | `5bef3d628ce59cc02a66e6e4ae0da48f60e78f7f` | `bb72565701b1b6faece07d68ea5da639` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/kick40060.CD32.ext` |
| `kick34005.CDTV` CDTV extended ROM | 256 KB | `7ba40ffa17e500ed9fed041f3424bd81d9c907be` | `89da1838a24460e4b93f4f0c5d92d48d` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/kick34005.CDTV` |

### APF M-1000

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `apfm1000.zip` | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/apfm1000.zip` |

`apfm1000.zip` is recognised when it holds `apf_4000.rom` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `apf_4000.rom` | 2048 | `cc6ac840` | `1110a234bcad99bd0894ad44c591389d16376ca4` |
| `trash-ii.bin` | 4096 | `3bd8640a` | `da4cd8163990adbc5acd3eab604b41e1066bb832` |
| `mod_bios.bin` | 4096 | `f320aba6` | `9442349fca8b001a5765e2fe8b84db4ece7886c1` |

### Apple IIGS

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `apple2gs.zip` | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/apple2gs.zip` |

`apple2gs.zip` is recognised when it holds `341s0632-2.bin`, `344s0047.bin`, `341-0728`, `341-0748` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `341s0632-2.bin` | 4096 | `e1c11fb0` | `141d18c36a617ab9dce668445440d34354be0672` |
| `344s0047.bin` | 16384 | `2d541944` | `5a5a77c8ec45632aea1a57cd9c9257f7f6e44668` |
| `341-0728` | 131072 | `8d410067` | `c0f4704233ead14cb8e1e8a68fbd7063c56afd27` |
| `341-0748` | 131072 | `18190283` | `c70576869deec92ca82c78438b1d5c686eac7480` |

### Apple Macintosh

Emulators: Mini vMac. Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `MacII.ROM` Macintosh II | 256 KB | `db6b504744281369794e26ba71a6e385cf6227fa` | `66223be1497460f1e60885eeb35e03cc` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/MacII.ROM` |

### Atari 5200

Emulators: a5200. Optional (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `5200.rom` | 2 KB | `6ad7a1e8c9fad486fbec9498cb48bf5bc3adc530` | `281f20ea4320404ec820fb7ec0693b38` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/5200.rom` |

### Atari 7800

Emulators: ProSystem. Optional (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `7800 BIOS (U).rom` USA | 4 KB | `d9d134bb6b36907c615a594cc7688f7bfcef5b43` | `0763f1ffb006ddbe32e52d497ee848ae` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/7800 BIOS (U).rom` |
| `7800 BIOS (E).rom` Europe | 16 KB | `5a140136a16d1d83e4ff32a19409ca376a8df874` | `397bb566584be7b9764e7a68974c4263` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/7800 BIOS (E).rom` |

### Atari 8-bit

Emulators: Atari800. Optional (not listed in setup). The core has a free OS built in; the real one runs more games.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `ATARIXL.ROM` XL/XE OS | 16 KB | `ae4f523ba08b6fd59f3cae515a2b2410bbd98f55` | `06daac977823773a3eea3422fd26a703` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/ATARIXL.ROM` |
| `ATARIBAS.ROM` BASIC | 8 KB | `3693c9cb9bf3b41bae1150f7a8264992468fc8c0` | `0bac0c6a50104045d902df4503a4c30b` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/ATARIBAS.ROM` |
| `ATARIOSA.ROM` 400/800 OS A | 10 KB | `6dd53356159a129ed12367beb3b24a771d41adb0` | `eb1f32f5d9f382db1bbfb8d7f9cb343a` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/ATARIOSA.ROM` |
| `ATARIOSB.ROM` 400/800 OS B | 10 KB | `f1f0741b1d34fb4350cf7cb8ab3b6ea11cdd8174` | `a3e8d617c95d08031fe1b20d541434b2` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/ATARIOSB.ROM` |

### Atari Lynx

Emulators: Beetle Lynx. Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `lynxboot.img` | 512 bytes | `e4ed47fae31693e016b081c6bda48da5b70d7ccb` | `fcd403db69f54290b51035d82f835e7b` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/lynxboot.img` |

### Bally Astrocade

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `astrocde.zip` | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/astrocde.zip` |

`astrocde.zip` is recognised when it holds `astro.bin` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `astro.bin` | 8192 | `ebc77f3a` | `b902c941997c9d150a560435bf517c6a28137ecc` |

### Coleco Adam

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `adam.zip` | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/adam.zip` |
| `adam_kb.zip` Keyboard | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/adam_kb.zip` |
| `adam_prn.zip` Printer | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/adam_prn.zip` |
| `adam_ddp.zip` Digital Data Pack | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/adam_ddp.zip` |
| `adam_fdc.zip` Disk drive | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/adam_fdc.zip` |

`adam.zip` is recognised when it holds `os7.u2`, `alf @1 rev 57 e3d5.u8`, `alf @2 rev 57 ae6a.u20`, `alf @3 rev 57 8534.u21`, `eos 6 rev 57 08dd.u22`, `wp_r80.rom`, `master rev a 174b.u6` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `os7.u2` | 8192 | `3aa93ef3` | `45bedc4cbdeac66c7df59e9e599195c778d86a92` |
| `alf @1 rev 57 e3d5.u8` | 8192 | `565b364a` | `ebdafad6e268e7ed1674c1fb89607622748a5b36` |
| `alf @2 rev 57 ae6a.u20` | 8192 | `44a1cff4` | `661cdf36d9699d6c21c5f9e205ebc41c707359dd` |
| `alf @3 rev 57 8534.u21` | 8192 | `77657b90` | `d25d32ab6c8fafbc21b4b925b3e644fa26d111f7` |
| `eos 6 rev 57 08dd.u22` | 16384 | `ef6403c5` | `28c7616cd02e4286f9b4c1c4a8b8850832b49fcb` |
| `wp_r80.rom` | 32768 | `58d86a2a` | `d4aec4efe1431e56fe52d83baf9118542c525255` |
| `master rev a 174b.u6` | 2048 | `035a7a3d` | `0426e6eaf18c2be9fe08066570c214ab5951ee14` |

`adam_kb.zip` is recognised when it holds `keyboard.u2` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `keyboard.u2` | 2048 | `ef204746` | `83162ffc75847328a05429135b728a63efb05b93` |

`adam_prn.zip` is recognised when it holds `printer.u2` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `printer.u2` | 2048 | `e8db783b` | `32b40679749ad0317c2c9ee9ca619fad6d850ce7` |

`adam_ddp.zip` is recognised when it holds `tape rev a 8865.u24` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `tape rev a 8865.u24` | 2048 | `6b9ea1cf` | `b970f11e8f443fa130fba02ad1f60da51bf89673` |

`adam_fdc.zip` is recognised when it holds `adam disk u10 ad 31 rev a 09-27-84.u10` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `adam disk u10 ad 31 rev a 09-27-84.u10` | 4096 | `4b0b7143` | `1cb68891c3af80e99efad7e309136ca37244f060` |

### ColecoVision

Emulators: GearColeco. Required (not listed in setup). Required for ADAM software (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `colecovision.rom` | 8 KB | `45bedc4cbdeac66c7df59e9e599195c778d86a92` | `2c66f5911e5b42b8ebe113403548eee7` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/colecovision.rom` |
| `eos.rom` ADAM EOS | 8 KB | MD5 only | `01df3140909f09aa9aac4f88890f676c` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/eos.rom` |
| `writer.rom` ADAM SmartWriter | 32 KB | MD5 only | `4fe4f6800076ea3d897d4285653447bd` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/writer.rom` |

### CPS-2

Emulators: MAME. Required for CPS-2 games in MAME. Recognised by its contents.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `qsound.zip` QSound sound chip | zip | recognised by its contents |  | yes | Put it anywhere in the folder you pick (any name; zips are opened) | RetroArch `system/mame/bios/qsound.zip` |

`qsound.zip` is recognised when it holds `dl-1425.bin` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `dl-1425.bin` | 8192 | `d6cf5ef5` | `555f50fe5cdf127619da7d854c03f4a244a0c501` |

Other files in the zip (other BIOS versions such as UniBIOS) come along with it.

### CreatiVision

Emulators: JollyCV. Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `bioscv.rom` | 2 KB | `5ac620c529e4965efb5560fe824854a44c983757` | `3b1ef759d8e3fb4071582efd33dd05f9` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/bioscv.rom` |

### Dragon 32 / 64

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `dragon32.zip` Dragon 32 | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/dragon32.zip` |
| `dragon64.zip` Dragon 64 | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/dragon64.zip` |
| `dragon_fdc.zip` Disk controller | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/dragon_fdc.zip` |

`dragon32.zip` is recognised when it holds `dragon_data_ltd_1-0.ic18`, `dragon_data_ltd_1-1.ic17` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `dragon_data_ltd_1-0.ic18` | 8192 | `e192b3eb` | `9fbba5128b8a53c65ee0586c10513a0a6fb05a7d` |
| `dragon_data_ltd_1-1.ic17` | 8192 | `0e1d7bf0` | `7088d75995cc2ec80a7eed9b9cc3d62f0f820a43` |

`dragon64.zip` is recognised when it holds `d64_1.rom`, `d64_2.rom` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `d64_1.rom` | 16384 | `60a4634c` | `f119506eaa3b4b70b9aa0dd83761e8cbe043d042` |
| `d64_2.rom` | 16384 | `17893a42` | `e3c8986bb1d44269c4587b04f1ca27a70b0aaa2e` |

`dragon_fdc.zip` is recognised when it holds `ddos10.rom` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `ddos10.rom` | 8192 | `b44536f6` | `a8918c71d319237c1e3155bb38620acb114a80bc` |

### Dreamcast

Emulators: Flycast. Optional. A few games need it. Optional (not listed in setup). Flash memory (settings) of a Dreamcast.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `dc_boot.bin` | 2 MB | `8951d1bb219ab2ff8583033d2119c899cc81f18c` | `e10c53c2f8b90bab96ead2d368858623` | yes | Put it anywhere in the folder you pick (any name) | Flycast `data/dc_boot.bin`; RetroArch `system/dc/dc_boot.bin` |
| `dc_nvmem.bin` Flash memory | 128 KB | matched by name and size |  | no | Must keep this exact name | Flycast `data/dc_nvmem.bin` (only if none there); RetroArch `system/dc/dc_nvmem.bin` (only if none there) |
| `dc_flash.bin` Flash memory | 128 KB | matched by name and size |  | no | Must keep this exact name | Flycast `data/dc_nvmem.bin` (only if none there); RetroArch `system/dc/dc_nvmem.bin` (only if none there) |

### FM Towns

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `fmtownsux.zip` FM Towns II UX | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/fmtownsux.zip` |
| `fmtmarty.zip` FM Towns Marty | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/fmtmarty.zip` |

`fmtownsux.zip` is recognised when it holds `fmt_dos_a.rom`, `fmt_dic.rom`, `fmt_fnt.rom`, `fmt_sys_a.rom`, `mytownsux.rom` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `fmt_dos_a.rom` | 524288 | `22270e9f` | `a7e97b25ff72b14121146137db8b45d6c66af2ae` |
| `fmt_dic.rom` | 524288 | `82d1daa2` | `7564020dba71deee27184824b84dbbbb7c72aa4e` |
| `fmt_fnt.rom` | 262144 | `dd6fd544` | `a216482ea3162f348fcf77fea78e0b2e4288091a` |
| `fmt_sys_a.rom` | 262144 | `92f3fa67` | `be21404098b23465d24c4201a81c96ac01aff7ab` |
| `mytownsux.rom` | 32 | `5cc7e6bc` | `e245f8086df57ce6e48853f0e13525f738e5c4d8` |

`fmtmarty.zip` is recognised when it holds `mrom.m36`, `mrom.m37` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `mrom.m36` | 2097152 | `9c0c060c` | `5721c5f9657c570638352fa9acac57fa8d0b94bd` |
| `mrom.m37` | 2097152 | `fb66bb56` | `e273b5fa618373bdf7536495cd53c8aac1cce9a5` |

### FM-7

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `fm7.zip` FM-7 | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/fm7.zip` |
| `fm77av.zip` FM77AV | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/fm77av.zip` |

`fm7.zip` is recognised when it holds `fbasic300.rom`, `subsys_c.rom`, `tl11-11.m152`, `kanji.rom` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `fbasic300.rom` | 31744 | `87c98494` | `d7e3603b0a2442c7632dad45f9704d9ad71968f5` |
| `subsys_c.rom` | 10240 | `24cec93f` | `50b7283db6fe1342c6063fc94046283f4feddc1c` |
| `tl11-11.m152` | 2048 | `3585ea2a` | `51580d4221b93c3d3a5c05bc5dfed8f395e43c2d` |
| `kanji.rom` | 131072 | `62402ac9` | `bf52d22b119d54410dad4949b0687bb0edf3e143` |

`fm77av.zip` is recognised when it holds `initiate.rom`, `fbasic30.rom`, `subsys_a.rom`, `subsys_b.rom`, `subsyscg.rom` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `initiate.rom` | 8192 | `785cb06c` | `b65987e98a9564a82c85eadb86f0204eee5a5c93` |
| `fbasic30.rom` | 31744 | `a96d19b6` | `8d5f0cfe7e0d39bf2ab7e4c798a13004769c28b2` |
| `subsys_c.rom` | 10240 | `24cec93f` | `50b7283db6fe1342c6063fc94046283f4feddc1c` |
| `subsys_a.rom` | 8192 | `e8014fbb` | `038cb0b42aee9e933b20fccd6f19942e2f476c83` |
| `subsys_b.rom` | 8192 | `9be69fac` | `0305bdd44e7d9b7b6a17675aff0a3330a08d21a8` |
| `subsyscg.rom` | 8192 | `e9f16c42` | `8ab466b1546d023ba54987790a79e9815d2b7bb2` |
| `kanji.rom` | 131072 | `62402ac9` | `bf52d22b119d54410dad4949b0687bb0edf3e143` |

### Gamate

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `gamate.zip` | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/gamate.zip` |

`gamate.zip` is recognised when it holds `gamate_bios_umc.bin` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `gamate_bios_umc.bin` | 4096 | `07090415` | `ea449dc607601f9a68d855ad6ab53800d2e99297` |
| `gamate_bios_bit.bin` | 4096 | `03a5f3a7` | `4e9dfbfe916ca485530ef4221593ab68738e2217` |

### Game Boy / Game Boy Color

Emulators: SameBoy, mGBA. Optional (not listed in setup). Shows the console's start-up screen. Optional (not listed in setup). Game Boy games in Game Boy Advance mode.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `dmg_boot.bin` Game Boy | 256 bytes | `4ed31ec6b0b175bb109c0eb5fd3d193da823339f` | `32fbbd84168d3482956eb3c5051637f5` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/dmg_boot.bin`; RetroArch `system/gb_bios.bin` |
| `cgb_boot.bin` Game Boy Color | 2304 bytes | `1293d68bf9643bc4f36954c1e80e38f39864528d` | `dbfce9db9deaa2567f6a84fde55f9680` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/cgb_boot.bin`; RetroArch `system/gbc_bios.bin` |
| `sgb_boot.bin` Super Game Boy | 256 bytes | `aa2f50a77dfb4823da96ba99309085a3c6278515` | `d574d4f9c12f305074798f54c091a8b4` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/sgb_boot.bin`; RetroArch `system/sgb_bios.bin` |
| `sgb2_boot.bin` Super Game Boy 2 | 256 bytes | `93407ea10d2f30ab96a314d8eca44fe160aea734` | `e0430bca9925fb9882148fd2dc2418c1` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/sgb2_boot.bin` |
| `agb_boot.bin` Game Boy Advance | 256 bytes | matched by name and size |  | no | Must keep this exact name | RetroArch `system/agb_boot.bin` (only if none there) |

### Game Boy Advance

Emulators: mGBA, gpSP. Optional (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `gba_bios.bin` | 16 KB | `300c20df6731a33952ded8c436f7f186d25d3492` | `a860e8c0b6d573d191e4ec7db1b1e4f6` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/gba_bios.bin` |

### Game Master

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `gmaster.zip` | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/gmaster.zip` |

`gmaster.zip` is recognised when it holds `d78c11agf_e19.u1` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `d78c11agf_e19.u1` | 4096 | `05cc45e5` | `05d73638dea9657ccc2791c0202d9074a4782c1e` |

### Game.com

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `gamecom.zip` | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/gamecom.zip` |

`gamecom.zip` is recognised when it holds `internal.bin`, `external.bin` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `internal.bin` | 4096 | `a0cec361` | `03368237e8fed4a8724f3b4a1596cf4b17c96d33` |
| `external.bin` | 262144 | `e235a589` | `97f782e72d738f4d7b861363266bf46b438d9b50` |

### GameCube

Emulators: Dolphin. Optional (not listed in setup). Plays the console's start-up animation. Optional (not listed in setup). DSP ROMs from a GameCube (Dolphin has its own).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `IPL.bin` Europe v1.2 | 2 MB | `80b8744ff5e43585392f55546bd03a673d11ef5f` | `db92574caab77a7ec99d4605fd6f2450` | no | Put it anywhere in the folder you pick (any name) | Dolphin `GC/EUR/IPL.bin` |
| `IPL.bin` Europe v1.1 | 2 MB | `f3cd0c7c61cbcefa85e7de3aff4cfa50bc508714` | `339848a0b7c2124cf155276c1e79cbd0` | no | Put it anywhere in the folder you pick (any name) | Dolphin `GC/EUR/IPL.bin` |
| `IPL.bin` Europe v1.0 | 2 MB | `f27c63e5394e2fd1606f70df004c4fc2d6027700` | `0cdda509e2da83c85bfe423dd87346cc` | no | Put it anywhere in the folder you pick (any name) | Dolphin `GC/EUR/IPL.bin` |
| `IPL.bin` NTSC v1.2 (USA, Japan) | 2 MB | `ef9194ab4804aa0aa8540d846caf291b28331165` | `b17148254a5799684c7d783206504926` | no | Put it anywhere in the folder you pick (any name) | Dolphin `GC/USA/IPL.bin`; Dolphin `GC/JAP/IPL.bin` |
| `IPL.bin` NTSC v1.1 (USA, Japan) | 2 MB | `239eacd86527ff9a75aeb7282da65797baeef010` | `019e39822a9ca3029124f74dd4d55ac4` | no | Put it anywhere in the folder you pick (any name) | Dolphin `GC/USA/IPL.bin`; Dolphin `GC/JAP/IPL.bin` |
| `IPL.bin` NTSC v1.0 (USA, Japan) | 2 MB | `a1837968288253ed541f2b11440b68f5a9b33875` | `fc924a7c879b661abc37cec4f018fdf3` | no | Put it anywhere in the folder you pick (any name) | Dolphin `GC/USA/IPL.bin`; Dolphin `GC/JAP/IPL.bin` |
| `dsp_rom.bin` DSP ROM | 8 KB | matched by name and size |  | no | Must keep this exact name | Dolphin `GC/dsp_rom.bin` (only if none there) |
| `dsp_coef.bin` DSP coefficients | 4 KB | matched by name and size |  | no | Must keep this exact name | Dolphin `GC/dsp_coef.bin` (only if none there) |

### Genesis / Master System / Game Gear

Emulators: Genesis Plus GX. Optional (not listed in setup). Shows the console's start-up screen.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `bios_MD.bin` Genesis | 2 KB | `3f50b76b0529db7f79c396b5e808cc0786ffc311` | `d3293ebaaa7f4eb2a6766b68a0fb4609` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/bios_MD.bin` |
| `bios_U.sms` Master System (USA, Europe) | 8 KB | `c315672807d8ddb8d91443729405c766dd95cae7` | `840481177270d5642a14ca71ee72844c` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/bios_U.sms`; RetroArch `system/bios_E.sms` |
| `bios_J.sms` Master System (Japan) | 8 KB | `a8c1b39a2e41137835eda6a5de6d46dd9fadbaf2` | `24a519c53f67b00640d0048ef7089105` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/bios_J.sms` |
| `bios.gg` Game Gear | 1 KB | `914aa165e3d879f060be77870d345b60cfeb4ede` | `672e104c3be3a238301aceffc3b23fd6` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/bios.gg` |

### MSX

Emulators: blueMSX. Required for MSX disk games (not listed in setup). Cartridge games already run on the built-in free C-BIOS.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `Machines, Databases` blueMSX 2.8.2 | folder | folder; recognised by `Databases/msxromdb.xml` (`573aa830b7bef4452808b40df883469907db2fed`) and `Machines/Shared Roms/MSX.rom` (`e998f0c441f4f1800ef44e42cd1659150206cf79`) |  | no | Copy the Machines and Databases folders as they are | RetroArch `system/Databases/` (whole folder); RetroArch `system/Machines/` (whole folder) |

### Neo Geo

Emulators: FBNeo, MAME, MAME 2003-Plus. Required for Neo Geo games. Recognised by its contents. MAME 2003-Plus reads it from the game's folder.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `neogeo.zip` Neo Geo BIOS set | zip | recognised by its contents |  | yes | Put it anywhere in the folder you pick (any name; zips are opened) | RetroArch `system/fbneo/neogeo.zip`; RetroArch `system/mame/bios/neogeo.zip` |

`neogeo.zip` is recognised when it holds `sm1.sm1`, `sfix.sfix`, `000-lo.lo`, `sp-s3.sp1` or `sp-s2.sp1` or any `sp-...` file (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `sp-s3.sp1` | 131072 | `91b64be3` | `720a3e20d26818632aedf2c2fd16c54f213543e1` |
| `sp-s2.sp1` | 131072 | `9036d879` | `4f5ed7105b7128794654ce82b51723e16e389543` |
| `sm1.sm1` | 131072 | `94416d67` | `42f9d7ddd6c0931fd64226a60dc73602b2819dcf` |
| `sfix.sfix` | 131072 | `c2ea0cfd` | `fd4a618cdcdbf849374f0a50dd8efe9dbab706c3` |
| `000-lo.lo` | 131072 | `5a86cff2` | `5992277debadeb64d1c1c64b0a92d9293eaf7e4a` |
| `sfix.sfx` | 131072 | `354029fc` | `4ae4bf23b4c2acff875775d4cbff5583893ce2a1` |
| `mame.sm1` | 131072 | `97cf998b` | `977387a7c76ef9b21d0b01fa69830e949a9a9626` |
| `mamelo.lo` | 65536 | `e09e253c` | `2b1c719531dac9bb503f22644e6e4236b91e7cfc` |

Other files in the zip (other BIOS versions such as UniBIOS) come along with it.

### NES / Famicom

Emulators: Mesen, FCEUmm. Required for Famicom Disk System games (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `disksys.rom` | 8 KB | `57fe1bdee955bb48d357e463ccbf129496930b62` | `ca30b50f880eb660a320674ed365ef7a` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/disksys.rom` |

### Nintendo 3DS

Emulators: Azahar. Optional (not listed in setup). From your own 3DS. Not needed for decrypted games: Azahar has its own fonts and system data. The real shared font fixes Chinese, Korean and Taiwanese text.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `boot9.bin` ARM9 boot ROM | 64 KB | matched by name and size |  | no | Must keep this exact name | Azahar `sysdata/boot9.bin` (only if none there) |
| `sector0x96.bin` | 512 bytes | matched by name and size |  | no | Must keep this exact name | Azahar `sysdata/sector0x96.bin` (only if none there) |
| `otp.bin` | 256 bytes | matched by name and size |  | no | Must keep this exact name | Azahar `sysdata/otp.bin` (only if none there) |
| `seeddb.bin` | any size | matched by name |  | no | Must keep this exact name | Azahar `sysdata/seeddb.bin` (only if none there) |
| `shared_font.bin` | any size | matched by name |  | no | Must keep this exact name | Azahar `sysdata/shared_font.bin` (only if none there) |
| `keys.txt` | any size | matched by name |  | no | Must keep this exact name | Azahar `sysdata/keys.txt` (only if none there) |
| `movable.sed` | 288 bytes to 320 bytes | matched by name and size |  | no | Must keep this exact name | Azahar `nand/private/movable.sed` (only if none there) |
| `SecureInfo_A` | 273 bytes | matched by name and size |  | no | Must keep this exact name | Azahar `nand/rw/sys/SecureInfo_A` (only if none there) |
| `LocalFriendCodeSeed_B` | 272 bytes | matched by name and size |  | no | Must keep this exact name | Azahar `nand/rw/sys/LocalFriendCodeSeed_B` (only if none there) |

### Nintendo 64

Emulators: M64Plus FZ, Mupen64Plus. Required for 64DD disks (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `64DD_IPL.bin` 64DD IPL (Japan) | 4 MB | `bf861922dcb78c316360e3e742f4f70ff63c9bc3` | `8d3d9f294b6e174bc7b1d2fd1c727530` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/Mupen64plus/IPL.n64`; M64Plus FZ `files/64DD_IPL.bin`; M64Plus FZ's 64DD IPL setting points at it if unset |

### Nintendo DS

Emulators: SeedlessDS, DraStic (not included; install it yourself). Optional (not listed in setup). Firmware of your own DS.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `bios7.bin` ARM7 BIOS | 16 KB | `24f67bdea115a2c847c8813a262502ee1607b7df` | `df692a80a5b1bc90728bc3dfc76cd948` | no | Put it anywhere in the folder you pick (any name) | DraStic `system/nds_bios_arm7.bin`; SeedlessDS `system/nds_bios_arm7.bin` |
| `bios9.bin` ARM9 BIOS | 4 KB | `bfaac75f101c135e32e2aaf541de6b1be4c8c62d` | `a392174eb3e572fed6447e956bde4b25` | no | Put it anywhere in the folder you pick (any name) | DraStic `system/nds_bios_arm9.bin`; SeedlessDS `system/nds_bios_arm9.bin` |
| `firmware.bin` DS firmware | 128 KB | matched by name and size |  | no | Must keep this exact name | DraStic `system/nds_firmware.bin` (only if none there) |
| `firmware.bin` DS Lite firmware | 256 KB | matched by name and size |  | no | Must keep this exact name | DraStic `system/nds_firmware.bin` (only if none there); SeedlessDS `system/nds_firmware.bin` (only if none there) |
| `firmware.bin` DS firmware (iQue) | 512 KB | matched by name and size |  | no | Must keep this exact name | DraStic `system/nds_firmware.bin` (only if none there) |

### Oric

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `oric1.zip` Oric-1 | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/oric1.zip` |
| `orica.zip` Oric Atmos | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/orica.zip` |

`oric1.zip` is recognised when it holds `basic10.rom` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `basic10.rom` | 16384 | `f18710b4` | `333116e6884d85aaa4dfc7578a91cceeea66d016` |
| `basic10uk.rom` | 16384 | `d6006a01` | `84759cc8faae53070f1c8ce8408982a0edcb0796` |

`orica.zip` is recognised when it holds `basic11b.rom` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `basic11b.rom` | 16384 | `c3a92bef` | `9451a1a09d8f75944dbd6f91193fc360f1de80ac` |
| `basic12.rom` | 16384 | `dc4f22dc` | `845e1a893de3dc0f856fdf2f69c3b73770b4094f` |
| `basic121.rom` | 16384 | `0a2860b1` | `b727d5c3bbc8cb1d510f224eb1e0d90d609e8506` |
| `basic122.rom` | 16384 | `5ef2a861` | `9ab6dc47b6e9dc65a4137ce0f0f12fc2b6ca8442` |
| `bas11_de.rom` | 16384 | `65233b2d` | `b01cabb1a21980a6785a2fe37a8f8572c892123f` |
| `bas11_es.rom` | 16384 | `47bf26c7` | `4fdbadd68db9ab8ad1cd56b4e5cbe51a9c3f11ae` |
| `bas11_fr.rom` | 16384 | `603b1fbf` | `2a4583df3b59ca454d67d5631f242c96ec4cf99a` |
| `bas11_se.rom` | 16384 | `a71523ac` | `ce53acf84baec6ab5cbac9f9cefa71b3efeb2ead` |
| `bas11_uk.rom` | 16384 | `303370d1` | `589ff66fac8e06d65af3369491faa67a71f1322a` |
| `bas12es_le.rom` | 16384 | `70de4aeb` | `b327418aa7d8a5a03c135e3d8acdd511df625893` |
| `bas12fr_le.rom` | 16384 | `47a437fc` | `70271bc3ed5c3bf4d339d6f5de3de8c3c50ff573` |
| `bas12ge_le.rom` | 16384 | `f5f0dd52` | `75359302452ee7b19537698f124aaefd333688d0` |
| `bas12sw_le.rom` | 16384 | `100abe68` | `6211d5969c4d7a6acb86ed19c5e51a33a3bef431` |
| `bas12uk_le.rom` | 16384 | `00fce8a6` | `d40558bdf61b8aba6260293c9424fd463be7fad8` |
| `bas121es_le.rom` | 16384 | `87ec679b` | `5de6a5f5121f69069c9b93d678046e814b5b64e9` |
| `bas121fr_le.rom` | 16384 | `e683dec2` | `20df7ebc0f13aa835f286d50137f1a7ff7430c29` |
| `bas121ge_le.rom` | 16384 | `94fe32bf` | `1024776d20030d602e432e50014502524658643a` |
| `bas121sw_le.rom` | 16384 | `e6ad11c7` | `309c94a9861fcb770636dcde1801a5c68ca819b4` |
| `bas121uk_le.rom` | 16384 | `75aa1aa9` | `ca99e244d9cbef625344c2054023504a4f9dcfe4` |
| `bas122es_le.rom` | 16384 | `9144f9e0` | `acf2094078af057e74a31d90d7010be51b9033fa` |
| `bas122fr_le.rom` | 16384 | `370cfda4` | `fad9a0661256e59bcc2915578647573e4128e1bb` |
| `bas122ge_le.rom` | 16384 | `9a42bd62` | `8a9c80f314daf4e5e64fa202e583b8a65796db8b` |
| `bas122sw_le.rom` | 16384 | `e7fd57a4` | `c75cbf7cfafaa02712dc7ca2f972220aef86fb8d` |
| `bas122uk_le.rom` | 16384 | `9865bcd7` | `2a92e2d119463e682bf10647e3880e26656d65b5` |

### PGM

Emulators: FBNeo, MAME, MAME 2003-Plus. Required for PGM games. Recognised by its contents.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `pgm.zip` PGM BIOS set | zip | recognised by its contents |  | yes | Put it anywhere in the folder you pick (any name; zips are opened) | RetroArch `system/fbneo/pgm.zip`; RetroArch `system/mame/bios/pgm.zip` |

`pgm.zip` is recognised when it holds `pgm_t01s.rom`, `pgm_m01s.rom`, `pgm_p01s.u20` or `pgm_p02s.u20` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `pgm_t01s.rom` | 2097152 | `1a7123a0` | `cc567f577bfbf45427b54d6695b11b74f2578af3` |
| `pgm_m01s.rom` | 2097152 | `45ae7159` | `d3ed3ff3464557fd0df6b069b2e431528b0ebfa8` |
| `pgm_p01s.u20` | 131072 | `e42b166e` | `2a9df9ec746b14b74fae48b1a438da14973702ea` |
| `pgm_p02s.u20` | 131072 | `78c15fa2` | `885a6558e022602cc6f482ac9667ba9f61e75092` |

Other files in the zip (other BIOS versions such as UniBIOS) come along with it.

### PlayStation

Emulators: Beetle PSX HW, SwanStation, PCSX ReARMed. Optional. Much better compatibility; recommended. Optional (not listed in setup). The PS1 BIOS of a PS3.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `scph5501.bin` USA | 512 KB | `0555c6fae8906f3f09baf5988f00e55f88e9f30b` | `490f666e1afb15b7362b406ed1cea246` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/scph5501.bin` |
| `scph5502.bin` Europe | 512 KB | `f6bc2d1f5eb6593de7d089c425ac681d6fffd3f0` | `32736f17079d0b2b7024407c39bd3050` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/scph5502.bin` |
| `scph5500.bin` Japan | 512 KB | `b05def971d8ec59f346f2d9ac21fb742e3eb6917` | `8dd7d5296a650fac7319bce665a6a53c` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/scph5500.bin` |
| `scph101.bin` USA v4.5 | 512 KB | `dcffe16bd90a723499ad46c641424981338d8378` | `6e3735ff4c7dc899ee98981385f6f3d0` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/scph101.bin` |
| `scph7001.bin` USA v4.1 | 512 KB | `14df4f6c1e367ce097c11deae21566b4fe5647a9` | `1e68c231d0896b7eadcad1d7d8e76129` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/scph7001.bin` |
| `scph1001.bin` USA v2.0 | 512 KB | `10155d8d6e6e832d6ea66db9bc098321fb5e8ebf` | `924e392ed05558ffdb115408c263dccf` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/scph1001.bin` |
| `psxonpsp660.bin` Region-free, from a PSP | 512 KB | `96880d1ca92a016ff054be5159bb06fe03cb4e14` | `c53ca5908936d412331790f4426c6c33` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/psxonpsp660.bin` |
| `ps1_rom.bin` From a PS3 | any size | `c40146361eb8cf670b19fdc9759190257803cab7` | `81bbe60ba7a3d1cea1d48c14cbcc647b` | no | Must keep this exact name | RetroArch `system/ps1_rom.bin` |

### PlayStation 2

Emulators: ARMSX2. Required. Any other PS2 BIOS that ARMSX2 accepts is installed too. Optional (not listed in setup). Files dumped together with a PS2 BIOS.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `ps2-0230a-20080220.bin` USA v2.30 | 4 MB | `f9229fe159d0353b9f0632f3fdc66819c9030458` | `21038400dc633070a78ad53090c53017` | yes | Put it anywhere in the folder you pick (any name) | ARMSX2 `bios/ps2-0230a-20080220.bin`; ARMSX2's BIOS setting is set to it if it has none |
| `ps2-0230e-20080220.bin` Europe v2.30 | 4 MB | `9915b5ba56798f4027ac1bd8d10abe0c1c9c326a` | `dc69f0643a3030aaa4797501b483d6c4` | yes | Put it anywhere in the folder you pick (any name) | ARMSX2 `bios/ps2-0230e-20080220.bin`; ARMSX2's BIOS setting is set to it if it has none |
| `ps2-0230j-20080220.bin` Japan v2.30 | 4 MB | `fbd54bfc020af34008b317dcb80b812dd29b3759` | `80ac46fa7e77b8ab4366e86948e54f83` | yes | Put it anywhere in the folder you pick (any name) | ARMSX2 `bios/ps2-0230j-20080220.bin`; ARMSX2's BIOS setting is set to it if it has none |
| `ps2-0220a-20060210.bin` USA v2.20 | 4 MB | `92e488d5b2705e4cca83d4d1efbc421012faf83e` | `cb801b7920a7d536ba07b6534d2433ca` | yes | Put it anywhere in the folder you pick (any name) | ARMSX2 `bios/ps2-0220a-20060210.bin`; ARMSX2's BIOS setting is set to it if it has none |
| `ps2-0200a-20040614.bin` USA v2.00 | 4 MB | `7a62e5f48603582707e9898eb055ea3eaee50d4c` | `d333558cc14561c1fdc334c75d5f37b7` | yes | Put it anywhere in the folder you pick (any name) | ARMSX2 `bios/ps2-0200a-20040614.bin`; ARMSX2's BIOS setting is set to it if it has none |
| `ps2-0200e-20040614.bin` Europe v2.00 | 4 MB | `434bc0b4eb4827da0773ec0795aadc5162569a07` | `dc752f160044f2ed5fc1f4964db2a095` | yes | Put it anywhere in the folder you pick (any name) | ARMSX2 `bios/ps2-0200e-20040614.bin`; ARMSX2's BIOS setting is set to it if it has none |
| `ps2-0160a-20020207.bin` USA v1.60 | 4 MB | `f9a5d629a036b99128f7cb530c6e3ca016e9c8b7` | `d5ce2c7d119f563ce04bc04dbc3a323e` | yes | Put it anywhere in the folder you pick (any name) | ARMSX2 `bios/ps2-0160a-20020207.bin`; ARMSX2's BIOS setting is set to it if it has none |
| `ps2-0100j-20000117.bin` Japan v1.00 | 4 MB | `aea061e6e263fdcc1c4fdbd68553ef78dae74263` | `acf4730ceb38ac9d8c7d8e21f2614600` | yes | Put it anywhere in the folder you pick (any name) | ARMSX2 `bios/ps2-0100j-20000117.bin`; ARMSX2's BIOS setting is set to it if it has none |
| Any other PS2 BIOS | 4 MB | checked by ARMSX2 |  | yes | Put it anywhere in the folder you pick (any name) | ARMSX2 `bios/<its name>`; ARMSX2's BIOS setting is set to it if it has none |
| `<BIOS name>.nvm .mec .rom1 .rom2 .erom` Beside the BIOS, same name | any size | matched by name |  | no | Next to its PS2 BIOS, same base name | ARMSX2 `bios/<BIOS name>.<ext>` (only if none there) |

### PS Vita

Emulators: Vita3K. Required. Free download from Sony, any version 3.60 or later.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `PSVUPDAT.PUP` System software | any size | recognised by its contents |  | yes | The PSVUPDAT.PUP file; Vita3K installs it, which takes about a minute | installed by Vita3K |
| `PSP2UPDAT.PUP` Font package | any size | recognised by its contents |  | yes | The PSP2UPDAT.PUP file; Vita3K installs it, which takes about a minute | installed by Vita3K |

### PSP

Emulators: PPSSPP. Optional (not listed in setup). Fonts from a PSP's flash0 (PPSSPP has its own).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `jpn0.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/jpn0.pgf` (only if none there) |
| `kr0.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/kr0.pgf` (only if none there) |
| `ltn0.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/ltn0.pgf` (only if none there) |
| `ltn1.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/ltn1.pgf` (only if none there) |
| `ltn2.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/ltn2.pgf` (only if none there) |
| `ltn3.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/ltn3.pgf` (only if none there) |
| `ltn4.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/ltn4.pgf` (only if none there) |
| `ltn5.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/ltn5.pgf` (only if none there) |
| `ltn6.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/ltn6.pgf` (only if none there) |
| `ltn7.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/ltn7.pgf` (only if none there) |
| `ltn8.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/ltn8.pgf` (only if none there) |
| `ltn9.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/ltn9.pgf` (only if none there) |
| `ltn10.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/ltn10.pgf` (only if none there) |
| `ltn11.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/ltn11.pgf` (only if none there) |
| `ltn12.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/ltn12.pgf` (only if none there) |
| `ltn13.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/ltn13.pgf` (only if none there) |
| `ltn14.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/ltn14.pgf` (only if none there) |
| `ltn15.pgf` | any size | matched by name |  | no | Must keep this exact name | PPSSPP `PSP/flash0/font/ltn15.pgf` (only if none there) |

### RCA Studio II

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `studio2.zip` | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/studio2.zip` |

`studio2.zip` is recognised when it holds `84932.ic11`, `84933.ic12`, `85456.ic13`, `85457.ic14` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `84932.ic11` | 512 | `283b7e65` | `4b6d21cde59712ecb5941ff63d8eb161420b0aac` |
| `84933.ic12` | 512 | `a396b77c` | `023517f67af61790e6916b6c4dbe2d9dc07ae3ff` |
| `85456.ic13` | 512 | `d25cf97f` | `d489f41f1125c76cc8ed9defa82a877ae014ef21` |
| `85457.ic14` | 512 | `74aa724f` | `085832f29e0d2a387c75463d66c54fb6c1e9e72c` |

### SAM Coupé

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `samcoupe.zip` | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/samcoupe.zip` |

`samcoupe.zip` is recognised when it holds `rom31.z5` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `rom31.z5` | 32768 | `0b7e3585` | `c86601633fb61a8c517f7657aad9af4e6870f2ee` |
| `rom30.z5` | 32768 | `e535c25d` | `d390f0be420dfb12b1e54a4f528b5055d7d97e2a` |
| `rom25.z5` | 32768 | `ddadd358` | `a25ed85a0f1134ac3a481a3225f24a8bd3a790cf` |
| `rom24.z5` | 32768 | `bb23fee4` | `10cd911ba237dd2cf0c2637be1ad6745b7cc89b9` |
| `rom21.z5` | 32768 | `f6804b46` | `11dcac5fdea782cdac03b4d0d7ac25d88547eefe` |
| `rom20.z5` | 32768 | `eaf32054` | `41736323f0236649f2d5fe111f900def8db93a13` |
| `rom181.z5` | 32768 | `d25e1de1` | `cb0fa79e4d5f7df0b57ede08ea7ecc9ae152f534` |
| `rom18.z5` | 32768 | `f626063f` | `485e7d9e9a4f8a70c0f93cd6e69ff12269438829` |
| `rom14.z5` | 32768 | `08799596` | `b4e596051f2748dee9481ea4af7d15ccddc1e1b5` |
| `rom13.z5` | 32768 | `2093768c` | `af8d348fd080b18a4cbe9ed69d254be7be330146` |
| `rom12.z5` | 32768 | `7fe37dd8` | `9339a0c1f72e8512c6f32dec15ab7d6c3bb04151` |
| `rom10.z5` | 32768 | `3659d31f` | `d3de7bb74e04d5b4dc7477f70de54d540b1bcc07` |
| `rom04.z5` | 32768 | `f439e84e` | `8bc457a5c764b0bb0aa7008c57f28c30248fc6a4` |
| `rom01.z5` | 32768 | `c04acfdf` | `8976ed005c14905eec1215f0a5c28aa686a7dda2` |
| `atom.z5` | 32768 | `dec75f58` | `cd342579f066c0863e4f769c2e6757085e21b0a1` |

### Saturn

Emulators: YabaSanshiro, Beetle Saturn. Required. Beetle Saturn needs the BIOS of the game's region.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `mpr-17933.bin` USA / Europe | 512 KB | `faa8ea183a6d7bbe5d4e03bb1332519800d3fbc3` | `3240872c70984b6cbfda1586cab68dbe` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/mpr-17933.bin` |
| `sega_101.bin` Japan | 512 KB | `df94c5b4d47eb3cc404d88b33a8fda237eaf4720` | `85ec9ca47d8f6807718151cbcca8b964` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/sega_101.bin` |
| `saturn_bios.bin` Japan v1.00 | 512 KB | `2b8cb4f87580683eb4d760e4ed210813d667f0a2` | `af5828fdff51384f99b3c4926be27762` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/saturn_bios.bin` (only for Japan, or when no other Saturn BIOS is there) |

### ScummVM

Emulators: ScummVM. Optional (not listed in setup). Roland MT-32 / CM-32L sound.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `MT32_CONTROL.ROM` MT-32 | 64 KB | matched by name and size |  | no | Must keep this exact name | ScummVM extra path `MT32_CONTROL.ROM` (only if none there) |
| `MT32_PCM.ROM` MT-32 | 512 KB | matched by name and size |  | no | Must keep this exact name | ScummVM extra path `MT32_PCM.ROM` (only if none there) |
| `CM32L_CONTROL.ROM` CM-32L | 64 KB | matched by name and size |  | no | Must keep this exact name | ScummVM extra path `CM32L_CONTROL.ROM` (only if none there) |
| `CM32L_PCM.ROM` CM-32L | 1 MB | matched by name and size |  | no | Must keep this exact name | ScummVM extra path `CM32L_PCM.ROM` (only if none there) |

### Sega CD

Emulators: Genesis Plus GX, PicoDrive. Required for Sega CD games. Games need the BIOS of their region.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `bios_CD_U.bin` USA | 128 KB | `f4f315adcef9b8feb0364c21ab7f0eaf5457f3ed` | `2efd74e3232ff260e371b99f84024f7f` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/bios_CD_U.bin` |
| `bios_CD_E.bin` Europe | 128 KB | `f891e0ea651e2232af0c5c4cb46a0cae2ee8f356` | `e66fa1dc5820d254611fdcdba0662372` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/bios_CD_E.bin` |
| `bios_CD_J.bin` Japan | 128 KB | `4846f448160059a7da0215a5df12ca160f26dd69` | `278a9397d192149e84e820ac621a8edd` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/bios_CD_J.bin` |

### Sega ST-V

Emulators: Beetle Saturn, MAME, MAME 2003-Plus. Required for ST-V games (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `epr-20091.ic8` Japan | 512 KB | `eff0f54c70bce05ff3a289bf30b1027e1c8cd117` |  | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/epr-20091.ic8` |
| `epr-17952a.ic8` USA | 512 KB | `eaf1c3e5d602e1139d2090a78d7e19f04f916794` |  | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/epr-17952a.ic8` |
| `epr-17954a.ic8` Europe | 512 KB | `af79cff317e5b57d49e463af16a9f616ed1eee08` |  | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/epr-17954a.ic8` |
| `stvbios.zip` ST-V BIOS set | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | RetroArch `system/mame/bios/stvbios.zip`, `system/stvbios.zip` |

`stvbios.zip` is recognised when it holds `epr-20091.ic8` or `epr-17952a.ic8` or `epr-17954a.ic8` or `epr-19730.ic8` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `epr-20091.ic8` | 524288 | `59ed40f4` | `eff0f54c70bce05ff3a289bf30b1027e1c8cd117` |
| `epr-17952a.ic8` | 524288 | `d1be2adf` | `eaf1c3e5d602e1139d2090a78d7e19f04f916794` |
| `epr-17954a.ic8` | 524288 | `f7722da3` | `af79cff317e5b57d49e463af16a9f616ed1eee08` |
| `epr-19730.ic8` | 524288 | `d0e0889d` | `fae53107c894e0c41c49e191dbe706c9cd6e50bd` |

Other files in the zip (other BIOS versions such as UniBIOS) come along with it.

### Super A'Can

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `supracan.zip` | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/supracan.zip` |

`supracan.zip` is recognised when it holds `internal_68k.bin`, `internal_6502_1.bin`, `internal_6502_2.bin` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `internal_68k.bin` | 4096 | `8d575662` | `a8e75633662978d0a885f16a4ed0f898f278a10a` |
| `internal_6502_1.bin` | 8192 | `fc9fb05f` | `8bf17bf311afeb9974bee058ba63eef5e8b6f5c1` |
| `internal_6502_2.bin` | 8192 | `bf950ab7` | `ab8f15506308b89d2f8ef01b88aa2595d4e1e779` |

### Super Cassette Vision

Emulators: EmuSCV. Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `upd7801g.s01` | 4 KB | `6e89d1227581c76441a53d605f9e324185f1da33` | `635a978fd40db9a18ee44eff449fc126` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/upd7801g.s01` |

### Super Nintendo

Emulators: bsnes, Snes9x. Required for Satellaview, Sufami Turbo, Super Game Boy and special-chip games (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `BS-X.bin` Satellaview | 1 MB | `604556b2e62860af18db5a77f2956ebc75450020` | `fed4d8242cfbed61343d53d48432aced` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/BS-X.bin` |
| `STBIOS.bin` Sufami Turbo | 256 KB | `ef86ea192eed03d5c413fdbbfd46043be1d7a127` | `d3a44ba7d42a74d3ac58cb9c14c6a5ca` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/STBIOS.bin` |
| `SGB1.sfc` Super Game Boy | 256 KB | `973e10840db683cf3faf61bd443090786b3a9f04` | `b15ddb15721c657d82c5bab6db982ee9` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/SGB1.sfc` |
| `SGB2.sfc` Super Game Boy 2 | 512 KB | `e5b2922ca137051059e4269b236d07a22c07bc84` | `8ecd73eb4edf7ed7e81aef1be80031d5` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/SGB2.sfc` |
| `cx4.data.rom` Cx4 | 3 KB | `a002f4efba42775a31185d443f3ed1790b0e949a` | `037ac4296b6b6a5c47c440188d3c72e3` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/cx4.data.rom` |
| `dsp1.data.rom` DSP-1 | 2 KB | `589c158685fe3cf88c7ca77cb6083f0e4de9c7c3` | `3d81b45fa0c2aa8b852dfb1ece7c0971` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/dsp1.data.rom` |
| `dsp1.program.rom` DSP-1 | 6 KB | `c5d283604beebb3d832f15a0b3fc60d10dd4bddc` | `ae209fbe789fbf11a48aea5ab1197321` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/dsp1.program.rom` |
| `dsp1b.data.rom` DSP-1B | 2 KB | `19d5e822fe15ee6942e9921c3d8275a761a969df` | `1e3f568634a7d8284020dddc0ae905bc` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/dsp1b.data.rom` |
| `dsp1b.program.rom` DSP-1B | 6 KB | `03c6c0bdb40846c42aeff6b716e1ea0e001c3ae3` | `d10f446888e097cbf500f3f663cf4f6d` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/dsp1b.program.rom` |
| `dsp2.data.rom` DSP-2 | 2 KB | `aed9167cc2897cd8d0432f0b8f713fabbc48f5c4` | `e9417e29223b139c3c4b635a2a3b8744` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/dsp2.data.rom` |
| `dsp2.program.rom` DSP-2 | 6 KB | `51a274955ef493cfcf2efad7e2ee54738c81eb15` | `aa6e5922a3ed5ded54f24247c11143c5` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/dsp2.program.rom` |
| `dsp3.data.rom` DSP-3 | 2 KB | `e7ec3da203a4edba2b17c06d9384787304a025a0` | `0a81210c0a940b997dd9843281008ee6` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/dsp3.data.rom` |
| `dsp3.program.rom` DSP-3 | 6 KB | `11b7ba3501a8542af4cdfdbd1c9702ae0a0e3fae` | `d99ca4562818d49cee1f242705bba6f8` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/dsp3.program.rom` |
| `dsp4.data.rom` DSP-4 | 2 KB | `8f2ffb9c1702aa2f76191e98f3fef43f73e5aab7` | `ee4990879eb68e3cbca239c5bc20303d` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/dsp4.data.rom` |
| `dsp4.program.rom` DSP-4 | 6 KB | `1abee528a59238fd5668fdcffd598d079cb056c2` | `a151023b948b90ffc23a5b594bb6fef2` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/dsp4.program.rom` |
| `st010.data.rom` ST010 | 4 KB | `bd116501ec41c438fff210fd03233c6c9d15a514` | `254d70762b6f59f99c27c395aba7d07d` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/st010.data.rom` |
| `st010.program.rom` ST010 | 48 KB | `2d0f34074ed3c6dfd61f767ee8b18e699b9769ac` | `1d70019179a59a566a0bb5d3f2845544` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/st010.program.rom` |
| `st011.data.rom` ST011 | 4 KB | `798ab84110ca93a6e73a540f4146d21f8d9e1442` | `10bd3f4aa949737ab9836512c35bcc29` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/st011.data.rom` |
| `st011.program.rom` ST011 | 48 KB | `375eca92592621e83881f3a9d7fd5ea3e5b306dc` | `95222ebf1c0c2990bcf25db43743f032` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/st011.program.rom` |
| `st018.data.rom` ST018 | 32 KB | `b19c0f8f207d62fdabf4bf71442826063bccc626` | `49c898b60d0f15e90d0ba780dd12f366` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/st018.data.rom` |
| `st018.program.rom` ST018 | 128 KB | `388e3721b94cd074d6ba0eca8616523d2118a6c3` | `dda40ccd57390c96e49d30a041f9a9e7` | no | Put it anywhere in the folder you pick (any name) | RetroArch `system/st018.program.rom` |

### Tandy Color Computer

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `coco.zip` Color Computer 1 / 2 | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/coco.zip` |
| `coco3.zip` Color Computer 3 | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/coco3.zip` |
| `coco_fdc.zip` Disk controller | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/coco_fdc.zip` |

`coco.zip` is recognised when it holds `bas12.rom`, `extbas11.rom` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `bas10.rom` | 8192 | `00b50aaa` | `1f08455cd48ce6a06132aea15c4778f264e19539` |
| `bas11.rom` | 8192 | `6270955a` | `cecb7c24ff1e0ab5836e4a7a8eb1b8e01f1fded3` |
| `bas12.rom` | 8192 | `54368805` | `0f14dc46c647510eb0b7bd3f53e33da07907d04f` |
| `extbas10.rom` | 8192 | `6111a086` | `8aa58f2eb3e8bcfd5470e3e35e2b359e9a72848e` |
| `extbas11.rom` | 8192 | `a82a6254` | `ad927fb4f30746d820cb8b860ebb585e7f095dea` |

`coco3.zip` is recognised when it holds `coco3.rom` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `coco3.rom` | 32768 | `b4c88d6c` | `e0d82953fb6fd03768604933df1ce8bc51fc427d` |

`coco_fdc.zip` is recognised when it holds `disk11.rom` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `disk10.rom` | 8192 | `b4f9968e` | `04115be3f97952b9d9310b52f806d04f80b40d03` |
| `disk11.rom` | 8192 | `0b9c5415` | `10bdc5aa2d7d7f205f67b47b19003a4bd89defd1` |
| `ados.rom` | 8192 | `24e807cf` | `a935ea11af4c600a771e4540b661cbb4258a21d6` |
| `ados2b.rom` | 8192 | `47a59ad4` | `66bd3cf08e7f1b318e82e8c4a9848233f38f1a56` |
| `ados3.rom` | 8192 | `6f824cd1` | `de602d8d219094f1237d37a4f80032c25808b358` |
| `ados3-40.rom` | 8192 | `8afe1a04` | `a8dcc6fc0aa5589612cea2a318e7fae58d930c6c` |
| `ados3-80.rom` | 8192 | `859762f5` | `957f7d5a10e61266193b636dbf642002bcedfaa3` |
| `rgbdos_mess.rom` | 8192 | `0b0e64db` | `062ffab14dc788ec7744e528bf9bb425c3ec60ed` |
| `hdbdw3bck.rom` | 8192 | `867a3f42` | `8fd64f1c246489e0bf2b3743ae76332ff324716a` |
| `hdbdw3bc3.rom` | 8192 | `309a9efd` | `671605d61811953860466f771c1594bbade331f4` |

### TI-99/4A

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `ti99_4a.zip` | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/ti99_4a.zip` |
| `ti99_speech.zip` Speech synthesizer | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/ti99_speech.zip` |

`ti99_4a.zip` is recognised when it holds `994a_rom_hb.u610`, `994a_rom_lb.u611`, `994a_grom0.u500`, `994a_grom1.u501`, `994a_grom2.u502` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `994a_rom_hb.u610` | 4096 | `ee859c5f` | `a45245707c3dccea902b718554a882d214a82504` |
| `994a_rom_lb.u611` | 4096 | `37859301` | `f4e774fd5913b387a763f1b8de5524c54b255434` |
| `994a_grom0.u500` | 6144 | `2445a5e8` | `ea15d8b0ac52112dc0d5f4ab9a79ac8ca1cc1bbc` |
| `994a_grom1.u501` | 6144 | `b8f367ab` | `3ecead4b83ec525084c70b6123d4053f8a80e1f7` |
| `994a_grom2.u502` | 6144 | `e0bb5341` | `e255f0d65d69b927cecb8fcfac7a4c17d585ea96` |

`ti99_speech.zip` is recognised when it holds `cd2325a.u2a`, `cd2326a.u2b` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `cd2325a.u2a` | 16384 | `1f58b571` | `0ef4f178716b575a1c0c970c56af8a8d97561ffe` |
| `cd2326a.u2b` | 16384 | `65d00401` | `a367242c2c96cebf0e2bf21862f3f6734b2b3020` |

### TRS-80

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `trs80l2.zip` Model I Level II | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/trs80l2.zip` |

`trs80l2.zip` is recognised when it holds `rom-a.z1`, `rom-b.z2`, `rom-c.z3`, `mcm6670p.z29` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `rom-a.z1` | 4096 | `37c59db2` | `e8f8f6a4460a6f6755873580be6ff70cebe14969` |
| `rom-b.z2` | 4096 | `05818718` | `43c538ca77623af6417474ca5b95fb94205500c1` |
| `rom-c.z3` | 4096 | `306e5d66` | `1e1abcfb5b02d4567cf6a81ffc35318723442369` |
| `rom-a_alt.z1` | 4096 | `be46faf5` | `0e63fc11e207bfd5288118be5d263e7428cc128b` |
| `rom-b_alt.z2` | 4096 | `6c791c2d` | `2a38e0a248f6619d38f1a108eea7b95761cf2aee` |
| `rom-c_alt.z3` | 4096 | `55b3ad13` | `6279f6a68f927ea8628458b278616736f0b3c339` |
| `mcm6670p.z29` | 1024 | `0033f2b9` | `0d2cd4197d54e2e872b515bbfdaa98efe502eda7` |

### TurboGrafx-16 / PC Engine

Emulators: Beetle PCE Fast. Required for CD games.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `syscard3.pce` Super CD-ROM2 System 3.0 | 256 KB | `79f5ff55dd10187c7fd7b8daab0b3ffbd1f56a2c` | `38179df8f4ac870017db21ebcbf53114` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/syscard3.pce` |
| `syscard2.pce` CD-ROM2 System 2.x | 256 KB | `88da02e2503f7c32810f5d93a34849d470742b6d` | `3cdd6614a918616bfc41c862e889dd79` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/syscard2.pce` |
| `syscard1.pce` CD-ROM2 System 1.x | 256 KB | `a39a66da7de6ba94ab84d04eef7afeec7d4ee66a` | `2b7ccb3d86baa18f6402c176f3065082` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/syscard1.pce` |
| `gexpress.pce` Game Express CD Card | 32 KB | `014881a959e045e00f4db8f52955200865d40280` | `6d2cb14fc3e1f65ceb135633d1694122` | yes | Put it anywhere in the folder you pick (any name) | RetroArch `system/gexpress.pce` |

### V.Smile

Emulators: MAME4droid 2024 (not included; install it yourself). Required (not listed in setup).

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `vsmile.zip` | zip | recognised by its contents |  | no | Put it anywhere in the folder you pick (any name; zips are opened) | MAME4droid 2024 `files/roms/vsmile.zip` |

`vsmile.zip` is recognised when it holds `vsmile_v103.bin` (checked by name, size and CRC32, then SHA-1). Known members:

| Member | Size | CRC32 | SHA-1 |
|---|---|---|---|
| `vsmile_v103.bin` | 2097152 | `387fbc24` | `5f2fd211b6ff3a6f5121b14adc6bbf4f49e89f33` |
| `vsmile_v102.bin` | 2097152 | `0cd0bdf5` | `5c8d1eada1b6b545555b8d2b09325d7127681af8` |
| `vsmile_v100.bin` | 2097152 | `205c5296` | `7fbcf761b5885c8b1524607aabaf364b4559c8cc` |

### Xbox

Emulators: hakuX. Optional (not listed in setup). Not needed to play: hakuX runs games without them. Only used to bring saves from your own Xbox or from xemu into hakuX.

| File | Size | SHA-1 / check | MD5 | Listed | How | Installed to |
|---|---|---|---|---|---|---|
| `mcpx_1.0.bin` MCPX 1.0 boot ROM | 512 bytes | MD5 only | `d49c52a4102f6df7bcf8d0617ac475ed` | no | Put it anywhere in the folder you pick (any name) | hakuX `console/mcpx.bin` (only if none there) |
| `Complex_4627.bin` BIOS (256 KB flash) | 256 KB | matched by name and size |  | no | Must keep this exact name | hakuX `console/bios.bin` (only if none there) |
| `Complex_4627.bin` BIOS (1 MB flash) | 1 MB | matched by name and size |  | no | Must keep this exact name | hakuX `console/bios.bin` (only if none there) |
| `bios.bin` BIOS (256 KB flash) | 256 KB | matched by name and size |  | no | Must keep this exact name | hakuX `console/bios.bin` (only if none there) |
| `bios.bin` BIOS (1 MB flash) | 1 MB | matched by name and size |  | no | Must keep this exact name | hakuX `console/bios.bin` (only if none there) |
| `flash.bin` BIOS (256 KB flash) | 256 KB | matched by name and size |  | no | Must keep this exact name | hakuX `console/bios.bin` (only if none there) |
| `flash.bin` BIOS (1 MB flash) | 1 MB | matched by name and size |  | no | Must keep this exact name | hakuX `console/bios.bin` (only if none there) |
| `eeprom.bin` EEPROM | 256 bytes | matched by name and size |  | no | Must keep this exact name | hakuX `console/eeprom.bin` (only if none there) |

## Where each emulator's files go

| Emulator | Before Move Emulator Files to microSD | After |
|---|---|---|
| RetroArch | RetroArch's system folder (system_directory in retroarch.cfg; by default Internal storage/RetroArch/system) | microSD card/emulator_config/retroarch/system |
| ARMSX2 | Internal storage/Android/data/com.armsx2/files/bios | unchanged (the BIOS stays on internal storage) |
| Dolphin | Internal storage/Android/data/org.dolphinemu.dolphinemu/files/GC | microSD card/emulator_config/dolphin/GC (the GC folder becomes a link to it) |
| Flycast | Flycast's home folder/data (by default Internal storage/Android/data/com.flycast.emulator/files/data) | microSD card/emulator_config/flycast/data |
| DraStic | DraStic's system folder (by default its app folder, files/DraStic/system) | microSD card/emulator_config/drastic/system |
| M64Plus FZ | Internal storage/Android/data/org.mupen64plusae.v3.fzurita/files (and its 64DD IPL setting) | unchanged |
| PPSSPP | PPSSPP's memory stick/PSP/flash0/font | microSD card/emulator_config/ppsspp/PSP/flash0/font |
| ScummVM | ScummVM's extra path (if unset: its app folder, files/extra, set as the extra path) | unchanged |
| Azahar | Azahar's folder (only when one has been chosen in Azahar) | microSD card/emulator_config/azahar |
| hakuX | hakuX's app folder, files/console | unchanged |
| Vita3K | installed by Vita3K itself into its memory card folder (pref-path) | microSD card/emulator_config/vita3k (moves with Vita3K's memory card) |
| SeedlessDS | SeedlessDS's app folder, files/SeedlessDS/system | unchanged |
| MAME4droid 2024 | Internal storage/Android/data/com.seleuco.mame4d2024/files/roms | unchanged |

Arcade BIOS zips go to RetroArch's `system/fbneo/` (FBNeo) and `system/mame/bios/` (MAME). MAME 2003-Plus only reads BIOS zips from the folder of the game itself, so copy `neogeo.zip` / `pgm.zip` / `stvbios.zip` next to those games for it.

MAME4droid 2024 isn't part of Thimsy OS; once you install it, its MAME zips go to its own `roms` folder, which Thimsy OS's launch commands add to MAME's ROM path along with the game's folder. They are MAME's non-merged or split sets: a split set's `aa3000.zip` (also used for the A440/1) or `fm77av.zip` needs `aa310.zip` or `fm7.zip` beside it, and a machine also needs the zips of its built-in devices (`coco_fdc.zip`, `dragon_fdc.zip`, `ti99_speech.zip`, `adam_kb.zip` and so on). In MAME4droid, choose the ROMs folder your games are in as its ROMs folder, so it may open them.
