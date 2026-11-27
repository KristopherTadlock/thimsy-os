# Linux build: decision log

Date: 2026-11-25. For the owner's audit.

The owner was away and asked for autonomous progress on the Linux build: decide with the best judgement available, never wait for an answer, and log every decision. This file is that log. Each entry gives what was decided, why, what would change it, and where `linux_handheld_plan.md` records it. In the plan every such item is marked in place "by Claude (autonomous; for owner audit)", so `grep -n "autonomous; for owner audit" linux_handheld_plan.md` finds them all. Section 17.6 of the plan lists the pass.

Rule used: the plan's own recommendation, unless a strong reason pointed elsewhere; where an entry refines or departs from the recommendation, it says so. Nothing was run on a device and nothing was built. Ids: `D` is a decision in plan section 14, `Q` an open question in section 15.

## Owner decisions

The owner's own decisions of 2026-11-25, listed for context. They were not changed.

- **D1, base:** a Thimsy Base in the FlipBase mould (Buildroot external tree, mainline kernel and U-Boot, BusyBox init, no systemd, no initramfs, EROFS root, Nano as the one frontend), not a ROCKNIX downstream.
- **D3, scope:** the Android build is maintained separately; one Nano source is shared through a platform seam, with merges later.
- **D5, boot target:** 3 s cold, power-on to Nano's first complete home frame, on RK3566; 1.5 s is a stretch goal, measured, not promised.
- **D21, device layer:** the broadest device support, initially ROCKNIX's coverage, through `thimsy-import` tracking ROCKNIX's hardware layer; families arrive in waves; the Odin 2 is not a focus.
- **D18, BL31 on RK3566:** "use the FlipBase TF-A": the unit's own vendor BL31 with FlipBase's pm-config patch, never redistributed; rkbin's as the fallback.
- **D11 amended (Q12):** test Panfrost and libmali in Phase 0, ship the most stable first.
- **The SD-only principle:** "Putting Linux on an SD card should always be reversible and non-destructive." Now: no install writes internal storage; erasing Android is dropped for good; writing a bootloader with a verified backup is deferred, not excluded.
- **The storage job:** the installer writes game cards and USB drives (format and lay out, pack-ins, ROMs and BIOS from the PC, non-destructive update) for Android and Linux devices.
- **Workflow:** the implementation lives in a fork of this repository (branch `linux`), merged back later.
- **Answers:** Q1 (Paper Plane is FlipBase or BaseOS), Q2 (an ARC-D only, Android kept), Q6 (builds on this machine), Q11 (shared states: yes, tested per core, after v1), Q13 (serial wiring later), Q14 (HDMI docked in v1), Q15 (GammaOS's Linux plans don't steer this), Q22 (ROCKNIX's licence, no author recovery), Q23 (own recipes; recommended, no objection), Q26 (don't weigh BaseOS's licence).

## Decided autonomously (design)

### D2. First device

- **Decided:** option (b): the ARC-D dual-boots by an SD-only route if Phase 0 finds one, and an RG ARC-S is the Linux-only reference unit (3 s number, 1 GB floor; the first device if no route passes). Until an ARC-S is bought, the ARC-D with `mem=1G` stands in for the floor.
- **Why:** the ARC-S shares the ARC-D's panel, device-tree include and pad, so it costs one board folder, and with no vendor loader in front it is the only clean 3 s measurement.
- **Would change it:** the owner not buying an ARC-S (then the RG353VS, or the ARC-D alone with the loader's cost stated), or Phase 0 finding the ARC-D's loader costs nothing.
- **Recorded:** decision 2; section 0.

### D4. Display stack, and its amendment

- **Decided:** as recommended: no compositor in front of the home screen; (A) KMS/GBM with the DRM-master hand-off and RetroArch's own menu by hotkey for the alpha and v1, (B) an in-process libretro host in Phase 5, (C) a DRM-lease spike, (D) a compositor only if (C) fails and a Quick Menu over standalones becomes a requirement. The rotation contract on the Mali families; a per-family display setting on the Qualcomm families.
- **Why:** any compositor before the first frame works against the 3 s target; FlipBase and RetroStoneOS prove the hand-off model on this class of hardware.
- **Would change it:** Phase 0's hand-off spike failing (the child can't take master, or Nano's re-commit is unreliable); the decision then reopens.
- **Recorded:** decision 4; sections 0, 5.2, 5.3.

### D6. Repositories and licences

- **Decided:** option (a), inside this repository's fork: a top-level `linux/` directory, a BR2_EXTERNAL tree (name THIMSY). MIT for everything Thimsy writes and for FlipBase's files (`linux/LICENSE`, FlipBase's notice kept); `linux/gpl/` (its own `COPYING`, GPL-2.0) for kernel, U-Boot and device-tree patches, ROCKNIX's imports and ported recipes. The rest of the repository stays Apache-2.0. The `thimsy-nano` recipe builds the repository's own Nano tree; no Nano code is copied into `linux/`. It can be split into its own repository (`thimsy-base`) later with `git subtree split --prefix=linux`, keeping its history.
- **Why:** the owner's workflow puts the implementation in this fork; one commit then versions Nano, the shared lists and the image together, and merging back stays a single merge. The licence lines stay directory lines, which `make legal-info` and the release's source publication follow.
- **Would change it:** a wish to publish the base for other projects' use (split it out then), or the owner preferring a separate repository from the start.
- **Recorded:** decision 6; sections 0, 3.2, 4.1.
- **Refined by I5, I9 and I41 (implementation design and its review):** `libthimsy/` is a second MIT area at the repository's top level; `gpl/` holds copyleft and third-party material under each item's own licence (GPL-2.0 for kernel, U-Boot and ROCKNIX imports; GPL-3.0 for RetroArch patches; zlib for SDL patches), each patch with an SPDX line, not GPL-2.0 only; Nano's Linux build declares the GPL-2.0+ Kanji table in its licence, and its source is published from `git archive` tarballs.

### D7. Order of the shared foundations

- **Decided:** the seam first (Phase 1, shared with `android_launcher_plan.md`, done once, in this fork), then the Thimsy Base and the Linux alpha (Phase 2), then the card contract and the installer (Phases 3 and 4). The SD-card writer is built once in the reserved `sdcard` crate by whichever plan reaches it first. The patch stays Nano's source of truth until the seam has settled and the fork has merged back; the extracted tree is a build input.
- **Why:** it keeps the RG Cube image byte-identical and upstream GammaOS merges one-sided; changing the source of truth now would change how every backport is made.
- **Would change it:** the launcher plan reaching the seam first (then it is reused), or the owner deciding to make the extracted tree the source of truth.
- **Recorded:** decision 7.
- **Refined by I1 and I21:** on the `linux` branch, `nano/` is what people edit and the patch's Nano block is generated from it, kept equal by a test and by a merge driver that regenerates it on every merge; on `nano-144`, the patch stays the source until the merge back.

### D8. The card contract

- **Decided:** yes to all four parts: `/storage/<VOLID>` canonical on Linux (Cube paths per D20); the small v1 sharing set, widened only by section 6.2's core table; `linux/` folders, `systems.linux.json` and `card.json`; exFAT recommended, FAT32 accepted.
- **Why:** section 7's analysis; sharing less is what keeps two platforms from overwriting each other's files.
- **Would change it:** a device test in Phase 3 showing a shared file is damaged by either side.
- **Recorded:** decision 8.

### D9. Real ES-DE on Linux

- **Decided:** Nano's ES-DE theme only for v1; a real ES-DE stays a Phase 8 option, with no linked gamelists until the label rule is the compatibility floor.
- **Why:** it is a second frontend to maintain and a second writer of the gamelists, for no v1 feature.
- **Would change it:** users asking for ES-DE itself on Linux after v1.
- **Recorded:** decision 9.

### D10. Feature cut

- **Decided:** yes: an alpha with RetroArch only and its library on internal storage; v1 without a Nano Quick Menu over a game, Quick Resume, the DS player and the media apps.
- **Why:** each is a large separate piece of work, and none is on the path to a booting, interoperable v1.
- **Would change it:** the owner naming one of them a v1 requirement.
- **Recorded:** decision 10.
- **Refined by I44:** the alpha's library lives on the boot card's `games` drive (the exFAT partition made at first boot), not on the 256 MiB data partition that holds `/storage/emulated/0`; internal storage keeps state and small files only.

### D11. GPU driver: extension and tie-break

- **Decided:** the owner's amendment stands (both drivers switchable until Phase 0's A/B; v1 ships the more stable). Added: on a tie, Panfrost wins. Vulkan stays off on RK3566 and H700. The Mesa-only extension is accepted conditionally: Freedreno and Turnip on Qualcomm regardless; Panfrost and Panthor on every Mali family if Panfrost wins on RK3566; otherwise each Mali family gets its own A/B when its wave starts.
- **Why:** with equal stability, Panfrost removes the blob, the EULA and the out-of-tree `mali_kbase`, and FlipBase measured it reaching the frontend about 0.25 s sooner.
- **Would change it:** the A/B result.
- **Recorded:** decision 11; question 12.

### Q12. GPU A/B protocol

- **Decided:** each v1 system 30 minutes per driver with the same game, 20 suspend and resume cycles and 10 cold boots per driver; counted: crashes, GPU hangs and kernel faults, visible rendering errors, failed resumes. Fewer failures wins; tie to Panfrost. Performance and first-frame time recorded second.
- **Why:** the owner's rule ("most stable first") needs a count both drivers are measured by the same way.
- **Would change it:** the owner setting a different bar.
- **Recorded:** question 12.

### D12. Package profiles on Linux

- **Decided:** as recommended: Small, and Medium on 2 GB devices; Large and Ultra not offered on RK3566 and H700; one superset image per device; the platform selector proposed to `device_profiles_plan.md`. Small does not gain Flycast. The chosen profile is written by the installer to `thimsy/install.json` beside `thimsy/device.json`, and the first-boot wizard shows it preselected (or asks when the file is absent). From Q10: the Linux core defaults are a column of their own that takes the Cube's (Large) cores where RK3566 runs them; `mamearcade` is chosen by RAM, not profile.
- **Why:** Dreamcast needs the 2 GB image and Small is the 1 GB floor; matching the Cube's cores makes saves travel; a preselected profile saves a question without blocking plain images.
- **Would change it:** Phase 2 finding Flycast runs well in 1 GB, or `device_profiles_plan.md` landing a different selector.
- **Recorded:** decision 12; sections 6.2, 6.3, 8.2.

### D13. Image distribution

- **Decided:** installer assembly from v1, as recommended, with a refinement: one assembler, a Rust crate in the installer's engine, is also run by the release build to make the plain `.img.zst` files of certified devices. The Phase 2 alpha, before that crate exists, uses Buildroot's `genimage`; from Phase 4 the crate replaces it, checked partition by partition against `genimage`'s output.
- **Why:** about 83 products would make about 66 GB of complete images a release *(guess)*; one assembler keeps the installer's cards and the published images from drifting.
- **Would change it:** assembly proving fragile on one OS in Phase 4 (then complete images for v1's few RK3566 products, assembly from wave 1).
- **Recorded:** decision 13; sections 0, 8.2.
- **Confirmed at the review:** the design's I6 (a Python `card.py` instead of `genimage` for the alpha) was withdrawn, so this decision stands as written; a small GPT checker (`gpt_check.py`) checks every image `genimage` writes against the vold rule, and the written image leaves the `games` entry to first boot (I39).

### D14. H700 panel choice

- **Decided:** (a): a variant per panel and DDR type, chosen by typed installer questions with pictures; under assembly a variant is a different hardware-pack selection, not a separate download. A first-boot chooser later.
- **Why:** no FAT edit and no first-boot logic.
- **Would change it:** wave 1 finding the panel can be read.
- **Recorded:** decision 14; section 8.4.

### D16. Public or private alpha

- **Decided:** public, as a GitHub pre-release, once the `legal-info` sources, the Buildroot tarball and the `linux/` tree at the tag are published beside it, FlipBase's notice and the "based on FlipBase, with device patches from ROCKNIX; not affiliated" line are in place, and every imported patch has its provenance line; on the ARC-D only by a proven SD-only route. Otherwise private until Phase 4.
- **Why:** waves 1 and later need testers, and an SD-only alpha can't harm a unit.
- **Would change it:** the owner preferring a private alpha.
- **Recorded:** decision 16.
- **Conditions added at the review (I37, I38, I41):** the public alpha is built from the release targets only (no adbd, no serial getty, dropbear off or key-only, root password locked); it contains no asset `ASSETS.md` marks REPLACE or ASK (the Rodin fonts, the PS3 and DSi assets among them); and `git archive` of the whole repository at the tag is published beside it, since `legal-info` does not save the source of local packages.

### D17. Rules from the second review

- **Decided:** all five accepted; the `/storage/roms` clause stays moot.
- **Why:** each was the reviewed recommendation and none conflicts with later decisions.
- **Would change it:** a Phase 0 or Phase 3 test failing one of them (for example `fsck.exfat` too slow to run always).
- **Recorded:** decision 17; section 7.2.

### D18. BL31 on the other families

- **Decided:** each family's BL31 starts from ROCKNIX's choice (upstream TF-A on RK3576, RK3399 and H700; rkbin's on RK3326 and RK3588; Amlogic's FIP on S922X), inside Thimsy's boot chain; revisited only if that family's sleep test fails.
- **Why:** ROCKNIX's choice is the one known to boot each family; RK3566 is the owner's.
- **Would change it:** a family's sleep current or stability failing.
- **Recorded:** decision 18.

### D19. How to take FlipBase

- **Decided:** with history, by a subtree merge into `linux/` at a pinned commit (`git merge --allow-unrelated-histories -X subtree=linux <commit>`, tried on a scratch branch first; fallback `git merge -s ours --no-commit` plus `git read-tree --prefix=linux/ -u`). Files Thimsy wrote first at FlipBase's paths are resolved in that merge, Thimsy's version winning, FlipBase's kept in history. Done at the start of Phase 2a, when fetching is allowed. Later fixes by hand.
- **Why:** it keeps FlipBase's history and credit while living in this fork (D6), and the implementation can start before the network is available.
- **Would change it:** the strategy option misbehaving on a first import *(guess that it works)*, or a split into its own repository first.
- **Recorded:** decision 19; section 3.2.
- **Refined by I42:** licence and notice files are excluded from "Thimsy's version winning": FlipBase's `LICENSE` (its MIT notice) is kept as `LICENSE.FlipBase` and its `NOTICE` as is, and `linux/LICENSE` says the BaseOS-derived files FlipBase's notice names are not covered by it.

### D20. Card identity on the Cube

- **Decided:** (b) plus (a): Linux never writes a Cube-style path; at adoption it builds a prefix map from the `/storage/00000000-0000-0000-0000-NNNNNNNNNNNN/` prefixes it finds in the card's configs (and from `card.json` once Android records its id) and resolves them to the adopted drive when reading. Everything new uses tokens and relative paths. (c), vold naming volumes by serial, is raised with the Android build separately.
- **Why:** cards the Cube already wrote keep working, and nothing new depends on the mount-order id.
- **Would change it:** the Android build fixing vold first (then (b) is only for old cards).
- **Recorded:** decision 20.

### D22. Kernel per family

- **Decided:** yes, as recommended, with Q33's and Q34's answers attached.
- **Why:** ROCKNIX's patches stay importable without rebasing only on the version they were written for.
- **Would change it:** ROCKNIX stopping its tags for long (Q33), or a family needing a kernel ROCKNIX doesn't use.
- **Recorded:** decision 22; sections 0, 3.2.

### D23. Input

- **Decided:** (a), `thimsy-padd` on every family; (b), kernel normalisation, only for a family where hiding fails; InputPlumber never ships.
- **Why:** one pad identity without D-Bus, udev or GPL-3.0 on the boot path.
- **Would change it:** Q4's Phase 0 test failing on the ARC.
- **Recorded:** decision 23; sections 5.2, 5.3.

### D24. Firmware policy

- **Decided:** (a) with (c): ship only `linux-firmware` files with known licences, fetch the rest from the user's own device at install time onto a partition, and leave out a device whose firmware can be neither.
- **Why:** it keeps the image redistributable without a maker's blobs.
- **Would change it:** a maker licensing its firmware for redistribution.
- **Recorded:** decision 24; sections 1, 3.3.

### D25. Image structure

- **Decided:** yes: a userland per CPU tier, GPU packs, a hardware pack per family, assembled per device. If Phase 2a can't build a GPU pack in its tier tree and run it on the tier root, RK3566 falls back to one full Buildroot tree and this reopens before wave 1.
- **Why:** a full rebuild of about 10 to 20 h on this machine instead of 30 to 60 *(guess)*.
- **Would change it:** the host-LLVM sharing failing (the fallback above).
- **Recorded:** decision 25; sections 0, 3.2, 10.

### D26. Coverage order and the support claim

- **Decided:** yes: waves 0 to 4, wave 3 deferred by the SD-only principle, AMD64 QEMU only, a per-product status instead of a support claim.
- **Why:** devices that boot Linux from SD first, as the owner asked, and an honest status per product.
- **Would change it:** the owner allowing bootloader writing (wave 3 returns), or a family proving much harder than its wave assumes.
- **Recorded:** decision 26; sections 0, 2.4.
- **Refined by I8:** the first QEMU target is aarch64 `virt` with a software-rendering GPU pack, which boots the devices' own tier root; AMD64 QEMU stays in the plan for later.

### Q10's three small choices

- **Decided:** `pcsx_rearmed_memcard2 = "none"` on Linux; `mamearcade` the arcade default on 2 GB images and `mame2003_plus` on 1 GB; MAME's `cfg/` per platform (mechanism settled in Phase 3).
- **Why:** card 2 otherwise leaves `pcsx-card2.mcd` beside the ROMs for one core only; `mamearcade` is 257 MB and tight in 1 GB *(guess)*; `cfg/` holds controls for one device's pad.
- **Would change it:** Phase 2 finding `mamearcade` fits in 1 GB.
- **Recorded:** section 6.2; question 10.

### Linux core library names

- **Decided:** Thimsy's recipes name cores as the Android buildbot does (`mednafen_vb`, not ROCKNIX's `beetle_vb`), so the share-or-sort comparison needs aliases only where a build truly differs (`mupen64plus_next_gles3` against `mupen64plus_next`). The alias map is a column of `mods/retroarch-cores.txt`.
- **Why:** fewer aliases, fewer wrong sort decisions.
- **Would change it:** a recipe ported from ROCKNIX that can't be renamed.
- **Recorded:** section 6.2.

### Build distro

- **Decided:** a new `thimsy-linux` WSL2 distro at `D:\WSL\thimsy-linux`, apart from `thimsy-aosp`.
- **Why:** its host packages and disk stay apart from the Android build's; the plan's recommendation.
- **Would change it:** disk space on D:, or the owner preferring to reuse `thimsy-aosp`.
- **Recorded:** section 10.

## Answered autonomously (questions)

### Q10. Default cores and save-compatible pairs

- **Answered:** from verified research (two researchers, one adversarial, reading each core's source): on RK3566 Linux runs the same core as the Cube's (Large) defaults for every RetroArch system except PlayStation (`pcsx_rearmed` against `mednafen_psx_hw`); N64, DS and Dreamcast differ because Android's defaults are Android-only apps. PCSX ReARMed, Beetle PSX HW and SwanStation write memory card 1 the same way, as do gpSP and mGBA; each pair becomes a save-compatible family only after its Phase 3 round trip. Several cores keep their own files and folders, shared or sorted whole.
- **Why:** this is what makes saves travel between the Cube and the ARC.
- **Would change it:** Phase 2's speed tests (Mesen) or Phase 3's round trips failing.
- **Recorded:** section 6.2 (table, consequences, tests), 7.5, 11.2, 12; question 10.

### Q28. SDL3 rotation for SDL2 apps

- **Answered:** by reasoning (no published test of `sdl2-compat` on KMSDRM was found): the base ships SDL3 with `sdl2-compat` as its only `libSDL2`, so SDL2 apps get the SDL3 patch's rotation; every Thimsy recipe builds against the system SDL with bundled copies off. Checked per v1 standalone in Phase 2b; a failure is built against SDL3 directly or uses `display: kiosk`.
- **Why:** Thimsy builds every standalone itself, so which SDL they link is its choice.
- **Would change it:** an emulator that fails the rotation test through `sdl2-compat`.
- **Recorded:** question 28.
- **Refined by I43 (review):** Q28's `display: kiosk` and the owner's per-game compositor fallback (Q27) exist only from Phase 5 (Q29), so Phase 2's order is: for the alpha (RetroArch only), RetroArch's own rotation settings if the `drm_ctx.c` patch fails *(guess that they cover the menu)*; for SDL standalones, build against SDL3 directly, else the emulator leaves the set until Phase 5.

### Q29. Weston's kiosk-shell as root (in part)

- **Answered:** Weston documents running as root through libseat's builtin backend (`LIBSEAT_BACKEND=builtin`, `SEATD_VTBOUND=0` without a VT), and its kiosk-shell runs one full-screen app. libinput through libudev-zero and the launch cost are unverified; no v1 system on RK3566 or H700 needs the kiosk path, so the spike moves to Phase 5, with cage as the next candidate.
- **Why:** Weston's running-weston documentation.
- **Would change it:** the Phase 5 spike.
- **Recorded:** question 29.

### Q30. RK3566's starting kernel

- **Answered:** a rule: at the start of Phase 2a, the newest ROCKNIX tag; FlipBase's 7.3.5 overlay as is if that tag's RK3566 is 7.3.x, else re-checked on its 7.2.x; never an unsoaked `next` commit.
- **Why:** the import stays one to one with a released tag.
- **Would change it:** no tag at all by Phase 2a (Q33's rule).
- **Recorded:** question 30.

### Q32. RK3326 EE-clone panels

- **Answered:** treated as undetectable: a typed installer variant question with pictures and a wrong-panel recovery, as for H700; automatic for a board where wave 2 finds a readable panel id.
- **Why:** consistent with D14; no detection method is known yet.
- **Would change it:** wave 2's bring-up.
- **Recorded:** question 32.

### Q33. ROCKNIX's tag cadence

- **Answered:** unknowable; D22's fallback is enough for fixes, plus: after six months with no tag, Thimsy names a `next` commit that ROCKNIX's nightly built images for as the import point for all families, and the release-image check uses those nightly images *(guess that they stay downloadable long enough)*.
- **Why:** families can't stay frozen indefinitely, and the nightly images keep the check meaningful.
- **Would change it:** ROCKNIX's tags resuming, or its nightly images being pruned quickly.
- **Recorded:** question 33.

### Q34. Stable point releases on .0 kernels

- **Answered:** yes: a family on an unpatched .0 takes the series' last stable point release as its base. The import diff is unaffected (the importer writes ROCKNIX's patches and config, not the base tarball); a patch that stops applying stops the import; the release-image check lists the differences.
- **Why:** these devices run Wi-Fi, and the stable series is the kernel's security channel; picking fixes by hand costs more.
- **Would change it:** a stable release breaking a family's imported patches often.
- **Recorded:** question 34; section 3.2.

### Q25 and Q27, applied from the pending answers

- **Q25:** the mainline facts were already recorded; this pass added its Phase 0 pass rule (see Deferred).
- **Q27:** "nothing to decide" was already recorded; this pass added the per-emulator Rotation row to section 11.2 and to Phase 2's exit. The owner's answer says failures use the per-game compositor fallback; that fallback exists only from Phase 5, so Phase 2's order is I43's (see Q28).

## Deferred (needs hardware or the owner)

| Id | What waits | Phase item and pass rule (in the plan) |
|---|---|---|
| ~~D15~~ | buying hardware. **Decided by the owner on 2026-11-26 (OA-18): none for now; the ARC-D is the only test device.** | decided (OA-18) |
| Q4 | `thimsy-padd` hiding, intercept and external pads | Phase 0 (c): exactly one built-in pad visible to RetroArch, SDL and `evtest` over 20 launches and a resume; held buttons released within one frame |
| Q12 | the GPU A/B run | Phase 0 (c), with the protocol above |
| Q13 | serial wiring on the ARC-D | the owner, later (`TODO.md`) |
| Q20, Q21 | the ARC-D's SD-only route and loader time | Phase 0 (a), section 8.6's order and pass rule |
| Q24 | deep-sleep current with the vendor BL31 | Phase 0 (b): at most about 15 mA *(guess)*, 50 suspend and resume cycles, power-key wake; else rkbin's BL31 |
| Q25 | the ARC's Wi-Fi, Bluetooth, audio, panel waits, touch | Phase 0 (b): the checklist's pass rule |
| Q27 | rotation per v1 emulator | Phases 0 and 2: picture, menus and touch the right way up, frame time within 5% *(guess)* |
| Q31 | SM8550 boot timings | wave 3: 10 cold boots, figures stated |
| Q35 | Vulkan rotation on Qualcomm | wave 3: the cheapest option within 5% frame-time cost *(guess)* |
| Q8, Q9, Q16, Q18 | device checks of answers from code | Phase 0 (g), Phase 3, Phase 4 |
| Q10 tests | core speed, builds and save round trips | section 6.2's core tests, Phases 2 and 3 |
| D4, D11, D23, D25 | Phase 0 or 2a confirmation | each entry above says what reopens it |
| ~~Slate (gfix)~~ | Linux's default ES-DE theme. **Decided by the owner on 2026-11-26 (OA-1, below): it follows the Android release**, a modified Colorful (Simplified) prepared as `mods/50-nano.sh` does; Slate stays out, as on Android. | decided (OA-1); the work is listed under OA-1 |
| ~~Android set-up prompt (review device-safety-3)~~ | Android offered to set up (format) a freshly written card. **Decided by the owner on 2026-11-26 (OA-2): option (b), a small formatted FAT stub entry.** | decided (OA-2) |
| ~~Display EGL moves (G7D-2, G7D-3)~~ | the Android EGL and SurfaceFlinger set-up left in shared code because NanoMenu.h was frozen. **Decided by the owner on 2026-11-26 (OA-11): NanoMenu.h is unfrozen for platform-seam work; the code moves.** | decided (OA-11) |
| ~~Release-build SSH keys (gfix2)~~ | release SSH had no way to install a key. **Decided by the owner on 2026-11-26 (OA-3): the handheld-firmware pattern, root password `thimsy` by default, changeable in Nano, LAN-only by default.** | decided (OA-3) |
| ~~AAudio callback pointer lifetime (G7-audio)~~ | the short-lived stream object became AAudio's user pointer. **Decided by the owner on 2026-11-26 (OA-12): restore the long-lived player as the user pointer.** | decided (OA-12) |

## Implementation design

Decided autonomously on 2026-11-25 while writing `docs/linux/IMPLEMENTATION.md`; each entry's reasons and what would change it are in that file's section I, under the same id. Nothing was built or run, except one regeneration of the patch's Nano block from the fetched tree, which matched the committed patch byte for byte apart from four executable bits.

- **I1, Nano as files:** Nano's source becomes real files in `nano/` on the fork; the patch's Nano block is regenerated from it by `tools/nano-tree/nano_tree.py` and committed, and a test keeps the two equal. The AOSP flow still consumes `patches/thimsy/0001-thimsy.patch` unchanged in form. Refines D7 for the fork; `nano-144` keeps the patch as its source until the merge.
- **I2, what stays fetched:** files under `ps3xmb/`, `nano_esde_themes/` and `nano_esde_assets/` that the patch does not touch; everything else is committed (about 22 MB).
- **I3, names:** `platform/firmware` is today's Android firmware code, `platform/android` stays reserved for the launcher plan's app.
- **I4, builds:** `Android.bp` stays the only Android firmware build; CMake builds Linux, host and the app; `nano/build/sources.txt` partitions `Android.bp`'s list, checked by a test.
- **I5, `libthimsy/`:** a new top-level MIT library for the config store and small helpers, shared by Nano's Linux and host shims, `thimsy-session`, `thimsy-padd` and `thimsy-prop`. It keeps its own small JSON reader: `NanoJson` is Apache-2.0 and Nano-specific, and the MIT tools stay single-licence (review).
- **I6, card assembler:** withdrawn at the review. The alpha uses `genimage` as D13 says, checked by `gpt_check.py`; `card.py` waits for Phase 4.
- **I7, card layout:** GPT names `uboot`, `boot`, `root`, `data`, `games`; the data partition typed Linux home; exFAT made on first boot, label `THIMSY` (Phase 3 confirms).
- **I8, QEMU:** aarch64 `virt` with a software-rendering GPU pack is the first QEMU target, not AMD64.
- **I9, package patches:** `linux/gpl/patches/<package>/`, each patch under its package's licence.
- **I10, core sources:** a new `mods/retroarch-cores.src.lock` (core, repository, commit), since the existing lock pins binaries; core recipes stay pending until it exists.
- **I11, importer:** revised at the review: `thimsy-import` waits until ROCKNIX's real files have been read (over the web now, by a clone later), and its fixtures are cut down from those real shapes.
- **I12, language:** `thimsy-session` and `thimsy-padd` in C++17, as pure cores plus thin I/O layers.
- **I13, pad identity:** `Thimsy Gamepad`, `BUS_VIRTUAL`, vendor 0x5448, product 0x0001, version 1; the kernel's gamepad layout.
- **I14, paths:** Android paths on Linux through links in the read-only root, not by editing Nano's path literals; widened at the review to `/data` as a whole, `/storage/self/primary` and a curated `/system/bin`, with a ratcheted inventory of every absolute root.
- **I15, shell commands:** routed on Linux by `--wrap=system,popen,pclose,execv,execvp,execve` through a gate that translates or refuses Android commands and programs; record-only on the host (widened at the review).
- **I16, stubs:** inert binder, SurfaceFlinger, sensor and AHardwareBuffer stubs for non-Android builds, under a shrinking budget.
- **I17, headless E2E:** Nano runs on its real thread and real time in a user-namespace jail laid out like the Linux root (with `/proc`, read-only binds, no real-root fallback), with a GLES stub, a host DRM substitute with a frame barrier, and FIFO input devices (revised at the review).
- **I18, build guard:** `linux/build.sh` and the C++ test Makefiles refuse while the build lock is held, `MemAvailable` is under 9 GB, an Android build container runs, `thimsy-aosp` is running or the VM's load is high; compilers run under a memory limit (widened at the review).
- **I19, Android chain:** no reimplementation; only the patch's producer changes in the fork, now run by a merge driver on every merge.
- **I20, Python:** new tools run on Python 3.8 as well as 3.12; the existing `tests/` suite is not held to 3.8 (clarified at the review).

### Design review, 2026-11-25

A three-lens review (architecture, testing, risk) of `docs/linux/IMPLEMENTATION.md` raised 52 findings. Each was re-checked against the applied Nano tree and the two machines' tools; the design was changed for all of them except three parts, skipped with a reason (a shared JSON reader, the `NANO_WITH_KANJI` switch, editing the Android-side `scripts/esde_config.py`). Every finding and what was done is listed in that file's section K; the decisions it produced are below, with full reasons in its section I.

- **I21, patch merges:** a `merge=nanopatch` driver regenerates the patch from `nano/` on every merge with `nano-144`; `regen` refuses to overwrite a block it did not write; `sync.json` is replaced by `git merge-base`.
- **I22, fork-only files:** Linux and host files (`platform/linux`, `platform/host`, `test/host`, `compat/stubs`, `build/`, CMake, `wpa_ctrl`) stay out of the patch; the shims and the firmware platform reach it.
- **I23, test placement:** fork-only tests live outside `tests/`, which gates Android release builds; the Android surface is computed against the `nano-144` merge base, with no manifest.
- **I24, binaries without `install()`:** files compiled by `drastic-nano`, `gammaos-nano-scrape` or the host tests never call `nanoplat::`; `NanoDeviceKeys.cpp` is not edited.
- **I25, scraper:** a `thimsy-nano-scrape` binary started by `thimsy-session` on `scrape_run`, as Android starts its scrape service.
- **I26, DRM:** the Linux build compiles `NanoMenuDrm.cpp` and substitutes only its buffer back end, after one mechanical split (G9a) flagged for cherry-pick to `nano-144`; the host substitutes the whole API under a symbol-set test.
- **I27, interfaces:** declared in the G7 step that moves their code; `Clock` dropped; `deviceMatchKeys()`; a pull-model `Audio`; G7-main first; `Cap` row hiding is its own step after the alpha.
- **I28, launching and control keys:** the launch handshake is gathered into one helper before the seam swaps it; control-key handlers observe and never consume; the trigger table is derived from `gammaos-nano.rc` and the framework listeners.
- **I29, no hand-written NanoMenu stubs:** media, overlay and SD-move files compile against the stubs; any stubs needed are generated at build time.
- **I30, image decoding:** a real `AImageDecoder` shim on libpng, libjpeg-turbo, libwebp and giflib, since the ES-DE theme (D9) depends on it.
- **I31, Bionic extras:** a force-included `nanocompat/bionic.h`.
- **I32, siblings:** Shares and RGB follow are firmware-only for v1; `compose` exports the sibling sources Nano reads.
- **I33, base moves:** `nano_tree.py import-base`; each side's base read from its commit; `upstream.json` the only base on the fork.
- **I34, regeneration:** pinned plumbing, binary sections copied verbatim, alternates instead of a local fetch; Windows git the reference if the two gits disagree.
- **I35, compiling now:** heavy C++ targets deferred to a queued G10 after the AOSP job and once downloads are allowed; until then "written, not compiled", reported as skipped.
- **I36, parallel work:** one git worktree per agent, owned-path staging, serial merges, a clean-worktree re-test.
- **I37, release targets:** no dev services, root password locked; only they may be published.
- **I38, assets:** `thimsy-data` installs from an allowlist of `ASSETS.md`'s KEEP rows; a D16 condition.
- **I39, the `games` partition:** created at first boot, reserved first, formatted, then retyped basic data.
- **I40, the boot disk only:** init and `thimsy-session` act on the boot disk, never an eMMC; Nano runs with the eMMC's nodes masked.
- **I41, source publication:** `git archive` tarballs as the recipes' sources; the whole repository published at a release tag; an honest licence for the Kanji table; `nano/NOTICE`.
- **I42, FlipBase's licence files:** kept at the subtree merge; refines D19.
- **I43, Phase 2 rotation fallback:** RetroArch's own settings for the alpha; SDL3 directly, else out of the set until Phase 5, for SDL standalones; refines Q27 and Q28.
- **I44, the alpha's library:** on the card's `games` drive; refines D10.
- **I45, scraper developer credentials:** not in public Linux images, until the owner decides.
- **I46, secrets in `persist.props`:** mode 0600 and redaction; the plan notes the removable-card exposure.
- **I47, CMake:** reads `sources.txt` directly.

## Implementation decisions (waves 1 and 2)

Recorded on 2026-11-25 from the step reports of waves 1 and 2, all merged into the `linux` branch (last merge `dfe7a77`). Each id is the step's own; G0 and G1a reported unnumbered lists, so their numbers were given here in the reports' order. Nothing ran on a device and nothing was built for one: Buildroot, genimage, mkimage, CMake, QEMU and the AOSP build were not run, and the C++ was compiled and tested on the host only. Where a step corrected an earlier design entry or another step's choice, the bullet says so.

### G0. Nano as files, `nano_tree.py`

- **G0-1:** `regen` deflates binary sections at `core.compression=1` (git's built-in default), not -1, so its raw output equals the committed patch; copying unchanged binary sections verbatim is only a fallback.
- **G0-2:** `regen` passes `-c core.abbrev=11` as well as `--abbrev=11`, because git before 2.31 ignores `--abbrev` on `index` lines (WSL wrote 9-character ids without it).
- **G0-3:** The Python package is `nanotree/`, not `nano_tree/`, so it cannot shadow the `nano_tree.py` script.
- **G0-4:** Six files carry the executable bit in the index, not four: the patch's four plus two untouched base files (`nano-ce-unlock.sh`, `tools/esde-parsecheck/run.sh`); 100644 there would add mode-change sections.
- **G0-5:** `regen` reads `nano/` as the index plus the working tree: changed and untracked files are hashed into the cache, modes always come from the index, and untracked files get 100644. (G9a found it treats `git add -N` entries as empty files; see the open items.)
- **G0-6:** On Windows, `import-tree` takes modes from the index, then the patch, then the base, then 100644; on POSIX it uses the file's mode and also sets the bit on disk.
- **G0-7:** The merge driver exits 1 whenever it changed `nano/`, because git holds the index lock during a merge and an automatic merge commit would leave those changes out; it prints the steps to finish.
- **G0-8:** If the incoming branch has its own `nano/` (a merge inside the fork), the driver keeps our patch and asks for `regen --force` after `nano/` is resolved.
- **G0-9:** `import-patch` with a branch keeps the fork's own sections outside the Nano block, and `merge-patch` takes the incoming side's; `import-patch` with no branch and `--old REV` recovers when git took nano-144's patch without running the driver.
- **G0-10:** The cache is never left configured as a partial clone: the partial-clone settings are passed with `-c` during a fetch only, and every other git call runs with `GIT_ALLOW_PROTOCOL=none` (git 2.25's `cat-file` otherwise dies when a refused lazy fetch fails).
- **G0-11:** `fetch --offline` (or `NANO_TREE_OFFLINE=1`) never downloads; `fetch_nano.sh` picks `$PYTHON`, then `python3`, then `py -3`.
- **G0-12:** `compose` writes a marker file, refuses to clear a directory it did not make, and puts siblings under `OUT/_siblings/`.
- **G0-13:** `siblings` lists `drastic-nano/DsScreenLayout.{cpp,h}`, `gammaos-sharefs/share_config.h` and `surfaceflinger/gammargb/GammaRgbProcess.h`.
- **G0-14:** `props.txt` (994 keys) is the string literals in shared C/C++ files that look like property keys under `persist`, `sys`, `ro`, `service`, `init`, `ctl`, `debug`, `vendor` or `dalvik`; the conditional ratchet (budget 0) counts `__ANDROID__` and `NANO_PLATFORM_*` in code only, not in comments or strings.
- **G0-15:** Ignore checks use `check-ignore --verbose`, because git before 2.27 also reported paths that a `!` rule re-includes.
- **G0-16:** `test_outside_block_unchanged` uses `git merge-base HEAD nano-144`, so it works on step branches and skips without a local nano-144; the round-trip test is marked slow, while `test_regen_matches` and `test_patch_applies_to_base` stay in the quick suite.
- **G0-17:** `import-base NEW` needs NEW's blobs in the cache (`fetch --base NEW`); it updates `upstream.json`, regenerates, and warns that the patch's other sections still target the old base.

### G1a. libthimsy and `thimsy-prop`

- **G1a-1:** An empty value means unset, as on Android: `set(k, "")` removes the key and `get` returns the default.
- **G1a-2:** Key names follow Android's rule (`[A-Za-z0-9_.:@-]`, no leading, trailing or double dot), capped at 255 bytes because a key is also a file name.
- **G1a-3:** `ro.*` is sealed by the file `<runDir>/ro/.sealed`, which `thimsy-prop boot` writes; before sealing, an `ro.*` key can still be set only once.
- **G1a-4:** Without the watcher, `get`, `serial` and `list` read the files directly, and `waitChange` checks the file every 20 ms.
- **G1a-5:** With the watcher on, reads come from a cache that inotify keeps current.
- **G1a-6:** Observers always see this process's own sets, and other processes' writes only while watching; an exception thrown by an observer is ignored.
- **G1a-7:** `addObserver` returns an id and there is a `removeObserver`; a pattern ending in `.` or `*` is a prefix, and an empty pattern matches every key.
- **G1a-8:** Run-dir key files are not fsynced (they live on tmpfs); `persist.props` is written durably, and stale temp files are removed under the flock.
- **G1a-9:** `isSecretKey` adds `psk`, `apikey` and `credential` to the design's list (`pw`, `pass`, `token`, `secret`, `ssdev`); it errs toward redacting.
- **G1a-10:** `thimsy-prop get` prints the real value (scripts need it); `list` and `watch` print `<redacted>`; output lines are `key=value` with the persist file's escaping.
- **G1a-11:** `thimsy-prop boot` reads `build.props`, then `device.json`, then `install.json`; the earliest wins on a conflict, with a warning.
- **G1a-12:** `install.json` can only add `ro.thimsy.install.*`, because it sits on a partition the user can write.
- **G1a-13:** From `device.json`, boot writes its `props` object (`ro.*` keys only); `ro.product.*` with its `vendor.` and `odm.` variants and `ro.board.platform` from `product.{manufacturer, model, brand, device, name, platform}`, falling back to the top-level `model`, `id` and `family`; and `ro.thimsy.device.<key>` for each top-level scalar and `ro.thimsy.device.panel.<key>` for panel scalars. The `product` and `props` fields were this step's proposal and are not in G2c's schema (see the open items).
- **G1a-14:** `build.props` lines are `ro.key=value`; whitespace around the value is trimmed, as Android does.
- **G1a-15:** Boot exits 0 on success, 2 when it sealed but printed warnings (a missing `install.json` is not a warning), and 1 on failure or a second run in the same boot.
- **G1a-16:** The JSON reader is strict: it rejects duplicate keys and nesting deeper than 256, and keeps members in file order.
- **G1a-17:** The gtest stand-in defines a weak `main` (switched off with `THIMSY_GTEST_NO_MAIN`).
- **G1a-18:** The stand-in uses `#pragma GCC system_header`, so mixing signed and unsigned in `EXPECT_EQ` compiles under `-Werror`, as with real GoogleTest.
- **G1a-19:** The stand-in prints no timings on `[       OK ]` lines and writes failure details to stderr, so the existing `test_nano_units.py` checks keep passing.
- **G1a-20:** The Makefile builds in `O ?= ~/.cache/thimsy/libthimsy`, refuses to build when MemAvailable is under 6,000,000 kB, and checks that `prlimit`, `setarch` and `timeout` are installed.
- **G1a-21:** Compilers run under `prlimit --as=3221225472`, `nice -n 19` and OOM score 1000.
- **G1a-22:** Sanitized test programs run without the address-space limit (ASan reserves terabytes it doesn't use), under `timeout 300` instead.
- **G1a-23:** Test programs run under `setarch -R`: GCC 9's ASan looped printing `AddressSanitizer:DEADLYSIGNAL` on WSL2's 6.6 kernel in 8 of 40 runs without it, and in 0 of 40 with it.

### G1b. Nano compat shims

- **G1b-1:** The init call is `nanocompat::init(InitOptions)` (persistFile, runDir, watch=true), not a C `nanocompat_init`. Calling it again replaces the store, replaced stores are kept until exit so a thread holding one stays safe, and a failed init keeps the previous store.
- **G1b-2:** With no init, the first property call opens a lazy default store that does not watch: `$THIMSY_PROP_ROOT/persist.props` and `$THIMSY_PROP_ROOT/run`, else `ConfigStore::defaultRoots()`. If it can't be opened, every get returns its default and every set returns -1, logged once.
- **G1b-3:** Property observers (the control-key hook) live in compat, not on the store; they are told after the value is stored and can't veto. Unlike ConfigStore observers (G1a-6) they see every successful set this process makes, even of an unchanged value, as Android's triggers do; other processes' writes reach them only while the store watches; they survive init(), and their exceptions are swallowed.
- **G1b-4:** `__system_property_find` returns a handle once the key has a value or this process has set it, as on Android, and handles live for the whole process. A key's serial is the store's serial plus this process's own sets plus 1, never 0 (PropEdge relies on a first wait with serial 0 returning at once); the global serial counts the sets and changes this process sees; a per-key wait re-reads the key every 20 ms when the store does not watch and every 1 s when it does.
- **G1b-5:** cutils and libbase keep their real differences: `property_get_int32` uses strtoimax with base 0 ("12abc" is 12, "010" is 8); `GetIntProperty` uses ParseInt ("12abc" gives the default, "010" is 10).
- **G1b-6:** LOG_NDEBUG follows Android: 1 with NDEBUG, 0 without. Disabled ALOGV and SLOGV still type-check their arguments but never evaluate them.
- **G1b-7:** Log lines are `<monotonic ms> <V|D|I|W|E|F> <tag>: msg`, one line per message line and one write() per message; the ring file rotates to `<path>.1` at half its maximum size; the default minimum priority is VERBOSE; fatal macros log at F, then abort().
- **G1b-8:** `android::Thread` follows libutils exactly: it holds a strong reference only while readyToRun and threadLoop run and a weak one between calls, so an unreferenced Thread ends after its current loop. This corrects B.2's "for the thread's life". The thread name keeps the last 15 bytes, and a priority other than PRIORITY_DEFAULT is applied with setpriority on the thread's tid (a failure is logged once).
- **G1b-9:** RefBase follows libutils, including that promoting a never-strong object skips onFirstRef. One safety difference: destroying a never-strong object that still has weak references keeps the weak block, so promote returns null instead of reading freed memory; destroying an object with strong references left aborts.
- **G1b-10:** AImageDecoder gives RGBA_8888 output only; still images scale to any size, filtered in premultiplied space; animated images refuse scaling with INVALID_SCALE, as Nano observed on Android; JPEG EXIF orientation is applied, with header sizes swapped; GIF backgrounds are transparent and frames composited. It needs libpng 1.6 and giflib 5.1 or later, animated WebP needs libwebpdemux, HEIF, AVIF, BMP and any format whose library is missing are UNSUPPORTED, images are capped at 64 Mpx, createFromBuffer does not copy the buffer, and createFromFd reads from the current offset and does not keep the fd.
- **G1b-11:** `bionic.h` uses Bionic's mallopt values (M_PURGE_ALL -104, M_CACHE_COUNT_MAX -105 and the rest); it adds strlcpy and strlcat only for glibc older than 2.38, gettid only for glibc older than 2.30, and getprogname always outside Bionic. In C it only defines constants and includes nothing.
- **G1b-12:** Headers beyond B.2's list: `android-base/parseint.h`, `parsebool.h` and `strings.h`, `android/rect.h`, `utils/ThreadDefs.h`; `android_filesystem_config.h` has Android's full table of fixed IDs.
- **G1b-13:** `compat.mk` run standalone runs libthimsy's check first (off with COMPAT_WITH_LIBTHIMSY=0), builds in `COMPAT_O` (default `~/.cache/thimsy/nano-host/compat`), exports NANOCOMPAT_CPPFLAGS, NANOCOMPAT_ASAN_LIBS, NANOCOMPAT_REL_LIBS, NANOCOMPAT_IMAGE_LDLIBS and the target `nanocompat-libs`, and compiles with `-Wall -Wextra -Werror -Wno-clobbered`. Its tests always use libthimsy's gtest stand-in, because the totals are read from its summary line.

### G2a. BR2_EXTERNAL skeleton, defconfigs, build guard

- **G2a-1:** Buildroot is pinned to 2026.08.2, FlipBase's version and hash, so the subtree merge needs no version move; option names come from FlipBase's working defconfig where they overlap, otherwise from memory of 2025.02, marked "(guess)".
- **G2a-2:** GCC 14 rather than FlipBase's 15, because GCC 15's C23 default breaks older C cores; kernel headers 6.12 (an LTS no newer than any family's kernel).
- **G2a-3:** A new `userland.fragment` holds the package set both tiers share; `tier-v8_0` and `tier-v8_2` hold only CPU tuning and BR2_ROOTFS_POST_SCRIPT_ARGS (one list per fact).
- **G2a-4:** `common.fragment` locks the root password (`# BR2_TARGET_ENABLE_ROOT_LOGIN is not set`) and only `dev.fragment` overrides it, so every target is locked unless it says otherwise.
- **G2a-5:** A Kconfig choice BR2_THIMSY_FLAVOUR_{RELEASE,DEV} in `linux/Config.in` marks the flavour for post-build and the policy check; it defaults to release.
- **G2a-6:** SSH policy is two thimsy-session options, SSH_AT_BOOT and SSH_PASSWORD, set only by `dev.fragment`; release keeps dropbear installed but off at boot and key-only. In effect superseded by G2e-11 and G2e-12: D50ssh decides by `ro.thimsy.build.type`, so the two options change nothing in the recipes (G2d's report).
- **G2a-7:** With BR2_INIT_NONE there is no Buildroot getty, so the dev serial console comes from a second overlay, `board/common/rootfs-overlay-dev`, that only `dev.fragment` adds; the policy check refuses a publish target that uses it or whose overlay inittab starts a getty. G2e-13 adds the getty from `30-permissions.sh` instead, and the dev overlay was never created (see the open items).
- **G2a-8:** Hardware packs use the tier's SDK as a preinstalled external toolchain at /build/sdk; `hw-common` overrides common's toolchain lines to "not set", and the lint checks GCC and header versions against the `sdk_from` tier.
- **G2a-9:** `thimsy_rk3566_hw` is publish:true with sdk_from `thimsy_v8_0_release`, because a public card needs its hardware pack; it includes no dev fragment and passes the release policy. The qemu pack is publish:false.
- **G2a-10:** In a fragment, "# override: <why>" applies to the next setting line; a dangling marker, or one that changes nothing in any target, is an error, and a repeated identical value is allowed.
- **G2a-11:** `targets.json` has a `pending_paths` map (path to owning step): a missing pending path is a note, a missing unlisted one an error, and a pending entry nothing references an error. A missing pending fragment is skipped and named in the defconfig header, and build.sh refuses that target until it exists.
- **G2a-12:** build.sh runs its read-only target checks (target exists, defconfig current, files present, `--publish` only for publish targets, clean tree and tag for `--publish`, not on /mnt/<drive>) before the guard; the guard runs before any work and re-runs build.sh under the exclusive lock on fd 9.
- **G2a-13:** Guard light mode treats the lock, the container, the aosp distro and load as "Android signs": small targets pass, others are refused. Memory floors are 6,000,000 kB for small targets and 9,000,000 kB otherwise; a failing or missing `docker ps` is only noted, and a missing or failing wsl.exe counts as a sign.
- **G2a-14:** Source tarballs go to `dl/thimsy-src/<name>-<full sha>.tar.gz` and Buildroot hash files to `dl/thimsy-src/hash/<pkg>/<pkg>.hash`; the container appends that directory to BR2_GLOBAL_PATCH_DIR, and recipes get THIMSY_GIT_SHA and THIMSY_SRC_DIR as make variables.
- **G2a-15:** The nano-assets tarball is built from `nano_tree.py export-assets`, packed reproducibly, not from `git archive`, because the assets are partly fetched rather than tracked (I2). This settles E.1 against E.3.
- **G2a-16:** `external.mk` includes `package/libretro/libretro.mk` explicitly first, because $(sort) puts "libretro-*/" before "libretro/" and the cores would expand an undefined macro.
- **G2a-17:** BL31 for RK3566 is rkbin's (with rkbin's DDR blob as TPL) until Phase 0 and D18 settle it; a temporary departure from the owner's D18 ("use the FlipBase TF-A").

### G2b. Card layout checks and the boot FIT

- **G2b-1:** The vold rule covers both types vold mounts (Microsoft basic data and Linux filesystem data, plan 8.2). A written image has neither; a card that has booted (`--mode card`) has at most one, at entry 7 or lower, carrying an exFAT, FAT, NTFS or ext4 signature; the Android meta and expand types are always refused.
- **G2b-2:** Both boards tile the regions from LBA 16384, each followed by an equal spare, with data right after root's spare (FlipBase's gptslot rule); gpt_check enforces this for the Thimsy layout.
- **G2b-3:** The QEMU disk keeps the cards' exact table (empty uboot and boot entries), so S00base and D60firstboot see the same entry numbers as on a device.
- **G2b-4:** Every offset and size in `genimage.cfg` is explicit and the idbloader has an explicit size (8160K), so genimage itself refuses an overrun; test_layout refuses implicit offsets. RK3566: idbloader at 32K, uboot at 8M (8M), boot at 24M (40M), root at 104M (1G), data at 2152M (256M), no games entry.
- **G2b-5:** The FIT's static bootargs (`init=/init rootfstype=erofs ro rootwait quiet loglevel=3`, no `root=`) go in the configuration node's `bootargs` property; the family's boot script reads them with `fdt get value` and appends `root=PARTUUID=` of entry 3, matching E.5. The DTB is not edited, unlike FlipBase, which uses the DTB's /chosen/bootargs.
- **G2b-6:** The boot.img header, as read from FlipBase's `board/my355/uboot.env`: an 8-byte magic, a u32 LE FIT sector count at +8, the rest zero, the FIT from the next sector. Thimsy's magic is "THIMSFIT" (FlipBase's "MY355FIT"), isolated in make_header/parse_header and still to be verified against FlipBase's `tools/mkfit.py` at the subtree merge.
- **G2b-7:** The rk3566 DRAM map follows FlipBase's (without OP-TEE): kernel at 0x02000000, FIT read to 0x0a000000, header at 0x0c000000. fit.py refuses a FIT over 32 MiB, a kernel that decompresses into the FIT, and a boot.img larger than the 40 MiB slot.
- **G2b-8:** qemu-aarch64-virt has no FIT (it boots with `-kernel`, E.2), and fit.py refuses that family with a clear message.
- **G2b-9:** Family facts (load addresses, slot size, base bootargs, magic) live in fit.py's FAMILIES table; device facts (device, family, dtb, optional bootargs) come from a device JSON, not yet aligned with the device.json schema (G8-9 works around it).
- **G2b-10:** GPT attribute bits 62 and 63 are not set: genimage's support for them was not verified, and Windows assigns no drive letter to these non-basic-data types anyway.

### G2c. `thimsy-import`

- **G2c-1:** The importer was built, and its fixtures cut, from ROCKNIX `next` at 0f4806b (a partial clone); `IMITATES.txt` names the path and commit of each fixture file.
- **G2c-2:** Provenance is a first line `Thimsy-Import-Provenance: ROCKNIX <path> at <commit> (tag <tag>); GPL-2.0, used under ROCKNIX's licence; author not recovered (Q22)`, which patch and git apply skip. The config gets it as a `#` line, a DTS as a `//` line after the SPDX line, and `gpl/generated/<f>/PROVENANCE` lists every file.
- **G2c-3:** The series is flattened into numbered directories, one per ROCKNIX source directory, named `NN-<scripts/unpack PATCH_DESC label>` (for example `04-device`); file names stay ROCKNIX's, BR2_LINUX_KERNEL_PATCH lists the directories, and `linux/series` records the exact order.
- **G2c-4:** Patches in a directory are sorted in C byte order; ROCKNIX's bash glob order is assumed to match (guess).
- **G2c-5:** As in scripts/unpack, `@DEVICE@` and `@TARGET_CPU@` are substituted in non-binary, non-rename patches; a directory applied twice stops the import.
- **G2c-6:** The PKG_PATCH_DIRS evaluator knows only the shapes ROCKNIX uses (`=`/`+=`, single `[[ ]]` tests with bash's quoted `=~` read as a literal, `==` as a glob); anything else stops the import with exit 2.
- **G2c-7:** Hash-guarded blocks, ignoring comment-only and blank lines: the family's case-branch body, the top-level PKG_PATCH_DIRS lines, every if/elif branch naming `"<FAMILY>"`, the whole post_patch function, the CFG80211 branches when the base config has CFG80211=y, the scripts/unpack patch loop (recorded as "absent" if missing), and the quirks/package.mk family branches.
- **G2c-8:** A first run with no `guards.json` records the guards; any later change exits 3 until `--accept-guards`.
- **G2c-9:** Family blocks the importer does not translate (the generic-dsi copy, unpack-time dependencies, the device_trees install) are listed in `rocknix.json` "handport" for the hardware recipe.
- **G2c-10:** Firmware licences come only from linux-firmware's WHENCE (`--whence`); wireless-regdb counts as ISC; extra-firmware and unknown packages are excluded, marked fetch_candidate and reported. Without WHENCE no linux-firmware file ships.
- **G2c-11:** The builtin firmware goes to `generated/<f>/linux-firmware.fragment` as CONFIG_EXTRA_FIRMWARE (shipped files only, sorted, regdb appended) with CONFIG_EXTRA_FIRMWARE_DIR="external-firmware".
- **G2c-12:** Conformance is a text merge of the base config and the fragment against `kernel-critical.conf`; it fails (exit 4, nothing written) only when a fragment is present (default `<out>/board/<fam>/linux.fragment`), and otherwise only reports.
- **G2c-13:** Quirk facts are read only from `cat <<EOF >.../profile.d/...` heredocs; values needing the shell (`$`, backtick, arrays) are rejected and reported; accepted facts go to quirk_facts, and the known keys are mapped to fields.
- **G2c-14:** DEVICE_PWM_MOTOR becomes a top-level `vibrator` object; `pad.rumble` only names the evdev source.
- **G2c-15:** A model comes from, in order, an out-of-tree DTS, a DTS a patch adds, `--kernel-src`, then a normalised match against quirk, InputPlumber and DTS model strings (marked "name-match", a guess); DTBs with no model are reported and get no device.json.
- **G2c-16:** A product id is the DTB name with the leading SoC tokens removed (`anbernic-rg-arc-d`).
- **G2c-17:** InputPlumber composites match by device-tree model; capability-map targets map to kernel codes (Guide to BTN_MODE, keyboard targets to KEY_*); where a code has two targets the first is kept and a note added.
- **G2c-18:** face_buttons 6 is inferred only when 4 face buttons and both stick clicks are mapped and the map has no axis or non-evdev mappings.
- **G2c-19:** rocknix-joypad DTS families get producer kernel-joypad, empty buttons meaning pass-through, sticks from amux-count, and no padd inversion (the driver inverts itself); if the DTS names no joypad-name, the family's only one is used and marked a guess.
- **G2c-20:** Generated device.json files have status "imported", added to the plan's built, boots, works and certified.
- **G2c-21:** Board device.json files are complete documents, not overrides; `devicejson.merge` (a deep merge) is provided for later use.
- **G2c-22:** On the ARC the home key (adc-keys-home BTN_BACK) maps to BTN_MODE and is the hotkey (guess), where ROCKNIX maps it to KEY_F1; the face layout is "saturn", and the labels come from the dtsi.
- **G2c-23:** For QEMU virt, the device-tree model is "linux,dummy-virt", dtb is null, and the virtio keyboard's Enter maps to BTN_SOUTH (E.9).
- **G2c-24:** A ROCKNIX tree checked out on Windows is undone on read (files that are all CRLF become LF; symlinks stored as text are followed), so Windows and Linux checkouts give identical output.
- **G2c-25:** The commit is read from `.git` without running git, or given with `--commit`; the two must agree.
- **G2c-26:** Emit owns `generated/<f>/` and `gpl/generated/<f>/` entirely: it writes atomically, removes stale files, and touches nothing else.
- **G2c-27:** For an unpatched x.y kernel (H700 7.2) the defconfig fragment carries a Q34 comment but keeps ROCKNIX's value; a non-kernel.org URL (RK3588) becomes CUSTOM_TARBALL; `linux.hash` is written in Buildroot's format.

### G2d. Package recipes, package lint, asset allowlist

- **G2d-1:** libthimsy, thimsy-nano and thimsy-data use a file site at `$(or $(THIMSY_SRC_DIR),$(DL_DIR)/thimsy-src)` with `_VERSION = $(THIMSY_GIT_SHA)`, and a pre-download hook refuses an empty sha; their only hash files are build.sh's hash directory in BR2_GLOBAL_PATCH_DIR. This answers G2a's open question from memory of Buildroot's manual, not checked in a Buildroot tree.
- **G2d-2:** thimsy-session and thimsy-padd are local sites at `linux/src/<pkg>` (version 1.0), built with CMake and `-DTHIMSY_BUILD_TESTS=OFF`.
- **G2d-3:** `package/libretro/libretro.mk` defines two infrastructures: `libretro-core-package` builds with the core's own Makefile (flags through the environment; CC, CXX, AR and `platform=` on the command line), and `libretro-cmake-core-package` is used by flycast and melondsds. Both install `/usr/lib/libretro/<core>_libretro.so` and add the core to LIBRETRO_CORE_NAMES.
- **G2d-4:** Cores with git submodules (picodrive, pcsx_rearmed, fake08, melondsds, flycast) use git sites with `_GIT_SUBMODULES = YES`; the others use the github tarball macro.
- **G2d-5:** Versions come from the Android build: RetroArch 1.22.2 and PPSSPP v1.20.4 from `mods/10-update-emulators.sh`, ScummVM 2026.3.0 from `mods/added-apps.txt`, and test_packages checks they still match. PPSSPP comes from git with submodules, ScummVM from its own tarball on downloads.scummvm.org.
- **G2d-6:** RetroArch also downloads libretro-core-info at its own tag and installs only the enabled cores' `.info` files to `/usr/share/libretro/info`; it is configured for KMS, EGL, GLES 3/3.1, ALSA and udev, with no X11, Wayland, Qt, Pulse, SDL, Vulkan, FFmpeg, Discord or online updaters (the flag names are a guess).
- **G2d-7:** A downloaded package with no hash yet gets a comment-only `.hash` naming its files; lint reports it as pending, and with BR2_DOWNLOAD_FORCE_CHECK_HASHES the build refuses it, so it fails closed. Cores get no `.hash` until they have a commit.
- **G2d-8:** `mods/retroarch-cores.src.lock` lines are `core repository commit source`: commit is a full commit, `short:<h>` or `pending:<reason>` (not-in-cache, bundled, not-in-android-build, not-in-lock, no-hash, ambiguous, unreadable), and source is `<zip>@<sha256>`.
- **G2d-9:** `core_sources.py` reads the short hash from the core binary's strings (a version-prefixed hash wins, else a single bare 7 to 12 digit hash, else the core is pending); `--resolve` looks the full commit up through GitHub's API, and a resolved commit is kept while the zip and prefix are unchanged. mupen64plus_next maps to Android's mupen64plus_next_gles3; melondsds is not in the Android build; fake08 is "bundled".
- **G2d-10:** `lint_packages.py` separates errors from pending items (`--strict` fails on pending too; `--sync-cores` writes each core's `_VERSION` from the source lock). It checks the header block, the variable prefix, exactly one known `$(eval ...)` line at the end, the VERSION, SITE, LICENSE and LICENSE_FILES fields, the hash-file format, that each Config.in is sourced and defines its symbol, each core's commit and repository against the lock, that `assets.list` is fresh and denies every REPLACE and ASK path, and that repository tarballs' licence files exist.
- **G2d-11:** `assets.list` is generated and committed: keep or deny patterns on installed paths (the `/system/etc` layout), deny always winning. Android.bp is used only to map a Nano path to where it is installed, with Nano's convention (`ps3xmb/X` to `nano_xmb/X`) for a folder Android.bp does not install.
- **G2d-12:** A sentence "X counts as REPLACE" in ASSETS.md's "Changes since the audit" turns the matching keep paths into deny lines, which today denies Akrobat. A REPLACE or ASK row about files inside an archive denies the whole archive (so `slate-es-de.zip`); a KEEP row about archive members adds nothing.
- **G2d-13:** Data files ASSETS.md does not cover are kept through a DATA table in the tool with the licences ATTRIBUTION.md gives (i18n, buttonmap, controllers with its LICENSE, daijishou); `install` refuses symlinks and reports every file it left out; thimsy-data installs to `/usr/share/thimsy/system/etc` using host-python3.
- **G2d-14:** thimsy-gpu-pack is a target-finalize hook running `src/gpu_pack.py`: it moves Mesa's files out of the tier root into `BINARIES_DIR/gpu-<driver>.erofs`, each pack carrying a `gpu.json` with the driver, the environment to set (swrast: `GALLIUM_DRIVER=softpipe`) and the file list; a rebuild that finds the files already gone reuses the earlier packs.
- **G2d-15:** thimsy-fonts uses Roboto 2.138 and the Noto releases CJK 2.004, Emoji 2.047, Naskh Arabic 2.018, Sans Thai 2.002 and Sans Hebrew 3.001 (versions and URLs a guess), installed to `/usr/share/fonts/thimsy`, with `DroidSans.ttf` a link to `Roboto-Regular.ttf`.
- **G2d-16:** The family overlay goes into the RK3566 hardware pack (`hw-rk3566.fragment`), not the tier roots, so the tiers stay family-agnostic; both hardware targets get `--family=<family>`. This replaces the "tier defconfigs" note in G2e's `board/rk3566/common/README.md`, which is not yet updated.
- **G2d-17:** sfdisk comes from `BR2_PACKAGE_UTIL_LINUX_BINARIES` (a guess at the symbol), e2fsck from e2fsprogs' default install, and the BusyBox applets (ip, udhcpc, ntpd, float sleep and related) from `board/common/busybox.fragment` through `BR2_PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES`.
- **G2d-18:** sdl2-compat on SDL3 (I43, Q28) is not done, because it can't be checked offline; ppsspp and scummvm use SDL only through the sdl2 pkg-config module, so the later swap is a Config.in and fragment change, not a recipe change.
- **G2d-19:** `gpl/package/Config.in` is an empty menu, sourced from `linux/Config.in` (G2a's open item).

### G2e. Init: rcS, the S and D scripts, first boot

- **G2e-1:** S00base finds the boot disk from the mounted root first (`/proc/self/mountinfo` major:minor), then `root=PARTUUID=`, then `root=/dev/NAME`; two cards written from one image share PARTUUIDs, so a PARTUUID that matches two partitions is never guessed. This refines E.5's order.
- **G2e-2:** Test seams: every path is `"$R/..."` with R=$THIMSY_ROOT, and every program goes through a library variable that `$THIMSY_TOOLDIR/<name>` overrides; rcK sends no signals while THIMSY_ROOT is set.
- **G2e-3:** S00base saves its result (BOOT_DISK, ROOT_PART, DATA_PART, BOOT_HOW, BOOT_EMMC) in `/run/thimsy/bootdisk` for D60firstboot and thimsy-session.
- **G2e-4:** An eMMC boot disk, a missing data partition, or a mount that still fails after `e2fsck -p` gives a tmpfs `/storage` for that boot, with a kernel-log warning. e2fsck is the only exception on the rcS path, marked "# rcs-allow:".
- **G2e-5:** The first-boot tree is made once, guarded by `/storage/.config/thimsy/.tree-v1` written last, and every step can be repeated; `/data/user/0` is the relative link `../data`, as on Android. State files stay in `data/system/` (B.5), not the plan's `state/`.
- **G2e-6:** D60firstboot puts entry 5 at the first 1 MiB boundary after the end of the last entry, so spare areas are never used, first moving the backup GPT to the card's end when it is more than 1 MiB short; the minimum size is 64 MiB, and it creates the entry with `sfdisk --force -N 5`, because sfdisk refuses a disk in use without `--force`.
- **G2e-7:** D60firstboot reads its state from `sfdisk --dump` every run: an entry 5 typed Linux reserved and named "games" is always (re)formatted, never probed for a filesystem; basic data means done; any other entry 5 is left alone.
- **G2e-8:** D10cards mounts nothing: it writes "add" to each non-eMMC block device's uevent, so thimsy-session's single volume policy handles drives present at boot.
- **G2e-9:** Family hooks are one file per family, `/usr/share/thimsy/family/<family>/hook`, sourced by the init scripts with THIMSY_HOOK=ACTION and run by thimsy-session as "hook ACTION". The family comes from ro.thimsy.family, else the only family directory present, and is written to `/run/thimsy/family`; a new D05family runs the hook's deferred action (governors back from performance after the first frame).
- **G2e-10:** rcD is the deferred runner, "start" in order and "stop" in reverse; thimsy-session runs `rcD start` after the first frame, and rcK runs `rcD stop`.
- **G2e-11:** Persistent switches are read straight from `persist.props`: persist.thimsy.wifi.enabled, persist.thimsy.time.ntp, persist.thimsy.ssh.enabled. SSH defaults on for dev builds; release builds start it only when turned on, and then with `-s` (key-only).
- **G2e-12:** `build.props` lives at `/usr/share/thimsy/build.props`, as B.3 says (not under /etc); ro.thimsy.build.type is "release" when there is no root login and no serial getty; the date comes from SOURCE_DATE_EPOCH.
- **G2e-13:** `30-permissions.sh` adds the dev serial getty to inittab (marked "# THIMSY_SERIAL", because the overlay's inittab replaces the one Buildroot edits), links `/etc/localtime` and `/root` into `/storage/.config/thimsy`, creates `/storage`, and fails the build if `/dev/block/by-name` exists. The getty here takes over G2a-7's dev overlay; the `/root` link may break Buildroot's makedevs (see the open items).
- **G2e-14:** `install.json` is read from `/storage/thimsy/install.json`, the data partition's `thimsy/` folder (the plan's card-level `thimsy/` folder). This settles G1a's open question, but `thimsy-prop` still defaults to `/boot/thimsy/install.json` (see the open items).
- **G2e-15:** The forbidden list for the synchronous rcS path is network and time daemons, SSH, D-Bus, Bluetooth, alsactl, sleep, mdev/udevadm, fsck/mkfs/partitioning tools, find, ldconfig, depmod, sync and wait; it applies to rcS, S*, `lib.sh` and the family hooks, and check_rcs also refuses `/dev/block/by-name` in overlay scripts.

### G3a. `thimsy-padd`

- **G3a-1:** Input code names come from a table generated from `linux/input-event-codes.h` (`tools/gen-input-codes.py`, committed as `InputCodes.inc`): aliases (BTN_A) are read, the name printed for a number is the last plain number defined for it (BTN_SOUTH, not BTN_GAMEPAD), and `*_MAX` and `*_CNT` are left out.
- **G3a-2:** The pad always declares ABS_X and ABS_Y, centred, even on the stickless ARC, because libudev-zero and SDL's evdev fallback only call a device with both a joystick (guess; checked in Phase 0).
- **G3a-3:** Output ranges: sticks -32768..32767, triggers (ABS_Z, ABS_RZ) 0..255, hat -1..1; the pad declares fuzz and flat 0, because padd applies the deadzone itself.
- **G3a-4:** Calibration takes device.json's values first, then the node's EVIOCGABS: the deadzone is `calibration.deadzone`, else `calibration.flat`, else the node's flat; the centre defaults to the middle of the range for a stick and to min for a trigger; inversion mirrors around the centre; `calibration.fuzz` is not used.
- **G3a-5:** Output is emitted at each source's SYN_REPORT as the difference between the pad's wanted and shown state (keys by code, then axes by code), one SYN_REPORT per device that changed; codes from several sources may share one target, pressed while any of them is held.
- **G3a-6:** Turning intercept on releases every held button and centres every axis in one batch; events then go to a tap instead of the pad, a SOCK_SEQPACKET socket at `/run/thimsy/padd-tap.sock` with 8-byte {type, code, value} records, one packet per batch. On every switch, buttons held at that moment are suppressed until released, in both directions; axes resume where they are.
- **G3a-7:** KEY_* targets go to a second uinput device, "Thimsy Keys" (BUS_VIRTUAL, 0x5448, product 0x0002), created only when device.json maps a button to a KEY_*.
- **G3a-8:** The pad advertises FF_RUMBLE and FF_GAIN with 16 effects; active effects are summed, clamped, scaled by the gain and sent as one FF_RUMBLE effect, re-uploaded each time, to the evdev node `pad.rumble.source` names. Each command's `replay.length` is the time until the soonest active effect ends, so the motor stops by itself if padd dies; padd does not use the top-level `vibrator` object.
- **G3a-9:** Nodes are found through `/sys/class/input/eventN`, and only those whose `device/name` equals a source's name are opened, so padd never opens other devices, its own pad included. If `/dev/input/eventN` was unlinked by an earlier padd, padd makes a private node in `/run/thimsy/padd`, opens it and unlinks it again.
- **G3a-10:** The pad appears once every required source is open, or after `--wait-ms` (default 2000) without the missing ones, with a log line; a node that appears later joins the existing pad, whose capabilities are fixed when it is created.
- **G3a-11:** kernel-joypad pass-through passes the node's EV_KEY codes and its ABS_HAT0X and ABS_HAT0Y, with BTN_DPAD_* becoming the hat; if the node is not open when the pad is created, the pad declares the 13 standard buttons and the hat.
- **G3a-12:** After SYN_DROPPED, events are dropped until the next SYN_REPORT and the node's whole state is read again (EVIOCGKEY, EVIOCGABS); the same read is done whenever a node is opened.
- **G3a-13:** padd sets `sys.thimsy.padd.ready=1` while the pad exists and watches `sys.thimsy.padd.intercept` ("1" means on); it does not act on the hotkey or the quit combo, which are thimsy-session's.
- **G3a-14:** device.json is read from `$THIMSY_DEVICE_JSON`, else `/usr/share/thimsy/device.json` (as `thimsy-prop`); `thimsy-padd --check` validates a pad section, and `make check` runs it on every `board/*/devices/*.json`.
- **G3a-15:** The trace format goes beyond F.2: header lines `device`, `node` (name, phys, uniq, bus, vendor, product) and `caps`; timed `sync`, `gone` and `ff upload|play|stop|erase|gain` lines; a label is a declared node matched through `pickSource` or a source id, and anything else is ignored and printed as such.

### G3b. `thimsy-session`

- **G3b-1:** The wiring lives in `src/Session.cpp` and `main.cpp` only parses options; the loop is single-threaded and reads the store without libthimsy's watcher thread, watching the props directory itself and calling get() on events.
- **G3b-2:** Requests are `[<seq>:]<verb>[:<arg>]` in sys.thimsy.session.request, answered in sys.thimsy.session.done as `<seq>:ok|error:<why>`; every write is acted on except a repeat of the last seq. Verbs: sleep, poweroff, reboot, quit-game, eject:<id>, scrape, restart-nano, brightness:<0-100>.
- **G3b-3:** The session also reads sys.powerctl (shutdown or reboot) and sys.gammaos.nano.scrape_run (any value starts the scraper, I25); G9's control table should map dosleep to sleep and park_restart to restart-nano.
- **G3b-4:** `volumes.json` is version 1, with per-volume id, state, path, media_rw, dev, disk, partition, fs, label, kind, boot_disk, read_only, size_bytes and error; states are checking, mounted, ejected, check_failed, mount_failed, unsupported, no_id and duplicate. It is written from the start with an empty list, and sys.thimsy.volumes.serial goes up after each write.
- **G3b-5:** Drive policy: on GPT disks only basic-data and Linux-data types are drives (vold's rule), and Linux reserved and Linux home are never mounted on any disk; on the boot disk only the partition named "games" and typed basic data; with an unreadable table, partitions named uboot, boot, root or data are not drives. Types are read from the disk's own GPT with both CRCs checked, because sysfs has no type.
- **G3b-6:** The ext4 volume id is the lowercase dashed UUID; FAT and exFAT use XXXX-XXXX; all-zero serials are refused.
- **G3b-7:** Two drives with the same id: neither is used. A twin already mounted is unmounted normally (not lazily), and if that fails it stays mounted and is flagged duplicate; when one twin goes, the other is checked and mounted.
- **G3b-8:** exFAT always gets `fsck.exfat -p` (the plan's choice until Phase 0 times it); vfat gets `fsck.fat -a` only when the boot-sector bit or the FAT[1] bit says dirty; ext4 always gets `e2fsck -p`. The drive is mounted after exit 0 or 1 (0 to 3 for e2fsck).
- **G3b-9:** The `/mnt/media_rw` aliases are made in `/run/thimsy/media_rw/<id>`, because the root is read-only, so `/mnt/media_rw` must be a link to that directory (G8-13 makes it).
- **G3b-10:** Mount points get the ext4 immutable flag through FS_IOC_SETFLAGS on a device; a failure is only logged.
- **G3b-11:** A long power press with Nano running bumps a counter in sys.thimsy.key.power_long; with Nano not running it powers off. There is no software hard power-off; that is left to the PMIC.
- **G3b-12:** Power events stamped within 1 s of a resume are dropped, and so is a release whose press was not seen.
- **G3b-13:** Volume keys move the level by 5 (default 60), run `amixer -q -c <device.json audio.card_index> sset <ro.thimsy.audio.volume_control|Master> N%`, set sys.thimsy.hud.volume, and save persist.thimsy.audio.volume 1.5 s after the last key.
- **G3b-14:** Brightness is only set by request (percent of max_brightness) and is not persisted by the session; sleep sets it to 0 and restores it on wake.
- **G3b-15:** Sleep: power.state=sleeping, STOP the game's group, `D30net stop` when wpa runs, backlight 0, sync, `hook sleep-pre`, mem_sleep (device.json power.mem_sleep, default deep, else s2idle), then mem (else freeze) into state; wake is the reverse; the hook is run as `sh hook ACTION`.
- **G3b-16:** The game's group is getpgid(sys.thimsy.game.pid), and is never Nano's group, the session's, or 1.
- **G3b-17:** Nano is restarted at once if it ran for 30 s or more, else after 1 s doubling to 30 s; if Nano exits while its game still runs, it is restarted when the game ends (watched with a pidfd). rcD runs at the first frame, or 30 s after Nano's first start if no frame comes, once per boot (marker `/run/thimsy/session/deferred-started`).
- **G3b-18:** padd is watched with a pidfd whether S02padd or the session started it, and restarted with the same back-off.
- **G3b-19:** A respawned session kills a Nano left from the previous session (`nano.pid`, matched by cmdline), scans block devices itself since the marker shows rcD already ran, and adopts `/storage/<id>` mounts found in `/proc/mounts`.
- **G3b-20:** NanoJail is planned at each Nano start from the `/dev` entries of eMMC disks (the disk, pN, bootN and rpmb): the child runs unshare(NEWNS), makes `/` an rslave, binds `/dev/null` read-only over each node and drops CAP_MKNOD from the bounding set, and fails closed (Nano not started) if any of that fails. The parent makes `/` rshared at start so later mounts reach Nano.
- **G3b-21:** The input nodes read are those whose sysfs capabilities include KEY_POWER, a volume key or SW_LID, minus gamepads (BTN_GAMEPAD), plus "Thimsy Gamepad" read without a grab; device.json's power.key and volume match names are not used.
- **G3b-22:** Fake hardware: uevents as text records from the FIFO `<root>/run/fake-hw/uevent` plus a replay file `uevent.replay` read once; keys as `NAME value [ms]` lines on FIFOs `<root>/dev/input/eventN`; mounts through mount and umount programs from the tool directory; the jail plan written to `<root>/run/thimsy/session/jail.plan` instead of applied; THIMSY_ROOT and THIMSY_TOOLDIR passed to every child.
- **G3b-23:** Logs go to `/run/thimsy/log/session.log`, plus a log per child program; kernel-log markers include `thimsy-session: nano started`, matching E.9.

### G8. Card assembly

- **G8-1:** Cards are put together by post-image.d in the hardware pack's tree. The tier comes from the sibling `output-<sdk_from>` of BASE_DIR (from `targets.json`), or from THIMSY_TIER_DIR or `--tier-dir=`; in a tier tree, `10-erofs.sh` only checks `rootfs.erofs` (the EROFS magic, and that it fits the smallest root slot) and the other parts skip.
- **G8-2:** Each device's root is the tier's fakeroot-built `rootfs.erofs` unpacked with `fsck.erofs --extract --overwrite`, then `gpu-<driver>.erofs` over it if present, then the hardware pack's `usr/lib/{modules,firmware}`, the family overlay, and the device JSON at `/usr/share/thimsy/device.json`; it is rebuilt with `mkfs.erofs -zlz4hc,LEVEL -C PCLUSTER -T epoch -U uuid5 --all-root`, LEVEL and PCLUSTER read from BR2_CONFIG. Unpacking the GPU pack contradicts thimsy-gpu-pack's Config.in ("mounted over the tier root at boot"; see the open items).
- **G8-3:** `users.table` defines no user, so everything in the root is owned by root and `--all-root` is correct; adding a user means `10-erofs.sh` must drop `--all-root`.
- **G8-4:** Each device's `data.ext4` is `mkfs.ext4 -F -q -L thimsy-data -U uuid5 -m 0 -E root_owner=0:0 -d` holding `thimsy/device.json`, sized from `genimage.cfg`, with E2FSPROGS_FAKE_TIME=SOURCE_DATE_EPOCH; S00base still makes the rest of `/storage`.
- **G8-5:** Filesystem UUIDs are uuid5("thimsy:<id>:<root|data>:<epoch>"), so rebuilds give the same ones; GPT partition UUIDs are still genimage's random ones.
- **G8-6:** Family facts are in `board/<family>/card.conf`, sourced by the library; for now only CARD_GPU_PACK (panfrost for rk3566, swrast for qemu). A missing GPU pack is a note, and the tier root's own Mesa is used.
- **G8-7:** The hw fragments pass "hw --family=<family>", since the runner needs `--family=` to run `board/<family>/post-image.d`; the library also accepts "hw FAMILY". The merged fragments carry G2d's form ("hw rk3566 --family=rk3566"), which both read.
- **G8-8:** RK3566 U-Boot files are `idbloader.img` and `u-boot.itb` from BINARIES_DIR, or cut from `u-boot-rockchip.bin` at 16320 sectors (CONFIG_SPL_PAD_TO 0x7f8000), covering hw-rk3566's FORMAT_CUSTOM_NAME without editing it; they are stored once in `cards/.family/`.
- **G8-9:** `25-boot-img.sh` passes `fit.py build --device ID --dtb <basename>` explicitly, because device.json's "dtb" carries "rockchip/", which fit.py's `--device-json` refuses; the DTB is looked up in BINARIES_DIR by basename, then by the full path.
- **G8-10:** genimage runs once per device with `cards/<id>/` as the inputpath; missing inputs are linked in from `cards/.family` or BINARIES_DIR, and a missing one fails before genimage starts. The result is `BINARIES_DIR/thimsy-<id>.img`, plus a `.bmap` when bmaptool is on PATH.
- **G8-11:** `40-gpt-check.sh` runs `gpt_check.py IMG --mode image --family F --layout thimsy`, adding `--idbloader-size <size of the idbloader.img genimage took>` when the cfg uses an idbloader; any failure fails the build, naming every failing device.
- **G8-12:** `boot.cmd` reads the FIT from mmc 1 by the partition name "boot", checks the THIMSFIT words 4d494854/54494653 at +0/+4, reads the u32 sector count at +8, takes bootargs from `/configurations/<default>` with `fdt get value`, appends `root=PARTUUID=` of `part uuid mmc 1:3`, then runs bootm. It never reads mmc 0, and every line is a whole command, so it can be joined into one bootcmd.
- **G8-13:** `10-android-links.sh` makes `/data`, `/sdcard`, `/system/{etc,fonts,bin}` and `/mnt/media_rw -> /run/thimsy/media_rw`, creating the link targets inside the root, empty. It never puts anything under `/storage` (`/storage/self/primary` stays S00base's), replaces empty directories, fails on non-empty ones, and a re-run changes nothing. The design gave this file to G9; G8 wrote it as instructed, and G9 takes it over.
- **G8-14:** `device.table` makes no device nodes; it sets the modes of `/storage`, `/run`, `/tmp` (1777), `/system` and the link targets, and deliberately does not list `/root`.
- **G8-15:** `run-qemu.sh` uses E.9's kernel line and devices, boots a sparse copy of the card grown by 1 GiB (`--grow`) so D60firstboot has room for the games partition (`--in-place` boots the card itself), adds `-no-reboot` and virtio-net, and uses KVM on an aarch64 host, TCG `-cpu max` otherwise.
- **G8-16:** `test_qemu_boot.py` runs only with THIMSY_QEMU=1, a built card, qemu-system-aarch64 and Unix sockets; its Nano markers are environment variables (guesses until Phase 2b), and `run-qemu.sh --dry-run`'s tests always run.

### G9a. DRM buffer back end split, Android-surface test

- **G9a-1:** The split is mechanical: only `drmAllocAhbTarget` moves, to `NanoMenuDrmBuffers.cpp`. The free code (`drmStop`'s `releaseSlot`, `drmSetupZeroCopy`'s unwind loops) and the CPU-read blit path (`AHardwareBuffer_describe`/`_lock`/`_unlock` in `drmFlipRingSlot`) are pieces of KMS functions and stay; a back end that is not AHardwareBuffer defines those four NDK functions itself (link-time substitution). Commit `6d6314c` is flagged for cherry-pick to nano-144 once the AOSP job is done.
- **G9a-2:** `NanoMenuDrm.cpp`'s `#include` list is left exactly as it was (vndk, `native_handle`, `ui/GraphicBufferMapper.h`, `ui/GraphicTypes.h` included), so the Android file sees the same declarations; the Linux build therefore needs empty stub headers for those four. Where `ioctl()` comes from on Android could not be checked.
- **G9a-3:** `NanoMenuDrmBuffers.cpp` takes `NanoMenuDrm.cpp`'s licence header (the code is moved), `LOG_TAG "GammaOSNano"`, and `NanoMenuDrm.cpp`'s include list without `<arm_neon.h>`, plus `<sys/ioctl.h>`.
- **G9a-4:** `NanoMenuDrmBuffers.h` redeclares `drmAllocAhbTarget` and keeps the contract as a comment; `NanoMenuDrm.h` keeps its own declaration, so that header is not edited.
- **G9a-5:** Mesa lacks the Android EGL gate that `drmSetupZeroCopy` checks first (`eglGetNativeClientBufferANDROID` plus two Android extension names), so Linux links `thimsy-nano` with `--wrap=eglGetProcAddress,--wrap=eglQueryString`, answering only for those names; the fallback is a later P4 move of the gate into the back end.
- **G9a-6:** B.8 is corrected: the ring is 5 deep, not 3; there is no free function; a target with `drmFbId == 0` is valid on Android only, so a Linux back end fails instead; `sDrmZeroCopy` needs the whole primary ring; and it now states GEM handle ownership and that the gbm device is keyed on the current `sDrmFd` (it changes on re-acquire).
- **G9a-7:** The Android surface is computed generously from HEAD's git objects: every file or glob named by any string property of any module in `nano/Android.bp`, plus `Android.bp`; the quoted and angle `#include` closure, resolved through the including folder, `nano/` and every `*include_dirs`; every header under `export_include_dirs`; and `nano/` files the patch's other sections include or name by path.
- **G9a-8:** Every differing surface path must have been changed by a non-merge commit since the merge base whose message matches `^android-surface: G<step> ... review`; any non-merge commit that changed a differing path without that line fails the test, and merges are ignored.
- **G9a-9:** The logic lives in `tools/nano-tree/nanotree/surface.py`, so `include_census.py` can reuse it; the test only drives it.

## Open items from waves 1 and 2

Each still-open problem from the reports, once, with who must act. Items a later report or the merged `linux` branch shows done were left out: G2d wrote the recipes for libthimsy, thimsy-session, thimsy-padd and thimsy-gpu-pack, sourced `gpl/package/Config.in`, and added the RK3566 overlay, the userland tools and `--family=`; G2e's `rootfs-overlay`, `post-build.sh` and `post-image.sh` exist; G3b installs to `/usr/libexec/thimsy/session`, reads `/run/thimsy/bootdisk`, ignores Linux reserved entries, mounts the games drive on its "change" uevent, restarts padd and acts on sys.powerctl; G8 wrote per-device genimage runs, `--idbloader-size`, `users.table`, `device.table`, the QEMU `linux.fragment` and the `/mnt/media_rw` link; `linux/tests/run.sh` discovers the importer's and init's tests; the merge driver is installed in the main checkout's `.git/config`. Items wave 3 is working on are tagged with its step.

### Android patch and Nano tree

- `regen` treats `git add -N` entries as empty files and `regen --check` agrees; only `test_nano_patch_consistent`'s post-image check catches it. It should hash the file on disk or refuse (G9a). *(wave 3: gfix)* — done in wave 3 (gfix, gfix-1)
- The merge driver exits 1 whenever it changed `nano/` (G0-7), leaving the merge to be finished by hand. *(wave 3: gfix)* — done in wave 3 (gfix) for merges between fork branches, which the drivers now finish (gfix-2, gfix-3); a nano-144 merge that changes `nano/` still exits 1
- When only nano-144 changed the patch since the merge base, git takes nano-144's patch without running the driver; the consistency test and `regen`'s refusal catch it, and the fix is `import-patch --old <previous merge base>` (G0). *(owner, at each nano-144 merge)*
- `6d6314c` (the DRM buffer split) is to be cherry-picked to nano-144 once the AOSP job is done, and its compile and drastic-nano's link are proven only by the AOSP build of H.7 (G9a). *(owner)*
- A leak that predates G9a: `drmSetupZeroCopy`'s unwind loops don't RMFB or GEM_CLOSE the earlier slots; recorded in B.8, not fixed, since fixing it changes Android behaviour. *(owner)*
- The four sibling files' base contents are not in the adopted cache, so `compose` warns until a network `fetch` gets them; drastic-nano's unmodified base sources can't be read offline either, so a Nano header included only by one of them would be missed by the surface test (none is known) (G0, G9a). *(G10)*
- `nano/NOTICE` and an Apache-2.0 LICENSE in `nano/` don't exist yet (I41; lint reports them pending) (G2d). *(wave 3: gfix)* — done in wave 3 (gfix, gfix-9)
- `tests/run.sh` at the repository root has no `nano` or `linux` mode yet; it should run `tools/nano-tree/tests` (with `test_android_surface.py`) and call `linux/tests/run.sh`; `tests/unit/test_nano_units.py` should point at the new gtest stand-in instead of `tests/fixtures/nano_gtest` (G0, G1a, G2a, G9a). *(wave 3: G1c)* — done in wave 3 (G1c)

### Host build, stubs and the Linux platform

- Include `compat.mk` in the host build (setting COMPAT_O first if wanted), use `$(NANOCOMPAT_CPPFLAGS)` and `$(NANOCOMPAT_ASAN_LIBS)`; libthimsy's Makefile and `compat.mk` keep their own MemAvailable guards instead of `linux/tools/build_guard.sh --light` (G1a, G1b). *(wave 3: G1c)* — done in wave 3 (G1c); the two own guards stay, after one `build_guard.sh --light` run (G1c-9)
- `sources.txt`: `NanoMenuDrmBuffers.cpp` is `[firmware]` and `NanoMenuDrm.cpp` is `[drm]` (G9a). *(wave 3: G1c)* — done in wave 3 (G1c, G4)
- Stubs: the `android/hardware_buffer.h` stub must declare, not define, `AHardwareBuffer_release`/`_describe`/`_lock`/`_unlock`; empty stubs for `vndk/hardware_buffer.h`, `cutils/native_handle.h`, `ui/GraphicBufferMapper.h` and `ui/GraphicTypes.h`; `utils/String8.h`, `String16.h` and the rest of the stub set (G9a, G1b). *(wave 3: G4)* — done in wave 3 (G4)
- B.2's ratchet on Bionic-only identifiers has no owner in section G (G1b). *(wave 3: G4)* — done in wave 3 (G4, G4-9)
- Call `nanocompat::init({"/storage/.config/thimsy/persist.props", "/run/thimsy/props", true})` before any Nano thread starts, and `nanocompat::setLogFile("/run/thimsy/log/nano.log")` (G1b). *(wave 3: G5, G9)* — done in wave 3 (G9 in `main_linux.cpp`; G5's `HostPlatform.cpp` for the host)
- Implement the two EGL wraps (G9a-5) and `DrmBuffersGbm.cpp` per B.8: `ahb` non-null, fail rather than return `drmFbId == 0`, `drmGemHandle` 0 when the gbm buffer owns the handle, define the four AHardwareBuffer functions (G9a). *(wave 3: G9)* — done in wave 3 (G9), written but not compiled (G10)
- ControlKeys registers its handlers with `nanocompat::addPropertyObserver` and maps dosleep to the `sleep` request and park_restart to `restart-nano` (G1b, G3b). *(wave 3: G9)* — done in wave 3 (G9)
- G9 takes over `post-build.d/10-android-links.sh` from G8 rather than writing it again (G8). *(wave 3: G9)* — done in wave 3 (G9)
- The nano recipe assumes the CMake options `NANO_PLATFORM=linux`, `NANO_WITH_AC3=OFF`, `NANO_BUILD_TESTS`, `NANO_BUILD_SCRAPE` (G2d). *(wave 3: G6)* — done in wave 3 (G6)
- The session and padd recipes pass `-DTHIMSY_BUILD_TESTS=OFF` (G2d-2), but the CMakeLists name the options THIMSY_SESSION_BUILD_TESTS and THIMSY_PADD_BUILD_TESTS; and `linux/src/thimsy-session` and `linux/src/thimsy-padd` have no LICENSE file though the recipes name one (G2d). *(wave 3: G6)* — done in wave 3 (G6)
- The CMake build should link png, jpeg, webp, webpdemux and gif when their headers exist, with the probe `compat.mk` uses (G1b). No CMakeLists.txt has been configured yet (G1a, G3a, G3b). *(wave 3: G6)* — done in wave 3 (G6, G6-9); still never configured (G10)

### Linux build and card assembly

- `build.sh` passes `--out` to `nano_tree.py export-assets`, which takes OUT as a positional argument; the asset tarball should also include ATTRIBUTION.md and NOTICE so legal-info has the licence texts (G2a, G2d). *(wave 3: gfix)* — the positional OUT done in wave 3 (gfix, gfix-5); ATTRIBUTION.md and NOTICE are still not in the tarball (`thimsy-data.mk`'s comment says build.sh should add them)
- `build.sh` should export THIMSY_BUILD_TARGET (it passes BR2_DEFCONFIG=/tmp/defconfig, so `20-build-props.sh` reads the target as "defconfig"), and `build.sh card DEVICE` still dies with "not written yet"; it could re-run the hw target's post-image with THIMSY_DEVICES=DEVICE (G8). *(wave 3: gfix)* — done in wave 3 (gfix, gfix-4, gfix-5)
- `/root` may break the build: Buildroot's `system/device_table.txt` has "/root d 700", and `30-permissions.sh` turns `/root` into a link to a path that doesn't exist at build time, so makedevs' chown may fail (likely, not checked against makedevs' source) (G8). *(wave 3: gfix)* — done in wave 3 (gfix, gfix-6), by avoiding the link; makedevs not run (G10)
- `fit.py --device-json` should read the device.json schema ("id", and a "dtb" with a directory) (G2b-9, G8-9). *(wave 3: gfix)* — done in wave 3 (gfix, gfix-7)
- GPU pack: thimsy-gpu-pack's Config.in says the pack is mounted over the tier root at boot, G8 unpacks it into each card's root; one must change, and if mounted, `gpu.json`'s environment must be applied at boot (G2d, G8). *(wave 3: gfix)* — done in wave 3 (gfix, gfix-8: unpacked, not mounted); `gpu.json`'s environment is still not applied (see wave 3's open items)
- Linux denies Nano's Akrobat files, while Android replaces them with Barlow under the same names (`mods/50-nano.sh`); Linux needs the same, or Nano falls back (G2d). *(wave 3: gfix)* — done in wave 3 (gfix, gfix-10)
- `dev.fragment` still adds `board/common/rootfs-overlay-dev`, which no step created (pending in `targets.json`, so build.sh refuses the dev targets), while G2e-13 already adds the dev getty from `30-permissions.sh`: create the overlay or drop it from `dev.fragment` and `pending_paths` (G2a, G2e). *(unassigned; gfix's area)*
- `thimsy-prop boot` defaults to `/boot/thimsy/install.json`, but S01props calls it with no arguments and G2e-14 puts the file at `/storage/thimsy/install.json` (G1a, G2e). *(unassigned; gfix's area)*
- The init scripts and thimsy-session read ro.thimsy.family and ro.thimsy.modules, but `thimsy-prop boot` writes the family as ro.thimsy.device.family and the device.json schema has no "modules"; the family falls back to the only family directory, the module list has no source (G2e, G1a). *(unassigned; gfix's area)*
- The device.json `product` and `props` fields `thimsy-prop` reads (G1a-13) are not in G2c's `device.schema.json`; the schema should adopt or rename them (G1a). *(unassigned; gfix's area)*
- G2e's `board/rk3566/common/README.md` still says the family overlay goes into the tier defconfigs (G2d-16). *(unassigned; gfix's area)*
- The thimsy-session SSH options SSH_AT_BOOT and SSH_PASSWORD (G2a-6) change nothing, since D50ssh decides by ro.thimsy.build.type; drop them or wire them to D50ssh (G2d, G3b). *(wave 3: G6, with the recipe option names)* — not done in wave 3: both are still in `package/thimsy-session/Config.in` *(wave 4: removed by gfix2, GF2-7)*
- GPT partition and disk UUIDs are genimage's random ones, so images are not reproducible; pinning them would make two cards from one image collide in an ARC-D (G2b, G8-5). *(owner)*
- One Mesa build gives identical GPU packs that differ only in `gpu.json`; smaller packs need one Mesa build per driver (G2d). *(G10: measure the sizes)*
- RetroArch's menu assets (retroarch-assets) are not packaged (G2d). *(G10)*

### Recipes and Buildroot guesses (need a Buildroot tree or the network)

- Hashes for retroarch, ppsspp (including the `-git4` tarball name, a guess), scummvm, thimsy-fonts and the cores; the font URLs and archive layouts (G2d). *(G10)*
- Core commits: run `scripts/core_sources.py --cache <work/cache>` where the build cache lives, then with `--resolve` once the network is allowed, then `linux/tools/lint_packages.py --sync-cores`; melondsds needs a Linux commit chosen and its CMake FetchContent dependencies pinned as `_EXTRA_DOWNLOADS`; fake08 needs a commit chosen another way (G2d). *(G10)*
- Buildroot names and behaviours to confirm: MESA3D_GBM, MESA3D_VULKAN_DRIVER_PANFROST, the ROCKCHIP_RKBIN_*_FILENAME options and file names, HOST_BMAP_TOOLS, EROFS_LZ4HC as a bool, GCC 14 and headers 6.12 in 2026.08.2, the SDK tarball name and `relocate-sdk.sh` path, the QEMU kernel 6.12.58 (G2a); UTIL_LINUX_BINARIES, BUSYBOX_CONFIG_FRAGMENT_FILES, `_PKGDIR`, `_PRE_DOWNLOAD_HOOKS`, hash files read from BR2_GLOBAL_PATCH_DIR for file sites, the mkfs.erofs flags (`-zlz4hc`, `-C16384`, `-T`, `--all-root`), the RetroArch, PPSSPP and ScummVM configure and CMake flags, the libretro Makefile names and subfolders (G2a, G2d); BR2_LINUX_KERNEL_CUSTOM_DTS_DIR copying into `arch/arm64/boot/dts/` and INTREE_DTS_NAME building out-of-tree DTS names (G2c). *(G10)*
- Whether Buildroot 2026.08.2 has its own `package/scummvm` (then ours must be renamed; `defconfig.py --buildroot` reports it), and whether it has sdl3 and sdl2-compat; ppsspp and scummvm select SDL2 until the I43 swap (G2a, G2d). *(G10)*
- Tool behaviours to confirm: `fsck.erofs --extract=DIR --overwrite` (erofs-utils 1.7 or later) keeping owners as root; mke2fs honouring E2FSPROGS_FAKE_TIME; host-bmap-tools installing `bmaptool`; genimage taking an empty `--rootpath`, partitions with no image (qemu's uboot and boot), and appending the backup GPT itself when the hdimage has no size; the QEMU `linux.fragment` options mostly =y in arm64's defconfig; the real mkimage build and read-back in `test_fit` (G2b, G8). *(G10)*
- Kernel-log markers are written with no level prefix, so with `quiet loglevel=3` they may not reach the serial console, which the QEMU test reads; its Nano markers ("thimsy-session: nano started", "nano: first frame", "nano: menu opened") are guesses until Phase 2b; the QEMU boot test itself has not run (G2e, G3b, G8). *(G10)*
- Check that Mesa's `EGL_ANDROID_native_fence_sync` works and that the EGL wraps hold (G9a). *(G10)*
- Compile and test AImageDecoder's libpng, libjpeg-turbo, libwebp and giflib paths once the -dev packages are installed, and add JPEG (with EXIF orientation) and WebP (still and animated) fixtures (G1b). *(G10)*
- thimsy-session was not cross-compiled for aarch64, and its `SYS_ioprio_set` fallback numbers are from memory (G3b). *(G10)*
- padd's input-code name table comes from Ubuntu 20.04's kernel 5.4 header; regenerate it from the image's kernel headers if a device.json needs a newer code (G3a). *(G10)*

### Importer

- Q34 is not applied: the importer can't know a series' last stable point release without the network (G2c). *(G10)*
- The scripts/unpack patch loop is not in the sparse clone, so the real RK3566 and H700 runs record that guard as "absent" and assume the order; `kernel_config_path`'s order is assumed from `config/functions` and not guarded (G2c). *(Phase 0/2a (device), with a full clone)*
- The release-image check against ROCKNIX's ikconfig is not built; it needs downloads (G2c). *(G10)*
- Mainline-only DTBs (most H700 boards, plus rgb10max3) have no model without `--kernel-src` or an overrides file, and `thimsy/<f>/overrides` is not implemented (G2c). *(unassigned; before the H700 wave)*
- InputPlumber stick and axis maps are not translated, so devices with sticks (for example RG-DS) get no sticks or face_buttons (G2c). *(unassigned; before a wave with sticks)*
- Excluded proprietary firmware is only reported as fetch_candidate; there is no UI to fetch it from the user's device (D24) (G2c). *(owner; installer, Phase 4)*

### Nano, input and session behaviour

- Nano sets and clears `sys.thimsy.padd.intercept` and reads the menu's input from `/run/thimsy/padd-tap.sock` while it is on (G3a). *(G7-input)*
- padd does not grab external pads while intercept is on, though plan 5.3 says it can (G3a). *(G7-input)*
- Nano's volumes and power code implements against `linux/docs/session.md`, including the pre-launch mount check (st_dev comparison, plan 7.3) before a launch (G3b). *(G7-volumes, G7-power)*
- Not done in the session: a `sync` when a game exits (the session can't see a game exit while Nano runs), a Nano "busy" sleep inhibitor, the exFAT clean-unmount record (plan 7.3's alternative), and re-sleeping when woken with the lid still closed (G3b). *(G7-power)*
- The jail does not stop a root process with CAP_SYS_ADMIN from mounting a new devtmpfs; dropping SYS_ADMIN needs an audit of Nano (G3b). *(owner)*
- S02padd has no stop action; padd shuts down cleanly on SIGTERM (G3a). *(unassigned; small)*

### On a device

- The RK3566 files from the Phase 2a hand import and FlipBase's subtree merge, still pending in `targets.json`: `generated/rk3566/defconfig.fragment`, `gpl/generated/rk3566/linux/{config,patches}`, `gpl/board/rk3566/{linux,uboot}/patches`, `board/rk3566/linux.fragment`, `board/rk3566/uboot/uboot.fragment`; whoever adds one drops it from `pending_paths` and re-runs `tools/defconfig.py` (G2a). *(Phase 0/2a (device))*
- `uboot.fragment` must embed `boot.cmd` (CONFIG_BOOTCOMMAND or a default environment file) and enable CMD_SETEXPR, CMD_FDT, CMD_PART and PARTITION_UUIDS; `bootm` ignores the FIT `bootargs` property, so the `fdt get value` path is required; to check on the device: the SD slot is mmc 1, setexpr's `*ADDR` load form, hex without 0x from part and setexpr; `boot.cmd` does not check the sector count before the FIT read (G2b, G8). *(Phase 0/2a (device))*
- Verify the FIT header's magic and bytes 12-511 against FlipBase's `tools/mkfit.py` at the subtree merge (G2b-6). *(Phase 0/2a (device))*
- RK3566 BL31: rkbin's until Phase 0 and D18 settle it (G2a-17). *(Phase 0/2a (device))*
- padd on the ARC (Q4): whether the always-declared ABS_X and ABS_Y make the pad a joystick under libudev-zero, SDL and RetroArch; EVIOCGRAB, hiding by unlink, the mknod fallback, a real `/dev/uinput` (CONFIG_INPUT_UINPUT), FF on the pwm-vibrator (G3a). *(Phase 0/2a (device))*
- Guesses in the ARC device.json files: the touch evdev name, the "rk805 pwrkey" name, the ARC-S RAM, the hotkey, the F label (G2c, G3a). *(Phase 0/2a (device))*
- Audio: the RK817 "Playback Path" values; the ARC's real mixer control name for volume (the family hook has no "volume" action, and `ro.thimsy.audio.volume_control` defaults to Master), to be supplied by device.json props or the hook (G2e, G3b). *(Phase 0/2a (device))*
- Init on a device: that `sfdisk --force -N 5` adds the entry through BLKPG on the pinned util-linux, that PARTUUID appears in partition uevents (only the fallback needs it), and how long rcS takes; deep sleep depending on the vendor BL31 (Q24) (G2e). *(Phase 0/2a (device))*
- How long `fsck.exfat -p` takes on every mount (G3b-8 keeps it always until Phase 0 times it) (G3b). *(Phase 0/2a (device))*
- The session's root-only paths, faked in tests: netlink, mount(2), EVIOCSCLOCKID, pidfd on padd, making `/` shared, and NanoJail's unshare and bind mounts (G3b). *(Phase 0/2a (device))*
- The hook's sleep-pre writes "deep" to mem_sleep and the session then writes its own choice; they agree on the ARC, and each new family should be checked (G3b). *(Phase 0/2a (device))*

### Assets, owner's choices

- ~~Slate is denied as a whole and the Colorful theme is not in the export, so Linux needs a filtered Slate or another default theme (G2d).~~ Decided: OA-1.
- ASSETS.md's table still says KEEP for Akrobat, though its "Changes since the audit" says REPLACE (G2d). *(owner)*

### Notes, no action needed

- Test times: `test_nano_tree.py` about 60 s on Windows and 40 s in WSL; `test_rcs.py` 42 s in WSL and 155 s on Windows; `test_post_image.py` about 290 s on Windows and 45 to 120 s in WSL; the linux suite is much faster from a copy under `~/.cache` than from `/mnt/c` (G0, G2c, G2e, G8).
- WSL git can't resolve a worktree made by Windows git, so WSL runs set GIT_DIR and GIT_WORK_TREE, which the tools honour; index modes are checked on Windows (G0, G2e).
- The main repository prints "unable to find all commit-graph files" on most git commands; harmless, and older than this work (G0).
- padd's rocknix-joypad golden reads G2c's generated H700 file; if it changes, run `make golden` and review the diff (G3a).
- `nano/compat/**` now reaches the patch (inert for Soong), so any later edit there needs a `nano_tree.py regen` commit (G1b).
- The hw fragments, `targets.json` and the defconfigs conflict whenever two steps regenerate them; re-run `tools/defconfig.py` after a merge (it passes on `linux` today) (G8).

## Implementation decisions (wave 3)

Recorded on 2026-11-25 from the six step reports of wave 3, all merged into the `linux` branch (merges `6f0127c` to `8dd8131`, then `1a1ce7e` regenerated the include census). gfix, G1c and G9 were cut from `dfe7a77`, G4 and G5 from `step/g1c`, G6 from `step/g4`. Each id is the step's own; all six reports numbered their decisions. Only G5 changed the Android patch: `1aa3ea3` adds exactly the sections for `NanoPlatform.h` and `NanoPlatform.cpp`, with no `android-surface:` line, since no `Android.bp` module lists them and no surface file includes them; every other new Nano file is fork_only. Nothing ran on a device and nothing was built for one: Buildroot, Docker, mkimage, CMake and QEMU were not run. In WSL (g++ 9.4, ASan and UBSan) the shell gate and control keys, the stubs, `check-unit-0` and `check-headless-0` were compiled and passed; `DrmBuffersGbm.cpp`, `EglWraps.cpp`, `main_linux.cpp`, `DrmHost.cpp` and `main_headless.cpp` were written but not compiled. Where a step corrected an earlier decision or a design section, the bullet says so.

### G1c. Host build: `sources.txt`, the include census, the host Makefile

- **G1c-1:** The first-cut `[firmware]` is exactly C.1's named list: main, NanoMenuDrmBuffers, NanoMenuRgbFollow, NanoMenuShares, NanoBootChime, NanoMenuDrasticPark, NanoBtStable, LibretroRunner, FakeJNI and the three Drastic*; everything else is `[core]`. G4 refines it from the census (G4-4 agrees with it).
- **G1c-2:** `[linux]` and `[host]` are empty sections until G9 and G5 add files; every section is always present, and the test checks the order. (G5 filled `[host]`; `[linux]` is still empty, see the open items.)
- **G1c-3:** `[test-unit]` lists its tests explicitly. The test requires them to equal `test/unit/*_test.cpp` without `DsScreenLayout_test.cpp`, and the production files to equal gammaos_nano_unit_tests' srcs exactly, not just a subset.
- **G1c-4:** Each include gets one class: nano, shim, system, gtest, libthimsy, optional (named in `__has_include`), lib:<name>, android, sibling, arch, foreign, missing, unknown (G4 adds "stub"). Only includes outside any `#if` count as needs; a header's include guard does not count as an `#if`.
- **G1c-5:** A program's closure is every `[test-unit]` .cpp whose same-named header it reaches, transitively. The `HEADER_SOURCES` override table is empty: all 19 programs linked without it.
- **G1c-6:** A program also needs an asset folder (written `@ps3xmb`, `@nano_esde_themes`) when a string literal in its test names one. Such programs run when `NANO_DATA` is set (compose's output; default `<repo>/work/nano-composed`), else they print SKIP with the reason.
- **G1c-7:** Each run lays out test data in `$(O)/unit/data`: test/fixtures and `nano/`'s asset files are copied, because `ReadFileToString` refuses symbolic links (O_NOFOLLOW, as on Android); the base's asset files are linked; directories are real, so a test's `fixtures/../..` stays inside the data tree.
- **G1c-8:** `-Wno-dangling-else` joins the GCC-only suppressions: GCC warns on `if (c) EXPECT_*` despite the macro's else blocker, and Clang does not. No Nano or stand-in file was edited.
- **G1c-9:** One guard run per make invocation, keyed on the heaviest goal asked (`build_guard.sh --light --target`), chained before `compat.mk`'s own memory check.
- **G1c-10:** syntax-core and headless check their headers before the guard and print `SKIP ...: missing ...` with exit 0, so nothing compiles on a machine without the headers; check-unit runs the guard first, because it would compile.
- **G1c-11:** Test totals come from the stand-in's `N tests, M failed` line, or from real GoogleTest's own lines (`GTEST=system`). The summary line is `nano <target>: N tests, M failed, S skipped, in P programs`.
- **G1c-12:** `tests/run.sh cxx` runs libthimsy's check, check-compat (with `COMPAT_WITH_LIBTHIMSY=0`), check-unit-0, thimsy-padd and thimsy-session, each behind the guard and the lock; a refused suite fails the mode; from Git Bash it re-runs itself in WSL, and a /mnt checkout is copied to `~/.cache/thimsy/cxx-src` first (thimsy-session's E2E timed out on /mnt).
- **G1c-13:** `test_nano_host.py` runs from Windows through `wsl.exe --exec` (plain `--` let an outer shell expand `$VAR`), on a copy in `~/.cache/thimsy/nano-host-src`; `THIMSY_NANO_HOST=0` turns it off.
- **G1c-14:** The census leaves out `compat/include` and `test/host/compat`, which build with `compat.mk`.

### G4. Layer-2 stubs, the census split, stubgen

- **G4-1:** Stubs cover only what `[core]` and `[drm]` actually use; identifiers only firmware files use are left out. The C NDK functions are defined in libnanostubs.a and the C++ stubs are inline in headers. Creates and lookups fail, the rest do nothing, and each stub logs once at its first call: warning priority in a debug build, debug priority with NDEBUG.
- **G4-2:** The budget counts the distinct names declared under `stubs/include` (macros, types, enumerators, functions with overloads counted once, variables and members, qualified by scope): 446. The test fails on growth, and on shrinkage until the budget file is lowered; `--update-budget` never raises it.
- **G4-3:** `jni.h` and `binder/ProcessState.h` are deliberately not stubbed (NOT_STUBBED): only firmware files (main, DrasticRunner, FakeJNI) reach them. A new unstubbed Android header reached from any file stops the census until it is stubbed or given a reason.
- **G4-4:** The split is computed from the census and agrees with G1c's first cut: `[core]` has 103 files (G1c's first cut counted 102), `[drm]` is NanoMenuDrm.cpp, `[firmware]` has 12. Placed by hand (SPLIT_DECISIONS): NanoMenuDrm.cpp (`[drm]`); NanoMenuDrmBuffers, NanoBootChime, NanoBtStable, NanoMenuDrasticPark, LibretroRunner, DrasticGpu3d and DrasticSettings (`[firmware]`). Placed by headers: main, FakeJNI and DrasticRunner (`jni.h`); NanoMenuShares and NanoMenuRgbFollow (sibling headers).
- **G4-5:** `[dropped-linux]` means "a `[firmware]` file defining anything `[core]` or `[drm]` names" (NanoMenu:: methods, free functions, variables), not only NanoMenu:: methods. NanoMenu.cpp calls into LibretroRunner, DrasticRunner, BootChime, BtStable and RgbFollow, so 10 of the 12 firmware files qualify. This replaces IMPLEMENTATION.md C.1's "empty unless a file truly can't compile". False positives are harmless, because gen.py only stubs symbols still undefined at link.
- **G4-6:** gen.py stubs only missing symbols (undefined in the nm output and defined nowhere in it) that a `--from` file defines or that match a `--scope` prefix. Return types come from the declarations (the class body for methods; `[abi:cxx11]` tags are stripped). Generated forms: `auto Q::f(params) cv -> Ret` with a value-initialised zero (a static object for references); empty constructors and destructors; `decltype(x) x{}` for data. A missing or ambiguous declaration, a template or an operator is an error, never a guess.
- **G4-7:** The Linux and host builds define `EGL_NO_PLATFORM_SPECIFIC_TYPES`, so EGLNativeWindowType is `void*`. The stub `Surface` derives from ANativeWindow, so `eglCreateWindowSurface(..., s.get(), ...)` and `native_window_*` compile unedited.
- **G4-8:** The NDK media stubs use off64_t as Bionic does; a C file on glibc must define `_LARGEFILE64_SOURCE` (g++ already has `_GNU_SOURCE`).
- **G4-9:** The B.2 ratchet covers shared files minus `compat/`; it records 12 Bionic or libutils identifiers (`bionic-identifiers.txt`), and the test also requires each to be present in the shims. The H.4/B.5 ratchet records string literals naming /dev/block or mmcblk in shared non-test files: only NanoScreenCal.cpp (`block-paths.txt`; NanoSdMove's `/sys/dev/block` is excluded, as in G9-19). Both live in `test_nano_stubs.py`, so `test_nano_ratchets.py` is left alone. This gives B.2's ratchet the owner waves 1 and 2 lacked.
- **G4-10:** `-Wno-misleading-indentation` joins GCC_SUPPRESS, because of NanoAudio.cpp:582.

### G5. The seam and the headless E2E

- **G5-1:** `nanoplat::Platform` is an empty struct with a virtual destructor. Platforms derive from it to keep their own state; G7 steps add services as members.
- **G5-2:** `install()` aborts on null or a second install. The platform is never destroyed (deliberately leaked), so `get()` stays valid during exit.
- **G5-3:** `NanoPlatform.cpp` is listed in `sources.txt [host]` (and must go in `[linux]` too) until G7-main adds it to gammaos-nano and moves it to `[core]`.
- **G5-4:** The frozen-API ratchet (`FrozenApi` in `test_nano_ratchets.py`) covers `drastic_nano_shared_srcs` plus every cc_* module except gammaos-nano, including fuzzers, the other binaries and the static libraries: a wider set than the design names (I24). It follows includes transitively and flags any include of `NanoPlatform.h` or any `nanoplat::` in code.
- **G5-5:** DrmHost emulates the PRIME path: non-zero `drmFbId` and the Y-flip folded into `sDrmRotMat`. Captures are binary PPM with the top row first, as the panel shows it.
- **G5-6:** DrmHost installs fake EGL native-fence functions, so the render loops' ring path runs. The GL stub's `eglGetProcAddress` returns null for everything else.
- **G5-7:** DrmHost defines `nanoRgbFollowSample()` as a no-op: it is declared in `NanoMenuDrm.h` but defined in the `[firmware]` file `NanoMenuRgbFollow.cpp`. DrmHost does not define the `AHardwareBuffer_*` functions, since no `[core]` file calls them.
- **G5-8:** Presents are paced to the configured refresh rate (default 60 Hz) by HostControl; `drmPaceWithoutVsync` is a no-op.
- **G5-9:** The runner and binary talk through `/run/thimsy/headless/`: `events.jsonl` (one JSON object per line) and a `ctl` FIFO (`barrier on|off`, `grant N`, `capture PATH`, `ping`, `quit`). The binary reads its config from `--host-config` or `THIMSY_HEADLESS_CONFIG`.
- **G5-10:** Every property write is logged as a `prop` event (prefix filter `record_props`, default all keys), because the launch-handshake assertions need write order.
- **G5-11:** Host shell gate: rules match by prefix, or exactly when `exact` is set; an unmatched command gets status 127; `popen` output goes through a pipe, cut at 64 KiB; `exec*` writes its event with one raw `write` and then `_exit(status)`, so it takes no lock in the forked child; `system(NULL)` returns 1.
- **G5-12:** FakeEvdev finds a device by the FIFO's path (readlink of `/proc/self/fd/N`), cached per inode. Answers follow the kernel's evdev semantics; `EVIOCGKEY` always reports nothing held.
- **G5-13:** GL stub: generic stubs are empty-parameter-list C definitions returning 0 for `gl*` and 1 for `egl*`; `glue.c` hand-writes the calls Nano reads back; it reports "OpenGL ES 2.0" with no extensions and counts draws, which go into each `present` event.
- **G5-14:** Jail binds are recursive (`--rbind`), and every submount is remounted read-only with its locked flags repeated, because WSL's `/usr/lib` has submounts and a plain bind of it is refused in a user namespace. `/usr/libexec` is not bound (`android-bin` lives on the jail's tmpfs), `/sys` is left empty, and a jail root may not contain whitespace (mountinfo escapes it).
- **G5-15:** Tests that run a jail, and the scenario runs, are marked slow, so the quick suite that gates Android builds does not run them; the pure tests stay in quick.
- **G5-16:** Before G7-launch, `child_exit` means: wait for Nano to exit after the launch, then restart it in the same jail.
- **G5-17:** A shell command no rule answers fails the scenario by default, as D.3 says; `"unexpected": "allow"` opts out.
- **G5-18:** `check-headless-0` is a light Makefile target; the guard treats it as `check-unit-0`.

### G6. Nano's CMake build, libthimsy's package

- **G6-1:** The flags come from `Android.bp` at configure time (`androidbp_flags.cmake`), not from `sources.txt`'s header. The reader handles bpfmt's layout only, and `test_nano_cmake.py` checks it against `androidbp.py`.
- **G6-2:** Only gammaos-nano's top-level cflags are used. `-Werror` is dropped unless `NANO_WERROR`. The arm64 `-mtune=cortex-a55` is not used: Buildroot sets each SoC's `-mcpu`, and the H700 is an A53.
- **G6-3:** The `[dropped-linux]` stubs and the GL stub's generated part are compiled into each executable, not into the platform library, because they are made from `nm` of the whole link. Each main is a one-file static library so that `nm` reads only archives through `$<TARGET_FILE>`. A deviation from C.2 (see the open items).
- **G6-4:** nano_core and the platform library call each other, so the executables link those two archives inside `-Wl,--start-group`/`--end-group`.
- **G6-5:** Feature switches leave `[core]` files out of nano_core without moving them in `sources.txt`; what they define gets generated stubs, as `[dropped-linux]` does. AC3 leaves out NanoAc3.cpp, PDF leaves out NanoMenuPdf.cpp, ARCHIVE leaves out NanoArchive.cpp and NanoOtaCheck.cpp (both need libziparchive), and MEDIA leaves out nothing; `NANO_WITH_MEDIA=ON` is refused. The nano recipe passes all four OFF.
- **G6-6:** `NANO_PLATFORM` takes linux, host, all or none. A requested platform whose entry points are missing from `[linux]`/`[host]` is a configure error, not a silently skipped target. `[linux]` must also list `platform/linux/display/DrmBuffersGbm.cpp`, so stubs never stand in for the GBM back end.
- **G6-7:** `platform/linux/ShellGate.cpp` becomes its own `nano_shellgate` library, which thimsy-nano-scrape links for its `--wrap` set; `NANO_BUILD_SCRAPE` therefore needs the shell gate listed in `[linux]`.
- **G6-8:** thimsy-nano's wrap list is read from G9's `platform/linux/linker-wraps.txt`, which must contain C.2's six shell symbols. The scraper uses C.2's six; nano-headless uses the six plus ioctl.
- **G6-9:** The image decoder links PNG, JPEG, WebP (with webpdemux when its header exists) and GIF when their headers exist, the same probe idea as `compat.mk`. A format whose header exists but whose library is missing is compiled out with `NANOCOMPAT_NO_<FMT>`; with `NANO_REQUIRE_IMAGE_LIBS=ON` (the recipe sets it) that case is an error instead.
- **G6-10:** nano_unit_tests is a custom target over one program per test from `unit-closure.txt`, with production files built once as object libraries. A program whose headers are missing is skipped with a message; programs that need asset folders are DISABLED when `NANO_DATA` is empty; a CTest fixture lays out the data the way the Makefile does.
- **G6-11:** libthimsy is found as a parent target, then an explicit source tree, then `find_package(thimsy CONFIG)`, then the repository's tree, the same in nano, session and padd. libthimsy installs its gtest stand-in under `include/thimsy/testing`, so it never shadows a real gtest. (The session and padd test option is now `THIMSY_BUILD_TESTS`, the name the recipes pass, which settles the G2d-2 mismatch.)
- **G6-12:** The stub generators run on the build host's python3 (3.8 or later; Buildroot requires python3 anyway) and on `CMAKE_NM`. No host-python3 dependency was added to the recipe.

### G9. The Linux platform: shell gate, control keys, GBM back end, main

- **G9-1:** Everything under `nano/platform/linux/**` is fork_only (already in `upstream.json`). It never reaches the patch or `Android.bp`, so no regen and no android-surface line are needed.
- **G9-2:** `nano/platform/linux/linker-wraps.txt` is the one list of `--wrap` symbols (system, popen, pclose, execv, execvp, execve, eglGetProcAddress, eglQueryString); CMake (G6) reads it.
- **G9-3:** The shell gate splits a shell string into its separate commands, honouring quotes. One refused command refuses the whole string. Routes are taken only when every command in it is a route joined by `;`, `&&`, `||` or newline. Nothing inside `sh -c` is routed or rewritten. (Its documented limits are in the open items.)
- **G9-4:** Gate routes: setprop and getprop use the config store; `reboot`, `reboot -p` and `svc power reboot|shutdown` set `sys.powerctl`, which thimsy-session acts on; restorecon and chcon succeed and do nothing (there is no SELinux); `/system/bin|xbin/<posix tool>` runs the Linux tool, and `toybox X` runs X; a program under /system, /vendor, /product, /odm, /apex or /system_ext passes only if the image has it (the curated /system/bin). Android tools are refused (exit 127, or no output from popen), logged once per tool.
- **G9-5:** A refused exec returns -1 with ENOENT rather than calling `_exit(127)` itself. Each of Nano's four exec sites calls `_exit(127)` right after, so they behave the same, and a caller that did not fork keeps running. (IMPLEMENTATION.md B.5 is to say so; see the open items.)
- **G9-6:** For exec, `nsenter -t 1 -m -- PROG` runs PROG (itself gated), and `gammaos-net download URL FILE` runs `curl -fsSL --retry 2 -o FILE URL`. The exec decision allocates nothing, so it is safe after fork.
- **G9-7:** Record-only host mode (`setRecorder`, with `setRoot` and `setRoutes` for tests): system, popen and exec run nothing. The recorder gives the exit status and the popen output, and an exec `_exit`s with the status.
- **G9-8:** Control-key dispositions: routed, store-only, refused, plus a new **not-reached** (nothing on Linux writes the key). Route kinds: `session:<verb>`, `session-key` (the session reads the key itself), `launcher`, `hook:<action>`, `first-frame`, `rc-reset`. A row may add `value` (the triggering value), `reset` (the rc's setprop, written before the action), or `source: manual` (a listener the patch doesn't show). `control-keys.json` has 67 rows. Extends B.2 (see the open items).
- **G9-9:** The C++ table `kControlRows` is kept by hand, and the test checks it against the JSON. There is no code generation, so the build needs no Python.
- **G9-10:** Framework listeners are Nano's `props.txt` keys found as literals in the patch's other sections (added and context lines). `do_launch` is a manual row because its watcher is GammaOS base code the patch doesn't show. Settings are covered by one glob row, `persist.*`, as store-only.
- **G9-11:** `sys.gammaos.nano.drm_active=1` (Nano's splash on the panel) is treated as the first frame: it sets `sys.thimsy.nano.first_frame=1` and `sys.boot_completed=1` once per process. Nano gates dosleep, its background scan and its idle work on boot_completed, and on Linux nothing else would ever set it.
- **G9-12:** The launch fires on `do_launch=1` or `service.bootanim.nano_retroarch=1`, debounced into one request when the handshake keys are unchanged within 3 s. It reads nine handshake keys: launch_app, launched_pkg, launch_intent, launch_core, launch_rom, launch_game, launch_game_core, launch_system, return_recent.
- **G9-13:** fixedperf=1 and 0 run the family hook with `fixedperf-on` and `fixedperf-off`, in the background via posix_spawn of `/bin/sh`. The family comes from `/run/thimsy/family`, else `ro.thimsy.family`.
- **G9-14:** Session requests are written as `<seq>:<verb>`, with seq one past the number already in `sys.thimsy.session.request`. The session ignores a repeated seq, so after a Nano restart the numbering continues instead of restarting at 1.
- **G9-15:** GBM back end: `gbm_bo_create(ABGR8888, SCANOUT|RENDERING|LINEAR)`, an EGL dma-buf import (with a modifier only when it is neither linear nor invalid), then ADDFB2 on the bo's own GEM handle; `drmGemHandle` is 0; one gbm_device per DRM fd, destroyed with its last buffer, so reusing an fd number after re-acquire is safe; `AHardwareBuffer_lock` and `_unlock` return -EINVAL; AFBC mode fails.
- **G9-16:** The EGL wraps answer only for `eglGetNativeClientBufferANDROID` (a stand-in, used only if Mesa returns null) and append the two EGL_ANDROID names, and only when EGL_EXT_image_dma_buf_import and EGL_KHR_image_base are present, so the gate fails honestly without them. Each display's extension string is kept for good.
- **G9-17:** `main_linux.cpp` keeps from main.cpp only the priority, drm_active=0, and NanoMenu's run and join, after `setLogFile`, `nanocompat::init` and `controlkeys::install()`, all before any thread. It refuses `--overlay`, and leaves out the drastic steps, the stray-instance guard, the early DRM-master grab and binder. `test_main_linux.py` fingerprints main(), so a change there forces a review. SIGPIPE stays at its default, as on Android.
- **G9-18:** Path-root dispositions: link, **same** (new: Linux has the path itself), gate, kernel, dropped, not-reached, **not-a-path** (a suffix Nano appends to a folder). Only literals under a real top-level root directory count, and test/ and compat/ files are left out. Under /storage, only `emulated` and `self` are named; any other name is a volume (`/storage/*`). /sdcard is a single root. `path-roots.json` has 33 rows.
- **G9-19:** The raw-block-device ratchet ignores sysfs `/sys/dev/block` (NanoSdMove.cpp reads it).
- **G9-20:** `10-android-links.sh` (taken over from G8, G8-13) now fails the build if the root has `/dev/block`, `/vendor` or `/mnt/shares`. The shell gate's /vendor rule and B.5's eMMC rule depend on their absence.
- **G9-21:** The B.5 ratchets live in new files, `test_path_roots.py` and `test_shell_sites.py`, not in G0's `test_nano_ratchets.py`, to avoid merge conflicts.

### gfix. Fixes from the waves 1 and 2 open items

- **gfix-1:** `regen`'s working-tree read treats an intent-to-add entry (porcelain " A") as changed and hashes the file on disk. Fixes G0-5's `git add -N` case.
- **gfix-2:** On a merge where the incoming side also has `nano/` (`upstream.json` present), the nanopatch driver regenerates the block from the merged `nano/` instead of giving up. It finds ours (HEAD), theirs (from `GITHEAD_<sha>`, `MERGE_HEAD` and friends, or the branch whose patch is the incoming blob) and the single merge base, and checks each against the driver's three versions; it three-way-merges the three `nano/` trees from git's objects, not the working tree, because merge-ort runs drivers before it writes the tree; it text-merges the patch's framework sections; it takes `upstream.json` from the side that changed it, and refuses when both sides changed it or the GammaOS bases differ. On any conflict or uncertainty (a rebase or cherry-pick, several merge bases) it keeps this side's files and exits 1, with instructions. Supersedes G0-8; G0-7's exit 1 still holds for a nano-144 merge that changes `nano/`.
- **gfix-3:** `tools/nano-tree/block.sha256` gets its own driver (`merge=nanoblock`, `nano_tree.py merge-block`) that computes the same result. The result is cached for a day under `<git-dir>/nanotree-merge/`, since git may run the two drivers in either order.
- **gfix-4:** `build.sh card DEVICE...`: the family comes from `board/<family>/devices/<id>.json`, one family per run; the hardware target is the one defconfig whose `BR2_ROOTFS_POST_SCRIPT_ARGS` has `--family=<family>`; it runs `make target-post-image` in the already-built output tree, with `THIMSY_DEVICES` exported, and does no source archives, no Buildroot download and no SDK unpack; it refuses `--publish`, `--clean`, bad ids and a tree that is not built.
- **gfix-5:** The container exports `THIMSY_BUILD_TARGET=<target>` to the post scripts, and `nano_tree.py export-assets` is now called with OUT as a positional argument.
- **gfix-6:** `/root` stays Buildroot's 0700 directory, and S00base bind-mounts `/storage/.config/thimsy/home/root` over it after /storage is up. Buildroot's `system/device_table.txt` is left untouched. This was the least risky option: it does not depend on how makedevs handles a dangling symlink. Replaces G2e-13's `/root` link.
- **gfix-7:** `fit.py --device-json` reads the schema: "id" is the device, and "family" must match; "dtb" must match `<vendor>/<name>.dtb`, and the FIT carries the file name only; a null dtb is refused (no FIT); bootargs are no longer read from the JSON (the schema has none), and `--bootargs-append` stays. Aligns G2b-9 with the schema; `25-boot-img.sh` still passes `--device` and `--dtb` (G8-9; only its comment changed).
- **gfix-8:** GPU packs are unpacked over the tier root into each card's root at assembly (G8-2). Nothing mounts them at boot; Config.in and `gpu_pack.py` now say so. Settles G2d's Config.in against G8 in G8's favour.
- **gfix-9:** `nano/LICENSE` (Apache-2.0 text) and `nano/NOTICE` are fork_only, so the patch is unchanged. NOTICE lists NanoOskKanji.h (SKK-JISYO, GPL-2.0+), liba52/ with NanoAc3.* (GPL-2.0+, only with `NANO_WITH_AC3=ON`), nanosvg/ (zlib) and rcheevos/ (MIT). thimsy-nano lists both in `_LICENSE_FILES`, and lint now errors if they are missing from it instead of reporting them pending. Fulfils I41's `nano/NOTICE`.
- **gfix-10:** thimsy-data installs Barlow Semi Condensed Medium and SemiBold as `nano_xmb/fonts/akrobat-{regular,semibold}.ttf`, plus `Barlow-OFL.txt`, exactly as `mods/50-nano.sh` does: same pairs, same commit, same sha256s, all held equal by a test. The mapping is `assets_allowlist.py`'s REPLACEMENTS with `install --replacements DIR`, which acts only when the export has the denied file. The files come from `THIMSY_DATA_EXTRA_DOWNLOADS`, copied into `$(@D)/.thimsy-barlow` by a post-extract hook; their hashes are in `package/thimsy-data/extra-downloads.hash`, which build.sh appends to the hash file it writes for thimsy-data, since Buildroot may read only that one hash file. Changes G2d-12's outcome for Akrobat: still denied, now replaced.

## Open items from wave 3

Each still-open problem from the wave 3 reports, once, with who acts: integ3 (the integration step now doing the cross-step hand-offs), a G7 step, G10, a device, or the owner. Left out as done on `linux` at `1a1ce7e`: both merge drivers are registered in the main checkout's `.git/config` (gfix); `include_census.py write` was re-run after the merges (G4, G5).

### Integration hand-offs

- `sources.txt [linux]` is empty: list `platform/linux/main_linux.cpp`, `ShellGate.cpp`, `ControlKeys.cpp`, `display/DrmBuffersGbm.cpp`, `display/EglWraps.cpp` and `NanoPlatform.cpp` (G5-3), never `platform/linux/tests/*`. Until then the default `NANO_PLATFORM=linux` and `NANO_BUILD_SCRAPE=ON` stop at configure with a message saying what is missing (G1c, G5, G6, G9). *(integ3)*
- `gEarlyDrmFd`: main.cpp defines it and `[core]`/`[drm]` use it, so `main_linux.cpp` and `main_headless.cpp` must define it as -1, or stubgen generates it as 0, which is stdin's fd; both define `int gEarlyDrmFd = -1;` at `1a1ce7e`, to be held there (G4, G5, G9). *(integ3)*
- The `android/hardware_buffer.h` stub must declare, not define, the opaque `AHardwareBuffer`, `AHardwareBuffer_Desc`, the format and usage constants, `ARect`, and the four functions as `extern "C"` with NDK signatures, while `DrmBuffersGbm.cpp` defines `struct AHardwareBuffer` and the four functions; the stub has `AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM` and `_USAGE_CPU_READ_OFTEN` for it; check the two agree without raising the stub budget (G4, G5, G9). *(integ3)*
- `IPCThreadState::stopProcess` must do nothing, since `main_linux.cpp` relies on `join()` returning (the stub's only logs once) (G9). *(integ3)*
- Define `nanoRgbFollowSample()` on Linux (DrmHost's no-op is host-only, G5-7); the symbol-set test should cover `DrmHost.cpp` too (G5). *(integ3)*
- CMake for Linux: `main_linux.cpp` in place of main.cpp, `DrmBuffersGbm.cpp` and `EglWraps.cpp` in place of NanoMenuDrmBuffers.cpp, plus `ShellGate.cpp` and `ControlKeys.cpp`; link gbm, EGL and GLESv2 and the wraps read from `linker-wraps.txt`, no LTO; `NANOSTUBS_CPPFLAGS` (with `-DEGL_NO_PLATFORM_SPECIFIC_TYPES`) on nano_core; libnanostubs linked into nano_platform_linux and nano_platform_host; the stubgen step (nm over the objects, then `gen.py --from <each [dropped-linux] file> --decls nano --include-root nano`, then compile the output, as the host Makefile's headless rules do); the GL stub's `gen.py` called as `--objects <archives> --defined-objects <glue archive>` (G4, G5, G9). *(integ3)*
- `tests/run.sh cxx` runs neither `check-stubs` nor `check-headless-0` (G4, G5). *(integ3)*
- Nothing reads `/usr/share/thimsy/gpu.json`'s "env" (swrast: `GALLIUM_DRIVER=softpipe`); thimsy-session should set it before starting Nano (gfix). *(integ3)*
- thimsy-session must not also set `sys.boot_completed` (ControlKeys does, G9-11); the control-key table assumes the session watches `sys.gammaos.nano.scrape_run` and `sys.powerctl` itself, as `session.md` says (G9). *(integ3)*
- The family hooks have no `fixedperf-on` and `fixedperf-off` actions (`usr/share/thimsy/family/<family>/hook`); rk3566's ignores unknown actions, so fixedperf does nothing (G9). *(integ3)*
- On Windows, `tests/util.slurp` fails with UnicodeDecodeError unless Python runs with `-X utf8` (cp1252); `test_nano_units` hits it before and after G1c (G1c). *(integ3)*
- ASSETS.md's Akrobat rows (lines 85 and 151) still say KEEP; they should say REPLACE, with the Barlow note (gfix). *(integ3)*
- IMPLEMENTATION.md is not updated: B.2 (the `not-reached` disposition and the route kinds), B.5 (exec refusal returns ENOENT, the path-root dispositions `same` and `not-a-path`, the test file names), B.7 (the main_linux step table) (G9); C.1's `[dropped-linux]` wording to follow G4-5 (G4); C.2's deviations: stubs in the executable, the link group, nano_shellgate, nano_unit_tests as a custom target, `NANO_WITH_ARCHIVE`, the `NANO_PLATFORM` values (G6). *(integ3)*

### Seam moves

- Supply `controlkeys::Handlers.launch`; until then the default only logs, and Nano exits after a launch with no game started (headless `child_exit` follows G5-16 until then) (G9, G5). *(G7-launch)*
- Move `NanoPlatform.cpp` into gammaos-nano and `[core]` (G5-3). *(G7-main)*
- On Linux the overlay mode and `nanoForceSfPath()` paths poll `checkService("SurfaceFlinger")` forever against the inert stubs; DRM must stay the path taken there (G4). *(G7-\*)*

### Needs headers, packages, Buildroot or CMake

- Compile `DrmBuffersGbm.cpp`, `EglWraps.cpp` (against the real headers), `main_linux.cpp`, `DrmHost.cpp` and `main_headless.cpp`, at least `-fsyntax-only` (G5, G9). *(G10)*
- Install DEPS.md's packages and run check-unit, syntax-core and headless; nothing on Ubuntu provides `ziparchive/zip_archive.h` (DEPS.md's TODO: `android-libziparchive-dev` or a small reader); check `unit_ldlibs` (`-lz -lcrypto -ltinyxml2 -lziparchive`) and the never-linked `GTEST=system` path. Eleven unit programs wait for headers: AppFiles, BiosCatalog, BiosUtil, ButtonMapNano, ButtonMapReaders, ButtonMap, HakuxSetup and RaConfig (`openssl/md5.h`, `openssl/sha.h`, `zlib.h`; the three ButtonMap programs also need the base's ps3xmb), NdsBanner (`zlib.h`), EsdeSystemName (`ziparchive/zip_archive.h`), EsdeTheme (`tinyxml2.h`, `ziparchive/zip_archive.h`) (G1c). *(G10)*
- Build `nano-headless` and run the scenarios: the key sequences in `launch_game` and `setting_persisted` are first guesses marked `TODO(G10)`, `navigate` has no focus assertion yet, shell rules must be filled in from the first run (unanswered commands fail the scenario), and `THIMSY_NANO_ASSETS` (`nano_tree.py export-assets`'s output) and `THIMSY_NANO_FONTS` must be supplied; the link will list any other `[firmware]` symbols `[core]` files need (G5). *(G10)*
- Check that Ubuntu 20.04's and Buildroot's `eglplatform.h` honour `EGL_NO_PLATFORM_SPECIFIC_TYPES` (libglvnd 1.3.1 may not; without it the SurfaceFlinger calls need another fix); the stubs for NanoMenu.cpp, PS3Clock, Render, Overlay and Video were written from the call sites and never compiled against them, so syntax-core will show gaps, to be fixed in `stubs/` with the budget updated and a reason; run headless's stubgen step for real (G4). *(G10)*
- Run cmake, at least `-DNANO_PLATFORM=none -DNANO_BUILD_TESTS=ON` on the host, then linux in Buildroot; check `check_include_files(... LANGUAGE CXX)`, the pkg-config names (egl, glesv2, gbm, libdrm, tinyxml2, libwebp, libwebpdemux), the `--start-group` items and the stub generators against real archives. The host Makefile's headless and syntax-core targets still compile NanoArchive.cpp, NanoOtaCheck.cpp and NanoMenuPdf.cpp (libziparchive, pdfium), which the CMake build leaves out by feature switch (G6). *(G10)*
- Buildroot behaviours gfix could not run: full-URL `EXTRA_DOWNLOADS` in a package whose `_SITE_METHOD` is file; that the global-patch-dir hash file with the extra lines is the one Buildroot checks; that `make target-post-image` re-runs cleanly in a built hw tree (it also re-runs target-finalize and the rootfs, a guess); the real `build.sh card` run; makedevs with the new `/root` (gfix-6 avoids the link, not confirmed) (gfix). *(G10)*
- Check that Nano really writes drm_active=1 at its splash (main.cpp's comment says "1 at splash"), which G9-11 relies on (G9). *(G10)*

### On a device

- On panfrost: a linear ABGR8888 scanout bo, the dma-buf import, ADDFB2, the EGL gate passing through the wraps, and `EGL_ANDROID_native_fence_sync` (G9; extends G9a's item) (G9). *(G10, device)*
- The `/root` bind mount from `/storage/.config/thimsy/home/root` (gfix). *(device)*
- The default family-hook runner (posix_spawn of `sh hook ACTION`) and the default launcher log ran only as untested defaults; the tests replace both handlers (G9). *(device)*

### Owner's choices

- ~~Linux's default ES-DE theme: Slate, filtered Slate, Colorful or none (gfix).~~ Decided: OA-1.
- The shell gate's documented limits: `$(...)` inside double quotes is not examined; a `sh -c` string is not rewritten, so `/system/bin/rm` inside `sh -c` would fail on Linux; `&&` and `||` are not told apart in route strings; when the gate refuses an exec, the forked child logs with a raw `write(2)` to stderr, not through the compat logger (G9). *(owner: accept, or ask a G7 step to narrow them)*
- `drmSetupZeroCopy`'s pre-existing leak is more visible with GBM: a ring that fails part-way keeps its earlier framebuffers until the fd closes; release does not RMFB itself, because DRM reuses fb ids at once (G9; the waves 1 and 2 item on the Android fix stands). *(owner)*
- Any nano-144 merge that adds a unit test or changes includes fails `test_nano_sources` until `sources.txt` is edited and `include_census.py write` is run; fork-only, so it never gates the Android build (G1c). *(owner, at each nano-144 merge)*
- `check-stubs` is not in `build_guard.sh`'s SMALL_TARGETS; it runs under the check-unit-0 guard class (the Makefile's GUARD_GOAL default); add it if wanted (G4). *(owner)*

### Notes, no action needed

- The fork-to-fork driver may disagree with git's own `nano/` merge in rare cases, since git's merge does rename detection and ours does not; then ours conflicts (safe), or `regen --check` and `test_nano_patch_consistent` catch it after the merge (gfix).
- Run `regen` again if a merge touches `nano/` outside the fork-only paths (G5); `regen --check` needs `work/nano-base` from `fetch --from-cache`, and `work/nano-base` and `work/nano-composed` are gitignored (G1c, G9).
- Every Makefile relinks its unit programs on each run, because `compat.mk`'s libthimsy targets are phony; the cost is small (G1c).
- In the Git Bash used by the agents, heredocs written through the Bash tool turned `\\` into `\` (and `\b` into a backspace) and dropped the backslash in `\\[`, `\\.` and `\\{`; write scripts, CMake and regex files with Write or Edit; `test_nano_cmake.py` guards the CMake files against this (G4, G6).

## Integration and review after wave 3

Step `integ3` joined wave 3's pieces (merge `ab0bf62`). A review of the whole fork then ran in six dimensions (Android build safety, device and data safety, C++ correctness, security, build integration, test quality). Each finding was checked by a second agent told to refute it: 23 were confirmed and 4 rejected. Of the 23, 21 were fixed with tests and 2 are left for the owner (below and in the Deferred table).

### Integration (INT)

- **INT-1:** [linux] is every platform/linux/**.cpp except tests/, plus NanoPlatform.cpp; test_nano_sources holds both platform sections equal to their folders.
- **INT-2:** Whatever [core] needs a real value for is defined by the platform, never by stubgen: gEarlyDrmFd=-1 in each main, nanoRgbFollowSample() as a no-op in DrmHost.cpp and DrmBuffersGbm.cpp.
- **INT-3:** The GBM back end keeps its own NDK usage-bit constants instead of growing the stub header, whose budget only shrinks.
- **INT-4:** CMake refuses a Linux configure whose [linux] lacks EglWraps.cpp, because linker-wraps.txt wraps the two EGL symbols it defines.
- **INT-5:** tests/run.sh cxx also runs the Linux platform's platform.mk suite, under check-compat's guard class; stubs and headless-0 run under check-unit-0's.
- **INT-6:** The session applies gpu.json's "env" only to Nano (games inherit it), reads it once at start, and skips names that are not shell variable names or values that are not strings.
- **INT-7:** The family property is ro.thimsy.family, written by thimsy-prop from device.json "family"; ro.thimsy.device.family stays as the generic mapping.
- **INT-8:** ro.thimsy.modules comes from a new optional device.json "modules" array (pattern [A-Za-z0-9_-]+, unique), space-joined by thimsy-prop.
- **INT-9:** install.json lives at /storage/thimsy/install.json. S01props passes all three paths to thimsy-prop explicitly, and thimsy-prop's defaults match them.
- **INT-10:** device.schema.json gains product (manufacturer, model, brand, device, name, platform) and props (ro.* keys with scalar values), the fields thimsy-prop reads.
- **INT-11:** The dev overlay holds only etc/motd. The serial getty stays Buildroot's BR2_TARGET_GENERIC_GETTY written by 30-permissions.sh; adbd's USB gadget set-up is deferred.
- **INT-12:** S02padd stop signals only a process whose /proc/<pid>/comm is thimsy-padd.
- **INT-13:** fixedperf-on/-off use the same governors as boot/deferred; the hook skips bad files and returns 0.
- **INT-14:** The ASSETS.md Akrobat rows are changed in place (same line count), so the assets.list line references stay stable.

### Review fixes (REV)

- **REV-1:** A control-key row for an exact value overrides that key's catch-all row (ControlKeys::onWrite). nano_action reboot and shutdown go through session requests, not sys.powerctl.
- **REV-2:** nanocompat's log ring owns stdout and stderr when they are the same file: no stderr copy, fstat-based size, dup2 on rotation.
- **REV-3:** The thimsy-data source tarball is the allowlist-filtered export plus `.thimsy-left-out.txt` (names only).
- **REV-4:** `--publish` implies `--clean` and empties images/<target>/. Every build writes images/thimsy-build.stamp. A hardware `--publish` requires a published, clean tier stamp at HEAD.
- **REV-5:** Local-site packages in this tree are versioned by THIMSY_GIT_SHA, enforced by lint_packages.py.
- **REV-6:** The C++ test runs hold /tmp/thimsy-cxx.lock for the whole run and use per-checkout copies and build trees keyed by cksum of the checkout path.
- **REV-7:** The scenario runner fails on any nano-headless exit other than 0 and on any sanitizer marker in its log.
- **REV-8:** A fork-only path changed by an incoming patch is a merge conflict, and regen refuses fork-only sections. The nano-144 merge driver refuses a dirty nano/.
- **REV-9:** The surface test counts a block difference for any path that is not a file of nano/ at HEAD.
- **REV-10:** The kernel command line carries `gpt`. D60 ignores TERM while it holds its lock, and rcK waits up to 10 s for that lock.
- **REV-11:** A respawned session stops orphaned fsck checks through pid files before coldplug.
- **REV-12:** eMMC nodes that appear after Nano started cause a single Nano restart with a new mask (no setns into Nano's namespace).

### Review findings left open

- **device-safety-2 (re-writing the image over a used card):** every image ends at 2408 MiB, and D60 recreates entry 5 at the same start, so writing the image again over a card that already holds a Thimsy games library makes D60 format that library on the next boot. Only the first 33 sectors are overwritten by the image's backup GPT, so the data was still recoverable until the format. Decided autonomously, being implemented after this review: the image ends with a gap, so the old exFAT boot region survives a re-write, and D60 re-adopts an exFAT labelled THIMSY that it finds at the games start (it recreates the entry over its extent and retypes it basic data without formatting). Anything else is formatted as at a first boot, so second-hand cards still work. For owner audit. *(wave 4: implemented by gfix2 as GF2-1 to GF2-4; the review widened the adoption rule to any complete exFAT whatever its label, REV4-4.)*
- **device-safety-3 (Android's 'Unsupported SD card' prompt before first boot):** with no vold-visible entry, Android offers to set up (format) a freshly written card. That would erase the Thimsy install, but not user data. I39 and R5's rationale is corrected in IMPLEMENTATION.md E.6, and the installer must tell users not to set the card up. A small formatted basic-data entry in the image would stop the prompt, but it changes the layout and gpt_check's vold rule. The owner decides (Deferred table).

## Wave 4: the G7 seam moves, gfix2, review

Wave 4 moved Android code behind the platform seam one area at a time. Each step's commit on the Android surface is a verbatim move checked with `git diff --color-moved=zebra --color-moved-ws=allow-indentation-change` (H.3) and carries an `android-surface:` line and "Flagged for cherry-pick to nano-144 after the AOSP job." Each step had an adversarial verifier and up to three repair rounds; all eight ended clean. step/g7-merge joined them with gfix2 (union resolutions, no fix commits), every suite including `tests/run.sh cxx` passed, then an Android- and Linux-focused review ran: 5 findings confirmed, 2 rejected, all 5 fixed (REV4). Merged into linux on top of `12173e1`. None of the firmware code has been compiled with the Android toolchain.

Cherry-pick order to nano-144 (after the AOSP job, gated by the AOSP build and the owner's device smoke test, H.7): G9a's `6d6314c`, then G5's NanoPlatform.h/.cpp (`004f1a1`, which G7-main's `6675c1a` depends on but which never had an H.3 listing), then each G7 step's Android-surface commit in merge order (main, display, launch-a, launch-b, audio, volumes, power, input), then G7-net's `b823c2f` (wave 5; it needs G7-power's runCmd linkage).

### G7-main. Boot sequence and platform install

- **G7-main-1:** One shared `nanoBootSequence(void (*platformSteps)())`; the firmware-only steps between drm_active=0 and new NanoMenu run through the callback (`firmwareBootSteps()` in main.cpp), so the order and thread are exact. `sp<ProcessState> proc` now leaves scope when the callback returns; libbinder keeps the singleton.
- **G7-main-2:** `install(createPlatform())` is main()'s first statement, before the overlay branch.
- **G7-main-3:** include_census.py gains SPLIT_PREFIXES: every gammaos-nano src under platform/firmware/ goes to [firmware] and is kept out of [dropped-linux], so a missing createPlatform fails the link instead of being stubbed.
- **G7-main-4:** The Linux platform is `platform/linux/LinuxPlatform.cpp`; main_linux.cpp keeps its own set-up and installs after it (B.7).
- **G7-main-5:** test_android_surface: a path changed while off the surface is covered by the annotated commit that brings it onto the surface (surface.py `surface_joins`, `Repo.surface_at`).
- **G7-main-6:** test_main_linux's fingerprint covers main.cpp from firmwareBootSteps() to the end, plus order checks for main.cpp and nanoBootSequence().
- **G7-main-7:** run()'s status is not checked on Linux (main.cpp ignores it too). Superseded for exit status by REV4-2.

### G7-display. Display

- **G7D-1:** Display owns readyToRun's DRM splash decision, the hand-off (releaseForChild) and reacquire. Display.cpp stays EGL-free.
- **G7D-2, G7D-3:** The headless pbuffer EGL set-up, the SurfaceFlinger window surface and NanoMenuRender.cpp's present are not moved (NanoMenu.h is frozen). Deferred to the owner (Deferred table).
- **G7D-11:** The Linux rotation order follows device.json's panel.rotation_source; the default is DRM property, then device.json, then fbcon, and an explicit `ro.surface_flinger.primary_display_orientation` still wins.
- **G7D-12:** The GPU finish before drmStop goes through a Kms hook (`finishGpu`), in NanoMenuDrasticPark.cpp's order.

### G7-launch-a and -b. The launch handshake

- **G7-launch-a:** LaunchTuple is an ordered builder (sites disagree on key order). A tuple holds only B.2's handshake writes (plus launch_rom); anything a site does between two writes stays at the site between two commits, so every property write stays in place (69 commit calls). setLaunchRomPath and setLaunchGame are tuple steps. commitLaunch writes with property_set everywhere (one `__system_property_set` on Bionic either way). drop_input is gathered only where it is the launch's drop_input=1; app_launched is not gathered. The test lives in check-headless-0, because Android.bp globs test/unit/*_test.cpp.
- **G7-launch-b-4 (superseded by REV4-1):** Linux's launch() queues the request and main_linux starts it after the menu thread ends. `nanoExitsOnLaunch()` stays false as the documented contract; nothing reads it yet.
- **G7-launch-b-11:** The launcher prefers the full value it wrote for each key while the store still holds what it held right after the write; otherwise the store wins.
- **G7-launch-b-12:** At a launch's end the launcher clears launch_app, launch_intent, launch_core and launch_rom (mirroring nanoStopAppToHome).
- **G7-launch-b-13:** launch_app "" in a package launch is RetroArch.
- **G7-launch-b-14:** Only a trigger written inside the launcher's own commit() is a launch; a trigger from another process is logged and refused while Nano runs.
- **G7-launch-b-15:** The control keys are installed before the platform; the launch handler is set after install() through `controlkeys::setLaunchHandler`.

### G7-audio

- **G7-audio-1:** Audio is a stream factory (`open()` returns an AudioStream), since NanoAudio keeps several streams open at once.
- **G7-audio-2:** No volume members: the player's gain stays in core, and the system volume (`cmd media_session volume` in NanoControlCenter.cpp) is not this step's. No Linux mixer code yet.
- **G7-audio-3:** Fill returns int, kFillContinue=0 and kFillStop=1, static_asserted equal to AAudio's values.
- **G7-audio-4/5:** AudioFormat::name carries the log prefix; the two AAudio builder copies are merged into one moved copy, the SFX's literals becoming AudioFormat fields.
- **G7-audio-7:** Linux audio is a testable PullAudio core (AudioPull.cpp) plus the ALSA sink (AudioAlsa.cpp, not compiled here: needs alsa-lib headers).
- **G7-audio-8:** Linux and host pause() return only once no Fill is running (stricter than AAudio's requestPause).
- **G7-audio-9:** The Android half and the Linux/host half are separate commits, so the Android one can be cherry-picked alone.
- The AAudio user-pointer lifetime is an AOSP review point (Deferred table).

### G7-volumes

- **G7V-1:** Volumes has cardRoot, isSlot, absent, present and writableFolders, moved from NanoSdMove.cpp's sdCardRoot/sdIsSlot/sdVolumeAbsent and esdeStoreFollowCard's /storage scan. NanoSdMove.cpp keeps one-line forwarders where a function pointer or another caller needs the old name.
- **G7V-2:** Linux Volumes reads volumes.json written by thimsy-session; the host fake takes a scripted list from host.json.
- The firmware helpers sit in `nanoplat::firmware::` here and `nanofirmware::` in the other steps; harmless, could be made uniform later.

### G7-power. Power and system actions

- **G7-power-1:** Typed methods named after the call sites (readBattery, setBrightnessViaHal, mirrorBrightness, savedBrightness, powerAction; six SystemActions methods). B.6's Action enum, run(), supported(), sleep() and setBusy() are not declared (rule I27: declare when used).
- **G7-power-2:** `runCmd` gets external linkage (the anonymous namespace in NanoMenuSettings.cpp is closed and reopened around it) so SystemActions.cpp can call it.
- **G7-power-3 to -6:** Not moved: NanoControlCenter.cpp (already sysfs; its svc/settings calls are built into shared strings), NanoMenuNet.cpp's poll, sleep (already a session request) and finishSetupWizard's provisioning block (calls a private member).
- **G7-power-8:** Linux brightness: setBrightnessViaHal returns false (NanoBacklight.h writes sysfs), mirrorBrightness does nothing, savedBrightness reads `persist.gammaos.nano.brightness`. Ends the 2 s brightness re-check loop.
- **G7-power-9:** Linux powerAction is a thimsy-session request (reboot, poweroff); recovery, safemode and android are logged and refused.
- **G7-power-10:** A small MIT-licensed wpa_supplicant control-socket client (`actions/WpaCtrl`) instead of vendoring hostap's wpa_ctrl.
- **G7-power-11/12:** Linux Wi-Fi on/off writes `persist.thimsy.wifi.enabled` and runs `D30net start|stop`; forget uses REMOVE_NETWORK and SAVE_CONFIG; Bluetooth uses bluetoothctl. Slow actions run in order on one worker thread.
- **G7-power-13:** The Linux time zone rewrites the `/storage/.config/thimsy/localtime` link atomically, writes `persist.sys.timezone`, then setenv("TZ") and tzset(). The review's thread-safety finding about setenv was refuted for glibc (TZ is the only variable Nano sets, and glibc does not free the startup environment).

### G7-input

- **G7-input-1:** InputPolicy is per device (deviceOpened/deviceClosing with a site tag) plus silenceGamePad, because the firmware grabs per fd at open.
- **G7-input-2/3:** isBuiltInPad and deviceMatchKeys are not declared (I27). The firmware's silenceGamePad does nothing (it silences at open; its home exits on launch).
- **G7-input-4:** Linux never EVIOCGRABs padd's pad, whatever grab_input says.
- **G7-input-5/6:** Linux connects to padd's tap before setting intercept "1" (no tap, no intercept: a game keeps the pad rather than a menu without input), clears with "0", and clears a stale "1" at start.
- **G7-input-7:** The host fake keeps the firmware's grab behaviour, so the boot scenario's expectations are unchanged.
- **G7-input-8:** The tap reader lives in the Linux class only until the menu-input follow-up wires it into pollInput.

### gfix2. Card re-write safety, root's home, SSH options, licences, regen warning

- **GF2-1:** Every image ends 1 MiB after data (`size = 2409M` in genimage.cfg), and D60 puts games 2 MiB after data (2410 MiB), so the backup GPT of a re-written image ends 1 MiB before the old library. gpt_check requires exactly that tail on images and at least that gap on cards.
- **GF2-2 (superseded by REV4-4):** re-adoption required the label THIMSY and 64 MiB.
- **GF2-3:** The re-adopted entry is created typed basic data in one sfdisk write (never reserved first, which a power cut could leave to the format path).
- **GF2-4:** The extent is the exFAT VolumeLength, clamped to the last usable LBA with a warning.
- **GF2-5:** Test fakes: the disk's bytes are a sparse dev/<disk>.img; D60 reads through a DD seam.
- **GF2-6:** Root's home stays on the ext4 data partition, never the exFAT; it is bound only on dev builds or release builds with SSH on; links are refused and 0:0, 0700 and 0600 are set at every boot. This closes gfix-6's root .ssh on the user-writable card.
- **GF2-7:** The dead SSH_AT_BOOT and SSH_PASSWORD options are removed; the lint refuses them.
- **GF2-8:** The thimsy-data source tarball carries ATTRIBUTION.md and NOTICE under .thimsy-licences/.
- **GF2-9:** regen warns on stderr (without failing, `--check` too) when its block includes uncommitted nano/ files, excluding fork-only paths.

### G7-merge

- struct Platform keeps G7-volumes' comment; linuxplatform_test's nano sources stay `NanoPlatform.cpp NanoLaunchCommit.cpp`; each step's Makefile and platform.mk additions stay as their own `+=` blocks.

### Review fixes (REV4)

- **REV4-1:** Linux Nano exits after every launch and leaves the game to the session, the way Android's home exits (`leaveQueued()`, `LinuxLauncher::startAndLeave()`). This fixes the render watchdog and detached workers reading a freed NanoMenu for the whole game (linux-host-impls-1). The resident path stays in the code for later.
- **REV4-2:** Nano's exit status tells the session how the menu ended: 0 after a launch (started or not), 1 when the menu ended without one. The session restarts Nano at once with no back-off for 0 with no game running, and game run time no longer counts as Nano's uptime (linux-host-impls-6).
- **REV4-3:** The launcher writes sys.thimsy.game.pid when the child has started and clears it when it reaps it; the session also clears it when the game it watched ends, unless the key changed (linux-host-impls-2: quit, sleep's SIGSTOP and poweroff's TERM need it).
- **REV4-4:** D60 re-adopts any complete exFAT at the games start (boot sector, allocation bitmap and up-case entries in the first root cluster), whatever its label or size (under 64 MiB with a warning). A read error creates nothing and retries next boot. Only a region with no exFAT, or one whose format was cut before the root directory, is formatted (linux-host-impls-4). This follows the owner's "always reversible and non-destructive" principle; the cost is that a second-hand card's old library is kept, not formatted, and the new owner formats it from a PC. For owner audit.
- **REV4-5:** The image ships /etc/asound.conf: default is plug over a shared dmix on card ALSA_CARD, which the session sets from device.json audio.card_index (linux-host-impls-3).
- Rejected: Quick Resume holding the preview on Linux (unreachable: LibretroRunner is firmware-only until Phase 5) and setenv("TZ") races (see G7-power-13).

### Integration fix

- The wave 3 conflict resolution of nano/test/host/Makefile had left a literal `\n` in the nano-headless link rule, which would have broken `make headless` in G10. Fixed on linux (fork-only file).

## Open items from wave 4

### Needs the Android toolchain or the AOSP review
- None of the G7 firmware code is compiled. G10 or the nano-144 cherry-pick must build gammaos-nano with `-Wall -Werror` and run the owner's device smoke test.
- Power.cpp includes NanoMenu.h for nanoThreadNormalPriority; no header-level dynamic initialiser was found, unconfirmed without a build.
- The AAudio user-pointer lifetime (Deferred table).

### Needs headers, packages or G10
- AudioAlsa.cpp against real alsa-lib headers; main_linux.cpp (includes NanoMenu.h) is compiled by no suite here.
- The headless scenarios with the merged platform (`make headless`, now that its rule is fixed).
- The Mesa pbuffer path may need a GBM or surfaceless EGL display (G7D-2).

### Linux behaviour still to do
- Nothing on Linux sets sys.gammaos.nano.app_launched or app_drawn, so threadLoop's exit path draws "Loading..." for up to about 10 s before the game starts.
- Linux Wi-Fi and Bluetooth screens (status, lists, scan, connect, pair, Net's poll) still parse Android output the shell gate refuses; they need their own step. BlueZ is not in the image.
- The PS3 menu's own time, zone, radio and auto-time commands are not moved (G7-caps); the shell gate routes its timezone setprop to the store, but the localtime link is not rewritten.
- The input tap is not wired into pollInput (an interface member such as menuInputFd()); no site calls silenceGamePad yet; NanoMenu.cpp's exit-time EVIOCGRAB of every fd and its destructor release are still direct ioctls; padd does not grab external pads during intercept.
- The dead `service.bootanim.nano_action=reboot|shutdown` control-key rows are kept and noted.
- The drastic launch sites `_exit(0)` after their trigger; a future Linux block for package launches must handle it.
- A launch that fails at Nano startup on every run would restart Nano with no back-off (not reachable today: no auto-launch at boot).
- Linux pause and kFillStop drop ALSA's queued frames (snd_pcm_drop); destroying a stream can block on a stalled ALSA device while the player's mutex is held.
- IMPLEMENTATION.md is stale in B.2 (control keys, nano_action), B.6 (the interface sketches, LaunchRequest, nanoExitsOnLaunch "Linux: false", the vendored wpa_ctrl), B.7 (main_linux's step table), D.3 and I28; main_headless.cpp still mirrors main.cpp by hand and cites old line numbers.

### On a device
- Rotation direction (sDrmRotationDeg against the device tree's rotation) and touch rotation.
- The dmix settings (48 kHz, period 512, buffer 2048) and alsa-lib's `@func getenv` card in a plain pcm; D20sound waits for card0 while both ARC device.json files say card_index 1.
- genimage honouring the image-level size with the backup GPT at its end; busybox dd and od on the target; dropbear key login as root.

## Wave 5: G7-net, Linux launch and session fixes, Bluetooth, docs sync

Two steps ran in parallel, each adversarially verified and repaired until clean: G7-net (Android surface: the Wi-Fi and Bluetooth screens' commands and the net poll behind `nanoplat::Network`) and linux-ux (fork-only). step/w5-merge joined them without conflicts and synced IMPLEMENTATION.md (B.2, B.6, B.7, D.3, I28), linux/docs/session.md and init.md with waves 4 and 5. A Linux-focused and an Android-focused review followed: 7 findings confirmed (all Linux, all fixed, REV5), 2 rejected; the Android dimension found nothing. Every suite passed, including `tests/run.sh cxx` (run only while thimsy-aosp was not running).

### G7-net. Wi-Fi and Bluetooth behind the seam

- **G7-net-1:** A new `nanoplat::Network` interface (25 methods named after their call sites) rather than growing SystemActions: its calls return data; SystemActions keeps its fire-and-forget actions. The wizard's persisting BT radio toggle (setBtRadio) and startWifiScan sit in Network beside the queries that read their results.
- **G7-net-3/4:** Plain entries `nanoplat::WifiNetEntry` and `BtDevEntry` with NanoMenu's fields; the moved parsers change only their type name and Settings copies into NanoMenu's types. WifiLevel/BtLevel reuse NanoMenu's names and values, static_asserted at the call site.
- **G7-net-5:** The NanoNetBridge paths stay at the call sites (they write NanoMenu members and are gated on bridge-only keys, inert on Linux).
- **G7-net-6:** runCmdUnlocked and shellQuote get external linkage (the G7-power-2 precedent).
- **G7-net-7:** connectWithWizardSettings (`gammaos-net wifi configure`) is not moved: it is built from a dozen private wizard members. On Linux, static IP, proxy and MTU have no path yet.
- **G7-net-8 to -11 (Linux Wi-Fi):** wrong password = the network shows [TEMP-DISABLED]; "validated" = an address, a default route and a nameserver, no probe; a WPA3 join sets `key_mgmt SAE WPA-PSK`, ieee80211w 1, sae_password and psk for transition networks; SELECT_NETWORK is followed by ENABLE_NETWORK all, and only a join runs SAVE_CONFIG. EAP networks are shown as WPA2-personal.
- **G7-net-12, -13 (Linux Bluetooth):** Unknown when bluetoothctl or bluetoothd is missing (30 s back-off); pairing uses `--agent NoInputNoOutput`, then info, trust and connect. PIN prompts and inbound pairing are logged and not supported.
- **G7-net-14:** Linux autoEnableWifi does nothing; D30net restores the radio from persist.thimsy.wifi.enabled.
- **G7-net-16:** The host fake's network state is separate from FakeActions'.
- Still Android-only: NanoControlCenter's wifi_on tile, NanoBtStable's A2DP dumpsys, the PS3 menu's discoverable and radio rows (G7-caps).

### linux-ux. Launch delay, launch loops, sound card, Bluetooth userspace

- **linux-ux-1/2:** The Linux launcher sets `sys.gammaos.nano.app_launched=1` for every launch Nano writes (including refused ones, since the menu ends either way), so threadLoop's exit path no longer waits about 10 s. Nano never clears it; thimsy-session writes 0 to app_launched and app_drawn when the watched game ends and before every Nano spawn with no game running.
- **linux-ux-3:** Quick Resume's app_drawn holds are left alone (unreachable while LibretroRunner and DrasticRunner are stubs on Linux).
- **linux-ux-4:** releaseForChild on a panel the menu already stopped logs at info, not as a warning.
- **linux-ux-5 (refined by REV5-f):** A launch-loop guard in the Supervisor: three quick cycles in a row are free, then the crash back-off; a crash leaves the count alone; a long cycle or restart-nano resets it.
- **linux-ux-6/7:** thimsy-prop writes device.json's audio scalars (`ro.thimsy.device.audio.*`); D20sound waits for `card<audio.card_index>` and warns, not fails, on an id or index mismatch.
- **linux-ux-8:** The ARCs keep audio.card_index 1 (rgxx3.dtsi enables hdmi_sound and ROCKNIX uses ALSA_PRIMARY_CARD=1); the rk3566 README's "one card (card0)" was wrong. /proc/asound/cards is a Phase 0 check.
- **linux-ux-9:** device.json gains a top-level boolean `bluetooth`: ARC-D true, ARC-S false (both from Anbernic's spec, unverified).
- **linux-ux-10:** BlueZ (bluez5_utils with bluetoothctl and bluetoothd) and D-Bus go in userland.fragment, so every tier carries them; a test keeps them only while some device.json says Bluetooth exists. Root size growth is not measured.
- **linux-ux-11, -12, -17, -18:** D35bluetooth (deferred, after Nano's first frame) starts D-Bus and bluetoothd only when the device has Bluetooth and `persist.thimsy.bluetooth.enabled=1` (default off, so no D-Bus at boot, as the plan says). It waits for the bus socket before bluetoothd and stops bluetoothd before the bus. Exit 3 means "no Bluetooth here" and Nano turns the switch back off; `bluetoothctl power on` is retried 5 times.
- **linux-ux-13:** bluetoothd's state at /var/lib/bluetooth links to /storage/.config/thimsy/bluetooth (new post-build part 15-state-links.sh).
- **linux-ux-14:** `--all-root` stays; the dbus recipe's user only disables the unused activation helper.
- **linux-ux-15:** Nano's runProgram (used for D30net and D35bluetooth) forks with PATH resolved first, setsid, reset signals and mask, closes fds 3 and up, and calls the raw SYS_execve, which bypasses the shell gate's execve wrap (these are Thimsy's own fixed commands). This fixes an evdev-grab leak into the daemons and process-group kills. For owner audit, since it is a deliberate hole in the gate.
- **linux-ux-16:** Daemons started from Nano stay in NanoJail's mount namespace (it masks only the eMMC).
- **linux-ux-19, -20:** ALSA_CARD stays the index until Phase 0; the kernel side of Bluetooth (BT, HCIUART with SERDEV and RTL, BT_RTL, HIDP, UHID, hci_uart in modules, rtl_bt firmware, the DT node) is a Phase 0 (b) check. Until then the ARC-D's D35bluetooth exits 3.

### Docs sync

- IMPLEMENTATION.md B.6 now shows what was declared (not the first sketch) and lists the sketch's members never declared (I27); B.2 describes commitLaunch and the own-commit rule; B.7 describes nanoBootSequence, main_linux's order and exit statuses 0, 1 and 2; D.3 lists the scenarios (none run yet, TODO(G10)); I28 is done. nano_action reboot/shutdown rows are kept for writes from outside Nano; Nano's own power path on Linux is Power::powerAction's session request.

### Review fixes (REV5)

- **REV5-a:** Linux setBtRadio (the PS3 Bluetooth wizard) is the whole switch through a shared `actions::switchBluetooth` (key, D35bluetooth start or stop, power), run synchronously, and it resets the bluetoothctl back-off (w5-linux-1).
- **REV5-b:** Bluetooth reads Off without forking bluetoothctl unless the device has Bluetooth, the key is 1 and the bus socket exists (w5-linux-3).
- **REV5-c:** pair() rescans for 5 s when BlueZ has dropped a scanned device (its 30 s TemporaryTimeout), instead of changing main.conf (w5-linux-9).
- **REV5-d:** D35bluetooth start and stop are serialized with flock on /run/thimsy/bluetooth.lock (BusyBox CONFIG_FLOCK=y added), re-reading the key under the lock (w5-linux-5).
- **REV5-e:** Pidfiles are trusted only while /proc/PID/comm is the daemon's name (w5-linux-4).
- **REV5-f:** A launch cycle is quick only if under 10 s and Nano's first frame was on screen under 2 s (or never), so a user trying several broken games is not held back; the session resets sys.thimsy.nano.first_frame to 0 before each spawn (w5-linux-6, -7).
- Rejected: a Bluetooth back-off armed while off (bluetoothctl fails fast without a bus) and the fixed ALSA card index (a documented Phase 0 choice).

## Open items from wave 5

- Not checked against real wpa_supplicant, BlueZ or BusyBox flock; bluetoothctl's exit statuses and wording vary by version; the comm names dbus-daemon and bluetoothd are assumed.
- Each poll tick with Bluetooth on still runs bluetoothctl N+2 times (show, devices, one info per paired device).
- Sleep does not stop or restart Bluetooth; bluez-alsa (Bluetooth audio) is not added; the launch-loop limits are untuned.
- A controller missing at a later boot makes that boot's D35bluetooth wait 5 s with the key still 1.
- tools/nano-tree/control-keys.json's notes on the nano_action rows, G's "G7-power and actions" row (names nano/third_party/wpa_ctrl), and upstream.json's fork_only `third_party/wpa_ctrl/**` glob are stale but harmless; NanoMenuSettings.cpp's and NanoMenuNet.cpp's file headers still describe `cmd wifi`.

## Owner audit answers (2026-11-26)

The owner went through AUDIT.md and the Deferred table one item at a time. Each answer has an id `OA-n`; the work it creates is listed with it and done in a later step unless it says otherwise.

- **OA-1, Linux's default ES-DE theme (Slate (gfix)):** the default theme follows the Android release: Colorful (Simplified) at the commit `mods/50-nano.sh` pins, modified the same way (Gilroy and Akrobat replaced by Urbanist 1.330 and Barlow Semi Condensed 1.422 with their OFL texts, `quake.webp` removed, Thimsy's console pictures and `theme-colours.json` added, `check_system_art.py` passing). Slate is not shipped, as on Android (50-nano.sh deletes `slate-es-de.zip`). The per-system webp art's ASK is settled the way Android settled it: shipped, with the credits in NOTICE and ATTRIBUTION.md. This is option (c) of the Deferred row.
  - Work: move 50-nano.sh's theme preparation (fetch and checksum, font swaps, theme.xml edits, console pictures, colours, art check) into one script both builds call, so the two themes cannot drift; Android's output must stay byte-identical. thimsy-data runs it at build time and installs the result where Nano's Linux build reads bundled themes; `assets_allowlist` accepts the prepared theme and still denies Slate and every removed file.
  - Work: Linux's property defaults match Android's theme block (`esde.themeset=colorful-simplified-es-de`, `esde.colorscheme=colorful-dark`, `esde.settingstheme=minima`, `ndstheme=3`, the Minima background and nav sounds off), from the same list rather than a copy.
- **OA-2, Android's set-up prompt on a fresh card (device-safety-3):** option (b). The image carries a small formatted FAT basic-data entry (a README for anyone who opens it on a PC or phone), so vold mounts it and Android never offers to set up the card before Linux's first boot.
  - Work: add the entry to the image layout (size, name and place chosen so D60's games entry, the 1 MiB tail and GF2-1's spacing still hold), change gpt_check's vold rule to allow exactly this one vold-visible entry, keep S00base's drive policy from mounting it as games, and update IMPLEMENTATION.md E.6 and the installer's warning. Checked on an Android device in Phase 0.
- **OA-3, SSH (release-build SSH keys, gfix2; supersedes G2e-11's release key-only rule and G2a-4's locked root for SSH):** follow what handheld Linux firmware does (ROCKNIX, ArkOS, muOS, Knulli): SSH is a switch in Nano's settings and logs in as root with a known default password, `thimsy`, in every build. A Nano setting lets the user set their own SSH password (and restore the default). Keys in root's authorized_keys keep working. By default dropbear accepts connections only from the local network: IPv4 private ranges (10/8, 172.16/12, 192.168/16), link-local (169.254/16, fe80::/10), IPv6 unique-local (fc00::/7) and loopback; a Nano setting, default off, allows any address. The owner first asked for no password at all, then chose the `thimsy` default instead.
  - Decided autonomously within OA-3 (for audit): the switch's default stays as it is, on in dev builds and off in release builds, because a known password is safe enough only while SSH is off unless the user wants it; dev builds use the same `thimsy` default (which also fixes the dev builds' empty password, which dropbear refuses without -B).
  - Work: `/etc/shadow` on the read-only root carries root's hash of `thimsy`; a password the user sets is stored as a hash (never the text) in a 0600 file under `/storage/.config/thimsy/ssh/`, and D50ssh applies it before starting dropbear (for example a bind mount of a generated shadow), so updates keep it. D50ssh drops `-s`. The LAN-only rule needs a mechanism, since dropbear has no address allowlist: the kernel's netfilter (nftables, with the kernel options it needs) or dropbear run per connection from BusyBox `tcpsvd` with its address rules; the step picks one and tests it. Nano's Linux settings gain the SSH switch, "Set SSH password", "Restore default password", "Allow SSH from any network" and the device's address, all behind the platform seam (no Android change). Release builds still run no getty, so the password is in practice an SSH login only.
  - Residual risk, accepted with the pattern: while SSH is on with the default password, anyone on the same local network (a shared or public Wi-Fi) can log in as root.
- **OA-4, the platform architecture (supersedes I15's shell gate as a translation layer, and the "zero edits at the call sites" goal behind it):** shared Nano code holds the model, view and controller only (menu rendering, input handling for pads, touch and mouse, and feature state such as Bluetooth's paired devices) and expresses intent through `nanoplat` interfaces. Every low-level platform action (shell commands, properties used to make another process act, Android-only APIs, Android or device paths) lives in that platform's implementation (`platform/firmware` for Android, `platform/linux` for Linux). No Android command ever runs on Linux, and no Linux command on Android, even when a program of the same name exists on both. A capability a platform has not implemented reports itself unsupported (and the menu can hide it) instead of being attempted. The owner accepts more changes to the Android code and a harder merge path from upstream Nano for a cleaner, more robust and extensible architecture.
  - Agreed shape (discussed 2026-11-26): unsupported is an explicit status from the interface's default, not a crash or C++ exception, with a capability query for the menu; unimplemented calls log in every build and fail the tests; the shell gate first becomes deny-all (translates and passes nothing, refuses and logs everything), then shrinks to nothing as each feature area moves behind the platform layer, held by a ratchet test on shared code (no `system`/`popen`/`exec`, Android-only headers, Android paths or control properties); user preferences stay shared behind a portable settings store, while properties used as IPC move into `platform/firmware`; no Linux `#ifdef`s in shared code either. Android changes stay verified moves (H.3), cherry-picked to nano-144 only after the AOSP build and the owner's smoke test.
  - The migration plan comes from the architecture audit of 2026-11-26 (to be recorded here).
- **OA-5, linux-ux-15 (Thimsy's own D30net and D35bluetooth started directly, not through the gate):** settled by OA-4. They are started by `platform/linux` code, which is the platform layer doing its own job, so they never go through the gate; the gate exists only to stop Android commands reaching Linux from shared code. The same holds for any platform-layer process start.
- **OA-6, the shell gate's documented limits (G9: `$(...)` in quotes, `sh -c`, `&&` and `||`):** moot under OA-4. A deny-all gate examines nothing in a command line before refusing it, so its parsing limits no longer matter.
- **OA-7, REV4-4 (a re-written or second-hand card keeps any complete exFAT at the games start):** kept as built. D60 never formats an existing games library; a fresh start is a format from the PC (or a future installer option).
- **OA-8, card interoperability between Linux and Android (new requirement):** a Thimsy card moves between a Linux Thimsy device and an Android Thimsy device with its ES-DE data intact and usable both ways: ES-DE settings, collections and playlists, gamelists (favourites, play stats, metadata), downloaded media (box art and the rest) and `emulator_config`. Linux Thimsy supports every on-card format Android Thimsy supports, and Android supports what Linux writes. This extends `android_launcher_plan.md`'s contract (the `ES-DE` folder in store mode, the `ROMs` layout, `emulator_config`) to Linux.
  - Work: an interoperability audit of each format (absolute paths such as es_settings.xml's ROMDirectory and the card's `/storage/<volume id>` path, emulator names stored in gamelists and settings, per-emulator config files that hold platform-specific values, Android-only files), then a written card contract, Linux support for it, and a round-trip test (Android-written fixture read by Linux, Linux-written read by the Android code's parsers on the host).
- **OA-9, GPT disk and partition UUIDs (waves 1-2 open item):** pinned in the build, unique per card once installed. Each device's image for a release carries fixed GPT disk and partition GUIDs derived from the device id and the release (name-based, like `card_uuid`'s filesystem UUIDs), so a rebuild is byte-identical. The installer writes fresh random GUIDs to each card it writes; a card written another way (Etcher, `dd`) gets fresh random GUIDs at its first boot if it still carries the image's pinned ones. They never change after that, updates included.
  - Why: two cards from one image in a two-slot device (the ARC-D) would otherwise share `root=PARTUUID=`, and the kernel could mount the other card's root, with S00base then treating that card as the boot disk.
  - Work: genimage's `disk-uuid` and per-partition `partition-uuid` set from the derived values (gpt_check verifies them); D60firstboot (which already rewrites the GPT) replaces pinned GUIDs with random ones (`sfdisk --disk-id` and `--part-uuid`) before anything else on the card is written, and it is idempotent if interrupted; the installer's write path does the same (installer plan). U-Boot reads entry 3's PARTUUID fresh on every boot, so nothing else depends on the old values.
  - Left open: two never-booted cards from the same image in both slots at once on the very first boot; the docs tell users to boot a new card alone first or to use the installer.
- **OA-10, `drmSetupZeroCopy`'s leak (waves 1-2 and G9 open item):** fix it on both platforms. When the zero-copy framebuffer ring fails part-way, the framebuffers and buffers already made are released at once (in creation order's reverse), not left until the DRM fd closes. A small Android-surface commit of its own, with a host test that fails the ring at each position, flagged for the nano-144 cherry-pick after the AOSP build.
- **OA-11, NanoMenu.h's freeze (Display EGL moves, G7D-2 and G7D-3):** lifted for platform-seam work, following OA-4. Android-only state leaves `NanoMenu`: the SurfaceFlinger objects (`mSession`, `mFlingerSurfaceControl`, `mFlingerSurface`, the secondary wallpaper surfaces) and the Android EGL set-up move into `platform/firmware`'s Display, and `NanoMenu` keeps only what every platform needs (a surface to draw into, its size and rotation). The same rule applies to each later area: its Android-only members leave `NanoMenu.h` with its code. Each change is still an Android-surface commit, reviewed and cherry-picked after the AOSP build. The FrozenApi ratchet (files compiled into drastic-nano, the scraper and the host tests never call `nanoplat::`) is a different rule and stays.
  - Not decided here: splitting `NanoMenu` itself (one class holding the model, views and controllers, 6,629 header lines, about 52 implementation files) into separate models and views, which would complete the MVC shape the owner described for OA-4. It is a larger refactor of code upstream also edits, for a later decision.
- **OA-12, the AAudio callback's user pointer (G7-audio):** restored to what it was before the move: the long-lived player object (alive as long as Nano), so a late error callback on AAudio's legacy path can never reach freed memory. The change stays inside `platform/firmware/Audio.cpp` and is folded into the G7-audio Android-surface commit's cherry-pick set (as a follow-up commit right after it, or squashed with it when cherry-picked). Linux is unaffected: its ALSA path pulls samples from a thread the stream owns (`AudioPull.cpp`), with no system-held pointer.
- **OA-13, card checks before mounting (G3b-8, changes it):** the automatic repair runs only when the filesystem says it was not cleanly unmounted: exFAT's VolumeDirty flag in the boot sector (read directly, no tool run on a clean card), ext4's state (`e2fsck -p`, which already returns at once on a clean ext4). A clean card mounts with no check; a dirty one gets `fsck.exfat -p` or `e2fsck -p` as before, and a repair is logged. Work: the read of the exFAT flag, tests with clean, dirty and damaged fixture images, and the mount-time measurement stays a Phase 0 item.
- **OA-14, NanoJail's eMMC protection (G3b-20, CAP_SYS_ADMIN):** accepted as it is. The jail hides the eMMC's nodes from Nano, which stops accidental writes to Android's storage, the realistic risk; a root Nano with CAP_SYS_ADMIN could still re-expose it, which is accepted. The waves 1-2 item "audit Nano before dropping CAP_SYS_ADMIN" is closed.
- **OA-15, BlueZ and D-Bus in the image (linux-ux-10, changes it):** only in images for devices whose device.json says `bluetooth: true`. Work: move bluez5_utils and dbus from `userland.fragment` into a Bluetooth fragment that the image build adds per device (or per family when every device in it has Bluetooth); D35bluetooth already exits 3 where Bluetooth is absent, and Nano's Bluetooth capability reports unsupported on images without BlueZ; the test that keeps BlueZ only while some device has Bluetooth becomes per image.
- **Architecture audit (2026-11-26):** recorded in `docs/linux/ARCHITECTURE-AUDIT.md`, with the raw reports in `docs/linux/audit-2026-11-26/`. It found, among others, that Nano exits during every Linux boot (Android's boot-mode decision in `readyToRun`, `NanoMenu.cpp:491-561`), that the shell gate passes unrecognised commands and misparses `timeout -s KILL`, and that the Type-C role flip can write sysfs on Linux without its crash guard. The migration order and the owner decisions it raises are listed there.
- **OA-16, features compiled out of the Linux build (G6-5, changes it):** the Linux alpha has archive browsing (games inside .zip and .7z in the file explorer) and the manual viewer (PDF and comic archives from ES-DE's downloaded media). The OTA update check stays off on Linux until Thimsy has its own update channel (`Cap::OtaUpdate` false). Under OA-4 each is a capability behind the platform layer. Work: choose and add the libraries (libarchive or minizip for archives; MuPDF or poppler for PDF, with a licence check against the repository's redistribution rules and the image-size budget), turn on `NANO_WITH_ARCHIVE` and `NANO_WITH_PDF` for Linux with the OTA code kept out, and report each as supported in Linux's `has()`.
- **OA-17, a long power-button hold on Linux (G3b-11, changes it):** the same as Thimsy Android's (`nano/powerwatch/nano_powerwatch.cpp`): a 7 s hold starts an orderly shutdown whether Nano, a game or nothing is in front (the game is told to save and close, the card is synced, then power off); if no shutdown is under way 12 s into the hold, the session forces one; if a shutdown under way has not finished 60 s later, an emergency sync, read-only remount and kernel power-off follow. A frozen kernel is left to the power chip's own long-press cut-off. This replaces the immediate power-off when Nano is not running. Work: a Linux power watchdog (in thimsy-session or a small service like nano-powerwatch, reading the key state with EVIOCGKEY so a grab does not hide it), a shutdown-started flag set before the game is closed, tests with a fake clock and fake key state, and checking each device's PMIC cut-off time on hardware (Phase 0).
- **OA-18, test hardware (D15):** none is bought for now; the owner's RG ARC-D is the only device. Consequences: the ARC-D is the reference unit for everything (D2's ARC-S role falls to it), with `mem=1G` standing in for the 1 GB floor and the 3 s boot figure stated with the Android loader's cost included; every other device's image is built and tested in software only and marked untested until someone runs it; the installer's multi-card, card-swap and counterfeit-card checks run against simulated disk images; Phase 0 and the wave plan keep their order, with "on hardware" meaning the ARC-D.
  - The owner also has one spare 128 GB microSD card. It becomes the ARC-D's Linux card in slot 1 (Android stays on the eMMC). The same card serves the card-interoperability tests (OA-8) on the one device: Android booted from the eMMC uses the card's games partition, then Linux boots from the card and reads what Android wrote, and back. Two-card cases (the ARC-D's second slot, OA-9's colliding IDs) are tested with disk images only, until a second card is available.
- **OA-19, card names across platforms (interop Q1):** both. Linux keeps a per-card alias list (`emulator_config/card.json`, plus the Cube-style `/storage/00000000-0000-0000-0000-NNNNNNNNNNNN` prefixes found in the card's own files, decision D20's map) and resolves those paths to the card, which works with every card already in use; and a later Thimsy Android build names public volumes by serial, as Linux and stock Android do (D20(c)), so new paths match everywhere. Android-surface work: the vold change, and reading the alias list. Until that build ships, nothing changes the meaning of existing fields (INTEROP-AUDIT.md's backward-compatibility rules).
- **OA-20, ES-DE compatibility and portable Thimsy settings (interop Q2):** the card stays fully usable by the real ES-DE app: ES-DE's files hold only ES-DE's own formats (an absolute ROMDirectory that Nano re-roots, labels ES-DE knows), with Thimsy-only data in Thimsy's own files. In addition, every relevant Thimsy setting is written to a user-editable file on the card, so settings are truly portable between devices and platforms (today they live in Android properties on the device). The owner accepts a significant change to how settings are stored if needed. Work: a research and design pass, `portable_settings_plan.md` (TODO.md links it), feeding OA-4's Prefs facade (migration step 8) and the wave 6 Settings interface.
- **OA-21, saves across platforms (interop Q5):** no save converters. In-game saves are shared between Android and Linux where both use the same emulator core (for example Mesen's saves on Android load in Mesen on Linux); systems whose cores differ by platform (PSX Beetle against PCSX ReARMed, N64 M64Plus FZ against Mupen64Plus-Next, DS DraStic or WatermelonDS against melonDS DS) keep separate saves per platform for now, so neither side overwrites or misreads the other's. Save states stay per platform. Work: the family table's `saveCompatible` column decides per system and core pair; Linux saves for a non-matching pair go to `emulator_config/retroarch/linux/saves/<system>/` and states to `.../linux/states/<system>/`; a device test confirms one shared-core save round trip (the Mesen case) on the ARC-D.
- **OA-22, systems without a Linux emulator on the first Linux devices (interop Q9):**
  - On Linux, Android's "Android Games" system is replaced by **PortMaster** (the Linux handheld ports manager that ROCKNIX, muOS and Knulli support): a Ports system listing installed ports, launched through the Linux launcher.
  - GameCube/Wii (Dolphin), 3DS (Azahar), PS2 (ARMSX2), Switch (hakuX) and Vita (Vita3K) are supportable on Linux but beyond the first Linux build's target devices (RK3566 class), so they are hidden on devices that cannot run them; their games, saves and settings on the card are left untouched for Android. Visibility follows the device: a system appears when the image ships an emulator for it and the device profile says the device can run it, so stronger devices in later waves show them without a format change.
  - Work: a PortMaster research and packaging pass (licence and redistribution, where ports live on the card and how that coexists with the Android card layout and ES-DE's `ports` system, its runtimes and gptokeyb input mapping, running it under the session and, later, as the non-root `nano` user, and the boot-time budget); the per-device system visibility rule in the Linux platform (from the emulator manifest and device.json), expressed through OA-4's capability and Emulators interfaces.
