# Nano mode without SystemUI for build 43: debugging prompts, Settings stability, crash sources

Status: proposed 2026-11-27; revised the same day to the owner's decision below (section 5.2 replaces the Settings-hosted prompts and Nano's own debugging prompt), then corrected after a technical review and an owner review (adbd's one-prompt-at-a-time rule, the reply race, the bootanim and su domains being permissive, USB "Always", Battery Saver and other gaps). Planning only: no code was changed, nothing was built, and no device was touched for this plan. Every technical claim is marked **[C]** (confirmed from source, the Thimsy patch or a device log), **[I]** (inferred) or **[U]** (unverified).

Owner reports (2026-11-27):
1. "When enabling wifi debugging in the android settings, I first have to boot android and then boot back into nano. The reason seems to be that a popup asks for confirmation to trust the network that nano isn't able to display."
2. "In general, I have also noticed instability sometimes when trying to access the android settings."

Owner decision (2026-11-27): "Convert it to Nano pop-ups." In Nano mode, every SystemUI pop-up a Thimsy user can hit is shown by Nano instead, in Nano's own style, and the answer goes back to Android; where a decision in section 8 says so, it is refused with a Nano notice instead. SystemUI stays off in Nano mode. Android mode keeps SystemUI as it is (section 5.2).

Evidence references:
- **P:N** is a line in `patches/thimsy/0001-thimsy.patch` (diff of the WSL AOSP tree `/aosp/gammaos` against BASE 0bc531e2).
- **BASE** files are the GammaOS base at 0bc531e2 (GammaOSNextDistribution-14), read on Windows through the blob:none clone in `work/android-launcher/upstream` (`git show FETCH_HEAD:<path>`). Shortened paths: `am/`, `wm/`, `adb/` are under `frameworks/base/services/core/java/com/android/server/`; `SystemServer.java` is `frameworks/base/services/java/com/android/server/SystemServer.java`; `FrameworkServicesModule.java` and `WMShellModule.java` are SystemUI's Dagger modules.
- **AOSP** is android-14.0.0_r67; LineageOS lineage-21.0 for Settings. LineageOS's `AdbDebuggingManager.java` is byte-identical to AOSP's.
- **PWM+:N** is a line in `policy/PhoneWindowManager.java` with the Thimsy patch applied. **AOSP X:N** without a path prefix uses the same shortened paths as BASE (`usb/`, `notification/toast/`, `sensorprivacy/`, `logcat/` are under `services/core/java/com/android/server/` or, for `usb/`, `services/usb/java/com/android/server/`).
- **Device evidence:** `out/dp-evidence/test43/settings/` (`dropbox_crashes.txt`, `state.txt`, `tombstone_head.txt`), from the Cube running test 41 plus the test 42 device patches, in Nano mode, uptime 13h40. Older live logcats: `out/dp-evidence/test42/A0/logcat.txt` (written **A0:N**) and `test42/A2/logcat.txt` (**A2:N**).

## 1. Summary for the owner

### What is wrong

- **Nano mode runs Android without SystemUI.** SystemUI is the part of Android that draws the status bar, the notification shade, the lock screen and many small system pop-ups. In Nano mode it is never started, on purpose, to save memory and keep Nano in charge of the screen. But it is not removed either. When something asks for one of its pop-ups, Android starts SystemUI, and SystemUI crashes about 0.2 s later because services it needs are switched off in Nano mode. Crash dialogs are hidden in Nano mode, so you never see this.
- **The "Allow wireless debugging on this network?" pop-up is one of those SystemUI pop-ups.** When you turn on Wireless debugging in Nano mode, Android asks SystemUI to show it, SystemUI crashes, nothing appears, and the switch flips back off. In Android mode SystemUI is running, so the pop-up shows, and "Always allow on this network" saves the network. That is why your workaround works. The "Allow USB debugging?" pop-up for a new computer is very probably broken the same way, if your build asks for USB authorisation (most likely it does; the Phase 0 check confirms it) [I].
- **The crashed pop-up is very likely the main cause of the Settings instability.** Android keeps the pop-up that never appeared as an invisible, dead window on top. Each time it comes to the front again, Android restarts SystemUI, which crashes again. In one capture, after you moved between Settings pages, it came back to the top, and at the same moment the Settings page was hidden and Android waited for a window to take the controller [C for the log lines, I that the dead pop-up hid the page]; how long the controller stays dead each time was not measured [U]. Nano also sees nothing in front while this happens [I]. In that capture it came back after Settings was closed, and again 30 minutes later when a RetroArch game ended. Every SystemUI start in every log we have (12 of 12) was this dead pop-up. Two later crash bursts on your Cube (13 crashes on 11-26 evening) have no log yet, so their cause is not known [U]. A reboot clears it, and `adb shell am force-stop com.android.systemui` probably does too [I, not tried].
- **Other things make Settings worse in Nano mode, but less often.** Some Settings pages also open SystemUI pop-ups or ask SystemUI for data (brightness level, "Play media to", lock screen shortcuts, wallpaper and style), and System > Backup can crash Settings because the backup service is off in Nano mode. A few other pages use services that are also off in Nano mode (Screen saver, Accessibility, Wallpaper & style) and have not been checked [U]. The device is also busy: a vendor kernel thread for the built-in controller uses about half a CPU core all the time, and gammapad writes up to 600 log lines a second.
- **Things that never show in Nano mode, because they are drawn by SystemUI:** notifications (for example "Wireless debugging connected", or the USB mode notification; USB settings are in Settings > Connected devices > USB instead), plain text toasts, including Settings' own short messages, and Android's own screenshot shortcut. Nano's own screenshot is not affected [I]. Two Settings switches also silently do nothing, because SystemUI should answer them: the first "Turn on Battery Saver?" confirmation, and the Internet panel that apps open [C code]. Under the new plan, text toasts become Nano banners (Phase 3), Battery Saver gets a Nano pop-up (Phase 4) and a low-battery warning becomes a Nano banner (Phase 5); notifications stay in Nano's Quick Menu. Not affected either, because they are not SystemUI: the folder picker, app permission questions, the "Open with" chooser, Bluetooth pairing, USB drives (mounted by Android's storage service; only SystemUI's "USB drive" notification is missing) and USB gamepads (no permission needed) [I].
- **Seedvault (Android backup) crashes each time a USB device is plugged in** in Nano mode, for the same reason as System > Backup. It is harmless noise. The one Bluetooth crash was a slow start on 2026-11-25 and is not linked to Settings.

### What we'll change

1. **Nano mode stops starting SystemUI at all (Phase 1a).** Anything that would start it gets a clean "no" instead, and no dead window is left behind. This alone should remove the main cause of the Settings instability.
2. **Nano shows the pop-ups itself (Phase 1b onwards).** Android hands each pop-up to Nano. Nano draws it as one of its own dialogs, in the theme you use (XMB, DSi or Minima), works with the controller, and fits a 640x480 screen. Your answer goes back to Android. It is meant to work at Nano's home screen, over Settings and over a game: the Nano layer comes up over the app just for the pop-up, the way Nano's keyboard does today. Raising it over a running game is not proven yet; a half-day test at the start of Phase 1b checks it before anything else is built.
3. **The two debugging pop-ups come first (Phase 1b):** "Allow wireless debugging on this network?" with the network name and address, and "Allow USB debugging?" with the computer's key fingerprint. Each has an "Always allow" box and Allow / Deny. This fixes report 1, in Nano mode, at the home screen too.
4. **Then the rest, in this order:** an app asking for a USB device (Dolphin with a GameCube adapter, for example) (Phase 2); Android's short text messages ("toasts", Settings' own messages included) appear as Nano banners, and short Nano notices say when something is not available in Nano mode (Phase 3); apps asking to record the screen get such a notice, and the first "Turn on Battery Saver?" becomes a Nano pop-up (Phase 4); then smaller ones, such as Settings' brightness slider opening Nano's brightness bar, a low-battery warning and volume changes made by apps showing Nano's volume bar (Phase 5).
5. **Smaller fixes:** System > Backup no longer crashes Settings in Nano mode; Seedvault is not started in Nano mode (recommended; your choice, D4); gammapad stops its log flood.

Android mode does not change: SystemUI's pop-ups stay as they are there.

Nothing is ever allowed without you pressing Allow. If you do not answer in time, or Nano is not running, the answer is "Deny". Only Android itself carries out an Allow; Nano only passes on which button you pressed.

### What you can do today

- **Wireless debugging:** keep using your workaround. Boot Android once while on the Wi-Fi network you use, turn on Wireless debugging, tick "Always allow on this network" and press Allow. Back in Nano mode it works on that network. The trust is per access point (BSSID): a different router, a mesh node or the other band (2.4 GHz vs 5 GHz) has a different address and needs the same step again [I].
- **If you already have USB debugging working with your PC,** `adb tcpip 5555` and then `adb connect <cube-ip>:5555` gives network adb without the pop-up, until the next reboot [I: standard adbd behaviour, not tried on Thimsy].
- **Avoid turning on Wireless debugging in Nano mode on a network you have not trusted yet.** If you did and Settings stops responding to the controller or flickers, close Settings from the Nano menu, then reboot to clear the dead pop-up. With USB adb, `adb shell am force-stop com.android.systemui` probably clears it without a reboot [I, not tried; SystemUI is not running in Nano mode, so this only removes the dead pop-up].

### What it costs

Nano pop-ups are more work than the first plan's Settings-hosted prompts, because Nano needs a new way to receive a question from Android and send the answer back safely. That piece is built once, in Phase 1b, and every later pop-up reuses it.

- **Phase 1a, about 1.5 to 2 days, low risk:** SystemUI is never started in Nano mode. It removes the dead pop-up and is expected to fix the main part of report 2. It can ship on its own. Until Phase 1b, the debugging switch still flips back and a new computer is refused at once, but nothing is left behind and nothing waits forever.
- **Phase 1b, about 6.5 to 7.5 days, medium risk:** first a half-day test that Nano can rise over Settings and a game and read the controller there; then the pop-up bridge and the two debugging pop-ups, drawn in the XMB style in every theme, English text. Fixes report 1.
- **Phase 1c, about 2.5 days:** DSi and Minima styles, touch, dual-screen handling, and the pop-up texts in Nano's other languages (D9).
- **Phase 2, about 2 days:** USB device permission for apps; no app is opened just because a USB device was plugged in.
- **Phase 3, about 2.5 to 3 days:** text toasts as Nano banners, and Nano notices.
- **Phase 4, about 1.5 days:** screen recording requests answered "no" with a Nano notice; the Battery Saver pop-up.
- **Phase 5, about 3.5 days:** the smaller ones (brightness, low battery, app volume changes, the rest).
- **Smaller fixes (Phase S), about 1 day,** independent of the rest.

All of it together is about 21 to 24 days. Phase 1 (1a plus 1b, about 8 to 9.5 days) is what fixes both reports. Section 7 has the table.

### Decisions needed first

