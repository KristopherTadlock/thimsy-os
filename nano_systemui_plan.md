# Nano mode without SystemUI for build 43: debugging prompts, Settings stability, crash sources

Status: proposed 2026-11-27. Planning only: no code was changed, nothing was built, and no device was touched for this plan. Every technical claim is marked **[C]** (confirmed from source, the Thimsy patch or a device log), **[I]** (inferred) or **[U]** (unverified).

Owner reports (2026-11-27):
1. "When enabling wifi debugging in the android settings, I first have to boot android and then boot back into nano. The reason seems to be that a popup asks for confirmation to trust the network that nano isn't able to display."
2. "In general, I have also noticed instability sometimes when trying to access the android settings."

Evidence references:
- **P:N** is a line in `patches/thimsy/0001-thimsy.patch` (diff of the WSL AOSP tree `/aosp/gammaos` against BASE 0bc531e2).
- **BASE** files are the GammaOS base at 0bc531e2 (GammaOSNextDistribution-14), read on Windows through the blob:none clone in `work/android-launcher/upstream` (`git show FETCH_HEAD:<path>`). Shortened paths: `am/`, `wm/`, `adb/` are under `frameworks/base/services/core/java/com/android/server/`; `SystemServer.java` is `frameworks/base/services/java/com/android/server/SystemServer.java`; `FrameworkServicesModule.java` and `WMShellModule.java` are SystemUI's Dagger modules.
- **AOSP** is android-14.0.0_r67; LineageOS lineage-21.0 for Settings. LineageOS's `AdbDebuggingManager.java` is byte-identical to AOSP's.
- **Device evidence:** `out/dp-evidence/test43/settings/` (`dropbox_crashes.txt`, `state.txt`, `tombstone_head.txt`), from the Cube running test 41 plus the test 42 device patches, in Nano mode, uptime 13h40. Older live logcats: `out/dp-evidence/test42/A0/logcat.txt` (written **A0:N**) and `test42/A2/logcat.txt` (**A2:N**).

## 1. Summary for the owner

### What is wrong

- **Nano mode runs Android without SystemUI.** SystemUI is the part of Android that draws the status bar, the notification shade, the lock screen and many small system pop-ups. In Nano mode it is never started, on purpose, to save memory and keep Nano in charge of the screen. But it is not removed either. When something asks for one of its pop-ups, Android starts SystemUI, and SystemUI crashes about 0.2 s later because services it needs are switched off in Nano mode. Crash dialogs are hidden in Nano mode, so you never see this.
- **The "Allow wireless debugging on this network?" pop-up is one of those SystemUI pop-ups.** When you turn on Wireless debugging in Nano mode, Android asks SystemUI to show it, SystemUI crashes, nothing appears, and the switch flips back off. In Android mode SystemUI is running, so the pop-up shows, and "Always allow on this network" saves the network. That is why your workaround works. The "Allow USB debugging?" pop-up for a new computer is very probably broken the same way, if your build asks for USB authorisation (most likely it does; the Phase 0 check confirms it) [I].
- **The crashed pop-up is very likely the main cause of the Settings instability.** Android keeps the pop-up that never appeared as an invisible, dead window on top. Each time it comes to the front again, Android restarts SystemUI, which crashes again. In one capture, after you moved between Settings pages, it came back to the top, and at the same moment the Settings page was hidden and Android waited for a window to take the controller [C for the log lines, I that the dead pop-up hid the page]; how long the controller stays dead each time was not measured [U]. Nano also sees nothing in front while this happens [I]. In that capture it came back after Settings was closed, and again 30 minutes later when a RetroArch game ended. Every SystemUI start in every log we have (12 of 12) was this dead pop-up. Two later crash bursts on your Cube (13 crashes on 11-26 evening) have no log yet, so their cause is not known [U]. A reboot clears it, and `adb shell am force-stop com.android.systemui` probably does too [I, not tried].
- **Other things make Settings worse in Nano mode, but less often.** Some Settings pages also open SystemUI pop-ups or ask SystemUI for data (brightness level, "Play media to", lock screen shortcuts, wallpaper and style), and System > Backup can crash Settings because the backup service is off in Nano mode. A few other pages use services that are also off in Nano mode (Screen saver, Accessibility, Wallpaper & style) and have not been checked [U]. The device is also busy: a vendor kernel thread for the built-in controller uses about half a CPU core all the time, and gammapad writes up to 600 log lines a second.
- **Things that never show in Nano mode, because they are drawn by SystemUI:** notifications (for example "Wireless debugging connected", or the USB mode notification; USB settings are in Settings > Connected devices > USB instead), plain text toasts, including Settings' own short messages, and Android's own screenshot shortcut. Nano's own screenshot is not affected [I]. Not affected either, because they are not SystemUI: the folder picker, app permission questions, the "Open with" chooser and Bluetooth pairing [I].
- **Seedvault (Android backup) crashes each time a USB device is plugged in** in Nano mode, for the same reason as System > Backup. It is harmless noise. The one Bluetooth crash was a slow start on 2026-11-25 and is not linked to Settings.

### What we'll change

1. **The two debugging pop-ups get a home that works in Nano mode.** In Nano mode, Android shows them from Settings (which runs fine in Nano mode) instead of SystemUI. They look like Android's own: the network name and address, or the computer's key fingerprint, an "Always allow" box, and Allow / Cancel, usable with the controller. Android mode keeps SystemUI's pop-ups as today.
2. **Nano mode stops starting SystemUI at all.** Anything that would start it gets a clean "no" instead: the pop-up just does not open, and no dead window is left behind. Settings pages that open SystemUI dialogs then do nothing in Nano mode instead of crashing; they keep working in Android mode.
3. **Smaller fixes:** System > Backup no longer crashes Settings in Nano mode; Seedvault is not started in Nano mode (recommended; your choice, D4); gammapad stops its log flood.
4. **Later:** a Nano pop-up for when Nano's own screen is in front (a new computer plugged in at the Nano home, for example), and optionally a Wireless Debugging row in Nano's own settings.

Nothing ever allows debugging without you pressing Allow.

### What you can do today

- **Wireless debugging:** keep using your workaround. Boot Android once while on the Wi-Fi network you use, turn on Wireless debugging, tick "Always allow on this network" and press Allow. Back in Nano mode it works on that network. The trust is per access point (BSSID): a different router, a mesh node or the other band (2.4 GHz vs 5 GHz) has a different address and needs the same step again [I].
- **If you already have USB debugging working with your PC,** `adb tcpip 5555` and then `adb connect <cube-ip>:5555` gives network adb without the pop-up, until the next reboot [I: standard adbd behaviour, not tried on Thimsy].
- **Avoid turning on Wireless debugging in Nano mode on a network you have not trusted yet.** If you did and Settings stops responding to the controller or flickers, close Settings from the Nano menu, then reboot to clear the dead pop-up. With USB adb, `adb shell am force-stop com.android.systemui` probably clears it without a reboot [I, not tried; SystemUI is not running in Nano mode, so this only removes the dead pop-up].

### What it costs

