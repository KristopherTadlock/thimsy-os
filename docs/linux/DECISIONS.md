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
| D15 | buying hardware (an ARC-S first, then an RGB30 or RG353VS, an H700 pair, an SD multiplexer, a second reader, a counterfeit card; a Mac to borrow) | the owner's decision; decision 15 |
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