Section 8. Your decision resolves D1 and D2 and replaces D3 and D8. The new ones that shape Phase 1: D10 (what happens when no Nano process can show a pop-up), D11 (pop-ups over a game and over Nano's own menus), D12 (how long a pop-up waits for an answer). Later phases: D13 (USB "open with" on plug-in), D14 (toasts over a running app), D15 (screen recording), D20 (which pop-ups get a notice instead of a Nano pop-up), D21 (the "Always open" box of the USB permission pop-up).

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
- Media output and the volume panel are SystemUI receivers (SystemUI manifest 1054-1069). Settings sends the volume panel there only when the `settings_volume_panel_in_systemui` flag is on, and it defaults to "false" (PanelFeatureProviderImpl; AOSP FeatureFlagUtils), so normally Settings shows its own panel [C]. Settings forwards the Internet panel to SystemUI as a broadcast that no manifest receiver takes, and SettingsLib sends the first Battery Saver confirmation to a receiver SystemUI registers at run time; both silently do nothing in Nano mode (5.2.1 rows 14, 15) [C code].
- System > Backup: `BackupSettingsHelper` takes `asInterface(getService("backup"))`, which is null in Nano, and calls `getDataManagementIntent(...)` catching only RemoteException (BackupSettingsHelper.java:43-44, 163-188; UserBackupSettingsActivity.java:57-66). A NullPointerException would kill Settings [I: code path, not seen on the device].
- Nano's own source already notes "the minimal_boot absent-service crashes the full Settings hits" (P:43323) [C].
- **Not audited yet** [U]: Settings pages backed by other services that are off in Nano (section 2): Display > Screen saver ("Start now" calls `IDreamManager`), Accessibility (AccessibilityManagerService), and ThemePicker / Wallpaper & style without WallpaperManagerService. Each may crash on its own, independently of SystemUI. The soak (6.3 step 3) and Phase 0's `data_app_crash` and exit-info checks cover them.
- **Shown by SystemUI, so never visible in Nano mode** [I]: notifications (no status bar or shade; "Wireless debugging connected", the USB mode notification), text toasts from apps targeting API 30+, Settings included (TextToastRecord.java:61-64 needs the status bar), and the system screenshot chord (PhoneWindowManager binds SystemUI's TakeScreenshotService; today that bind would start and crash SystemUI, after Part B it is refused). Nano's own screenshot (`maybeNanoScreenshot`, P:24943) does not use SystemUI.
- **Not SystemUI, so not affected** [I]: the SAF folder picker (DocumentsUI; Nano already has NanoUriGrant), runtime permission dialogs (PermissionController), the app chooser (IntentResolver), and Bluetooth pairing (Settings' own BluetoothPairingDialog; Nano's own pairing ask, P:164020-164130).

**A waiting provider client** [C code]: when a provider's process dies while launching, `ContentProviderHelper.removeDyingProviderLocked` (1773-1830) restarts it up to `MAX_RETRY_COUNT` times for a waiting client, then kills clients with a stable connection ("depends on provider ... in dying proc"). For a SystemUI provider in Nano that means up to 4 fork-and-crash cycles while the caller waits, then possibly a killed caller [I; not seen on the device].

## 4. Root causes, ranked

1. **[C] The debugging confirmations point at SystemUI, which crashes on start in Nano.** AdbDebuggingManager counts the start request as success and turns the switch back off (3.3 steps 3-5). This is owner report 1. The USB key prompt has the same cause [I, high].
2. **[C dead record and restarts; I for the effect on the owner] The dead WifiDebuggingActivity record stays on top.** Each time it is the top activity again (a second toggle, a Settings page change, Settings closing, a game ending), AMS restarts SystemUI, which crashes: repeated fork-and-crash churn [C]. Once, after a Settings page change, Settings' page window went invisible and the input dispatcher waited for a focused window (A0:11746-11862) [C]; how long the controller is lost is not measured [U]. Nano sees nothing in front while the record is resumed (Nano's `overlayResolveForegroundPkg()` deliberately returns "" for `com.android.systemui`, NanoMenuOverlay.cpp, BASE about 310-314, about 335-338 after the patch) [I for the effect on Nano's return logic]. To the owner this probably looks like Settings ignoring the controller, flickering or relaunching [I]. It is the cause behind every SystemUI start captured (12 of 12), so it is very likely the main part of owner report 2 [I]; the two uncaptured test 43 bursts may have another trigger [U]. It lasts until a reboot, or probably `am force-stop com.android.systemui` (force-stop finishes a package's activity records even with no process) [I, not tried].
3. **[I] Other Settings paths that start SystemUI.** Brightness level, media output, lock screen shortcuts (a blocking provider query, up to 4 crash cycles), wallpaper and style. Each leaves either a dead activity record (as in 2) or a stalled caller. Not seen in the captures.
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

### 5.2 Part A: Nano pop-ups for SystemUI dialogs (Phases 1b-5)

**Owner decision (2026-11-27): "Convert it to Nano pop-ups."** In Nano mode, every SystemUI pop-up a Thimsy user can hit is shown by Nano instead, or refused with a Nano notice where a decision says so (D13, D15, D16, D20):
- in Nano's own style (XMB, DSi or Minima), driven by the controller, and readable at 640x480;
- the answer goes back to Android, and system_server completes it;
- SystemUI stays off in Nano mode. There is no "pop-ups only" SystemUI;
- Android mode keeps SystemUI's pop-ups unchanged.

Rules on top of 5.1: nothing is granted without the user's press. A timeout, a missing Nano or a Nano crash ends in deny (a USB request may be shown again by a restarted Nano while its time is left, 5.2.6). Only system_server calls a grant API; Nano returns a choice, never a grant.

This replaces two earlier designs (5.8 says where the second went, 5.9 why the first was dropped):
- the Settings-hosted `ThimsyWifiDebuggingActivity` and `ThimsyUsbDebuggingActivity` (the first version of Part A);
- Nano's own debugging prompt for when Nano's screen is in front (the first version of Part G: `sys.gammaos.adb.confirm_seq` and NanoNetBridge `adb_allow` / `adb_deny`).

**The Settings-hosted activities are not kept as a fallback** (D10 can bring them back). The research found no common state where an app is in front and no Nano process can draw:
- With the overlay on (the default: `persist.gammaos.nano.overlay` and `overlay_home` are 1 in BASE gammaos-nano.rc:96-108, 121-122), the `--overlay` process is alive whenever an app is in front. It can raise itself over the app the way the keyboard-over-app bridge already does (BASE NanoMenuOverlay.cpp:1458-1530; still in the patch, P:38236-38330) [C mechanism; I that it works for pop-ups, until the Phase 1b spike (7) and 6.3].
- At Nano's own screen only Nano can draw. An Android window shown while `sys.gammaos.nano.drm_active=1` is hidden behind the DRM plane or "stomps" it and can black the screen (PWM+:7929-7942) [C]. A Settings activity there was never an answer.
- The gaps are rare: the overlay switched off by the user (`persist.gammaos.nano.overlay=0`; no Nano process may be alive while an app is in front) [I], a drastic-nano session, where DraStic owns DRM [U], and Nano crashing or restarting. In each of these a request is denied or waits (5.2.8), which is safe. A Settings activity over DraStic would disturb the game as much as a Nano pop-up would.

#### 5.2.1 Inventory

Sources: every `config_*` component that names SystemUI (BASE core config.xml:3259-3328, 3559, 3578, 3586, 4493, 5021-5028), SystemUI's manifest, each system_server caller, and the services that run in minimal boot (BASE SystemServer.java). "Today" is build 43 without this plan. Priority: P1 needed for the owner's reports or security, P2 a real user path, P3 rare.

| # | Pop-up (SystemUI) | When a Thimsy user hits it | Today in Nano mode | Nano handling | Prio | Phase |
|---|---|---|---|---|---|---|
| 1 | Allow wireless debugging on this network (`WifiDebuggingActivity`) | Turns on Wireless debugging in Settings on a network not yet trusted; possibly at boot with `persist.adb.tls_server.enable=1` [U] | SystemUI crashes, the switch flips back, a dead record stays on top (owner report 1; 12 of 12 captured SystemUI starts) [C] | **Pop-up** `adb_wifi`: SSID, BSSID, "Always allow on this network", Deny / Allow | P1 | 1b |
| 2 | Allow USB debugging (`UsbDebuggingActivity`) | Plugs in a computer not seen before, if `ro.adb.secure=1` [I, Phase 0] | Same crash; the PC stays "unauthorized", and adbd holds back any later key until it reconnects (it dispatches one prompt at a time, AOSP adbd_auth.cpp:120-140) [I, high] | **Pop-up** `adb_usb`: key fingerprint, "Always allow from this computer", Deny / Allow | P1 | 1b |
| 3 | Text toasts (ToastUI) | Any app targeting API 30 or later calls `Toast.makeText(...).show()`: Settings ("You are now a developer!"), most emulators, Aurora Store [I] | Dropped silently: `TextToastRecord.show()` logs "StatusBar not available to show text toast" and returns false (AOSP notification/toast/TextToastRecord.java:57-68) [C]; how often is unmeasured [U] | **Toast** (Nano banner) | P2 (display only) | 3 |
| 4 | USB device or accessory permission (`UsbPermissionActivity`) | An app asks for a raw USB device, for example Dolphin with a GameCube adapter or DolphinBar [I: not checked in Dolphin's APK]. USB HID gamepads need no permission and never get here [I, Phase 0] | SystemUI crashes; a dead record stays on top of the emulator; the app's PendingIntent is never answered [I] | **Pop-up** `usb_permission`: app label and package, device name, Deny / Allow. The "Always" box per D21 (recommended: not shown in Nano mode) | P2 | 2 |
| 5 | USB "Open <app> to handle <device>?", the chooser, accessory URI (`UsbConfirmActivity`, `UsbResolverActivity`, `UsbAccessoryUriActivity`) | Plugs in a USB device that an installed app declares an attach filter for [U which shipped apps do] | SystemUI crashes, dead record [I] | **Deny** quietly: no app is opened on plug-in and nothing is granted; the attach broadcast still goes out. An app that then opens the device gets pop-up 4 (D13) | P2 | 2 |
| 6 | Screen capture consent (`MediaProjectionPermissionActivity`) | An app asks to record or cast the screen | The start reaches SystemUI and crashes it; MediaProjectionManagerService is off in Nano mode (BASE SystemServer.java:3320-3322), so nothing could be granted anyway [C] | **Deny** plus a Nano notice "Screen capture isn't available in Nano mode" (D15) | P3 | 4 |
| 7 | Brightness level (`BrightnessDialog`) | Settings > Display > Brightness level | SystemUI crashes, dead record [I] | **OSD**: Nano's own brightness bar, through a new `sys.gammaos.nano.qs_summon` value | P2 | 5 |
| 8 | Low battery warning; high-temperature warnings (SystemUI's PowerUI; not activities) | Battery reaches 20 % and 10 % (`config_lowBatteryWarningLevel`, `config_criticalBatteryWarningLevel`, BASE config.xml:1419-1428) [C]; the device or the USB port runs hot [I] | Nothing; Nano shows only the battery icon [I: no warning code found in Nano] | **Toast** from Nano's own battery poll; no system_server hook. Temperature: a toast only if Nano already reads a thermal status [U], else left out | P2 (battery), P3 (temperature) | 5 |
| 9 | Media output ("Play media to"; `MediaOutputDialogReceiver`, a manifest broadcast receiver, SystemUI manifest 1054-1062) and the contrast dialog (`ContrastDialogActivity`, `SHOW_CONTRAST_DIALOG`, 830-842) | Settings' Sound and Display pages; Bluetooth headphones | SystemUI crashes [I] | **Deny** with a Nano notice "Not available in Nano mode" (D20); Part B reports the refused broadcast or activity (5.3). The volume panel is **not** in this row: Settings shows its own `VolumePanel` unless the `settings_volume_panel_in_systemui` flag is on, and it defaults to "false" (LineageOS Settings PanelFeatureProviderImpl; AOSP FeatureFlagUtils) [C] | P3 | 5 |
| 10 | Sensor-privacy reminder (`SensorUseStartedActivity`) | An app uses the microphone while it is blocked (the block is set in Android mode) | SystemUI crashes (AOSP sensorprivacy/SensorPrivacyService.java:626-656) [C path] | **Deny** (stays blocked) plus a **toast** "Microphone is blocked. Turn it on in Android mode." Whether the Cube has a microphone is [U] | P3 | 5 |
| 11 | Log access consent (`LogAccessDialogActivity`) | An app holding READ_LOGS (granted by hand) reads logcat | SystemUI crashes (AOSP logcat/LogcatManagerService.java:429-452; component hardcoded at :68) [C path] | **Deny** (`declineAccessForClient`) plus a toast (D20) | P3 | 5 |
| 12 | Screenshot (TakeScreenshotService) | Volume-down + Power over an app [I: the chord rule runs before the Nano volume branch] | The bind starts SystemUI, which crashes (AOSP ScreenshotHelper.java:171, 211) [C path] | **Deny** for now; or a Nano screenshot plus a "Screenshot saved" toast (D16) | P3 | 5 |
| 13 | Heads-up notifications | Any new notification ("Wireless debugging connected", downloads) | Never shown; Nano's Quick Menu > Notifications lists and dismisses them (NanoMenuPS3Menu.cpp:4973-5048) [C] | Optional **toast** for high-importance notifications (D18) | P3 | 5, optional |
| 14 | "Turn on Battery Saver?" first-use confirmation (PowerNotificationWarnings) | Settings > Battery > Battery Saver, the first time (until `LOW_POWER_WARNING_ACKNOWLEDGED` is set, for example by confirming once in Android mode) | **Silently nothing**: SettingsLib `BatterySaverUtils.setPowerSaveMode` sends `PNW.startSaverConfirmation` to `com.android.systemui` and returns false without turning saver on; SystemUI registers that receiver only at run time, so no process starts and nothing is logged [C: AOSP android-14.0.0_r67 BatterySaverUtils] | **Pop-up** `battery_saver`: the stock text, Cancel / Turn on. system_server completes it as SystemUI does: `PowerManager.setPowerSaveModeEnabled(true)` and the two acknowledged flags | P2 | 4 |
| 15 | Internet panel (`Settings.Panel.ACTION_INTERNET_CONNECTIVITY`) | An app (or Settings) opens the Internet panel, for example "No internet, choose a network" | **Silently nothing**: Settings' PanelFeatureProviderImpl forwards it as a broadcast to `com.android.systemui` with no manifest receiver [C: LineageOS lineage-21.0 Settings] | **Nano's Wi-Fi screen** through a new `qs_summon` value, if Nano has a Wi-Fi screen that can be summoned [U]; otherwise Settings' own Wi-Fi page (D20) | P3 | 5 |
| 16 | Volume bar for changes not made by keys (SystemUI's volume controller) | An app sets the volume with `FLAG_SHOW_UI`; Bluetooth headphones change the volume | Nothing shown: no `IVolumeController` is registered in Nano mode [I] | **OSD**: Nano's volume bar, poked from `AudioService`'s volume-UI path in Nano mode the way PhoneWindowManager already shows it for keys (PWM+:7922-7970) [I] | P3 | 5 |

Already handled by Nano or system_server, no change [C unless marked]:
- Volume keys: PhoneWindowManager takes the keys in Nano mode and shows Nano's HUD or its `NanoVolume` window (PWM+:7922-7970, 8764-8843). Safe-volume warnings look disabled (`config_safe_media_volume_enabled=false`, BASE config.xml:3157) [I; Phase 0 checks]. Volume changes not made by keys are row 16.
- Brightness keys: Nano's HUD or the `NanoBrightness` window (PWM+:7943-7958).
- Power menu: Nano's Quick Menu Power submenu, or LegacyGlobalActions, a system_server dialog (BASE policy/GlobalActions.java:59-77).
- Notification list: Nano's Quick Menu. Bluetooth pairing: Nano's own wizard (P:48205-48250).
- USB drives: StorageManagerService and vold mount them without SystemUI; only SystemUI's "USB drive" and "unsupported, format?" notifications are missing; Settings > Storage and its StorageWizard still work [I; Phase 0 checks with `sm list-volumes`].
- USB gamepads (HID): the input stack takes them without any permission, so only raw-USB apps reach row 4 [I; Phase 0].

Never reachable in Nano mode; Part B refuses them if they ever are [C or I as marked in the research]: slice permission, NetworkOverLimit (the Cube has no mobile data), screen pinning (a silent no-op without the status bar, BASE LockTaskController.java:663-667), the biometric prompt (AuthService is off), the Quick Settings dialogs other than the Internet panel (Bluetooth, cast, screen recording, user switcher; the Internet panel is row 15), USB contaminant, work-profile lock, Somnambulator (DockObserver is off), the secondary-user adb prompts, the keyguard, the clipboard overlay.

Silent gaps: SystemUI pop-ups asked for with a broadcast to a receiver SystemUI registers at run time (rows 14 and 15) start no process, so Part B's refusals (call sites 1 and 2) never see them and their log cannot find them. Part B's broadcast report (5.3, call site 5) logs every broadcast addressed to `com.android.systemui` in Nano mode, which finds any that this inventory missed.

Not SystemUI, so outside this decision, but shown as Android windows in Nano mode [I]: the ANR dialog (not hidden in Nano mode, unlike crash dialogs: BASE AppErrors.java:1058-1061 gates crashes only; none in the test 42 logs) (D19), LegacyGlobalActions with the overlay off, AppWarnings, the permission controller, DocumentsUI, the app chooser, the keyboard picker.

#### 5.2.2 Where Nano can draw

The DRM home and the `--overlay` process never run at the same time (BASE main.cpp:972-984, 1039-1058) [C]. Both are the same binary and class (`NanoMenu`), so one prompt module serves both.

| Nano state | What happens to a pop-up |
|---|---|
| Nano's home on DRM (cold boot, no app launched yet) | Modal dialog over the home. The home loop polls for requests, as it does for `sys.gammaos.bt.pair_request` and `dualscreen_detected` [C precedent]. |
| Overlay shown (Quick Menu, Applications, the overlay home) | Modal dialog over the current screen. Input goes to the pop-up first. On close, back to where the user was. |
| Overlay parked, an app in front (Settings, a game) | **Prompt-only raise**, copied from the keyboard-over-app bridge: wait while an app is launching; set `show_overlay=1` (it keeps `drop_input` across focus changes, BASE InputDispatcher.cpp:1129-1144) and raise with a scrim and no XMB, as the PSP clock summon does (BASE NanoMenuOverlay.cpp:465-478). No Quick Menu snap, no GPU unpark and no recents reload, so a heavy game does not stall the raise (the zram stall behind the render watchdog, BASE overlay.cpp:398-409). The app is not paused (D11). After the answer, `overlayHide()`; its `drop_fence` drops the key that closed the pop-up, so it does not reach the app (BASE NanoMenuOverlay.cpp:785-802) [C]. Window focus never moved, so the app gets the controller straight back. `show_overlay=1` also switches PhoneWindowManager's over-app hotkeys off, changes gammapad's profile and may trigger the orientation clamp (PWM+ about 3301, P:170384-170392, P:174651, P:175221, overlay.cpp:603-607) [C readers]; that is wanted while a modal pop-up holds the controller, but whether a game relayouts under the clamp is checked in the spike [U]. |
| Launch hand-off, OTA flashing, keyboard over an app | Nano answers `busy`; system_server keeps the request and publishes it again 1 s later (a new `<seq>`, 5.2.4), within the answer timeout. |
| DRM-to-overlay hand-off | The old process sets `ready` to `handoff` instead of clearing it; system_server treats that as `busy` for up to 10 s and publishes again when a `ready` with a new pid appears (5.2.6). |
| drastic-nano session (`sys.gammaos.drastic_nano.session=1`) | Whether the overlay can draw over DraStic's DRM is [U]. Until tested: `busy`, so it waits for the session to end, within the timeout. |
| Overlay switched off by the user, an app in front | No Nano process [I]. No reply arrives, so the no-Nano rules apply (5.2.8). |
| Screen off | Nano answers `busy` until the screen is on, and does not wake it, as in stock. |

#### 5.2.3 system_server side

New files in `frameworks/base/services/core/java/com/android/server/gammaos/`:
- **`ThimsyPromptInternal.java`**, a LocalService interface. It has no binder, so nothing outside system_server can call it. Phase 1b methods only: `isActive()`, `request(spec, callback)` returning an id, `cancel(id, reason)`, `isCurrent(id)`. Later phases add `showToast(...)`, `hideToast(...)` and `showNotice(...)` (Phase 3) and `onRefusedSystemUi(kind, component, action, callingUid)` (Phase 4).
- **`ThimsyPromptService.java`**, on its own `HandlerThread("ThimsyPrompt")`. It holds one active pop-up, a FIFO behind it and (from Phase 3) a separate toast slot. It writes the request file, sets the request property, polls the reply property every 150 ms only while a pop-up is outstanding, applies the timeouts and cancel triggers, checks that Nano is still alive (5.2.6), and calls each callback exactly once. When it starts it sets `sys.gammaos.prompt.req` to `cancel`, because properties outlive a system_server restart and an old request may still be on Nano's screen.
- **`ThimsyPromptCodec.java`** (pure, host-testable like `NanoUriGrant`): builds the request JSON, sanitises untrusted text, parses and validates the reply.
- **`ThimsyPromptPolicy.java`** (pure): per-type timeouts, which request replaces which, the arming rule and the state machine `QUEUED -> PUBLISHED -> SHOWN -> ANSWERED | TIMED_OUT | CANCELLED | NANO_LOST`.

Start: `SystemServer.java` creates the service in Nano mode only, before `StartAdbService` (BASE about 2682). AdbService can ask for the Wi-Fi pop-up at boot (3.3 step 7), and NanoNetBridge starts only after boot_completed (BASE SystemServer.java:4330-4337) [C]. `isActive()` is minimal boot (read once, 5.1) and the kill switch `persist.gammaos.prompt.enable` (default 1; it only matters inside Nano mode). With the kill switch off, every pop-up is denied and Part B still refuses SystemUI.

Hooks (each one is `if (prompt != null && prompt.isActive()) { ...; return; }` in front of the stock code):

| Pop-up | Hook | Completion, inside system_server | Cancelled (sends deny) when |
|---|---|---|---|
| `adb_wifi` | `startConfirmationForNetwork(ssid, bssid)` (adb/AdbDebuggingManager.java:1523-1539) | `allowWirelessDebugging(always, bssid)` or `denyWirelessDebugging()` (1712-1724), posted to the manager's handler. `MSG_ADBWIFI_ALLOW` re-checks the BSSID before it starts adbd (1132-1135), but with "always" it calls `addTrustedNetwork` before that re-check (1125-1127), as in stock [C] | Wi-Fi turns off, or the network or BSSID changes. The prompt service registers its own `WIFI_STATE_CHANGED` / `NETWORK_STATE_CHANGED` receiver for this, copied from SystemUI's WifiDebuggingActivity (130-177): AdbDebuggingManager's own receiver exists only while wireless adb is on (1091-1093, 1141-1143) [C]. A newer Wi-Fi request replaces the old one, which is denied |
| `adb_usb` | `startConfirmationForKey(key, fingerprints)` (1541-1566) | `allowDebugging(always, key)` or `denyDebugging()` (1686-1698). The full key stays in the callback; Nano gets only the fingerprint. `MESSAGE_ADB_ALLOW` already rejects a key whose fingerprint does not match the pending one (918-925) [C]. **adbd's OK/NO carry no key**: they apply to whichever prompt adbd has dispatched at that moment, and adbd dispatches one at a time; a second `MESSAGE_ADB_CONFIRM` arrives only after adbd dropped its framework socket, which also forgets the old prompt (AOSP adbd_auth.cpp:120-140, 159-167, 207-233) [C]. So the callback sends `NO` (deny, timeout or cancel) only while its fingerprint still equals `mFingerprints` and `isCurrent(id)` holds | USB is unplugged (`UsbManager.ACTION_USB_STATE`, connected=false), registered only when `service.adb.tcp.port` is 0, as SystemUI does (UsbDebuggingActivity.java:72-77), so a network client's prompt survives a cable change. A newer key request means adbd has already forgotten the old one: the old pop-up is **closed silently** (cancel, no `NO`), and the new one is shown |
| `usb_permission` | the 7-argument `requestPermissionDialog` (AOSP usb/UsbUserPermissionManager.java:524-555), where the PendingIntent, uid, package and `canBeDefault` are all known | As SystemUI's helper does (AOSP SystemUI UsbDialogHelper.java:169-256): `grantDevicePermission` (162) or `grantAccessoryPermission` (180); then `pi.send(ctx, 0, intent{EXTRA_DEVICE or EXTRA_ACCESSORY, EXTRA_PERMISSION_GRANTED})`; all inside `clearCallingIdentity()`. Deny: `pi.send` with granted=false. "Always" per D21: stock "always" is `setDevicePackage`, which lives on `UsbProfileGroupSettingsManager` (reached through `UsbService` and `getSettingsForProfileGroup`, not from this class) and only acts through the default-app launch that row 5 blocks, while plain permission is keyed by the device name, which changes on replug (UsbUserPermissionManager.java:162-171; UsbProfileGroupSettingsManager.java:1108-1131) [C]. So in Nano mode "always" would do nothing; recommended: no box. Alternative: `setDevicePersistentPermission(device, uid, true)` / `setAccessoryPersistentPermission` on this same class (275), keyed by the device's filter, which survives replugs and reboots and applies in Android mode too [C code; U whether Settings shows a way to revoke it] | the device is detached; the requesting app dies; a duplicate (uid, device) request is merged |
| USB open-with (row 5) | `UsbHandlerManager.confirmUsbHandler` (85), `selectUsbHandler` (118), `showUsbAccessoryUriActivity` (57); and the default-app auto-launch in `UsbProfileGroupSettingsManager.resolveActivity` (1101-1132), whose permission grant is skipped with the launch | none: log once and return (D13) | n/a |
| `battery_saver` (row 14, Phase 4) | Part B's broadcast report (5.3, call site 5) for `PNW.startSaverConfirmation` addressed to SystemUI, only when the sender holds `DEVICE_POWER` (SystemUI registers its receiver with that permission [I: check PowerNotificationWarnings at implementation]); broadcast reports for the Internet panel are taken only from the system uid | Turn on: `PowerManager.setPowerSaveModeEnabled(true)` and `Secure.LOW_POWER_WARNING_ACKNOWLEDGED` / `EXTRA_LOW_POWER_WARNING_ACKNOWLEDGED` = 1, as SettingsLib's `setBatterySaverConfirmationAcknowledged` does when SystemUI's dialog is confirmed (the same state Android mode would write). Cancel: nothing | a newer request; the 30 s timeout |
| `toast`, `notice` (Phase 3) | `TextToastRecord.show()` and `hide()` (5.2.10); notices from the rows below | none (display only) | the toast's own duration; 4 s for a notice |
| Refused SystemUI start (Phase 4 onwards) | Part B's call sites report the refused activity, process start (hosting type and component) or broadcast to `onRefusedSystemUi`, posted, because the WM or AMS lock is held there (5.3) | a small table: `ACTION_SHOW_BRIGHTNESS_DIALOG` raises Nano's brightness bar (Phase 5); `PNW.startSaverConfirmation` opens `battery_saver` (Phase 4); `MediaProjectionPermissionActivity` (Phase 4), media output and contrast (Phase 5) show a notice; `ACTION_INTERNET_CONNECTIVITY` summons Nano's Wi-Fi screen (Phase 5, D20). Everything else is logged at most once a minute, which also finds pop-ups this inventory missed | n/a |
| Sensor privacy, log access (rows 10, 11) | `SensorPrivacyService.showSensorUserReminderDialog` (626-656); `LogcatManagerService.processNewLogAccessRequest` (429-452) | stays blocked; `declineAccessForClient` (218-228). A toast says why | n/a |
| App volume changes (row 16, Phase 5) | `AudioService`'s volume-UI path (where it calls the registered `IVolumeController`) in Nano mode [I: exact method at implementation] | none (display only): Nano's volume bar, as PhoneWindowManager shows it for keys | n/a |

Not chosen:
- **Extending NanoNetBridge** for the answers. It starts after boot_completed, and its `sys.gammaos.nano.net_cmd` channel can be written by every `platform_app` and `system_app` (5.2.7) [C].
- **Pointing the `config_*` components at a Thimsy forwarder.** The value is the same in both modes, so Android mode would change unless every reader also checks the mode: the same amount of hook code, with more moving parts.
- **One generic "turn any SystemUI intent into a pop-up" rule.** Completing a pop-up needs per-type knowledge (the USB PendingIntent and grant tables live in UsbService), so direct hooks are cleaner.

#### 5.2.4 The channel (schema v1)

Properties, each with its own SELinux type. A longer prefix wins over `sys.gammaos.` (precedent: `persist.gammaos.share.` gets `gammaos_private_prop`, P:185637-185638) [C]:
```
sys.gammaos.prompt.req    u:object_r:gammaos_prompt_req_prop:s0    exact  # "<id>.<seq>", "<id>.<seq>|cancel" or "cancel"
sys.gammaos.prompt.toast  u:object_r:gammaos_prompt_req_prop:s0    exact  # "<gen>" (Phase 3)
sys.gammaos.prompt.reply  u:object_r:gammaos_prompt_reply_prop:s0  exact
sys.gammaos.prompt.ready  u:object_r:gammaos_prompt_reply_prop:s0  exact
```
- `gammaos_prompt_req_prop`: set only by system_server, read by bootanim. `gammaos_prompt_reply_prop`: set only by bootanim, read by system_server. The neverallows are `neverallow { domain -init -system_server } gammaos_prompt_req_prop:property_service set;` and `neverallow { domain -init -bootanim } gammaos_prompt_reply_prop:property_service set;`: init must be excluded, because `allow init property_type:property_service set` (public/init.te:558) would otherwise fail the policy build [C].
- **What the types do and do not stop:** they keep out every enforcing domain: `platform_app`, `system_app`, other apps and the unrooted `adb shell` [C rules]. They do not stop permissive domains: `su` is permissive on userdebug (private/su.te:26-27), so `adb root` or `su` can set the reply, and so can the Thimsy services that run as `seclabel u:r:su:s0` (P:73957, 73980, 73988, 74039, 74046, 74058). The `bootanim` domain is permissive too (`permissive bootanim;`, BASE private/bootanim.te:3, not changed by the patch), so anything in it can do anything SELinux would otherwise refuse [C]. Root can do everything anyway; the point of the types is that a system-signed app cannot answer a pop-up.
- **The `seq`:** system_server bumps it on every publish of the same id (after `busy`, after a hand-off, after a new Nano `ready`), so Nano always sees a changed value. Nano keys its state on `<id>` and shows the pop-up again on a new `<seq>` if it was not on screen.
- Files: `system/sepolicy/private/property.te`, `property_contexts`, `file.te`, `file_contexts`, `system_server.te`, `bootanim.te`, mirrored into `prebuilts/api/{34.0,202404}/private` as the patch already does for `file_contexts`.

Request file: `/data/system/nano_prompt/current.json` (and `toast.json`), written to a temp file, synced, then renamed, as `oskPublishLive` does (BASE overlay.cpp:1440-1456). Mode 0640 system:system. The directory gets its own file type, `gammaos_prompt_data_file` (system_server read and write; bootanim read), because plain `system_data_file` is open to more than Nano: uid 1000 apps (Settings, TvSettings) own a system:system file by DAC [C], and whether `system_app` may read it under SELinux is [U]. The nonce is **not** treated as a secret: bootanim (permissive, see above) and root can read the file. It only ties a reply to one pop-up so a stale or mistyped reply is refused. system_server keeps the request in memory and never re-reads the file to decide a grant.
```json
{"v":1,"id":73104,"nonce":"9c1f02e4a7b35d11","type":"adb_usb","timeout_ms":60000,"display":0,
 "args":{"fingerprint":"AB:CD:...:EF"},"requester":null,
 "checkbox":{"id":"always","checked":false},"options":["deny","allow"],"default":"deny"}
```
- `adb_wifi` args: `ssid`, `bssid`. `usb_permission` args: `app_label`, `app_pkg`, `device`, `vid`, `pid`, `kind` (device or accessory), `audio_warning`; `requester.icon` is `/data/system/nano_app_icons/<pkg>.png` when system_server's existing icon cache has it (BASE SystemServer.java:4296-4297, 4519, 4593) [C]. `battery_saver`: no args. `toast` (in `toast.json`): `text`, `pkg`, `duration_ms`. `notice` (also in `toast.json`, Phase 3): a Nano text key and untrusted arguments; a notice is a passive banner, never modal (5.2.10).
- The JSON carries data only, never format strings. Nano owns the text templates, so translations and `test/i18n_check.py` stay inside Nano. An unknown type gets an `unsupported` reply, and system_server denies.
- Ids start from a random per-boot base and only increase. Each pop-up has a 64-bit random nonce.

Reply (fits in PROP_VALUE_MAX, 92 bytes): `<id>.<seq>|<nonce>|<state>|<shown_ms>[|<choice>|<press_ms>|<flags>]`
- `state`: `shown` (the first fully visible frame is on screen), `answer`, `busy`, `unsupported`, `error`.
- `shown_ms` and `press_ms`: Nano's `CLOCK_MONOTONIC` time in ms of the first fully visible frame and of the key-up that chose; the same clock as `SystemClock.uptimeMillis()` [C], so system_server compares them with its own times. `shown_ms` is 0 for `busy`.
- `choice`: `allow`, `deny`, `option:<n>`. `flags`: `always=0|1`.
- **`answer` implies `shown`.** The reply is one property polled every 150 ms, so a quick B can overwrite `shown` before system_server reads it; the `shown_ms` inside the answer carries it. Nothing is lost by a skipped poll.

Ready: `sys.gammaos.prompt.ready = <pid>|<maxSchema>|<types>|<home or overlay or handoff>`, set when Nano's prompt module starts, set to `handoff` by the old process at the DRM-to-overlay hand-off, and cleared on a clean exit. system_server sends only types that Nano lists.

#### 5.2.5 Nano side: queue and rendering

New `cmds/gammaos-nano/NanoMenuPrompt.cpp` (the file-per-feature pattern) with declarations in `NanoMenu.h`:
- **`promptPoll()`**: one property read per tick, from the home loop and from `overlayPoll`'s idle tick (it runs while parked [I]; the Phase 1b spike checks it). It reads the JSON, checks the version, type and fields, and caps every string. A new `<id>` replaces the dialog on screen; a new `<seq>` of the same id shows it again if it is not on screen; `<id>.<seq>|cancel` and a bare `cancel` close it. system_server owns the queue, so Nano only ever shows one id.
- **Own expiry:** Nano closes the pop-up by itself at `timeout_ms` plus 2 s after its first frame, replies `answer` with `deny`, and hides the raise. A system_server restart (watchdog) or a lost poll can then never leave the layer up with `drop_input` held.
- **Text**: `promptBuildText(type)` builds the title, body lines, checkbox label and button labels from English-keyed `trDyn` strings and `ps3Fmt` with the untrusted values as arguments only. About 20-25 new keys. Phase 1b adds them in English only, so it does not wait on D9; the 21 `ps3xmb/i18n/<code>.json` files get the translations in Phase 1c (from SystemUI's AOSP translations, if D9 says so), and until then the lookup falls back to English.
- **Renderers**: `renderPromptXmb()` (the PS3 dark panel), `renderPromptNds()` (DSi glossy card and bottom bar), `renderPromptMinima()` (pills). They reuse the existing primitives (`ps3DlgText`, `ps3DlgOption`, `drawRoundedRect`, `drawNdsGlossyBtn`, `wrapTextToFit`, NanoBidi) with their **own state**: the theme dialogs share one `mPs3Dlg*` state (BASE NanoMenu.h:2095-2120), and a pop-up must not clobber a dialog already open [C]. Phase 1b ships the XMB renderer in every theme; 1c adds DSi and Minima. ES-DE engine themes use the XMB renderer [U: whether they need their own].
- **640x480 layout**: scale by `min(w/640, h/480)`; the panel is at most 92 % by 80 % of the screen; the title takes 1-2 lines; the body goes through `wrapTextToFit` with a minimum font size; the checkbox label wraps to at most 2 lines; the checkbox row and both buttons are always visible; the footer hint uses `themeButtonText`. Text that still does not fit at the minimum font size is cut with an ellipsis: untrusted values (app label, device name, SSID) are ellipsized first, Nano's own template text never. The fingerprint is shown in fixed colon groups over two lines (47 characters). Checked at 640x480, 720x720 (the Cube) and 1920x1080 (a monitor), and in the g++ layout test with maximum-length values (64-character label and device name, 32-byte SSID).
- **Input** (the "fresh press" rule):
  - every key except Power and Volume goes to the pop-up; Nano's hotkeys and combos are off while it is up;
  - on open, stale evdev events are drained (as `overlayShow` does) and every key already down must be released first;
  - the dialog arms 600 ms after its first fully visible frame; a small fill on Allow shows the arming;
  - Allow needs a key-down and a key-up, both after arming, with Allow focused. Touch needs the press and the release inside the button, both after arming;
  - focus starts on Deny and the "Always" box starts unticked (as `openConfirmDialog` starts on Cancel, P:68327-68340) [C]; B or Circle means deny;
  - an optional countdown shows the time left (D12). The theme's dialog sound plays;
  - Back, Mode, F10 and Home still reach PhoneWindowManager's 10 s emergency hold while `drop_input` is set; dispatch-time drop keeps them from the app (BASE InputDispatcher.cpp:1187-1199) [C]. If the emergency exit hides the overlay during a pop-up, Nano replies `answer` with `deny` first.
- **Overlay**: a new `mPromptStandalone` mode for the prompt-only raise (5.2.2). While a pop-up is up, `overlayPoll` must not hide the layer, and it re-asserts `show_overlay=1` (for example after a Power hold).
- Build and tests: `Android.bp`; `test/unit/NanoPrompt_test.cpp` (parse, replace, `seq`, own expiry, arming and the press rule, layout with maximum-length values); `test/i18n_check.py` checks positional-argument parity for the new keys.

#### 5.2.6 Completion and timeouts

| Type | Nano must reply (`shown`, `busy` or `answer`) within | Answer timeout (from the first publish) | If no Nano is there, or Nano is lost |
|---|---|---|---|
| `adb_wifi` | 3 s | 60 s | deny at once; the switch is already off, and the user can toggle it again |
| `adb_usb` | 3 s | 60 s | keep it queued and publish it again (new `seq`) when a Nano reports `ready`; deny at 60 s |
| `usb_permission` | 3 s | 30 s | as `adb_usb`; the app always gets its PendingIntent, granted=false on deny |
| `battery_saver` | 3 s | 30 s | cancel (saver stays off); nothing to complete |
| `notice`, `toast` | none (passive, 5.2.10) | the toast's duration (2 s or 3.5 s); 4 s for a notice | drop |

- **Deadlines.** Any valid reply meets the 3 s deadline: `shown`, `busy` or `answer`. After `busy` the request stays published (a new `seq` every 1 s) until it is shown or the answer timeout ends; the answer timeout never restarts. A `ready` of `handoff` counts as `busy` for up to 10 s. "No Nano is there" means no `ready`, or a `ready` whose Nano is lost (next point), when the request is published.
- **Nano lost.** On every poll tick while a pop-up is outstanding, system_server checks that the Nano in `ready` is still there: the `init.svc.<service>` state of Nano's init services is `running` and the `ready` pid has not changed [I: the service names are in gammaos-nano.rc; that system_server can read `init.svc.*` is checked at implementation]. If not, the request is NANO_LOST: `adb_wifi` is denied at once; `adb_usb`, `usb_permission` and `battery_saver` are published again (new `seq`) when the respawned Nano sets `ready`, within the original answer timeout, and denied or cancelled at its end. So the deny-at-timeout rule and the republish rule are one rule: republish while time is left.
- An id is answered once. A reply with an unknown id, a wrong nonce, or a `seq` older than the current one is logged and ignored. `answer` needs no earlier `shown` (5.2.4).
- An `allow` whose `press_ms` is less than 500 ms after its `shown_ms` counts as deny (defence in depth behind Nano's 600 ms arming; measured on Nano's own times, so poll delay and load do not turn a valid Allow into a deny). system_server also refuses `shown_ms` before the first publish or `press_ms` later than its own clock.
- Timeouts are D12. A deny on timeout leaves adbd "unauthorized" until the cable is plugged in again, as in stock.

#### 5.2.7 Security

- **Who can ask:** only system_server code, through the LocalService.
- **Who grants:** only the system_server callback, through AdbDebuggingManager's or UsbUserPermissionManager's own methods (and PowerManager for `battery_saver`). Nano never calls IAdbManager or IUsbManager.
- **Why a new reply type:** `sys.gammaos.` maps to `persist_gammaos_prop` (BASE property_contexts:1691). The patch takes writes away from apps in general (P:185569-185576), but `platform_app`, `system_app` (P:185648), `bootanim` and shell on userdebug can still write it [C]. A reply on that channel could be forged by any system-signed app. Those apps can already call IAdbManager directly, so it would not widen their power, but it would break "nothing is granted without the user's press" and is an easy confused-deputy path [I]. Hence the bootanim-only reply type. It is the reply type, not the nonce, that keeps apps out (5.2.4).
- **Untrusted text:** SSIDs, USB descriptors, app labels and toast text come from outside. system_server strips control characters, newlines and bidi overrides (U+202A-202E, U+2066-2069) and caps the length (SSID 32 bytes, device names and labels about 64 characters, toast text about 200). Nano parses with fixed-size fields, isolates bidi, and never puts the text into a shell command (Nano's notification code shows the validation pattern, NanoMenuPS3Menu.cpp:4960-4970) [C]. The package name is shown under the app label, so a renamed app cannot pass as the system.
- **Covered screen:** an app cannot draw over Nano's SurfaceFlinger layer or inject evdev input [I].
- **Residual risk:** anything that runs as root in a permissive domain can answer a pop-up: `adb root` and `su` on userdebug, the Thimsy services labelled `u:r:su:s0`, and everything in the bootanim domain, which also runs `gammaos-nano-scrape` (network-facing) and `drastic-nano` (P:185544-185547). A compromised scraper could answer a pop-up. Later: move the scraper into its own domain; making bootanim enforcing is a larger, separate job.
- **Out of scope, flagged:** NanoPermGrantBridge already grants storage and microphone access without a press (BASE SystemServer.java:4120-4166). That conflicts with the same principle (section 9).

#### 5.2.8 Failure handling

- **No Nano process can draw** (overlay off with an app in front, Nano crashed or restarting): no reply within 3 s, then the per-type rule in 5.2.6. A request is never left unanswered: the answer timeout always ends in deny. D10 is the option to fall back to Settings-hosted activities instead.
- **Nano crashes with a pop-up on screen:** system_server sees NANO_LOST at the next poll (5.2.6) and publishes the request again to the respawned Nano while time is left, or denies it (`adb_wifi`). init respawns the overlay with `show_overlay=1` and `overlay_wallpaper=1` (gammaos-nano.rc:145-149), so the user lands in Nano's overlay home, not back in the app, and resumes the app from there; input is not stuck [C rc; I for the flow]. The InputDispatcher focus-gain clear does not help here: it runs only while `show_overlay` is 0 (BASE InputDispatcher.cpp:1137-1143 with the patch) [C].
- **system_server restarts with a pop-up on screen:** Nano's own expiry closes it (5.2.5), and the new prompt service publishes `cancel` when it starts (5.2.3).
- **Several requests:** one modal pop-up at a time. A newer `adb_wifi` replaces the active one, which is denied. A newer `adb_usb` replaces the active one, which is closed silently: adbd has already forgotten it, and a `NO` would land on the new key (5.2.3). `usb_permission` is merged on (uid, device), then FIFO. Toasts and notices have their own slot and never block a pop-up.
- **The user is busy in Nano** (keyboard over an app, launch hand-off, OTA): `busy`, published again every 1 s, within the timeout. Over an open Quick Menu or Applications screen the pop-up shows on top (D11).
- **Before Phase 1b ships** (or with the kill switch off): Part B answers the two requests that would otherwise wait forever, a USB key with `denyDebugging()` and a USB permission request with granted=false (5.3), and refuses any SystemUI start. The Wi-Fi switch flips back, but no dead record is left and nothing waits.

#### 5.2.9 Dual screen and monitors

- The pop-up is drawn on the panel that has the controller (`sys.gammaos.nano.focus.display`, else the primary panel or the monitor) [C property; I choice]. The request's `display` hint is used when Nano renders that panel.
- The other panel shows a small "Answer on the other screen" card, so the user never looks at a frozen game without knowing why.
- DSi touch buttons on the bottom panel come later (1c covers touch on the panel with the pop-up).
- Nano follows display 0 (`followDisplay0`) when a monitor is attached; the pop-up follows Nano. Boot-without-monitor rules still apply to every device test.

#### 5.2.10 Text toasts

- **Why they are missing:** for apps targeting API 30 or later, the system draws text toasts (`CHANGE_TEXT_TOASTS_IN_THE_SYSTEM`, AOSP Toast.java:121-122, 197). NotificationManagerService makes a `TextToastRecord` holding `mStatusBar` (AOSP NotificationManagerService.java:3615-3626), which is null in Nano mode, so `show()` returns false and the toast is dropped (TextToastRecord.java:57-68; `showNextToastLocked` 9601-9632) [C]. Custom-view toasts and apps targeting below 30 draw in their own window and still work [C].
- **Hook:** in Nano mode, `show()` calls `prompt.showToast(uid, pkg, token, text, duration, displayId)` and returns true. `hide()` must change too: its `checkNotNull(mStatusBar)` (TextToastRecord.java:72-75) would throw inside NotificationManagerService once `show()` returns true [C code; a crash if forgotten]. In stock, system_server never calls the app's `ITransientNotificationCallback`: it hands `mCallback` to SystemUI, whose ToastUI calls `onToastShown()` / `onToastHidden()` [C]. So the hook calls them itself (when `mCallback` is not null), posted to the prompt service's handler, because `show()` and `hide()` run under NotificationManagerService's `mToastQueue` lock (`showNextToastLocked` about 9651, `cancelToastLocked` about 9682) [C]. NotificationManagerService keeps its own duration timer and per-app quota.
- **Channel:** `toast.json` (text, app label, duration) and `sys.gammaos.prompt.toast=<gen>`. Display only, nothing to answer. Nano drops duplicates and shows at most one toast every 2 s.
- **Notices** (screen capture, media output and so on) use the same slot and the same passive banner, with a Nano text key instead of app text. They never take the controller and need no press, so one firing over a game costs only the banner.
- **Nano in front** (home or raised overlay): Nano draws the toast in its banner style (`drawPhotoBanner` geometry, BASE NanoMenu.h:2493) in the current theme.
- **An app in front, overlay parked** (D14):
  - (recommended) a **passive banner mode** in the overlay: only Nano's SurfaceFlinger layer is shown, for the toast's duration, without the XMB, without `drop_input` and **without `show_overlay=1`**. `show_overlay` is read by PhoneWindowManager (over-app hotkeys and bindings off, P:170384-170392), gammapad (cursor and browser profile, P:174651, 175221), InputDispatcher's focus-gain clear and the orientation clamp, so setting it for a 2-3.5 s toast would change the controller and could relayout a game [C readers]. If other code needs to know a banner is up, it gets its own property (`sys.gammaos.nano.banner`). The overlay is not an input window, so the app keeps the controller and touches [I]. The cost of 2-3.5 s of drawing over a game is measured in 6.3 (the overlay already uses about 37 % CPU while parked, 3.1).
  - (alternative) a system_server `NanoToast` window, not focusable and not touchable, styled like Nano and hidden while `drm_active=1`, copied from the existing `NanoVolume` window (PWM+:8764-8843). Cheaper, but drawn by Android, not Nano.
  - either way: optionally suppressed while an emulator is in front.

### 5.3 Part B: never start SystemUI in Nano mode (Phase 1a)

A new pure class `frameworks/base/services/core/java/com/android/server/gammaos/NanoProcessGuard.java` (Phase 1a, with call sites 1, 2 and 4; call site 5 comes with Phase 4):
- `static boolean refuses(String packageName, boolean minimalBoot, Set<String> extra)`: true in Nano mode for `com.android.systemui`, and for the packages in `extra` (D4: Seedvault).
- The `minimalBoot` value and the list are read once at boot and cached.

Call sites 1 and 2 refuse, each with one log line per component per minute (`GammaOS Nano: refused <kind> <component>: <package> doesn't run in Nano mode`):
1. **Activities:** `wm/ActivityStarter.executeRequest`, next to the IntentFirewall `abort |=` checks, with a null check on `aInfo`. A refused target takes the existing abort path (BASE ActivityStarter executeRequest abort block, about 1254-1262): the caller's result gets `RESULT_CANCELED` and the start returns `START_ABORTED` [C]. No activity record is created, so nothing is left on top. This removes cause 2 for every SystemUI activity (brightness dialog, contrast, USB permission dialogs, and the stock debugging prompts if anything still reaches them). A refused stock prompt still looks like a successful start to its caller (START_ABORTED, no exception) [C], so the two callers that would wait for an answer get one from call site 4 below. From Phase 4 the abort also reports the component and action to `ThimsyPromptInternal.onRefusedSystemUi` (5.2.3), posted, which maps a few refused starts to Nano's OSD, a pop-up or a notice.
2. **Processes (services, broadcasts, providers):** the top of the `ProcessRecord startProcessLocked(String processName, ApplicationInfo info, ...)` overload (BASE ProcessList.java:2572) returns null for a refused package, before a ProcessRecord is created. The other overloads (1786, 2564) are reached only through persistent and restart paths, which Nano already skips for SystemUI [C overloads; I, high for the callers]. Activity process starts also arrive here, so call site 1 is still needed to stop the activity record being created. ActiveServices logs "unable to launch app" and the broadcast queue skips the receiver [I: check each caller at implementation]. For providers, `ContentProviderHelper.getContentProviderImpl` then returns null at once ("process is bad", about 556-568): a query gets a null cursor (CustomizableLockScreenUtils handles that [C]); `ContentResolver.call` throws "Unknown URI" [I]. No 4x retry and no waiting caller. From Phase 4 the refusal also reports the hosting type and component (a manifest receiver such as `MediaOutputDialogReceiver`, a service, a provider) to `onRefusedSystemUi`, posted, because the AMS lock is held; without that, a refused broadcast would never get its notice.
3. *Optional:* a check in `ContentProviderHelper.getContentProviderImpl` itself only adds a clearer log line (Thimsy already patches this file, P:166706) [C]; not needed for correctness.
4. **Answers for the stock requests (Phase 1a):** a refused `UsbDebuggingActivity` would leave adbd's dispatched key unanswered, which also holds back every later key until adbd reconnects (5.2.3), and a refused `UsbPermissionActivity` would never answer the app's PendingIntent [C code]. So in Nano mode, while the prompt service is not active (before Phase 1b and Phase 2, or with the kill switch off), `AdbDebuggingManager.startConfirmationForKey` calls `denyDebugging()` instead of starting SystemUI, and `UsbUserPermissionManager.requestPermissionDialog` sends the PendingIntent with granted=false. `startConfirmationForNetwork` needs nothing: the switch is already off and nothing waits. These are the same answers as the user pressing Deny, so nothing is granted.
5. **Broadcasts addressed to SystemUI (Phase 4, report only):** in `ActivityManagerService.broadcastIntentLocked` [I: the exact place in the 14 tree], a broadcast whose package or component is `com.android.systemui` is reported to `onRefusedSystemUi` in Nano mode and otherwise left alone. This is the only way to see requests aimed at receivers that SystemUI registers at run time (Battery Saver's `PNW.startSaverConfirmation`, the Internet panel), because they start no process and reach no other call site (5.2.1).

**Phase 1a (quick win):** call sites 1, 2 and 4 remove the dead window behind report 2 before any Nano pop-up exists, and leave no request waiting. Call site 1 alone already removes the dead record, if the ProcessList change needs more time. It can ship before Part A.

Why this is safe: in Nano mode SystemUI cannot run anyway. Every refused start would otherwise have forked SystemUI and crashed within 0.4 s. A refusal swaps a crash (and a dead record or a waiting caller) for a clean "no" [I, high]. Android mode never reaches the guard.

What the owner will notice in Nano mode after Phase 1a, until each pop-up's own phase (5.2.1): Settings > Display > Brightness level, "Play media to", lock screen shortcuts and the SystemUI parts of Wallpaper & style do nothing (Settings' volume panel is its own and keeps working). A new computer on USB is refused at once and stays "unauthorized" until Phase 1b (today it waits for a pop-up that never comes). USB permission dialogs (an emulator asking for a USB device, for example a GameCube adapter in Dolphin) do not appear, and the app is told "denied" at once (today they crash SystemUI, leave a dead record on top of the emulator, and the app gets no answer) [I]; Phase 2 makes them Nano pop-ups. Lock screen shortcuts and Wallpaper & style have no pop-up to convert and stay refused.

### 5.4 Part C: Settings' System > Backup (Phase S)

`packages/apps/Settings/src/com/android/settings/backup/BackupSettingsHelper.java`: if `mBackupManager` is null, or any call throws, use `getIntentForDefaultBackupSettings()` and the default label and summary, as `isBackupServiceActive` already does. About 10 lines. Optional: hide System > Backup in Nano mode (D5).

### 5.5 Part D: Seedvault in Nano mode (Phase S, D4)

Recommended: add `com.stevesoltys.seedvault` to `NanoProcessGuard`'s list. Its process is then never started in Nano mode; USB attach no longer forks and crashes it, and the USB broadcast is no longer delayed. Android mode is unchanged. Alternatives: guard Seedvault's `App.kt` against a null `IBackupManager` (a change to a prebuilt app's source), drop Seedvault from the build, or start BackupManagerService in Nano mode (not recommended: it binds Seedvault's transport at boot and keeps it resident, which minimal boot avoids on purpose).

### 5.6 Part E: gammapad's log flood (Phase S)

`frameworks/native/services/gammapad/InputTransformer.cpp` (Thimsy already patches it, P:175449) and `GamepadManager.cpp` (P:175251): log "Device maps set" only when the device or its maps change, and skip the per-batch map copy when the device is the same as the last batch. Cuts about 250-600 log writes a second, and keeps logcat long enough to hold crash evidence [C rate; I gain].

### 5.7 Part F: `[joypad_spi_read]` (measure only)

- It is the vendor SPI-MCU pad transport (`sprd,spi-mcu-joy`, SPI at about 1.2 MHz, buf-len 64; built into the vendor kernel 5.4.233) [C from the DT and kernel log, per the stability research]. Thimsy ships no kernel changes, so this is stock GammaOS and Anbernic behaviour [I]. No source is public [U].
- The built-in pad sends events even when idle: the singleadc-joypad input has fuzz=1, flat=1 over +/-16384 and a 4 ms poll [C], so ADC jitter becomes events [I].
- Nothing changes before the read-only checks in section 10 (priority, scheduling class, D state, CPU frequency). The only known knobs (the input-polldev `poll` interval, `init.joypad_control.rc`) trade input latency for games, so any change is a separate owner decision (D7).

### 5.8 Part G: folded into Part A

The first version of this plan had Nano's own debugging prompt here, for requests that arrive while Nano's screen is in front (`sys.gammaos.adb.confirm_seq`, `/data/system/nano_adb_confirm.txt`, NanoNetBridge verbs `adb_allow` / `adb_deny`). The owner's decision makes every pop-up a Nano pop-up, at the home and over apps, so this is now part of 5.2. The bridge there differs on purpose: answers do not go through `net_cmd`, which system-signed apps can write (5.2.7).

Kept as an option (D6): a "Wireless Debugging" row under Nano's Developer Options, next to USB Debugging (NanoMenuSettingsTree.cpp:733), showing the port and a pairing code from `WIRELESS_DEBUG_STATE_CHANGED_ACTION` and `enablePairingByPairingCode`. Phase 5, optional.

### 5.9 Not chosen

- **Run SystemUI in Nano mode with its UI hidden.** It needs StatusBarManagerService, DreamManagerService, MediaProjection and probably Accessibility in minimal boot, then the status bar, nav bar, volume, keyguard and notification windows suppressed. About 150-250 MB and constant CPU on a device already at load 15, window and focus fights with Nano and RetroArch, and a large regression surface [I].
- **Headless SystemUI** (forcing `SystemUIInitializer` down its stub-shell path in Nano). Other non-null providers (IDreamManager, IMediaProjectionManager) may still fail, and every hit still costs a fork [U].
- **SystemUI running in Nano mode for pop-ups only.** The owner ruled it out: SystemUI stays off in Nano mode. It would also need the services above.
- **Settings-hosted debugging prompts** (`ThimsyWifiDebuggingActivity`, `ThimsyUsbDebuggingActivity`; the first version of Part A). Superseded by the owner's decision. They could not draw at Nano's own screen (DRM plane, 5.2), would cover a game with an Android activity and pause it, and would be a second UI to keep in step with Nano's. Kept only as the D10 alternative.
- **Point the `config_customAdb*` strings at Settings in both modes.** Simpler, but changes Android mode's pop-ups.
- **A separate tiny privileged app for the prompts.** Dropped with the Settings-hosted prompts.
- **Answers through NanoNetBridge's `net_cmd`.** Writable by every system-signed app (5.2.7).
- **A property-change callback instead of polling the reply.** Java has no call that waits on one property, and `SystemProperties.addChangeCallback` fires only on an explicit sysprop report, not on `setprop` [I]. A 150 ms poll, only while a pop-up is outstanding, is enough once `answer` carries `shown` and Nano's own times (5.2.4).

### 5.10 Files

| File | Change | Phase |
|---|---|---|
| `frameworks/base/services/core/java/com/android/server/gammaos/NanoProcessGuard.java` | new pure class | 1a |
| `frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java` | refuse SystemUI activities; report them to the prompt service | 1a (report from 4) |
| `frameworks/base/services/core/java/com/android/server/am/ProcessList.java` | refuse process starts (services, broadcasts, providers); report hosting type and component | 1a (report from 4) |
| `frameworks/base/services/core/java/com/android/server/adb/AdbDebuggingManager.java` | Nano mode without the prompt service: deny a USB key at once (Part B call site 4). Then: hand both debugging prompts to Nano; current-prompt check before `NO`; unplug cancel only when `service.adb.tcp.port` is 0 | 1a, 1b |
| `frameworks/base/services/usb/java/com/android/server/usb/UsbUserPermissionManager.java` | Nano mode without the prompt service: answer the PendingIntent with granted=false (call site 4). Then the `usb_permission` pop-up and completion (D21) | 1a, 2 |
| `frameworks/base/services/core/java/com/android/server/am/ContentProviderHelper.java` | optional: clearer "refused provider" log line | 1a |
| `frameworks/base/cmds/gammaos-nano/test/java/NanoProcessGuardCheck.java`, `tests/unit/test_nano_process_guard.py` | new host check and test | 1a |
| `frameworks/base/services/core/java/com/android/server/gammaos/ThimsyPromptInternal.java`, `ThimsyPromptService.java`, `ThimsyPromptCodec.java`, `ThimsyPromptPolicy.java` | new: the pop-up bridge (1b interface only; toasts, notices and reports added in 3 and 4) | 1b |
| `frameworks/base/services/java/com/android/server/SystemServer.java` | start the prompt service in Nano mode, before AdbService | 1b |
| `system/sepolicy/private/property.te`, `property_contexts`, `file.te`, `file_contexts`, `system_server.te`, `bootanim.te`, and the `prebuilts/api/{34.0,202404}/private` copies | `gammaos_prompt_req_prop`, `gammaos_prompt_reply_prop`, `gammaos_prompt_data_file`, neverallows that exclude init | 1b |
| `frameworks/base/cmds/gammaos-nano/NanoMenuPrompt.cpp` (new), `NanoMenu.h`, `NanoMenuOverlay.cpp`, the home loop, `Android.bp` | prompt module, XMB renderer, prompt-only raise, `ready` and `handoff`, own expiry | 1b |
| `frameworks/base/cmds/gammaos-nano/ps3xmb/i18n/*.json`, `test/i18n_check.py` | pop-up strings: English in 1b, other languages in 1c (D9) | 1b, 1c (more in 2-5) |
| `frameworks/base/cmds/gammaos-nano/test/unit/NanoPrompt_test.cpp`, `tests/unit/test_thimsy_prompt.py` | new tests | 1b |
| `NanoMenuPrompt.cpp` | DSi and Minima renderers, touch, other-screen card | 1c |
| `.../usb/UsbHandlerManager.java`, `.../usb/UsbProfileGroupSettingsManager.java` | no app opened and no default-app grant on plug-in in Nano mode | 2 |
| `frameworks/base/services/core/java/com/android/server/notification/toast/TextToastRecord.java` | `show()` and `hide()` go to the toast sink in Nano mode; the app's toast callback is called from the prompt service | 3 |
| `NanoMenuOverlay.cpp`, `NanoMenuPrompt.cpp` | passive banner mode without `show_overlay` (D14); notices | 3 |
| `frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java` | report broadcasts addressed to SystemUI (Part B call site 5) | 4 |
| `ThimsyPromptService.java` | `battery_saver` completion; screen-capture notice | 4 |
| `frameworks/base/services/core/java/com/android/server/sensorprivacy/SensorPrivacyService.java`, `.../logcat/LogcatManagerService.java` | deny with a toast in Nano mode | 5 |
| `frameworks/base/services/core/java/com/android/server/policy/PhoneWindowManager.java`, Nano's `qs_summon` handling | brightness bar and Wi-Fi screen summons; screenshot chord per D16 | 5 |
| `frameworks/base/services/core/java/com/android/server/audio/AudioService.java` | app volume changes show Nano's volume bar in Nano mode | 5 |
| Nano's battery poll (`pollBattery`) | low-battery toast | 5 |
| `frameworks/base/cmds/gammaos-nano/NanoMenuSettings.cpp`, `NanoMenuSettingsTree.cpp` | optional Wireless Debugging row (D6) | 5 |
| `packages/apps/Settings/src/com/android/settings/backup/BackupSettingsHelper.java` | null-safe | S |
| `frameworks/native/services/gammapad/InputTransformer.cpp`, `GamepadManager.cpp` | log only on change | S |
| `README.md` (Known issues, Developer options) | Nano pop-ups, what is refused in Nano mode, the per-network trust, a default USB app from Android mode not followed in Nano mode | 1a, 1b, then each phase |

No Settings changes for the pop-ups any more; Settings is touched only by Part C.

All framework and Settings changes go into `patches/thimsy/0001-thimsy.patch`, regenerated from `/aosp/gammaos` after the edits (never while an AOSP build runs in WSL).

## 6. Tests

### 6.1 Host tests (no device)

- **`tests/unit/test_nano_process_guard.py`** compiles `NanoProcessGuard.java` and `NanoProcessGuardCheck.java` from the files the patch creates (the `new_file` helper pattern of `test_screen_arrangement.py`), and checks:
  - Nano mode: `com.android.systemui` refused; Seedvault refused only when listed; `com.android.settings`, `android`, `com.android.networkstack` and an empty or null package never refused;
  - Android mode: nothing refused;
  - the cached value never comes from a `persist.` property (a text check on the patch);
  - text checks: in Nano mode without the prompt service, `startConfirmationForKey` reaches `denyDebugging()` and `requestPermissionDialog` sends granted=false (call site 4).
- **`tests/unit/test_thimsy_prompt.py`** (Phase 1b) compiles `ThimsyPromptCodec.java` and `ThimsyPromptPolicy.java` from the patch and checks:
  - the codec: sanitising (control characters, newlines, bidi overrides, length caps), the JSON has no format strings, replies with a wrong id, a wrong nonce, an old `seq` or a bad grammar are refused, an `answer` with no earlier `shown` is accepted, an id is answered once, the longest reply fits in 91 characters;
  - the policy: per-type timeouts end in deny; `allow` with `press_ms` less than 500 ms after `shown_ms` is deny, whatever the poll delay; `busy` meets the 3 s deadline and is republished with a new `seq` until the answer timeout; `handoff` waits up to 10 s; a new `adb_wifi` denies the old one; a new `adb_usb` closes the old one with no `NO`, and a deny or timeout for a request that is no longer current sends nothing; `usb_permission` merges (uid, device); `adb_wifi` with no Nano is denied at once; NANO_LOST republishes `adb_usb` and `usb_permission` within the original timeout;
  - text checks on the patch: the `config_customAdb*` and `config_usb*` strings in `core/res/res/values/config.xml` are unchanged (Android mode); every hook is behind `isActive()` (minimal boot read once, never a `persist.` property for the mode); among the lines the patch adds (`+` lines only), no call to `allowDebugging`, `allowWirelessDebugging`, `grantDevicePermission`, `grantAccessoryPermission`, `setDevicePackage`, `setAccessoryPackage`, `setDevicePersistentPermission`, `setAccessoryPersistentPermission` or `setPowerSaveModeEnabled` appears outside the prompt callbacks; `gammaos_prompt_reply_prop` is set only by `bootanim` and `gammaos_prompt_req_prop` only by `system_server`, with neverallows that exclude `init`, in `private` and both `prebuilts` copies; `/data/system/nano_prompt` has `gammaos_prompt_data_file`; the adb_usb unplug receiver is registered only when `service.adb.tcp.port` is 0; `TextToastRecord.hide()` no longer reaches `checkNotNull(mStatusBar)` in Nano mode and the toast callback is posted, not called under `mToastQueue` (Phase 3); the passive banner path never sets `show_overlay` (Phase 3).
- **`test/unit/NanoPrompt_test.cpp`** (Phase 1b, in `test_nano_units.py`'s g++ suite): request parsing (bad version, unknown type, missing fields, oversized strings), replace, a new `seq` of the same id, `cancel`, own expiry at `timeout_ms` plus 2 s, the arming delay, the release-first rule, Allow needing a down and an up edge after arming, B meaning deny, focus starting on Deny, the reply carrying `shown_ms` and `press_ms`, and the 640x480 layout maths with maximum-length values (all rows inside the panel, checkbox label on at most 2 lines, untrusted values ellipsized first).
- **`test/i18n_check.py`**: the new keys exist in English (1b) and, from 1c, in all 21 files with matching positional arguments.
- **Phase S:** extend the gammapad host check (if `test_nano_units.py`'s g++ suite covers InputTransformer; otherwise a text check) that "Device maps set" is logged only on change.

### 6.2 Build checks

- Services, Nano, sepolicy and gammapad compile in the full system build (after the AOSP build in progress finishes; never two builds at once).
- The built sepolicy contains the two new property types and the file type, the neverallows compile (init excluded), and `property_contexts` resolves `sys.gammaos.prompt.reply` to `gammaos_prompt_reply_prop` (not `persist_gammaos_prop`).

### 6.3 Device steps

Rules for every step: nothing touches dwc3 debugfs or `/sys/class/display/dispc1`; no `stop`/`start` with a monitor attached; boot without the monitor.

Phase 1a, Nano mode:
1. **No SystemUI ever.** On an untrusted network, turn on Wireless debugging: the switch flips back, `logcat` shows "refused activity ... WifiDebuggingActivity", no SystemUI process starts (`logcat -b events | grep am_proc_start` shows none for systemui). Then Display > Brightness level and Sound > "Play media to": nothing opens, Settings keeps the controller, `dumpsys dropbox --print system_app_crash` gains no SystemUI entry. Then Wallpaper & style: no SystemUI process; ThemePicker may still crash on its own (WallpaperManagerService is off); record any `data_app_crash` for it as a separate finding. Plug in a computer not authorised yet: it is refused at once ("Denying adb confirmation" in logcat), with no wait. An app asking for a USB device is told "denied" at once.
2. **No dead window.** After 1, close Settings from Nano, start and quit a RetroArch game: no "Start proc ... com.android.systemui", no "Skip zero pid ANR", `dumpsys window | grep mCurrentFocus` shows the game or nothing stale.
3. **Settings stability soak.** 15 minutes browsing every top-level Settings page with the controller, Developer options included, plus Display > Screen saver ("Start now"), Accessibility and System > Backup: no Settings crash, ANR or freeze (`dumpsys activity exit-info com.android.settings`, `dumpsys dropbox --print data_app_crash`). Crashes on those three pages are separate findings (missing services, not SystemUI).

Phase 1b, Nano mode:
4. **Spike, before the bridge is built (half a day).** With a test build of Nano, raise a prompt-only layer from parked, without building the XMB (precedent: `qs_summon=3`, P:26283-26285), over Settings, over a RetroArch game and over a DraStic game. Check: the layer shows within a few hundred ms; Nano reads the pad while RetroArch or DraStic holds it with EVIOCGRAB (overlay.cpp:389-392); `overlayPoll`'s idle tick runs while parked; after closing, the `drop_fence` drops the closing key; the game does or does not relayout under the orientation clamp. The result decides D11 and whether the over-game path ships in 1b or waits.
5. **Wi-Fi pop-up over Settings.** On an access point (or band) not trusted yet, open Settings from Nano and turn on Wireless debugging. The Nano layer rises over Settings with the pop-up: SSID, BSSID, "Always allow on this network", focus on Deny. Allow with the box ticked, controller only: the switch stays on, `adb pair` / `adb connect` work, and Settings has the controller again (no key leaks into Settings). `logcat` shows the prompt id, `shown` and `answer`.
6. Same, Deny, and Back: the switch goes off. Same, Allow without the box: works until the next reboot; after a reboot the pop-up shows again. Change networks, or turn Wi-Fi off, while it is up: it closes as denied.
7. **Press safety.** Hold A while turning the switch on: the pop-up appears, Allow cannot be pressed until A is released and pressed again after the arming delay. Press B at once after the pop-up appears: the request ends at once in `logcat`, not after 60 s. Press Allow just after the arming fill completes: accepted. Leave it 60 s: it closes, deny.
8. **USB pop-up** (only if `ro.adb.secure=1`). Prefer a second PC never authorised. Otherwise, on the test PC: `adb kill-server`, move `~/.android/adbkey*` aside, and **move them back afterwards** (then `adb kill-server` again). If the pop-up fails, authorise the PC once in Android mode to get adb back. Check: at the Nano home (no app launched); over Settings; over a RetroArch game (the game keeps running unless D11 says pause, and gets the controller back after the answer; the Quick Menu still closes the game). Unplug during the pop-up: it closes as denied. Replug while one is up, so adbd reconnects and asks again: the old pop-up closes without a deny, the new one shows, and Allow on it authorises the PC (no "authorization for unknown prompt" in logcat).
9. **Nano gone.** Kill the overlay process with a USB pop-up on screen (with the owner present): no grant; init respawns the overlay into Nano's overlay home (not back into Settings); the new Nano shows the pop-up again while time is left, or it is denied at 60 s; the user resumes Settings from the overlay home and has the controller. With the overlay switched off (`persist.gammaos.nano.overlay=0`) and Settings in front, turn on Wireless debugging: denied within 3 s, logged, no crash (D10).
10. **Boot case.** With `persist.adb.tls_server.enable=1` on an untrusted network, boot: the Wi-Fi pop-up shows at the Nano home, or is denied with a log line if Nano is not ready; never a crash.
11. **SELinux and spoofing.** No AVC denials for system_server or bootanim on the request file or the properties (`logcat -b events | grep avc`); `ls -Z /data/system/nano_prompt` shows `gammaos_prompt_data_file`. From an unrooted `adb shell`, `setprop sys.gammaos.prompt.reply ...` is refused. After `adb root` the same setprop is expected to succeed (`su` is permissive on userdebug, 5.2.4); record it, it is not a failure.

Phase 1c, Nano mode:
12. Steps 5 and 8 in the DSi and Minima themes, and the XMB theme at 640x480 (a 640x480 GammaOS device if one is at hand; otherwise the g++ layout test plus a 720x720 and a 1920x1080 check), with Nano set to a language other than English (D9). Touch on the buttons works only after arming. On a dual-screen setup the pop-up is on the panel with the controller and the other panel shows the "answer on the other screen" card.

Phase 1, Android mode (must be unchanged):
13. Wireless debugging and a new USB computer show SystemUI's own prompts. Brightness level and "Play media to" open SystemUI's dialogs. Toasts show as before. No "GammaOS Nano: refused" lines, no Phase 1a deny lines and no prompt-service log lines.

Phase 2:
14. Dolphin with a GameCube adapter (or any app that calls `UsbManager.requestPermission`): the Nano pop-up over the emulator names the app and the device; Allow gives access; with D21's recommendation there is no "Always" box and the app asks again after a replug (with the alternative: no second pop-up after a replug or a reboot, in either mode). Deny and the 30 s timeout leave the app without access and the app does not hang. Unplug during the pop-up: closes as denied.
15. Plug in a USB device an installed app declares an attach filter for: no app opens and nothing is granted in Nano mode (D13), also when that app was made the default in Android mode; in Android mode the stock dialog or the default app shows.

Phase 3:
16. Settings: tap Build number 7 times; "You are now a developer!" shows as a Nano banner over Settings. A toast at the Nano home and over the raised Quick Menu shows in the theme's banner style. Over a game per D14: the game keeps the controller, `getprop sys.gammaos.nano.show_overlay` stays 0 during the banner, and the overlay's CPU is measured. Many toasts at once: one every 2 s at most, duplicates dropped. No NotificationManagerService exception in logcat.

Phase 4:
17. An app that asks for screen capture (for example a screen recorder): a Nano notice "Screen capture isn't available in Nano mode" as a passive banner, the app gets RESULT_CANCELED and does not hang.
18. Battery Saver (with the owner present; it changes a setting): `settings put secure low_power_warning_acknowledged 0` and `settings put secure extra_low_power_warning_acknowledged 0`, then turn on Battery Saver in Settings: a Nano pop-up; Turn on: saver is on and both settings are 1; a second try with Cancel (after resetting them): saver stays off.

Phase 5:
19. Display > Brightness level opens Nano's brightness bar. The battery at 20 % and 10 % gives a Nano warning once each. Media output and the contrast dialog give a Nano notice. An app that opens the Internet panel gets Nano's Wi-Fi screen (or Settings' Wi-Fi page, D20). An app volume change with `FLAG_SHOW_UI` (or Bluetooth headphones) shows Nano's volume bar. The screenshot chord follows D16.

Phase S:
20. In Nano mode, System > Backup opens without crashing Settings.
21. Plug and unplug a USB pad and the dock 5 times: no Seedvault start, no Seedvault crash, no "Skip zero pid ANR" for it (if D4 = guard). In Android mode Seedvault still works.
22. `logcat -d -s gammapad | grep -c "Device maps set"` after a minute of play: a handful, not tens of thousands.

## 7. Phases

| Phase | Content | Effort | Risk |
|---|---|---|---|
| 0 | Owner's read-only checks (section 10): adb props, config overlays, what starts SystemUI, Settings exits, load and the joypad thread; plus safe volume, USB attach filters, Dolphin's USB path, USB drives and pads, the microphone | Small (under an hour) | None |
| 1a | Part B: NanoProcessGuard in ActivityStarter and ProcessList, so SystemUI is never started in Nano mode and no dead window is left; the stock USB key and USB permission requests answered "deny" at once instead of waiting forever (call site 4); host test; README note | Small (1.5 to 2 days) | Low (process-start paths in AMS; gated on Nano mode) |
| 1b | Half-day spike first (6.3 step 4). Then the Part A bridge: ThimsyPromptService (minimal interface), codec, policy, SELinux types and file type, Nano prompt module, XMB renderer in every theme, prompt-only overlay raise and home hook, `ready`, `handoff`, `seq`, Nano-lost check and Nano's own expiry; the `adb_wifi` and `adb_usb` pop-ups; English text; host tests; README | Medium (6.5 to 7.5 days) | Medium (a new grant path; Nano raising over games; new sepolicy) |
| 1c | DSi and Minima renderers, 640x480 checks, touch, dual-screen card, translations of the pop-up texts (D9) | Small-medium (about 2.5 days) | Low |
| 2 | `usb_permission` pop-up and completion ("Always" per D21); no app opened and no default-app grant on USB plug-in in Nano mode (D13) | Small-medium (about 2 days) | Medium (USB grants; an app's PendingIntent must always be answered) |
| 3 | Text toasts: TextToastRecord hook with the app's callback posted, toast sink, Nano banner at the home and over the raised overlay, passive banner mode over apps without `show_overlay` (D14); the passive `notice` type | Medium (2.5 to 3 days) | Medium (an NMS crash if `hide()` is missed; overlay cost over games) |
| 4 | Refused-start reports (call sites 1, 2 and 5); screen capture refused with a Nano notice (D15); the `battery_saver` pop-up | Small (about 1.5 days) | Low |
| 5 | The rest: brightness dialog to Nano's brightness bar, low-battery warning, notices for media output and contrast, Internet panel to Nano's Wi-Fi screen (D20), app volume changes to Nano's volume bar, sensor privacy and log access, screenshot chord (D16); optional heads-up toasts (D18) and Wireless Debugging row (D6) | Medium (about 3.5 days; plus 1-2 for the row) | Low |
| S | Part C (System > Backup null-safe), Part D (Seedvault per D4), Part E (gammapad log only on change) | Small (about 1 day) | Low |
| Measure | `[joypad_spi_read]` and overlay CPU, from Phase 0's output; any change is its own decision (D7) | Small to measure | None until a change |
| Watch | Bluetooth start timeout: raise `bluetooth.gd.start_timeout` only if the abort repeats | Tiny | Low |

Phase 1a fixes the dead window and is expected to fix the main part of report 2; Phase 0's `am_proc_start` check says whether the uncaptured test 43 bursts had the same trigger. Phase 1b fixes report 1, at the Nano home too; its spike comes first because report 1 needs the raise over Settings. Phases 2-5 need 1b's bridge. The order follows the priorities in 5.2.1: the P1 debugging pop-ups first, then the larger P2 items (USB permission, toasts, Battery Saver); Phase 5 puts the small P2 items (brightness, low battery) together with the P3 ones, because each takes under half a day. Phase 3 comes before Phase 4 because notices use the toast banner. Phase S is independent. All of it is about 21 to 24 days; effort includes host tests and the device steps.

## 8. Owner decisions

Recommendations first.

Resolved or replaced by the owner's decision "Convert it to Nano pop-ups" (2026-11-27):
- **D1. New prompts in Nano mode only: resolved.** Android mode keeps SystemUI's pop-ups unchanged; Nano mode gets Nano pop-ups.
- **D2. Refuse SystemUI in Nano mode: resolved, yes.** SystemUI stays off in Nano mode (Part B). What it used to show becomes a Nano pop-up, toast, OSD or a clean deny (5.2.1).
- **D3. Debugging requests while Nano's own screen or a game is in front: superseded.** Nano draws the pop-up at its home and over apps (5.2.2). The remaining questions are D10 and D11.
- **D8. Phase 4 USB permission dialogs: superseded.** The USB permission pop-up is now Phase 2, as a Nano pop-up.
- **D9. Translations: still open, now for Nano.** Copy SystemUI's AOSP translations of the dialog texts into Nano's 21 i18n files in Phase 1c (recommended), or English only. Phase 1b ships English either way.

Unchanged:
- **D4. Seedvault in Nano mode:** never start it in Nano mode (recommended; backup works in Android mode only, as today in practice). Alternatives: patch Seedvault, drop it from the build, or start the backup service in Nano mode (not recommended).
- **D5. System > Backup in Nano mode:** null-safe fallback (recommended). Also hide it in Nano mode?
- **D6. A Nano "Wireless Debugging" row** with the port and a pairing code shown in Nano (Phase 5 option)?
- **D7. `[joypad_spi_read]`:** run the read-only checks first (recommended). Any poll-interval change adds input latency in games and needs its own go-ahead.

New:
- **D10. When no Nano process can show a pop-up** (the overlay switched off with an app in front, a drastic-nano session, Nano crashed): the Wi-Fi request is denied at once and logged (toggle again later); the USB debugging, USB permission and Battery Saver requests wait and are shown when a Nano is ready, and are denied at the timeout (recommended). Alternative: keep the Settings-hosted debugging activities as a fallback for these cases only (about 2 days more, and a second UI to keep in step).
- **D11. Pop-ups over a game and over Nano's own screens:** the overlay rises over the game without pausing it, as the keyboard over an app does (recommended, if the Phase 1b spike shows it works); or pause the game with `NanoPauseActivity` while the pop-up is up. Over an open Quick Menu or Applications screen the pop-up shows on top (recommended); while the keyboard is open or an app is launching it waits (`busy`).
- **D12. Timeouts:** 60 s for both debugging pop-ups, 30 s for USB permission and Battery Saver, with a visible countdown (recommended). Longer timeouts keep a game behind the pop-up longer.
- **D13. USB "Open <app> to handle <device>?" on plug-in, in Nano mode:** open nothing and grant nothing (recommended; apps are launched from Nano, and the app still gets the USB permission pop-up when it opens the device). Alternative: a Nano pop-up that launches the app. Either way, a default chosen in Android mode ("always open X") is not followed in Nano mode; the README says so.
- **D14. Toasts over a running app:** Nano's passive banner (only Nano's layer shows for the toast; `show_overlay` stays 0 and the app keeps the controller; recommended, because it is Nano's own style), or a system_server window styled like Nano (cheaper, drawn by Android). Also: hide toasts while an emulator is in front?
- **D15. Screen capture in Nano mode:** deny with a Nano notice (recommended). Alternative: start MediaProjectionManagerService in Nano mode and give it a real Nano consent pop-up. In Android 14 the consent result is an activity result (the app calls `startActivityForResult` on `config_mediaProjectionPermissionDialogComponent` and gets the projection through `setResult`; the re-consent path starts the same activity, MediaProjectionManagerService.java:433-508), so this needs a small forwarder activity in Android besides the Nano pop-up: more memory and more work (about 2-3 days), for screen recorders and casting.
- **D16. The Volume-down + Power screenshot chord in Nano mode:** refuse it (recommended for now), or take a screenshot into Pictures/Screenshots with a Nano "Screenshot saved" toast.
- **D17. Which presses count for Allow:** only physical controls (recommended, if gammapad lets Nano tell its virtual device and macros apart [U]), or any input Nano reads.
- **D18. Heads-up notifications** as Nano toasts for high-importance notifications (optional, Phase 5)?
- **D19. The ANR dialog** (not SystemUI, but an Android window in Nano mode): a Nano "Wait / Close app" pop-up later, or leave it?
- **D20. Pop-ups refused with a notice instead of a Nano pop-up:** media output ("Play media to"), the contrast dialog, log access (auto-declined) and the microphone reminder get a passive Nano notice; the Internet panel opens Nano's Wi-Fi screen, or Settings' own Wi-Fi page if Nano has none that can be summoned (recommended). Alternative for media output, the most likely of these on a handheld with Bluetooth headphones: a Nano output picker (a list of outputs, completed by system_server through the media router; about 2 days more, Phase 5 or later).
- **D21. The "Always open <app> when <device> is connected" box in the USB permission pop-up:** leave it out in Nano mode (recommended): stock "always" only makes the app the default for plug-in, which D13 turns off, so the box would do nothing. Alternative: show "Always allow <app> to use <device>", completed with `setDevicePersistentPermission`: no pop-up for that app and device model again, after replugs and reboots, in Nano and Android mode alike; whether Android's Settings offers a way to take it back is [U].

## 9. Open questions

- What is `ro.adb.secure` on the build? Very likely 1 (LineageOS userdebug) [I]; Phase 0 confirms whether the USB key prompt is in play.
- Does any overlay replace `config_customAdbWifiNetworkConfirmationComponent` or `config_customAdbPublicKeyConfirmationComponent`?
- What started the test 43 bursts at uptime 95-120 s (11-26 19:43) and 165 s (21:51)? A Wireless debugging toggle right after boot, or AdbService at boot with `persist.adb.tls_server.enable=1` on an untrusted network? The second would mean the pop-up can fire at boot while Nano's home is in front (5.2.2, 6.3 step 10). `am_proc_start` in the events log names the cause.
- Does AdbDebuggingManager prompt again when the Cube moves to another access point while wireless debugging is on, or just switch off?
- Did the owner see Settings ANRs or kills? Only `system_app_crash` was captured; `system_app_anr`, `data_app_crash` and Settings' exit-info will tell.
- ~~Which signal best says "Nano's own screen is in front" in system_server?~~ No longer needed: system_server does not choose where the pop-up goes; Nano does (5.2.2).
- ~~How long does a cold start of Settings take in Nano mode?~~ No longer needed: no Settings-hosted prompts.
- `[joypad_spi_read]`: real-time priority or not, D state or running, and the CPU governor and frequency at the time (`persist.gammaos.performance_mode`). Does stock GammaOS show the same 52%?
- Why does `gammaos-nano --overlay` use 37% CPU while Settings is in front: reading evdev at the pad's event rate, or drawing while hidden?
- Is the dead activity record cleared by anything short of a reboot? `am force-stop com.android.systemui` should finish it (force-stop finishes a package's activity records even with no process) [I, not tried; nothing else cleared it in the captures].
- How long is the controller lost when the dead record comes back on top of Settings (A0:11746-11862 shows the page hidden and a focus wait, not its length)?
- With a game in front, does the prompt-only overlay raise work over every emulator: input with RetroArch or DraStic holding the pad with EVIOCGRAB (overlay.cpp:389-392), z-order against drastic-nano's own layer, and the orientation clamp while `show_overlay=1` (overlay.cpp:603-607) making the game relayout [U]? (The Phase 1b spike, 6.3 step 4, then step 8.)
- Does the overlay's idle tick keep running while it is parked behind a game, so `promptPoll` sees a request within a few hundred ms [I]? (The spike.)
- Does the new `gammaos_prompt_data_file` type need more than `file_contexts`, a `type_transition` or a `restorecon` in the service's start code, so that system_server creates the directory with that label [I]? No AVC denial is expected for bootanim either way, because it is permissive.
- Can system_server read `init.svc.<service>` for Nano's services, and what are their exact names in gammaos-nano.rc (5.2.6) [I]?
- Does SystemUI's PowerNotificationWarnings register its `PNW.startSaverConfirmation` receiver with the `DEVICE_POWER` permission, so the Phase 4 hook copies that check [I]?
- Does Nano have a Wi-Fi screen that `qs_summon` can open over an app (row 15) [U]? Does Nano already read a thermal status (row 8) [U]?
- With D21's alternative, does Android mode's Settings show a way to revoke a persistent USB permission [U]?
- Where exactly in the 14 tree does the broadcast report go: `ActivityManagerService.broadcastIntentLocked` or a helper it calls [I]?
- Which locale do pop-ups use: Nano's own language setting or `persist.sys.locale`? (Recommended: Nano's, so the pop-up matches the rest of Nano.)
- Do ES-DE engine themes need their own pop-up style, or is the XMB one enough [U]?
- Which shipped apps declare `USB_DEVICE_ATTACHED` activity filters, and does Dolphin's GameCube adapter path call `UsbManager.requestPermission` [U]? (Phase 0.)
- Is the Volume-down + Power chord reachable over apps in Nano mode [I]? Is safe-media volume or sound dose active (`dumpsys audio`) [I: looks off]? Does the Cube have a microphone [U]?
- How often are text toasts dropped today? No captured logcat has "StatusBar not available to show text toast" (0 in the test 42 logs) [U]; Phase 0 greps for it.
- NanoPermGrantBridge grants storage and microphone access without a press (BASE SystemServer.java:4120-4166). It conflicts with "nothing is granted without the user's press" but is outside this plan; does the owner want it reviewed separately?
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
adb shell 'dumpsys audio | grep -iE "safe|sound dose|csd" | head -20'
adb shell 'dumpsys package r | grep -B2 -A6 USB_DEVICE_ATTACHED | head -60'
adb shell 'logcat -d | grep -c "StatusBar not available to show text toast"'
adb shell 'cat /proc/asound/cards; ls /dev/snd'
adb shell 'getprop service.adb.tcp.port; getprop | grep -E "init.svc.*(nano|bootanim)"'
adb shell 'settings get secure low_power_warning_acknowledged; settings get secure extra_low_power_warning_acknowledged'
adb shell 'sm list-volumes all'   # with a USB drive plugged in: mounted without SystemUI?
adb shell 'dumpsys input | grep -E "^ +[0-9]+: " | head -30'   # with a USB pad plugged in: listed as an input device
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
