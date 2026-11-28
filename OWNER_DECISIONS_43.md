# Owner decisions for build 43

Where a plan left a decision open, build 43 uses the plan's recommended option. Each entry says what was chosen and how to change it. Please review them.

## sdcard: card fixes (scummvm_autoadd_plan.md B1, B2, B5, B6)

### sdcard-D8: ship the card fixes first
- Plan: scummvm_autoadd_plan.md, D8.
- Chosen: yes (recommended). B1, B2 and B6 ship in build 43, before the ScummVM listing work. B5 ships with them because it is small.
- To change: leave the `sdcard` commit out of the release.

### sdcard-B1: how a long folder grant gets to system_server
- Plan: scummvm_autoadd_plan.md, B1 names two ways and recommends neither: a root-written file, or a short volume alias.
- Chosen: the file. An argument too long for the property goes in `/data/system/nano_uri_grant_<token>.txt`. The command then sends `<package>|@<token>`. Nano deletes the file after the answer, and before writing one it deletes any over a minute old (from a Nano stopped mid-command). Short arguments go as before. The file way needs no volume lookup in system_server. It needs no SELinux change while SELinux stays permissive, as for Nano's other `/data/system` files (Nano runs as bootanim).
- To change: `sdUriCommand` in NanoSdMove.cpp, and `NanoUriGrant.argFile` and `resolve` (with the caller in NanoNetBridge.java).

### sdcard-B1b: no length limit on hakuX's card folders
- Plan: scummvm_autoadd_plan.md, B1 (hakuX's card `ROMs/xbox` was exactly at the limit).
- Chosen: `nanohakux::folderOf` no longer leaves out a folder whose grant is long. The file carries it.
- To change: put a length check back in `folderOf` (NanoHakuxSetup.cpp).

### sdcard-B6: what a scummvm.ini merge keeps
- Plan: scummvm_autoadd_plan.md, B6 ("game sections only the app copy has").
- Chosen: every section but `[scummvm]`, `[keymapper]`, `[cloud]` and `[__...]` counts as a game, also one Nano does not know. Names compare ignoring case, as in ScummVM. Comments above a section go with it. The merged file goes to ScummVM and to the card's copy, so the card has the games at once. A ScummVM copy that another card's sync wrote, unchanged since, is not merged: its games are that card's, and this card's copy goes in whole. A game deleted from one copy can come back once, when the other copy changed in the same sync window.
- To change: `kOwn` in `emusync::mergeGames` and when it applies in `emusync::keepsAppGames` (NanoEmuSync.cpp). To stop writing the card's copy, remove the `sdWriteFileSync` after the merge in `emuSettingsSync` (NanoSdMove.cpp).

### sdcard-B5: which volume is named "microSD card"
- Plan: scummvm_autoadd_plan.md, B5 ("Show microSD card").
- Chosen: only the microSD slot's card (`emuSdSlotCard()`). A USB drive keeps "SD: <id>", also when no card is in. The raw "Removable: <id>" entries stay as they are.
- To change: `buildFolderBrowser` and `folderBrowserName` in NanoMenuPS3Folder.cpp.
