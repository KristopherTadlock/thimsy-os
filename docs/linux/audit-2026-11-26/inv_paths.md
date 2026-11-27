PATHS INVENTORY: hard-coded platform paths and device nodes in Nano's shared code (thimsy-os-linux, branch linux, read-only audit)

## How I counted

**Shared set: 204 files.**
- The .cpp files of sections [core] (105), [drm] (1, NanoMenuDrm.cpp) and [scrape] (6) in nano/build/sources.txt. Removing duplicates leaves 107, which include bootscreen/NanoBootArt.cpp.
- Plus every top-level nano/*.h and nano/*.inc.
- Not counted: the [firmware] files (main.cpp, Drastic*.cpp, LibretroRunner.cpp, NanoMenuShares.cpp, NanoMenuRgbFollow.cpp, NanoBootChime.cpp, NanoBtStable.cpp, NanoMenuDrasticPark.cpp, FakeJNI.cpp, NanoMenuDrmBuffers.cpp, platform/firmware/*), platform/*, compat/* and test/*.

**Method.**
- A small C++ literal scanner reads each file, skips // and /* */ comments, keeps "..." and R"(...)" literals, and matches absolute roots inside them.
- A root must start a path and must not be followed by a word character (so "/sys" does not match "/system").
- Counts are matches inside string literals. One literal can hold more than one, for example a shell command string.
- Script: C:\Users\krist\AppData\Local\Temp\claude\a05db0c91d486b22\216a381f-418c-48c8-9542-04375b17814b\scratchpad\paths.py
- Full per-match listing: C:\Users\krist\AppData\Local\Temp\claude\a05db0c91d486b22\216a381f-418c-48c8-9542-04375b17814b\scratchpad\all.txt
- Other numbers below marked "grep" are whole-file grep counts, comments included.

**Total.** 784 path-root literals, not counting /dev/null (129, almost all "2>/dev/null" inside shell strings).

## The Linux design today (context for every row)

The project deliberately leaves shared path literals unedited.
- **The rule.** IMPLEMENTATION.md I14 (line 1041) says "zero edits to shared files". IMPLEMENTATION.md:428 says "Paths are not an interface yet".
- **The links.** The Android layout is rebuilt on the Linux root with links:
  - linux/board/common/post-build.d/10-android-links.sh:70-75 links /data, /sdcard, /system/etc, /system/fonts, /system/bin and /mnt/media_rw.
  - :78 fails the build if /dev/block, /vendor or /mnt/shares exist.
  - linux/board/common/rootfs-overlay/etc/init.d/S00base:91-104 (first_boot_tree) makes the /data tree on the card, plus user/0 -> ../data, media/0 -> /storage/emulated/0 and /storage/self/primary.
- **The ledger.** tools/nano-tree/path-roots.json gives each root a disposition (link/same/gate/kernel/dropped/not-reached/not-a-path). tools/nano-tree/tests/test_path_roots.py holds it as a ratchet.
- **NanoJail.** It masks only the eMMC nodes (DECISIONS G3b-20). It does no path mapping.
- **ShellGate.** It acts only on system/popen/exec* (platform/linux/ShellGate.cpp:66-94). Plain open()/fopen()/opendir() of a path is never gated. Paths survive only through the links, or fail with ENOENT.

So for paths, Linux copes by **imitating Android's filesystem**, not through the platform layer. This is the opposite of the owner's principle.

## Inventory by family

Format: count in literals / number of files: Linux disposition. Samples follow each family.

**1. /system: 106 / 31 files**