Phase 1 (both pop-ups in Nano mode, SystemUI never started in Nano mode) is medium work, about 3 to 4 days, low to medium risk. It fixes report 1 and is expected to fix the main part of report 2. It can ship in two steps: Phase 1a (about 1 day) only stops Nano mode from starting SystemUI, which removes the dead pop-up; Phase 1b adds the working pop-ups. Phase 2 (smaller fixes) is about 1 day. Phase 3 (Nano's own pop-up) is about 3 to 4 days. Section 7 has the table.

### Decisions needed first

Section 8. The ones that shape Phase 1: D1 (new pop-ups in Nano mode only), D2 (refuse SystemUI in Nano mode, and accept that its dialogs do nothing there), D3 (what happens to a pop-up while Nano's own screen or a game is in front, until Phase 3).

## 2. How Nano mode treats SystemUI

- **Nano mode is the runtime property `sys.gammaos.minimal_boot=1`** [C]. SystemServer reads it once (BASE SystemServer.java:1562-1566). The Nano/Android switch is `persist.bootanim.skip_nano` ("0" means Nano) [C, upstream gammaos-nano main.cpp:956-970]. Thimsy adds services back to minimal boot one by one the same way (RestrictionsManagerService, P:173461-173489) [C].
- **SystemUI is not started, not disabled** [C]:
  - `startSystemUi()` runs only `if (!minimalBoot)` (BASE SystemServer.java:4097-4100); the else branch logs "skipping SystemUI" (4159).
  - The package stays installed and enabled, and its manifest still says `android:persistent="true"` (BASE SystemUI AndroidManifest.xml:377).
  - AMS skips persistent apps in Nano (BASE ActivityManagerService.java:7179-7199) and never restarts a dead one ("GammaOS Nano: not restarting persistent app", 13846-13866; seen at A0:4083).
  - The window manager policy skips keyguard (BASE PhoneWindowManager.java:8775-8779). Nano draws its own volume and brightness overlays (530-535).
- **Services SystemUI needs are not started in Nano** [C] (BASE SystemServer.java): StatusBarManagerService (`statusbar`, 2174-2182), DreamManagerService (3103), BackupManagerService (3024-3027), MediaProjectionManagerService (3320-3322), WallpaperManagerService (2571-2575), AccessibilityManagerService (1897-1903).
- **Crash dialogs are hidden in Nano** (BASE AppErrors.java:1058-1061), so every crash below is silent [C].
- **Thimsy's own patch is not the cause** [C]. Its SystemUI changes are Quick Settings strings and tiles only (P:165734-165812). Nothing in the patch touches how SystemUI starts, its Dagger modules, `AdbDebuggingManager`, `AdbService` or the `config_customAdb*` strings.
- **RescueParty is off** [C]: "RescueParty: Disabled because of manual property" (A0:39924). The SystemUI crash bursts cannot trigger its settings resets or reboots.

## 3. Evidence

### 3.1 Test 43 device state (Nano mode, uptime 13h40)

- No `com.android.systemui` process; `com.android.settings` running; `adb_wifi_enabled=0` [C, state summary].
- **25 SystemUI crashes** in `dropbox_crashes.txt`, all the same [C]: "Unable to get provider com.android.systemui.keyguard.CustomizationProvider: java.lang.NullPointerException: Cannot return null from a non-@Nullable @Provides method", through `WMComponentImpl.getShell` <- `SystemUIInitializer.init:140` <- `SystemUIAppComponentFactoryBase.instantiateProviderCompat`. Each process lived 150-370 ms (one 1045 ms). They come in bursts:

  | When | Crashes |
  |---|---|
  | 11-25 08:17:38-08:18:37 | 5 (the A0 capture below), plus 08:48:59 (A2) |
  | 11-26 08:51:24-29 | 4, plus 08:57:20 and 08:57:35 (the test 42 t3 capture, all WifiDebuggingActivity) |
  | 11-26 19:43:08-33 | 5 (uptime 95-120 s) |
  | 11-26 21:51:58-22:07:27 | 8 (uptime 165-1094 s, the current boot) |

- **40 Seedvault crashes**: `com.stevesoltys.seedvault` App.onCreate -> Koin `IBackupManager` factory -> NullPointerException "asInterface(...) must not be null" [C].
- **One Bluetooth SIGABRT**, 2026-11-25 08:59:42, `isPrevious: true`: "assertion 'init_status == std::future_status::ready' failed - Can't start stack, last instance: starting AclSchedulerModule" (`tombstone_head.txt`) [C].
- **Load average about 15 on 8 cores.** Top: kernel thread `[joypad_spi_read]` 52% CPU (155 min CPU time), `gammaos-nano --overlay` 37%, surfaceflinger 33% [C, state summary].

### 3.2 A live crash burst (test 42 A0 and A2, 11-25)

- A0:3838: `START u0 {flg=0x10000000 cmp=com.android.systemui/.wifi.WifiDebuggingActivity (has extras)} ... from uid 1000 ... result code=0`. This is AdbDebuggingManager asking for the Wi-Fi network prompt [C].
- A0:3851: `Start proc 7140:com.android.systemui/u0a66 for next-top-activity {...WifiDebuggingActivity}`; A0:4031-4065 the fatal exception in `SystemUIInitializer.init`; A0:4083 "not restarting persistent app" [C].
- A0:4371: a second toggle: the same START, `result code=3` (delivered to the existing top record). A0:4387: SystemUI started again "for top-activity" [C]. In the t3 capture a second START returns `result code=0` (t3:214363, `test42/t3/T3-logcat-4k60.txt`), a second dead record, so records can stack [C].
- A0:11746, A0:13523, A0:39734: SystemUI restarted "for top-activity WifiDebuggingActivity" with no new START: the dead record is on top again each time [C].
- A0:11746-11862: Settings loads a window transition (a page change), SystemUI is restarted for the dead record (11746), the input dispatcher logs "Waiting because no window has focus but ActivityRecord{... com.android.settings/.SubSettings} may eventually add a window ... Will wait for 5000ms" (11784), and the SubSettings window goes invisible (11862) [C]. So the dead record came back on top of Settings and hid its page [I]. How long input was lost is not logged [U]. The only other focus wait (A0:2289, 08:17:35) is before the toggle at A0:3838: an ordinary page change, unrelated. No log has "Input dispatching timed out" [C].
- "Skip zero pid ANR, process=com.android.systemui" (A0:13137, 13555, 20718, 22522, 48742) each fire 15 s after a SystemUI start (3851, 4387, 11746, 13523, 39734). That is AMS's start-up timeout, `BIND_APPLICATION_TIMEOUT` = 15 s ("failed to complete startup", BASE ActivityManagerService.java:585, 5006-5009, 5245-5252), firing for the process that died in bindApplication; AnrHelper drops it because the pid is 0. They are not input timeouts [C].
- A0:13484-13504: Nano closes Settings ("closing standalone app com.android.settings, force-stopping"), and all Settings activities are removed. **A0:13523, 9 ms later: SystemUI is started again for the WifiDebuggingActivity record.** The dead record is not in Settings' task and survives closing Settings [C].
- A2:112770 (11-25 08:48:58, 30 minutes later): RetroArch exits (A2:~112750), and SystemUI is started again "for top-activity WifiDebuggingActivity" [C]. The dead record outlives games too.
- **Every SystemUI start in every captured test 42 logcat (12 of 12: A0 5, A2 1, t3 6) is for WifiDebuggingActivity** [C]. The test 43 bursts on 11-26 at 19:43 and 21:51-22:07 have no logcat, so their trigger is unknown [U] (section 9). None was for a content provider. `CustomizationProvider` appears in the stack only because it is the first provider SystemUI installs while starting [C, stack].
- gammapad logs "Device maps set" 111,347 times in A0's 124,382 lines (about 550-600 lines/s) [C].

### 3.3 Source

**Why SystemUI dies on start** [C source, I mapping to the obfuscated stack]:
1. `SystemUIInitializer.init` builds the WM component (`setupWmComponent`, line 83), then calls `mWMComponent.getShell()` in the SysUI builder chain (BASE SystemUIInitializer.java:73-103, getShell at 91).
2. `ShellInterface` needs the `@ShellCreateTrigger` object (BASE WMShellBaseModule.java:836-842), which takes `Optional<BubbleController>` (914-938).
3. `WMShellModule.provideBubbleController(..., IStatusBarService statusBarService, ...)` (BASE WMShellModule.java:157-194, line 166).
4. `FrameworkServicesModule.provideIStatusBarService()` is `@Singleton`, not `@Nullable`, and returns `IStatusBarService.Stub.asInterface(ServiceManager.getService(Context.STATUS_BAR_SERVICE))` (BASE FrameworkServicesModule.java:368-373). `statusbar` is not registered in Nano (SystemServer.java:2174-2182, inside the `!minimalBoot` block 2141-2249), so it returns null and Dagger throws [C]. The obfuscated stack fits: three WM-level provider frames, then one global-root frame [I, high]. BubbleController's other global singletons (WindowManager, UserManager, IWindowManager, LauncherApps; LauncherAppsService starts in minimal boot, SystemServer.java:3309-3313) are non-null, and `statusBarService` is evaluated first [C].

Fixing this one provider would not be enough: `provideIDreamManager` (283-288) and `provideIMediaProjectionManager` (425-430) are also non-null and would fail next [C].

**The wireless debugging flow** (AOSP, unchanged in BASE) [C]:
1. Settings' switch writes `Settings.Global.ADB_WIFI_ENABLED=1` (WirelessDebuggingEnabler.java:118-125).
2. AdbService sees it and posts `MSG_ADBDWIFI_ENABLE` (AdbService.java:183/201/217; AdbDebuggingManager.java:1674).
3. `MSG_ADBDWIFI_ENABLE` (adb/AdbDebuggingManager.java:1067-1105) calls `verifyWifiNetwork(bssid, ssid)` (1422-1430). A trusted BSSID starts wireless adbd. Otherwise it calls `startConfirmationForNetwork(ssid, bssid)` and **immediately sets `ADB_WIFI_ENABLED` back to 0** (1079-1086). That is the switch flipping back.
4. `startConfirmationForNetwork` (1523-1539) starts `config_customAdbWifiNetworkConfirmationComponent` = `com.android.systemui/com.android.systemui.wifi.WifiDebuggingActivity` (core config.xml:3272-3273) with the extras `ssid` and `bssid`. It tries an activity, then a service, of that component.
5. `startConfirmationActivity` (1571-1585) checks only `resolveActivity` and returns true once the start is requested. Nothing checks that the prompt appeared; there is no fallback.
6. The answer: `IAdbManager.allowWirelessDebugging(alwaysAllow, bssid)` or `denyWirelessDebugging()` (WifiDebuggingActivity.java:196-219), gated by `MANAGE_DEBUGGING` (AdbService.java:351-361; signature|privileged). `MSG_ADBWIFI_ALLOW` (1120-1154) adds the BSSID to the trusted list (`/data/misc/adb/adb_temp_keys.xml`) only when "always" is set, then sets `ADB_WIFI_ENABLED=1` and `persist.adb.tls_server.enable=1`.
7. On the next boot, AdbService reads `persist.adb.tls_server.enable` (systemReady, 258-284) and enables wireless debugging (bootCompleted, 289-295). A trusted BSSID needs no prompt. That is why the owner's workaround holds [C].
8. **The USB key prompt is the same shape:** `startConfirmationForKey` (1541-1566) starts `config_customAdbPublicKeyConfirmationComponent` = SystemUI's `.usb.UsbDebuggingActivity` (config.xml:3260-3267) with `key` and `fingerprints`; the answer is `allowDebugging` / `denyDebugging` (UsbDebuggingActivity.java:184-186). It is equally broken in Nano [I, high]. It only matters when adbd requires authorisation (`ro.adb.secure=1`). The build is a LineageOS userdebug GSI (`lineage_arm64_bgN ... userdebug/test-keys`, `tombstone_head.txt`), and LineageOS sets `ro.adb.secure=1` on non-eng builds [I, not checked here]; neither the patch nor the repo sets it. So the USB prompt is very likely live for any new PC; Phase 0 confirms. The Wi-Fi prompt does not depend on it: AdbService creates AdbDebuggingManager unconditionally (BASE AdbService.java:248) [C].
9. The pairing-code and QR dialogs are Settings' own (AdbWirelessDialog, AdbQrcodeScannerFragment) and are not affected [C].

**Settings code that reaches SystemUI or missing services** [C code, LineageOS lineage-21.0 Settings]:
- `CustomizableLockScreenQuickAffordancesPreferenceController.getAvailabilityStatus()` (lines 42-43) calls `CustomizableLockScreenUtils.isFeatureEnabled()`, which queries `content://com.android.systemui.customization/flags` (CustomizableLockScreenUtils.java:40-43, 76-86). Its own comment calls it "a slow, blocking call that shouldn't be made on the main thread". It runs only when an `ACTION_SET_WALLPAPER` activity exists, and ThemePicker is shipped (APP-REVIEW.md:62).
- Display > Brightness level starts `ACTION_SHOW_BRIGHTNESS_DIALOG` (BrightnessLevelPreferenceController.java:141-149), SystemUI's BrightnessDialog (SystemUI manifest 823-825).
- Media output and the volume panel are SystemUI receivers (SystemUI manifest 1054-1069).
- System > Backup: `BackupSettingsHelper` takes `asInterface(getService("backup"))`, which is null in Nano, and calls `getDataManagementIntent(...)` catching only RemoteException (BackupSettingsHelper.java:43-44, 163-188; UserBackupSettingsActivity.java:57-66). A NullPointerException would kill Settings [I: code path, not seen on the device].
- Nano's own source already notes "the minimal_boot absent-service crashes the full Settings hits" (P:43323) [C].
- **Not audited yet** [U]: Settings pages backed by other services that are off in Nano (section 2): Display > Screen saver ("Start now" calls `IDreamManager`), Accessibility (AccessibilityManagerService), and ThemePicker / Wallpaper & style without WallpaperManagerService. Each may crash on its own, independently of SystemUI. The soak (6.3 step 8) and Phase 0's `data_app_crash` and exit-info checks cover them.
- **Shown by SystemUI, so never visible in Nano mode** [I]: notifications (no status bar or shade; "Wireless debugging connected", the USB mode notification), text toasts from apps targeting API 30+, Settings included (TextToastRecord.java:61-64 needs the status bar), and the system screenshot chord (PhoneWindowManager binds SystemUI's TakeScreenshotService; today that bind would start and crash SystemUI, after Part B it is refused). Nano's own screenshot (`maybeNanoScreenshot`, P:24943) does not use SystemUI.
- **Not SystemUI, so not affected** [I]: the SAF folder picker (DocumentsUI; Nano already has NanoUriGrant), runtime permission dialogs (PermissionController), the app chooser (IntentResolver), and Bluetooth pairing (Settings' own BluetoothPairingDialog; Nano's own pairing ask, P:164020-164130).

**A waiting provider client** [C code]: when a provider's process dies while launching, `ContentProviderHelper.removeDyingProviderLocked` (1773-1830) restarts it up to `MAX_RETRY_COUNT` times for a waiting client, then kills clients with a stable connection ("depends on provider ... in dying proc"). For a SystemUI provider in Nano that means up to 4 fork-and-crash cycles while the caller waits, then possibly a killed caller [I; not seen on the device].

## 4. Root causes, ranked

1. **[C] The debugging confirmations point at SystemUI, which crashes on start in Nano.** AdbDebuggingManager counts the start request as success and turns the switch back off (3.3 steps 3-5). This is owner report 1. The USB key prompt has the same cause [I, high].
2. **[C dead record and restarts; I for the effect on the owner] The dead WifiDebuggingActivity record stays on top.** Each time it is the top activity again (a second toggle, a Settings page change, Settings closing, a game ending), AMS restarts SystemUI, which crashes: repeated fork-and-crash churn [C]. Once, after a Settings page change, Settings' page window went invisible and the input dispatcher waited for a focused window (A0:11746-11862) [C]; how long the controller is lost is not measured [U]. Nano sees nothing in front while the record is resumed (Nano's `overlayResolveForegroundPkg()` deliberately returns "" for `com.android.systemui`, NanoMenuOverlay.cpp, BASE about 310-314, about 335-338 after the patch) [I for the effect on Nano's return logic]. To the owner this probably looks like Settings ignoring the controller, flickering or relaunching [I]. It is the cause behind every SystemUI start captured (12 of 12), so it is very likely the main part of owner report 2 [I]; the two uncaptured test 43 bursts may have another trigger [U]. It lasts until a reboot, or probably `am force-stop com.android.systemui` (force-stop finishes a package's activity records even with no process) [I, not tried].
3. **[I] Other Settings paths that start SystemUI.** Brightness level, media output, the volume panel, lock screen shortcuts (a blocking provider query, up to 4 crash cycles), wallpaper and style. Each leaves either a dead activity record (as in 2) or a stalled caller. Not seen in the captures.
4. **[I] Settings code that does not handle a missing service.** System > Backup can crash Settings. Many other missing services (fingerprint, face, biometrics, pac_proxy, safety_center, trust, system_update, oem_lock, blob_store) are caught and only logged: noise and slowness, not crashes [C for the log lines].
5. **[C partly, I] Load.** `[joypad_spi_read]` at 52% of a core, `gammaos-nano --overlay` at 37% and surfaceflinger at 33% while Settings is in front, and gammapad's per-batch log line (BASE InputTransformer.cpp:210-216, called for every input batch at GamepadManager.cpp:1022-1026). This makes every wait in 2 and 3 longer. A load average of 15 may include threads waiting in the SPI driver (D state), which count toward load without using CPU [I]. The crash churn itself (forking SystemUI and building its Dagger graph) adds to it.
6. **[C] Seedvault on USB attach.** All 10 Seedvault starts in the test 42 logcats (A0, A1, A2, A4, B2-auto1080, q1440 2, t3 3; some logs may overlap) are "for broadcast UsbIntentReceiver" (USB_DEVICE_ATTACHED; for example A2:24434). Its `App.onCreate` needs the backup service, which is off in Nano (BASE SystemServer.java:3024-3027). Each is followed 15 s later by "Skip zero pid ANR, process=com.stevesoltys.seedvault" (10 times): the same AMS start-up timeout as for SystemUI (3.2), not a broadcast timeout [C]. The broadcast moves on when the process dies, so any delay to other USB receivers is about the crash time (well under a second) [I]. Background noise; not why Settings is unstable.
7. **[C/I] Bluetooth SIGABRT, once.** `StackManager::StartUp` waits `bluetooth.gd.start_timeout` (default 3000 ms; stack_manager.cc:49-58, 98-104) and then asserts. "last instance: starting AclSchedulerModule" means the module start-up ran past 3 s: a slow controller or CPU contention at start [I]. com.android.bluetooth restarts on its own. Unrelated to SystemUI and Settings, except the Bluetooth page while it restarts.

Not causes: RescueParty (off, A0:39924) [C]; Thimsy's SystemUI changes (strings and tiles only) [C].

## 5. Design

### 5.1 Rules for every change

- **Gate on `sys.gammaos.minimal_boot` only**, read once per boot. Never on a `persist.` property, so Android mode stays exactly as it is.
- **Never `pm disable` SystemUI or Seedvault**, and never `setApplicationEnabledSetting`. That state persists into Android mode and could leave Android without SystemUI.
- **Never auto-allow** a network or a key. The "Always allow" box keeps SystemUI's default and stays the user's choice.
- **Pure decisions in a host-testable class**, as `GammaosScreenArrangement` and `NanoUriGrant` already are.

### 5.2 Part A: the debugging prompts, hosted in Settings in Nano mode (Phase 1)

Why Settings: it runs fine in Nano mode (it was running in test 43) [C]; it is `android:sharedUserId="android.uid.system"` and already holds `MANAGE_DEBUGGING` (Settings AndroidManifest.xml:5, :81) [C]; the Thimsy patch already carries Settings code and strings (P:179399-180683) [C]; and Nano's foreground tracking keeps seeing `com.android.settings` while the prompt is up [I].

New activities in `packages/apps/Settings/src/com/android/settings/handheld/` (next to `GammaOSToolboxFragment.java`):
- **`ThimsyWifiDebuggingActivity`**, ported from SystemUI's `WifiDebuggingActivity`:
  - extends `com.android.internal.app.AlertActivity` (Settings builds with platform APIs);
  - reads `ssid` and `bssid`, shows them, with the "Always allow on this network" box;
  - Allow calls `IAdbManager.allowWirelessDebugging(always, bssid)`; Cancel, Back and finishing without an answer call `denyWirelessDebugging()`;
  - a Wi-Fi change receiver denies and finishes when the network changes, as SystemUI's does;
  - keeps the obscured-touch filter (`touch_filtered_warning`);
  - Allow and the box take d-pad focus; Allow is not the initial focus (so a held button can't confirm by accident) [proposed];
  - `onNewIntent`: AdbDebuggingManager starts the prompt with NEW_TASK, so a repeat request reaches the existing instance (A0:4371, `result code=3`). The activity sends deny for the old request, then replaces ssid/bssid (or key/fingerprints) and redraws, so Allow never answers a stale request (for USB that would authorise the previous key) [C delivery; proposed handling];
  - window flags: SystemUI's activities call `window.addSystemFlags(SYSTEM_FLAG_HIDE_NON_SYSTEM_OVERLAY_WINDOWS)` and `window.setType(TYPE_SYSTEM_DIALOG)` (WifiDebuggingActivity.java:67-69, UsbDebuggingActivity.java:66-68) [C]. Keep the overlay-hiding flag (Settings holds `HIDE_NON_SYSTEM_OVERLAY_WINDOWS`, manifest :116). Proposed: drop `TYPE_SYSTEM_DIALOG`, so the prompt stays an ordinary activity window that takes focus normally and that Nano's foreground tracking sees as `com.android.settings`; a system-type window on an activity gets its own window token and behaves differently over games and Nano [I]. Decided at implementation and tested in 6.3 step 5.
- **`ThimsyUsbDebuggingActivity`**, ported from `UsbDebuggingActivity`:
  - reads `key` and `fingerprints`, "Always allow from this computer";
  - Allow / Cancel call `allowDebugging(always, key)` / `denyDebugging()`;
  - a plain `registerReceiver` for `UsbManager.ACTION_USB_STATE` (denies and finishes on unplug) replaces SystemUI's BroadcastDispatcher;
  - the same `onNewIntent` and window-flag rules as the Wi-Fi prompt.
- **If Nano force-closes Settings while a prompt is up** (hold-Back, `nanoStopAppToHome`, P:169961), the process is killed without a deny. For Wi-Fi that is harmless (the switch is already off). A USB key request stays pending until the cable is plugged in again [I]. Recorded under D3 and tested in 6.3 step 5. Since the prompts have their own `taskAffinity`, a later option is for Nano's close to skip that task.
- Manifest entries (`packages/apps/Settings/AndroidManifest.xml`): `android:exported="false"`, `android:permission="android.permission.MANAGE_DEBUGGING"`, `android:excludeFromRecents="true"`, `android:finishOnCloseSystemDialogs="true"`, `android:noHistory="true"`, their own `taskAffinity` (so they never mix with a Settings back stack), theme `@android:style/Theme.DeviceDefault.Dialog.Alert`. system_server may start a non-exported activity [C: same uid].
- Strings in new per-locale files `res/values*/thimsy_adb_strings.xml` (the `gammaos_toolbox_strings.xml` pattern), copied from SystemUI's own AOSP translations of the four Wi-Fi and four USB dialog strings (`wifi_debugging_title`, `_message` (2 args: ssid, bssid), `_always`, `_allow`; `usb_debugging_title`, `_message` (1 arg: fingerprint), `_always`, `_allow`; BASE SystemUI res/values/strings.xml:134-176 [C]), plus `touch_filtered_warning`, under `thimsy_` names so they never collide with Settings' strings. Cancel is `android.R.string.cancel` [C, WifiDebuggingActivity.java:90].

The redirect, in `adb/AdbDebuggingManager.java` (about 15 lines):
- In `startConfirmationForNetwork` and `startConfirmationForKey`, when Nano mode is on, use the Thimsy component instead of the `config_customAdb*` string. If it doesn't resolve, fall back to the stock component (today's behaviour).
- Log one line: `GammaOS Nano: adb confirmation via <component>`.
- `config.xml` stays untouched, so Android mode keeps SystemUI's pop-ups (D1).

**When Nano's own screen is in front** (no Android app launched yet, or the Nano overlay home): an Android activity there may be invisible, unreachable, or may trigger the launch hand-off (the same reason Bluetooth pairing prompts were moved to Nano, P:164030-164060) [C precedent, U for this case]. Phase 1 then does not start any activity (D3):
- the Wi-Fi request is dropped with a log line. Wireless debugging stays off, as today, and the user can toggle it again from Settings, where the prompt does show;
- the USB key request stays pending in AdbDebuggingManager, and the PC shows "unauthorized" until it is plugged in again with an Android app in front (or until Phase 3).
- The signal for "Nano's own screen is in front": candidate `sys.gammaos.nano.app_launched` (0 means no Android app launched). `nanoStopAppToHome` sets it to 0 (P:169979), and BASE's AMS `handleAppDied` hook already reads it (BASE ActivityManagerService.java:~3506) [I; picked at implementation, section 9].

**When a game is in front** (a USB key request while RetroArch or another emulator runs; the Wi-Fi request normally comes from Settings, so it is in front already): the Settings prompt starts as a new task and becomes the foreground app [I]. That pauses RetroArch, changes gammapad's foreground app and Nano's `overlayResolveForegroundPkg()` result, and moves the Quick Menu's close target to Settings [I]. After Allow or Cancel the game's task should come back to the front [U]. Risk: a game that does not handle pause well, or Nano treating the prompt as a launch hand-off. D3 covers it (prompt over games, or hold the USB request while a game is in front), and 6.3 step 5 tests it.

### 5.3 Part B: never start SystemUI in Nano mode (Phase 1)

A new pure class `frameworks/base/services/core/java/com/android/server/gammaos/NanoProcessGuard.java`:
- `static boolean refuses(String packageName, boolean minimalBoot, Set<String> extra)`: true in Nano mode for `com.android.systemui`, and for the packages in `extra` (D4: Seedvault).
- The `minimalBoot` value and the list are read once at boot and cached.

Two call sites, each with one log line per component per minute (`GammaOS Nano: refused <kind> <component>: <package> doesn't run in Nano mode`):
1. **Activities:** `wm/ActivityStarter.executeRequest`, next to the IntentFirewall `abort |=` checks, with a null check on `aInfo`. A refused target takes the existing abort path (BASE ActivityStarter executeRequest abort block, about 1254-1262): the caller's result gets `RESULT_CANCELED` and the start returns `START_ABORTED` [C]. No activity record is created, so nothing is left on top. This removes cause 2 for every SystemUI activity (brightness dialog, media output, USB permission dialogs, and the stock debugging prompts if anything still reaches them). A refused stock fallback still looks like a successful start to AdbDebuggingManager (no exception), which is acceptable: the switch is off and nothing is left behind.
2. **Processes (services, broadcasts, providers):** the top of the `ProcessRecord startProcessLocked(String processName, ApplicationInfo info, ...)` overload (BASE ProcessList.java:2572) returns null for a refused package, before a ProcessRecord is created. The other overloads (1786, 2564) are reached only through persistent and restart paths, which Nano already skips for SystemUI [C overloads; I, high for the callers]. Activity process starts also arrive here, so call site 1 is still needed to stop the activity record being created. ActiveServices logs "unable to launch app" and the broadcast queue skips the receiver [I: check each caller at implementation]. For providers, `ContentProviderHelper.getContentProviderImpl` then returns null at once ("process is bad", about 556-568): a query gets a null cursor (CustomizableLockScreenUtils handles that [C]); `ContentResolver.call` throws "Unknown URI" [I]. No 4x retry and no waiting caller.
3. *Optional:* a check in `ContentProviderHelper.getContentProviderImpl` itself only adds a clearer log line (Thimsy already patches this file, P:166706) [C]; not needed for correctness.

**Phase 1a (quick win):** call site 1 alone (or, smaller still, AdbDebuggingManager not starting the SystemUI component in Nano mode) removes the dead window behind report 2, without the new prompts. It can ship before Part A.

Why this is safe: in Nano mode SystemUI cannot run anyway. Every refused start would otherwise have forked SystemUI and crashed within 0.4 s. A refusal swaps a crash (and a dead record or a waiting caller) for a clean "no" [I, high]. Android mode never reaches the guard.

What the owner will notice in Nano mode (D2): Settings > Display > Brightness level, "Play media to", the volume panel launched from Settings, lock screen shortcuts and the SystemUI parts of Wallpaper & style do nothing. USB permission dialogs (an emulator asking for a USB device, for example a GameCube adapter in Dolphin) do not appear; the app does not get the permission (today they crash SystemUI and leave a dead record on top of the emulator) [I]. Phase 4 can give those a Settings-hosted dialog too.

### 5.4 Part C: Settings' System > Backup (Phase 2)

`packages/apps/Settings/src/com/android/settings/backup/BackupSettingsHelper.java`: if `mBackupManager` is null, or any call throws, use `getIntentForDefaultBackupSettings()` and the default label and summary, as `isBackupServiceActive` already does. About 10 lines. Optional: hide System > Backup in Nano mode (D5).

### 5.5 Part D: Seedvault in Nano mode (Phase 2, D4)

Recommended: add `com.stevesoltys.seedvault` to `NanoProcessGuard`'s list. Its process is then never started in Nano mode; USB attach no longer forks and crashes it, and the USB broadcast is no longer delayed. Android mode is unchanged. Alternatives: guard Seedvault's `App.kt` against a null `IBackupManager` (a change to a prebuilt app's source), drop Seedvault from the build, or start BackupManagerService in Nano mode (not recommended: it binds Seedvault's transport at boot and keeps it resident, which minimal boot avoids on purpose).

### 5.6 Part E: gammapad's log flood (Phase 2)

`frameworks/native/services/gammapad/InputTransformer.cpp` (Thimsy already patches it, P:175449) and `GamepadManager.cpp` (P:175251): log "Device maps set" only when the device or its maps change, and skip the per-batch map copy when the device is the same as the last batch. Cuts about 250-600 log writes a second, and keeps logcat long enough to hold crash evidence [C rate; I gain].

### 5.7 Part F: `[joypad_spi_read]` (measure only)

- It is the vendor SPI-MCU pad transport (`sprd,spi-mcu-joy`, SPI at about 1.2 MHz, buf-len 64; built into the vendor kernel 5.4.233) [C from the DT and kernel log, per the stability research]. Thimsy ships no kernel changes, so this is stock GammaOS and Anbernic behaviour [I]. No source is public [U].
- The built-in pad sends events even when idle: the singleadc-joypad input has fuzz=1, flat=1 over +/-16384 and a 4 ms poll [C], so ADC jitter becomes events [I].
- Nothing changes before the read-only checks in section 10 (priority, scheduling class, D state, CPU frequency). The only known knobs (the input-polldev `poll` interval, `init.joypad_control.rc`) trade input latency for games, so any change is a separate owner decision (D7).

### 5.8 Part G: Nano's own prompt (Phase 3)

For a request that arrives while Nano's own screen is in front (5.2), following the Bluetooth `--ask` precedent (P:164020-164130, `sys.gammaos.bt.pair_ask`):
- AdbDebuggingManager (Nano mode, Nano in front) keeps the pending request with a sequence number, writes `/data/system/nano_adb_confirm.txt` (`<seq>|usb|<fingerprint>` or `<seq>|wifi|<ssid>|<bssid>`; a full RSA key is too long for a property) and sets `sys.gammaos.adb.confirm_seq=<seq>`.
- Nano polls that property as it does for Bluetooth pairing, shows a native dialog with the same texts, and answers through `nanoNetBridgeCmd("adb_allow", "<seq>|<always>")` or `("adb_deny", "<seq>")`.
- New `NanoNetBridge` verbs `adb_allow` and `adb_deny` (dispatch at P:~332) check the sequence number and call `IAdbManager` from system_server. If a verb runs inside a binder call from Nano, wrap the `IAdbManager` calls in `Binder.clearCallingIdentity()`, or AdbService's `MANAGE_DEBUGGING` check sees Nano's uid [U]. A new request replaces the old one; USB unplug or a network change sends deny, so adbd never waits forever.
- Files: `adb/AdbDebuggingManager.java`, `gammaos/NanoNetBridge.java`, `cmds/gammaos-nano/NanoMenuSettings.cpp`, `NanoMenu.h`, the dialog code and the i18n JSON.
- Optional (D6): a "Wireless Debugging" row under Nano's Developer Options, next to USB Debugging (NanoMenuSettingsTree.cpp:733), showing the port and a pairing code from `WIRELESS_DEBUG_STATE_CHANGED_ACTION` and `enablePairingByPairingCode`.

### 5.9 Not chosen

- **Run SystemUI in Nano mode with its UI hidden.** It needs StatusBarManagerService, DreamManagerService, MediaProjection and probably Accessibility in minimal boot, then the status bar, nav bar, volume, keyguard and notification windows suppressed. About 150-250 MB and constant CPU on a device already at load 15, window and focus fights with Nano and RetroArch, and a large regression surface [I].
- **Headless SystemUI** (forcing `SystemUIInitializer` down its stub-shell path in Nano). Other non-null providers (IDreamManager, IMediaProjectionManager) may still fail, and every hit still costs a fork [U].
- **Point the `config_customAdb*` strings at Settings in both modes.** Simpler, but changes Android mode's pop-ups (D1).
- **A separate tiny privileged app for the prompts.** No Settings start-up cost, but a new package with a privapp-permissions entry, and a new foreground package for Nano to learn about. Kept as the fallback if Settings' cold start in Nano turns out slow (section 9).

### 5.10 Files

| File | Change | Phase |
|---|---|---|
| `frameworks/base/services/core/java/com/android/server/adb/AdbDebuggingManager.java` | Nano redirect of both prompts; Nano-in-front handling; later the Nano prompt hand-off | 1, 3 |
| `packages/apps/Settings/src/com/android/settings/handheld/ThimsyWifiDebuggingActivity.java` | new | 1 |
| `packages/apps/Settings/src/com/android/settings/handheld/ThimsyUsbDebuggingActivity.java` | new | 1 |
| `packages/apps/Settings/AndroidManifest.xml` | two activity entries | 1 |
| `packages/apps/Settings/res/values*/thimsy_adb_strings.xml` | new, every locale Settings ships | 1 |
| `frameworks/base/services/core/java/com/android/server/gammaos/NanoProcessGuard.java` | new pure class | 1 |
| `frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java` | refuse activities | 1 |
| `frameworks/base/services/core/java/com/android/server/am/ProcessList.java` | refuse process starts (services, broadcasts, providers) | 1 |
| `frameworks/base/services/core/java/com/android/server/am/ContentProviderHelper.java` | optional: clearer "refused provider" log line | 1 |
| `frameworks/base/cmds/gammaos-nano/test/java/NanoProcessGuardCheck.java` | new host check | 1 |
| `tests/unit/test_nano_process_guard.py`, `tests/unit/test_adb_confirm.py` | new host tests | 1 |
| `packages/apps/Settings/src/com/android/settings/backup/BackupSettingsHelper.java` | null-safe | 2 |
| `frameworks/native/services/gammapad/InputTransformer.cpp`, `GamepadManager.cpp` | log only on change | 2 |
| `frameworks/base/services/core/java/com/android/server/gammaos/NanoNetBridge.java` | `adb_allow`, `adb_deny` | 3 |
| `frameworks/base/cmds/gammaos-nano/NanoMenuSettings.cpp`, `NanoMenu.h`, dialog and i18n files | Nano prompt; optional Wireless Debugging row | 3 |
| `README.md` (Known issues, Developer options) | the Nano-mode behaviour and the per-network trust | 1 |

All framework and Settings changes go into `patches/thimsy/0001-thimsy.patch`, regenerated from `/aosp/gammaos` after the edits (never while an AOSP build runs in WSL).

## 6. Tests

### 6.1 Host tests (no device)

- **`tests/unit/test_nano_process_guard.py`** compiles `NanoProcessGuard.java` and `NanoProcessGuardCheck.java` from the files the patch creates (the `new_file` helper pattern of `test_screen_arrangement.py`), and checks:
  - Nano mode: `com.android.systemui` refused; Seedvault refused only when listed; `com.android.settings`, `android`, `com.android.networkstack` and an empty or null package never refused;
  - Android mode: nothing refused;
  - the cached value never comes from a `persist.` property (a text check on the patch).
- **`tests/unit/test_adb_confirm.py`**, text checks on the patch:
  - the `config_customAdb*` strings in `core/res/res/values/config.xml` are unchanged (D1);
  - both manifest entries are `exported="false"` with `permission="android.permission.MANAGE_DEBUGGING"`;
  - `AdbDebuggingManager`'s redirect is inside a `minimal_boot` check, and its fallback is the stock component;
  - every `thimsy_adb_strings.xml` has the same keys as the default one and the same format arguments (`%1$s`, `%2$s`);
  - among the lines the patch adds (`+` lines only; stock AdbService, IAdbManager and diff context contain these names), no call to `allowDebugging` or `allowWirelessDebugging` appears outside the two activities and (Phase 3) the bridge's `adb_allow`;
  - both activities override `onNewIntent` and send deny for the replaced request.
- **Phase 2:** extend the gammapad host check (if `test_nano_units.py`'s g++ suite covers InputTransformer; otherwise a text check) that "Device maps set" is logged only on change.
- **Phase 3:** a `NanoAdbConfirm` parse and seq-match check (same style as `test_nano_uri_grant.py`): stale seq refused, malformed lines refused, deny on replace.

### 6.2 Build checks

- Settings, services and gammapad compile in the full system build (after the AOSP build in progress finishes; never two builds at once).
- `aapt2 dump xmltree` of the built Settings APK shows both activities with the permission.

### 6.3 Device steps

Rules for every step: nothing touches dwc3 debugfs or `/sys/class/display/dispc1`; no `stop`/`start` with a monitor attached; boot without the monitor.

Phase 1, Nano mode:
1. **Wi-Fi prompt.** Use an access point (or band) not trusted yet. Open Settings from Nano, turn on Wireless debugging. The Thimsy prompt shows with the SSID and BSSID. Allow with "Always allow" ticked works with the controller only. The switch stays on, and `adb pair` / `adb connect` work. `logcat` shows "adb confirmation via com.android.settings/...".
2. Same, Cancel: the switch goes off; no SystemUI process started (`logcat -b events | grep am_proc_start` shows none for systemui).
3. Same, Allow without "Always allow": works until the next reboot; after a reboot the prompt shows again.
4. Change networks while the prompt is up: it closes as denied.
5. **USB prompt** (only if `ro.adb.secure=1`). Prefer a second PC that has never been authorised. Otherwise, on the test PC: `adb kill-server`, move `~/.android/adbkey*` aside, and **move them back afterwards** (then `adb kill-server` again). If the new prompt fails, authorise the PC once in Android mode to get adb back. Plug in with Settings in front: the Thimsy USB prompt shows; Allow works. Then with a game in front (per D3): the prompt shows over the game, the game pauses, and after Allow or Cancel the game comes back to the front with the controller, and the Quick Menu still closes the game; or, if D3 holds USB requests during games, no prompt and "unauthorized". Then at the Nano home: no prompt, PC shows "unauthorized", no crash; plugging in again with an app in front shows it. Also: a second request while a prompt is up replaces it (the old one is denied); Nano force-closing Settings with a prompt up leaves no crash (USB stays "unauthorized" until replug).
6. **No SystemUI ever.** After 1-5, open Display > Brightness level and Sound > "Play media to": nothing opens, Settings keeps the controller, logcat shows "refused activity" or "refused process", and `dumpsys dropbox --print system_app_crash` gains no SystemUI entry. Then open Wallpaper & style: no SystemUI process starts; ThemePicker may still crash on its own because WallpaperManagerService is off in Nano mode. Record any `data_app_crash` for it as a separate finding, not a failure of this step.
7. **No dead window.** After 6, close Settings from Nano, start and quit a RetroArch game: no "Start proc ... com.android.systemui", no "Skip zero pid ANR", `dumpsys window | grep mCurrentFocus` shows the game or nothing stale.
8. **Settings stability soak.** 15 minutes browsing every top-level Settings page with the controller, Developer options included, plus Display > Screen saver ("Start now"), Accessibility and System > Backup: no Settings crash, ANR or freeze (`dumpsys activity exit-info com.android.settings`, `dumpsys dropbox --print data_app_crash`). Crashes on those three pages are recorded as separate findings (missing services, not SystemUI).

Phase 1, Android mode (must be unchanged):
9. Wireless debugging and a new USB computer show SystemUI's own prompts. Brightness level and "Play media to" open SystemUI's dialogs. No "GammaOS Nano: refused" lines.

Phase 2:
10. In Nano mode, System > Backup opens without crashing Settings.
11. Plug and unplug a USB pad and the dock 5 times: no Seedvault start, no Seedvault crash, no "Skip zero pid ANR" for it (if D4 = guard). In Android mode Seedvault still works.
12. `logcat -d -s gammapad | grep -c "Device maps set"` after a minute of play: a handful, not tens of thousands.

Phase 3:
13. At the Nano home, plug in a new PC: Nano's own prompt shows; Allow and Deny both reach adbd; unplugging during the prompt closes it as denied.
14. A Wi-Fi prompt at boot on an untrusted network (if `persist.adb.tls_server.enable=1`): Nano's prompt, not a crash.

## 7. Phases

| Phase | Content | Effort | Risk |
|---|---|---|---|
| 0 | Owner's read-only checks (section 10): adb props, config overlays, what starts SystemUI, Settings exit reasons, load and the joypad thread | Small (under an hour) | None |
| 1a | Part B call site 1 (ActivityStarter refuses SystemUI activities in Nano mode), or at least AdbDebuggingManager not starting SystemUI in Nano mode; host test | Small (about 1 day) | Low |
| 1b | Part A (both prompts in Settings in Nano mode, Nano-in-front and game-in-front rules) and the rest of Part B (process starts), host tests, README | Medium (2-3 days; more if every locale's strings need checking) | Low-medium (process-start paths in AMS; gated on Nano mode) |
| 2 | Part C (System > Backup null-safe), Part D (Seedvault per D4), Part E (gammapad log only on change) | Small (about 1 day) | Low |
| 3 | Part G: Nano's own debugging prompt when Nano's screen is in front; optional Wireless Debugging row (D6) | Medium (3-4 days; plus 1-2 for the row) | Medium (Nano UI, a new bridge verb that grants debugging) |
| 4 (optional) | Settings-hosted USB permission, confirm and resolver dialogs in Nano mode for emulators that use USB devices; a text-toast fallback (text toasts need the status bar today, TextToastRecord.java:61-64) | Medium (2-3 days) | Medium |
| Measure | `[joypad_spi_read]` and overlay CPU, from Phase 0's output; any change is its own decision (D7) | Small to measure | None until a change |
| Watch | Bluetooth start timeout: raise `bluetooth.gd.start_timeout` only if the abort repeats | Tiny | Low |

Phase 1 (1a plus 1b, 3-4 days in all) fixes owner report 1 and is expected to fix the main part of report 2 (the dead record behind every captured SystemUI start); Phase 0's `am_proc_start` check says whether the uncaptured test 43 bursts had the same trigger. Phases 2-4 are independent of each other.

## 8. Owner decisions

Recommendations first.

- **D1. New prompts in Nano mode only** (recommended), so Android mode keeps SystemUI's pop-ups untouched. Alternative: use the Settings-hosted prompts in both modes (one code path, but Android mode changes look).
- **D2. Refuse SystemUI in Nano mode** (recommended). Brightness level, "Play media to", the volume panel from Settings, lock screen shortcuts, the SystemUI parts of Wallpaper & style and USB permission dialogs do nothing in Nano mode instead of crashing and leaving a dead window. They keep working in Android mode. OK?
- **D3. A debugging request while Nano's own screen or a game is in front, until Phase 3:**
  - Nano's own screen in front: drop the Wi-Fi request and leave the USB request pending (recommended; never auto-allow). Alternative: try the Settings prompt anyway and accept that it may be invisible or close the Nano home.
  - A game in front (a new PC plugged in mid-game): show the Settings prompt over the game, which pauses it until you answer (recommended, if 6.3 step 5 shows the game comes back cleanly). Alternative: hold the USB request ("unauthorized" on the PC) until the game is closed and the cable is plugged in again.
  - Accepted either way: if Nano force-closes Settings while a prompt is up, a USB request stays pending until the cable is plugged in again.
- **D4. Seedvault in Nano mode:** never start it in Nano mode (recommended; backup works in Android mode only, as today in practice). Alternatives: patch Seedvault, drop it from the build, or start the backup service in Nano mode (not recommended).
- **D5. System > Backup in Nano mode:** null-safe fallback (recommended). Also hide it in Nano mode?
- **D6. A Nano "Wireless Debugging" row** with the port and a pairing code shown in Nano (Phase 3 option)?
- **D7. `[joypad_spi_read]`:** run the read-only checks first (recommended). Any poll-interval change adds input latency in games and needs its own go-ahead.
- **D8. Phase 4** (USB permission dialogs for emulators in Nano mode): wanted for build 43, later, or not at all?
- **D9. Translations:** copy SystemUI's existing AOSP translations of the dialog texts (recommended) or English only.

## 9. Open questions

- What is `ro.adb.secure` on the build? Very likely 1 (LineageOS userdebug) [I]; Phase 0 confirms whether the USB key prompt is in play.
- Does any overlay replace `config_customAdbWifiNetworkConfirmationComponent` or `config_customAdbPublicKeyConfirmationComponent`?
- What started the test 43 bursts at uptime 95-120 s (11-26 19:43) and 165 s (21:51)? A Wireless debugging toggle right after boot, or AdbService at boot with `persist.adb.tls_server.enable=1` on an untrusted network? The second would mean the prompt can fire at boot while Nano's home is in front (5.2, Phase 3). `am_proc_start` in the events log names the cause.
- Does AdbDebuggingManager prompt again when the Cube moves to another access point while wireless debugging is on, or just switch off?
- Did the owner see Settings ANRs or kills? Only `system_app_crash` was captured; `system_app_anr`, `data_app_crash` and Settings' exit-info will tell.
- Which signal best says "Nano's own screen is in front" in system_server (5.2)?
- How long does a cold start of Settings take in Nano mode (for a USB prompt when Settings is not running)? If slow, the fallback is a tiny dedicated app (5.9).
- `[joypad_spi_read]`: real-time priority or not, D state or running, and the CPU governor and frequency at the time (`persist.gammaos.performance_mode`). Does stock GammaOS show the same 52%?
- Why does `gammaos-nano --overlay` use 37% CPU while Settings is in front: reading evdev at the pad's event rate, or drawing while hidden?
- Is the dead activity record cleared by anything short of a reboot? `am force-stop com.android.systemui` should finish it (force-stop finishes a package's activity records even with no process) [I, not tried; nothing else cleared it in the captures].
- How long is the controller lost when the dead record comes back on top of Settings (A0:11746-11862 shows the page hidden and a focus wait, not its length)?
- With a game in front, does the Settings-hosted USB prompt pause and return to the game cleanly, and do gammapad, Nano's foreground tracking and the Quick Menu follow it back (5.2, D3)?
- Do Display > Screen saver, Accessibility and Wallpaper & style crash in Nano mode on their own (missing services, 3.3)?

## 10. Read-only checks on the Cube (Phase 0, commands only)

None of these touch dispc1, dwc3 debugfs or `stop`/`start`.

Mode, adb and services:
```
adb shell 'getprop sys.gammaos.minimal_boot; getprop persist.bootanim.skip_nano; getprop ro.adb.secure; getprop persist.adb.tls_server.enable'
adb shell 'settings get global adb_wifi_enabled; settings get global adb_enabled'
adb shell 'for s in statusbar dreams backup media_projection wallpaper adb; do service check $s; done'
adb shell cmd overlay lookup android android:string/config_customAdbWifiNetworkConfirmationComponent
adb shell cmd overlay lookup android android:string/config_customAdbPublicKeyConfirmationComponent
adb shell 'dumpsys adb | head -80'
```

What starts SystemUI and Seedvault, and the dead record:
```
adb shell 'logcat -b events -d | grep -E "am_proc_start.*(systemui|seedvault)|am_crash|am_anr" | tail -40'
adb shell 'logcat -d | grep -E "Start proc [0-9]+:(com.android.systemui|com.stevesoltys.seedvault)|WifiDebuggingActivity|UsbDebuggingActivity|not restarting persistent|Skip zero pid ANR|depends on provider" | tail -40'
adb shell 'dumpsys activity activities | grep -E "ResumedActivity|WifiDebugging|UsbDebugging"'
adb shell 'dumpsys window | grep -E "mCurrentFocus|mFocusedApp"'
```

Settings exits and other dropbox tags:
```
adb shell 'dumpsys activity exit-info com.android.settings | head -80'
adb shell 'dumpsys dropbox --print system_app_anr | grep -E "^Process:|^Subject:"'
adb shell 'dumpsys dropbox --print data_app_crash | grep -E "^Process:|Exception" | sort | uniq -c'
adb shell 'cmd package resolve-activity --brief -a android.intent.action.SET_WALLPAPER'
```

Load and the joypad thread:
```
adb shell cat /proc/loadavg
adb shell 'ps -A -o PID,STAT,PCPU,NAME | awk "\$2 ~ /^D/"'
adb shell 'ps -A -T -o PID,TID,CLS,RTPRIO,NI,PCPU,STAT,WCHAN,CMD | grep -iE "joypad|spi|gammapad|logd|gammaos-nano"'
adb shell 'su 0 cat /proc/$(pgrep joypad_spi_read | head -1)/sched | head -20'
adb shell 'top -H -b -n 1 -m 30'
adb shell 'getprop persist.gammaos.performance_mode; cat /sys/devices/system/cpu/cpufreq/policy*/scaling_governor /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq'
adb shell 'logcat -d -s gammapad | grep -c "Device maps set"'
adb shell 'cat /vendor/etc/init/init.joypad_control.rc'
```

Bluetooth:
```
adb shell 'getprop bluetooth.gd.start_timeout; dumpsys dropbox --print SYSTEM_TOMBSTONE | grep -E "^20|Abort message" | head'
```

Optional, with the owner present (this one changes state): `adb logcat -c`, then turn on Wireless debugging in Settings in Nano mode on an untrusted network while running `adb logcat | grep -iE "adbwifi|WifiDebugging|am_proc_start|systemui"`. It confirms the start request and the crash, and leaves the dead record until a reboot (or, untried, `am force-stop com.android.systemui`).
