# What the owner should audit first

A short list, per implementation wave, of the autonomous decisions most worth auditing and the decisions left to the owner. The full record, with every decision id, is DECISIONS.md (its "Deferred" table holds the pending choices with options and recommendations). Nothing here has been built with Buildroot or the Android toolchain, or run on a device.

## Waves 1-2

Source: `docs/linux/DECISIONS.md`, section "Implementation decisions (waves 1 and 2)" and "Open items from waves 1 and 2".

### (a) Decisions to audit (12)

1. **G9a-1, G9a-2:** Android Nano source changed: `drmAllocAhbTarget` moved to a new `NanoMenuDrmBuffers.cpp` and added to `Android.bp`; never compiled, so the Android build's safety rests on the AOSP build (H.7) and a cherry-pick to nano-144.
2. **G0-7 (with G0-5):** the merge driver that regenerates `0001-thimsy.patch` on every merge exits 1 and leaves the merge to finish by hand, and `regen` mis-hashed `git add -N` files once (caught); the Android patch's integrity depends on these tools.
3. **G2e-6, G2e-7:** D60firstboot repartitions the boot card on first boot (moves the backup GPT, `sfdisk --force -N 5`, mkfs, retype); the one place Linux writes a partition table.
4. **G2e-1 (with G2e-4):** how S00base picks the boot disk (mounted root first, PARTUUID never guessed when two match; eMMC boot gives tmpfs `/storage`); decides which disk every later write touches.
5. **G3b-20:** NanoJail masks the eMMC's nodes from Nano and fails closed; but a root Nano with CAP_SYS_ADMIN can still mount a new devtmpfs (open item), so the eMMC protection is partial.
6. **G3b-5 (with G2b-1):** the drive policy (vold's types only, boot disk only its "games" basic-data partition, Linux reserved and home never mounted); keeps Linux off Android's and the boot card's own partitions.
7. **G3b-8:** every exFAT and ext4 drive is auto-repaired (`fsck.exfat -p`, `e2fsck -p`) before mounting, including cards Android uses; a repair can change files Android wrote.
8. **G2a-4, G2a-9, G2e-12:** root login locked except in dev targets; the RK3566 hardware pack is publishable; "release" is inferred from no root login and no serial getty.
9. **G2e-11 (supersedes G2a-6):** SSH is on by default in dev builds and off, key-only (`-s`) in release unless `persist.thimsy.ssh.enabled` is set on the user-writable card; the SSH_AT_BOOT/SSH_PASSWORD options are dead.
10. **G1a-9, G1a-10, G1a-12:** secret redaction (key-name heuristics) in `list`/`watch` but not `get`; `install.json` on a user-writable partition can only add `ro.thimsy.install.*`.
11. **G2d-12, G2d-13 (with G2c-10):** what ships: the asset allowlist denies Akrobat and the whole `slate-es-de.zip` (user-visible: fonts fall back, no default theme yet), keeps undocumented data files under ATTRIBUTION.md's licences; firmware ships only with a WHENCE licence.
12. **G3b-11 (with G2c-22):** a long power press powers off at once when Nano isn't running, with no software hard power-off; the ARC's hotkey is a guessed mapping of the home key to BTN_MODE.

### (b) Deferred to the owner or needing hardware

Owner:
- GPT partition and disk UUIDs: random (non-reproducible images) or pinned (two cards from one image collide in an ARC-D).
- Cherry-pick `6d6314c` to nano-144 after the AOSP job; confirm with the AOSP build.
- `drmSetupZeroCopy`'s pre-existing leak on Android: fix (an Android behaviour change) or leave.
- Merging nano-144 when only nano-144 changed the patch: run `import-patch --old <previous merge base>`.
- Audit Nano before dropping CAP_SYS_ADMIN from its jail.
- ~~Linux default ES-DE theme~~: decided 2026-11-26, it follows the Android release's modified Colorful (Simplified) (DECISIONS.md OA-1).
- A UI to fetch excluded proprietary firmware from the user's device (installer, Phase 4).

Hardware (Phase 0/2a):
- RK3566 BL31: rkbin's for now (G2a-17) instead of the owner's D18 choice, until Phase 0.
- The RK3566 hand import and FlipBase subtree merge files; `uboot.fragment` embedding `boot.cmd`; the FIT header against FlipBase's `mkfit.py`; mmc 1 as the SD slot and the setexpr forms.
- padd on the ARC (Q4): joystick detection with ABS_X/ABS_Y, grab, hiding, mknod fallback, uinput, rumble.
- ARC device.json guesses (touch name, power key name, ARC-S RAM, hotkey, F label); RK817 "Playback Path" values and the volume mixer control name.
- `sfdisk --force -N 5` via BLKPG, PARTUUID in uevents, rcS time; deep sleep with the vendor BL31 (Q24).
- `fsck.exfat -p` time on every mount; the session's root-only paths (netlink, mount, pidfd, shared `/`, NanoJail).
- ROCKNIX's scripts/unpack patch order, with a full clone.

## Wave 3

Source: `docs/linux/DECISIONS.md`, sections "Implementation decisions (wave 3)" and "Open items from wave 3", plus the new "Slate (gfix)" row in "Deferred".

### (a) Decisions to audit (8)

1. **G5 (`1aa3ea3`) with G5-4:** the only wave-3 change to the Android patch adds `NanoPlatform.h`/`.cpp` sections with no `android-surface:` line (no `Android.bp` module lists them); the `FrozenApi` ratchet, wider than I24, is what keeps every Android module off `nanoplat::`.
2. **gfix-2, gfix-3:** the nanopatch and new nanoblock drivers now finish fork-to-fork merges by themselves (three-way merge of the `nano/` trees from git's objects, exit 0), so a wrong merge could change the Android patch unseen; the backstops are `regen --check` and `test_nano_patch_consistent`, and git's rename detection can disagree.
3. **G9-20 with G4-9, G9-19:** the build fails if the root has `/dev/block`, `/vendor` or `/mnt/shares`, and two ratchets hold the shared files naming raw block devices to NanoScreenCal.cpp; the shell gate's /vendor rule and B.5's eMMC rule depend on this.
4. **G9-3, G9-4, G9-6 (and the documented limits):** the gate routes setprop/getprop, `reboot`/`svc power` (to `sys.powerctl`), no-ops restorecon/chcon, passes /system, /vendor and the like only for programs the image has, and rewrites `gammaos-net download` to `curl -fsSL`; it does not examine `$(...)` in double quotes, rewrite `sh -c`, or tell `&&` from `||` (owner open item).
5. **gfix-6:** `/root` stays a 0700 directory and S00base bind-mounts `/storage/.config/thimsy/home/root` (with `.ssh`) over it, so root's home and SSH keys live on the user-writable card; replaces G2e-13's link, not run on a device or under makedevs.
6. **gfix-9, gfix-10:** `nano/NOTICE` declares GPL-2.0+ for NanoOskKanji.h (SKK-JISYO) and for liba52/NanoAc3 (only with `NANO_WITH_AC3=ON`); Barlow (OFL-1.1) is installed under Akrobat's file names, matching Android's sha256s, from full-URL extra downloads not yet tried in Buildroot.
7. **G9-11:** `drm_active=1` (Nano's splash) sets `sys.boot_completed=1`, which gates dosleep, the background scan and idle work; whether Nano really writes 1 at the splash is unchecked (G10), and the session must not also set it.
8. **G6-5 with G4-1:** the Linux build compiles out the PDF manual viewer, zip/7z browsing and the OTA check (`NANO_WITH_PDF/ARCHIVE=OFF`, `NANO_WITH_MEDIA=ON` refused, so media uses inert stubs); user-visible missing features, and the overlay/`nanoForceSfPath()` paths would poll SurfaceFlinger forever if taken.

### (b) Pending the owner

- ~~Linux's default ES-DE theme~~: decided 2026-11-26, the Android release's modified Colorful (Simplified) (DECISIONS.md OA-1).
- Accept the shell gate's documented limits, or have a G7 step narrow them.
- `drmSetupZeroCopy`'s leak, now more visible with GBM on Linux (the waves 1-2 Android-fix question stands).
- At each nano-144 merge that adds a unit test or changes includes: edit `sources.txt` and run `include_census.py write`.
- Whether to add `check-stubs` to `build_guard.sh`'s SMALL_TARGETS (minor).
- ~~The dead SSH_AT_BOOT/SSH_PASSWORD options~~: removed in wave 4 (GF2-7).

Wave 3 updates from wave 4: gfix-6 (root's home on the card) is replaced by GF2-6 (ext4 data partition only); the dead SSH options are removed (GF2-7).

## Wave 4

Source: `docs/linux/DECISIONS.md`, "Wave 4: the G7 seam moves, gfix2, review" and "Open items from wave 4", plus three new Deferred rows.

### (a) Decisions to audit (7)

1. **The G7 Android-surface commits (eight, one per area):** verbatim moves of Android code into `platform/firmware/*` behind `nanoplat::get()`, each colour-moved checked and adversarially verified, none compiled for Android. All are flagged for cherry-pick to nano-144 in a fixed order, and G7-main depends on G5's `004f1a1` (NanoPlatform.h/.cpp), which never had an H.3 listing.
2. **G7-launch-a:** the launch handshake now goes through 69 `commitLaunch` calls; every write stays in place, but all sites now use `property_set` where some used `android::base::SetProperty` (equivalent on Bionic).
3. **G7-audio:** the AAudio callback's user pointer is now a short-lived stream object (an AOSP review point, Deferred).
4. **REV4-1, REV4-2:** Linux Nano exits after every launch (like Android's home) and its exit status tells the session whether a launch happened; the resident-Nano path is kept but unused.
5. **REV4-4 (changes device-safety-2):** D60 keeps any complete exFAT found at the games start, whatever its label, and formats only when there is none; a second-hand card's old library is kept for the new owner to format.
6. **GF2-1:** every image now ends with a 1 MiB tail and games starts 2 MiB after data, enforced by gpt_check.
7. **G7-power-9, -10, -11:** Linux reboot and power-off are session requests; Wi-Fi uses a new MIT wpa_supplicant client and the D30net script; Bluetooth uses bluetoothctl (BlueZ added to the image in wave 5).

### (b) Pending the owner

- Display EGL moves (G7D-2, G7D-3): allow a step that adds accessors to the frozen NanoMenu.h, or leave it to G10.
- Release-build SSH: add a key-import path or keep release SSH unusable (recommended: the import path).
- AAudio pointer lifetime: confirm in the AOSP review before cherry-picking G7-audio.

## Wave 5

Source: `docs/linux/DECISIONS.md`, "Wave 5" and "Open items from wave 5".

### (a) Decisions to audit (4)

1. **G7-net (`b823c2f`):** the Wi-Fi and Bluetooth screens' Android commands (715 lines) moved verbatim into `platform/firmware/Network.cpp` behind a 25-method `nanoplat::Network`; not compiled for Android; cherry-pick after G7-power's commit.
2. **linux-ux-15:** Nano's runProgram calls the raw execve syscall, bypassing the shell gate, for Thimsy's own D30net/D35bluetooth commands.
3. **linux-ux-10, -11:** BlueZ and D-Bus ship in every image's userland but start only when the device has Bluetooth and the user turns it on (default off); root size growth not measured.
4. **linux-ux-1/2, REV5-f:** Linux sets app_launched itself (no 10 s wait) and the session clears it; launch loops are held back only when Nano was on screen under 2 s.