- **/system/etc: 43 / 19 files.** Link to /usr/share/thimsy/system/etc (only files allowed by thimsy-data's assets.list).
  - Samples: NanoMenuButtonMap.cpp:85 (nano_xmb/buttonmap); NanoMenuPS3Icons.cpp:320, 906, 1203; NanoMenuRender.cpp:4722, 4759, 4792; NanoThemeEngine.cpp:60, 92, 5817; NanoMenuEsdeMenu.cpp:197; NanoI18n.cpp:94.
  - Files not installed on Linux:
    - /system/etc/gammaos/device_keys.json (NanoDeviceKeys.h:14) is missing from linux/package/thimsy-data/assets.list. NanoDeviceKeys.cpp:181 then logs "using the RG Cube's keys" and uses the built-in Cube map (NanoDeviceKeys.cpp:24-28: Anbernic=F10/68, Mode=316, Back=158). **Every Linux device gets RG Cube key semantics.**
    - Also absent: /system/etc/gammaos/hotkeys (NanoHotkeys.cpp:29) and /system/etc/gammaos/nano_systems.json (NanoSystemsHeal.h:33). Both fall back softly.
- **/system/fonts: 19 / 3 files.** Link to /usr/share/fonts/thimsy, which installs fonts under Android file names.
  - Samples: NanoMenuRender.cpp:4734-4740, NanoBootScreen.cpp:38-47, NanoMenuButtonMap.cpp:223-228.
- **/system/bin: 41 / 11 files.** Breakdown: rm 8, restorecon 7, curl 7, am 5, mkdir 3, gammaos-net 2, unzip 2, logcat, pidof, nsenter, mv, chown, syncthing and drastic-nano 1 each.
  - Gate (ShellGate):
    - Rewritten to the Linux tool of the same name: curl, rm, mkdir, mv, unzip, pidof, chown.
    - Refused: am, logcat.
    - Turned into a no-op: restorecon.
    - Handled by gate routines of their own: gammaos-net and nsenter.
  - Samples: NanoHls.cpp:147, 249; NanoIcyDemux.cpp:44; NanoMenuIptv.cpp:107; NanoMenuRadio.cpp:108; NanoOtaCheck.cpp:244, 249, 327, 531, 621; NanoMenuEsdeMenu.cpp:833-991; NanoSdMove.cpp:250, 1355, 1770, 1787, 1971; NanoBiosWorker.cpp:122, 179, 717, 863, 868, 890; NanoBiosUtil.cpp:448, 486.
  - Exceptions:
    - /system/bin/syncthing (NanoMenuSyncthing.cpp:68): no wrapper exists, so it is refused.
    - /system/bin/drastic-nano (NanoMenuUtils.h:208) is an access() probe that fails, so the DS player is reported missing.
- **/system/lib64/libdrastic_arm64.so (NanoMenuXmb.cpp:3363): 1.** Not reached; the access() fails.
- **Bare "/system": 2.** A folder suffix (NanoBiosWorker.cpp:530, NanoRaConfig.cpp:52).

**2. /vendor: 3 / 1 file**
- NanoMenu.cpp:1241 (system("/vendor/bin/setclock_%s.sh &")), and 1746 and 2042 (setclock_max.sh).
- Linux: the gate refuses all /vendor/ commands, and /vendor is forbidden in the root. CPU clocks get no Linux implementation through this path.

**3. /product, /odm, /apex, /system_ext, /metadata, /cache: 0.** Prop files (build.prop, init .rc): 0 real paths. The only hit is a log message naming gammaos-nano.rc (NanoMenu.cpp:1425).

**4. /data: 323 / 64 files.** All of /data links to /storage/.config/thimsy/data.
- **/data/system: 177 / 49 files.** Nano's own state files kept under Android names.
  - Samples: nano_xmb* (34+8), nano_scrape 9, nano_esde_themes 8, nano_launch_intent.txt 5, nano_systems.json 4, nano_bios 4.
  - Disposition: link.
  - Exception: /data/system/packages.list, 7 sites. It is Android's package database and does not exist on Linux. Behaviour per site:
    - NanoMenuXmb.cpp:1333: "packageInstalled says yes to everything when the package list cannot be read" (comment at :1332).
    - NanoMenuXmb.cpp:1398 and NanoMenuXmb.cpp:3365: heal skipped / launch never blocked.
    - NanoMenuState.cpp:339: user-app list empty.
    - NanoMenuPS3EmuCatalog.cpp:190: installed set empty.
    - NanoBiosWorker.cpp:208: Packages not readable.
    - NanoHakuxSetup.cpp:285: kNotInstalled.
  - Net effect: **it fails open or closed by accident, not by a platform answer.**
- **/data/media: 70 / 23 files.** media/0 is a link to /storage/emulated/0.
  - Samples: NanoAppFiles.cpp:53, 559; NanoBiosWorker.cpp:55-56; NanoBiosUtil.cpp:389; NanoMenuSyncthing.cpp:69; NanoRaConfig.h:36.
- **/data/data: 50 / 13 files.** Android app private dirs, linked to data/data on the card.
  - Packages named, distinct literals: com.retroarch.aarch64 9, com.rfandango.haku_x 8, org.mupen64plusae.v3.fzurita 4, com.seedlessds.app 4, org.scummvm.scummvm 3; one each for org.ppsspp.ppsspp, com.flycast.emulator, com.dsemu.drastic, com.armsx2, org.azahar_emu.azahar, org.vita3k.emulator, me.magnum.melondualds, org.lineageos.setupwizard.
  - Samples: NanoAppFiles.h:32-41; NanoHakuxSetup.h:35-36; NanoBiosWorker.cpp:60; NanoButtonMapReaders.cpp:1839; NanoMenuXmb.cpp:1397 (stat /data/data/com.retroarch.aarch64/cores).
  - Linux: the folders exist, but no Linux emulator uses them. Linux PPSSPP and ScummVM ship (linux/package/ppsspp, scummvm) yet shared code looks for org.ppsspp.ppsspp and org.scummvm.scummvm homes.
- **/data/user/0: 8 / 6 files.** Link (user/0 -> ../data).
  - Samples: NanoRaConfig.h:31 (kRaPkgData), NanoButtonMapReaders.cpp:466, 1591, NanoAppFiles.h:36.
- **/data/local/tmp: 7 / 4 files.** Link (1777).
  - Samples: NanoMenu.cpp:657, NanoMenuEsdeMenu.cpp:841, NanoMenuOverlay.cpp:99, NanoMenuRender.cpp:437, 6164, 6171.
- **/data/gammaos_ota: 4.** Link; the hand-off itself is refused (NanoMenuOta.cpp:302, 592; NanoOtaCheck.cpp:81-82).
- **/data/misc: 2.** NanoMenu.cpp:494 (bootanim/nano_skip) and NanoSyncthing.cpp:39 (syncthing/config.xml). The subfolders are never made, so reads and writes fail softly.
- **Android scoped storage (grep, comments included):** "Android/data/<pkg>" and "Android/obb" appear in 15 files. Most: NanoBiosWorker.cpp 9, NanoEsdeEmulators.inc 6, NanoSdMove.cpp 5, NanoBiosCatalog.cpp 5, NanoAppFiles.h 5. Packages: dolphinemu, ppsspp, retroarch, armsx2, vita3k, mupen64plusae, mame4d2024, flycast.
  - On Linux these become real folders named "Android/data/..." on the user's card.

**5. /sdcard: 34 / 16 files.** Link to /storage/emulated/0.
- Samples: NanoLaunch.cpp:82, 101-102, 204; NanoMenuXmb.cpp:1694, 1811, 2082, 2820-2822; NanoMenu.cpp:4555 (/sdcard/ROMs/nds/); NanoMenuOta.cpp:302 (/sdcard/Download); NanoMenuScraper.cpp:337, 468, 500, 1192.
- /mnt/sdcard (NanoRaConfig.cpp:192) is not reached; it is only matched as an alias.

**6. /storage: 142 / 28 files.** Disposition: same. S00base makes emulated/0 and self/primary; thimsy-session mounts drives at /storage/<id> with vold-style ids (linux/docs/session.md:56-59).
- Breakdown: /storage/emulated/0 73, the prefix "/storage/" 38, /storage/self/primary 12, bare /storage 10, other 9.
- Heaviest files: NanoMenuXmb.cpp 18, NanoMenuPS3Folder.cpp 13, NanoSdMove.cpp 11, NanoEsdeStore.cpp 11, NanoButtonMapReaders.cpp 9.
- Samples: NanoAppFiles.h:30-44, NanoRaConfig.h:33, NanoMenuUtils.h:268-275, NanoSystemsHeal.cpp:215.
- **Seam bypass.** The platform has a Volumes interface (NanoPlatform.h:177-194), but shared code uses it at only 5 sites: NanoEsdeStore.cpp:163, 172 and NanoSdMove.cpp:399, 405, 1578. Against that stand 142 /storage literals and 42 /mnt/media_rw literals.
- NanoRaConfig.cpp:190-199 and 466-471 (storageSpelling, raRawPath) hard-code Android's split between the FUSE view and the raw mount (/storage vs /mnt/media_rw vs /data/media/0). On Linux this only works because the links collapse the three into one.

**7. /mnt: 59 / 22 files**
- **/mnt/media_rw: 42 / 17 files.** Link to /run/thimsy/media_rw/<id> -> /storage/<id>.
  - Samples: NanoAppFiles.cpp:53, 55, 78; NanoBiosUtil.cpp:54, 67, 133, 223; NanoLaunch.cpp:83, 105-106, 206; NanoMenuXmb.cpp:191, 1401, 1848-1853, 2823, 3467, 4020; NanoScraper.h:133, 137; NanoSyncthing.cpp:593, 610.
- **/mnt/shares: 14 / 9 files.** Dropped (not made). The checks find nothing.
  - Samples: NanoMenuFileExplorer.cpp:270, 343, 477; NanoMenuPS3Folder.cpp:439, 470, 480, 533; NanoMenuVideo.cpp:367; NanoMenuPhotos.cpp:611; NanoMenuScraper.cpp:434.
- **/mnt/user/ (NanoSyncthing.cpp:578): not reached. Bare /mnt (NanoMenuFileExplorer.cpp:344): same.**

**8. /dev, excluding /dev/null: 21 / 6 files**
- **/dev/input: 14 / 2 files.** Same path on Linux (devtmpfs). Nano scans it itself, and InputPolicy only gets deviceOpened/Closing callbacks.
  - Sites: NanoMenuInput.cpp:76-246 (10 sites); NanoMenuPS3Menu.cpp:3745, 3750, 4035, 4041.
- **/dev/dri/card0: 5 / 2 files.** Hard-coded; there is no Display-seam override.
  - Sites: NanoMenuDrm.cpp:499, 982 (drmEarlySplash, which Linux reaches through platform/linux/display/DisplayKms.cpp:48), 3241; NanoScreenCal.cpp:248, 499.
  - Linux depends on the display controller probing as card0 (panfrost also registers a card).
- **/dev/block/by-name/baseparameter (NanoScreenCal.cpp:335): Rockchip vendor partition, written with pwrite.**
  - Not reached: there is no /dev/block on the root (10-android-links.sh:78), and NanoJail masks the eMMC. Ratchet: block-paths.txt (G4-9).
- **Bare /dev (NanoBiosUtil.cpp:100): a filter list.**

**9. /sys: 54 / 11 files.** Disposition: kernel (sysfs exists). Nothing substitutes the nodes; each site works only if the node name matches.
- **Backlight, 10 sites.** NanoPlatform.h:201-202 states the design: NanoBacklight.h drives /sys/class/backlight on every platform.
  - NanoBacklight.h:65, 71 enumerate generically (fine).
  - NanoBacklight.h:83, 88 fall back to /sys/class/leds/lcd-backlight (Android LED-class name).
  - NanoMenu.cpp:905-906 read max brightness from "panel0-backlight", then "leds/lcd-backlight" (Android/Unisoc node names; on mainline the read falls back to 255).
  - NanoControlCenter.cpp:146-159 use default names "backlight" and "backlight1", from persist.gammaos.nano.cc.bl_bottom/bl_top (RG DS dual-panel naming, comment at :138).
- **Battery, 6 sites: hard-coded /sys/class/power_supply/battery/*.**
  - Sites: NanoControlCenter.cpp:257-261 (capacity, voltage_now, current_now, status) and NanoMenuMtp.cpp:340 (capacity). Also NanoMenuMtp.cpp:92 /sys/class/power_supply/usb/online (Android/Unisoc charger name).
  - **This bypasses the existing Power seam.** NanoMenuSystem.cpp:496 calls power->readBattery, and platform/linux/Power.cpp:45, 158-168 already scans power_supply by type=="Battery". The Control Center gauge and typec low-battery check do not use it.
  - I have not checked what the supply is called on mainline RK3566 (rk817).
- **Wakelocks, 17 / 5 files.** /sys/power/wake_lock and wake_unlock (Android wakelock ABI).
  - Sites: NanoMenu.cpp:456, 460, 5141, 5150, 5199, 5208; NanoMenuInput.cpp:1128, 1156, 1276, 1301, 1376; NanoMenuMtp.cpp:350, 359; NanoMenuOta.cpp:238, 658.
  - Linux: these exist only with CONFIG_PM_WAKELOCKS. The ROCKNIX kernel config is not in the repo, so this is unverified. Either way they have no effect without Android-style autosleep.
- **/sys/power/suspend_stats/success (NanoMenuDrm.cpp:3302): generic.**
- **CPU, generic cpufreq paths, but with a device-flavoured default: NanoControlCenter.cpp:169, 174 (fallback 2160000 kHz).**
- **GPU: NanoControlCenter.cpp:201, 208, 213, 217 read /sys/class/devfreq/fde60000.gpu/*.** This is the RK356x Mali address. It is right on RK3566 by coincidence and reads 0 on T820/T618/H700.
- **Thermal: NanoControlCenter.cpp:228, 235 match zone types "soc-thermal" and "gpu-thermal" (NanoControlCenter.cpp:355-356).** These are Rockchip zone names.
- **rfkill / Wi-Fi:**
  - NanoMenuInput.cpp:933 reads rfkill0 (comment: "rfkill0 == sunxi-bt ... on the Brick", TrimUI Brick).
  - NanoMenuSystem.cpp:541, 550 enumerate rfkill generically (fine).
  - NanoMenuSystem.cpp:559 assumes /sys/class/net/wlan0.
- **USB-C and gadget:**
  - NanoTypec.h:11 /sys/class/typec; NanoMenuMtp.cpp:75, 80 /sys/class/udc.
  - NanoMenuMtp.cpp:316 /sys/module/sprd_typec_displayport (Unisoc T820).
  - NanoMenuMtp.cpp:266-268 /sys/class/drm (generic connector status), also NanoDisplayTargetSf.h:39, 46.
- **/sys/devices/system/cpu: the 2 cpufreq sites above.**

**10. /proc: 36 / 17 files.** Disposition: kernel. The paths exist; the content assumptions do not always hold.
- **Generic, works:** /proc/meminfo ×4 (NanoControlCenter.cpp:243, NanoMenuPS3Menu.cpp:1396, NanoMenuVideo.cpp:3079, NanoScrapeQueue.cpp:541), /proc/stat, /proc/self/mounts ×4, /proc/self/mountinfo, /proc/self/maps, /proc/self/fd ×3, /proc/sys/vm/drop_caches, /proc/net/fib_trie, /proc/asound/card%d/... (NanoMenuSystem.cpp:572).
- **Android process-name assumption: /proc/<pid>/cmdline scans.** These sites assume the process name is the Android package name:
  - NanoMenuPS3Folder.cpp:1057, 1064 compares against "com.retroarch.aarch64", so **Linux RetroArch (/usr/bin/retroarch) is never detected**.
  - The same assumption is in NanoSdMove.cpp:234, NanoHotkeys.cpp:230-237 (process names as package names), NanoMenuSystem.cpp:671, 699 and NanoMenuMtp.cpp:114-118 ("com.android.providers.media.module").
  - NanoMenuMtp.cpp:132, 204 write /proc/%d/oom_score_adj for that process.
- **Android-kernel only:** /proc/bluetooth/sleep/lpm (NanoMenuInput.cpp:941) is a written node; it fails softly on Linux.
- **Rockchip BSP device tree:** NanoScreenCal.cpp:358, 360 read /proc/device-tree/vop@fe040000 and display-subsystem/route/route-dsi*. This is RG DS / RK3568 BSP; mainline has no route node, so it fails softly.

**11. /config: 6 / 4 files.** Real configfs uses: 2, NanoMenuMtp.cpp:100, 104 (/config/usb_gadget/g1/configs/b.1). Not reached; Linux mounts no configfs. The other 4 are a RetroArch "<dir>/config" suffix: NanoMenuPS3Folder.cpp:1188, NanoMenuPS3Menu.cpp:12053, NanoRaConfig.cpp:55.

## Device-specific knowledge in shared code (Thimsy must support many devices)

**SoC/family node names hard-coded:**
- **Unisoc T820:** sprd_typec_displayport (NanoMenuMtp.cpp:316); the "two typec ports" logic (NanoTypec.h comment on pdPort).
- **Rockchip RK356x / RG DS:** fde60000.gpu devfreq (NanoControlCenter.cpp:201-217), soc-/gpu-thermal (:355-356), vop@fe040000 and route-dsi (NanoScreenCal.cpp:358-360), baseparameter partition (NanoScreenCal.cpp:335), "backlight"/"backlight1" dual panels (NanoControlCenter.cpp:145, 157).
- **Android/Unisoc names:** panel0-backlight and leds/lcd-backlight (NanoMenu.cpp:905-906, NanoBacklight.h:83-88), power_supply/battery and power_supply/usb (above), wlan0 (NanoMenuSystem.cpp:559).
- **TrimUI Brick:** rfkill0 = sunxi-bt (NanoMenuInput.cpp:932).

**SoC branching by property (not paths, same problem):**
- NanoMenu.cpp:432-436: ro.board.platform contains "ums", "sc98", "sc99" or "sharkl" (Unisoc), which forces the SF path.
- NanoMenuDrm.cpp:1155-1156: "rk356x" (AFBC).

**Default device = RG Cube:**
- NanoDeviceKeys.cpp:24-28 and 181 (no table on Linux, so the Cube keys are used).
- NanoMenu.cpp:4040: typecStart() runs unconditionally, with the comment "the Cube offers it instead".

**Mentions (grep, comments included):** "RG DS" 127 hits in 24 files; "Rockchip"/"RK356" 42 in 12; "Brick" 32 in 13; "RG Cube"/"the Cube"/"Cube's" 28 in 11; "Unisoc"/"sprd" 15 in 8.

## Where the "it copes" story silently breaks (Linux gets a wrong answer, not a no-op)

1. **The RetroArch config Nano edits is not the one Linux RetroArch reads.**
   - Nano edits NanoRaConfig.h:33/36 (/storage/emulated/0/Android/data/com.retroarch.aarch64/files/retroarch.cfg) and returns it from NanoRaConfig.cpp:465.
   - The Linux launcher passes /storage/.config/retroarch/retroarch.cfg (platform/linux/Launcher.h:81, Launcher.cpp:404-411).
2. **RetroArch "is running" is never true on Linux** (NanoMenuPS3Folder.cpp:1064). Same for the other package-name /proc scans.
3. **packages.list is absent,** so app-installed checks fail open in some places and closed in others (the 7 sites above), with no platform answer. The Linux launcher refuses every package except RetroArch: "no Linux emulator for package" (Launcher.cpp:414-415).
4. **Device keys fall back to the RG Cube's on every Linux device** (NanoDeviceKeys.cpp:181).
5. **The typec role flipper runs on Linux, as root, against whatever /sys/class/typec exists:**
   - The thread starts at NanoMenu.cpp:4040.
   - It is gated only on sys.boot_completed, which platform/linux/ControlKeys.cpp:82 sets.
   - It writes port_type (NanoMenuMtp.cpp:351, 353) and takes /sys/power/wake_lock (:348).
   - Its ctl.restart of nano-typec-guard is refused (ControlKeys.cpp:64), so **the crash guard it relies on is gone while the write still happens.** This Cube-specific port policy can reach RK3566 typec controllers.
6. **The USB-mode tick also runs every loop (NanoMenu.cpp:4039, NanoMenuMtp.cpp:368-420).**
   - It reads /sys/class/udc and power_supply/usb.
   - It shells "gammaos-net usb <mode>" (NanoMenuMtp.cpp:180-195), which reaches the gate's GammaosNet route rather than a USB seam.
7. **The Control Center battery, GPU and thermal gauges read Android/Rockchip node names directly** (NanoControlCenter.cpp:201-261), while the Linux Power seam already does battery discovery properly.

## Assessment against the owner's principle

**The design records rather than abstracts.**
- path-roots.json is a good ledger of where paths go. It is not abstraction.
- Every /data, /system/etc, /system/fonts, /sdcard and /mnt/media_rw literal (about 450 of the 784) works on Linux only because the Linux root imitates Android's layout. The Android-named state stays: /data/system/nano_*, Android/data/<pkg>, /data/data/<pkg>.
- The comment at IMPLEMENTATION.md:428 defers a Paths interface.

**Existing seams are bypassed by shared code:**
- Volumes: 5 call sites vs 184 /storage + /mnt/media_rw literals.
- Power::readBattery vs direct battery reads in NanoControlCenter.cpp and NanoMenuMtp.cpp.
- Display vs /dev/dri/card0 (5 sites).

**Hardware nodes with no seam at all:** backlight (by design, NanoPlatform.h:201-202), GPU, thermal, rfkill/wlan0, typec, udc/configfs/MTP, wakelocks, baseparameter, device tree.

**Missing seams implied by this inventory:**
- **Paths:** state root, asset root, font lookup, primary storage, volume root and raw alias.
- **DeviceInfo / Sensors:** battery, GPU, CPU, thermal, backlight nodes, radio state, panel calibration.
- **Apps:** installed?, app home/config location, running?
- **UsbPort / Typec:** a capability Linux reports as unsupported, so it is a no-op, instead of the shared loop poking sysfs.
- **DeviceKeys:** its source should be the Linux device profile (linux/board/rk3566/devices/*.json) rather than an absent Android table.

**Not covered here:** how the ShellGate command families and property keys (persist.gammaos.rgb.*, fan, performance_mode) are handled belongs to the other audits. I only noted where they meet paths.
