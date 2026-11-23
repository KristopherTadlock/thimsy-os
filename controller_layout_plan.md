# Controller Layout Plan

Status: design plan with a read-only audit. Nothing is implemented.

Date: 2026-11-15

Trees: AOSP `~/gammaos-src` (GammaOS Next 1.4.4 base, branch `nano-144`). Build repo `~/thimsy`.

Paths:
- `nano/` means `~/gammaos-src/frameworks/base/cmds/gammaos-nano/`.
- `drastic-nano/` means `~/gammaos-src/frameworks/base/cmds/drastic-nano/`.
- `pwm/` means `~/gammaos-src/frameworks/base/services/core/java/com/android/server/policy/`.
- `gammapad/` means `~/gammaos-src/frameworks/native/services/gammapad/`.
- `kl/` means `~/gammaos-src/frameworks/base/data/keyboards/`.
- `mods/` means `~/thimsy/mods/`.
- `bmap plan` means `~/thimsy/button_map_overlay_plan.md`. Its §3 holds the per-emulator source evidence (shipped tags, cloned under `/tmp/bmap-*`), and this plan cites it rather than repeat it.
- Line numbers are for the current tree.

Audit sources outside the trees (all read-only, nothing executed):
- `/tmp/ctl-audit/pac/kernel`, `config`: the kernel and its `IKCFG` config, taken from `boot.img.signed` in `out/RG_Cube_Thimsy_OS_v1.0a.pac` with `scripts/pactool.py extract`.
- `/tmp/ctl-audit/src/`: `hid-nintendo.c`, `hid-sony.c`, `hid-playstation.c`, `xpad.c` from Android Common Kernel `android12-5.4`.
- `/tmp/ctl-audit/sdl/`: `controller_list.h`, `controller_type.c`, `SDL_joystick.c` from SDL `fa2c02bb` (the commit Vita3K ships, bmap plan §3.2). Also `/tmp/bmap-sdl` (`SDL_gamepad.c`, `SDL_gamepad_db.h`).
- `/tmp/bmap-payload/`: the shipped RetroArch payload (`RA.cfg` below is its `sdcard/Android/data/com.retroarch.aarch64/files/retroarch.cfg`).
- `/tmp/bmap-retroarch/ra`, `/tmp/bmap-armsx2`, `/tmp/bmap-flycast`: emulator sources at the shipped tags.

"Unverified" means it was read from source or inferred, not checked on a device with `getevent`/`evtest`.

---

## 0. Summary

The user wants menu buttons fixed by **position**, labels shown as **printed on the controller**, and an external controller to take over the prompts when one is connected. Android apps are left alone.

| Decision (user, final) | What it means here |
|---|---|
| Android apps and games untouched | No `Generic.kl`/`Generic.kcm` change and no fallback layout. Whether a **per-device** `.kl` for the Switch Pro counts is open (§14, Q1). |
| **ABXY Swap controls menus only** (2026-11-16, §16) | Game buttons always follow position, for every pad, with no swap. ABXY Swap keeps its name and place but only swaps accept and back in Thimsy's menus. Off: the handheld's own layout (RG Cube: right = accept, bottom = back). On: the other way round. It covers Nano (home, Quick Menu overlay, setup wizard, OSK, button map panel), drastic-nano's menu, and Nano's in-process runners' menus. |
| Menus fixed by position, whatever controller | Every evdev event is turned into a physical position (`Phys`) per device, then into a menu role. The top and left buttons keep their actions by position too (§2.3). |
| External controller = primary | The most recently connected external pad is primary. Prompts use its printed labels: letters, or PlayStation shapes. |
| **Merge Controllers**, default On | On: built-in and external both drive player 1. Off: the external pad is player 2. Done per emulator through provisioning (§9). |
| Emulators by position | Each console's buttons land by position (SNES A right, B bottom; PS Circle right, Cross bottom; Xbox A bottom). The button map overlay and Thimsy's labels use the primary or holding pad's print. |

**Corrections to the brief** (details in §1):

1. **The kernel is 5.4.233, not 5.15.** `strings kernel` gives `Linux version 5.4.233 (dell@dell-PowerEdge-R730XD) … Fri Feb 28 22:44:43 CST 2025`, and the config header says `Linux/arm64 5.4.233`. It has `CONFIG_HID_NINTENDO=y`, `CONFIG_HID_PLAYSTATION=y`, `CONFIG_HID_SONY=y`, `CONFIG_JOYSTICK_XPAD=y`, `CONFIG_HID_MICROSOFT=y`, `CONFIG_HID_GENERIC=y`, `CONFIG_UHID=y` (`/tmp/ctl-audit/pac/config`).
2. **`device-keys.json` does not know any device's layout.** Its entries hold only the function and Back keys (`mods/device-keys.json:3`, the `note`). No field says Nintendo or Xbox. **`pads.json` knows only the RG Cube** (`nano/ps3xmb/buttonmap/pads.json:3-139`, one pad, `south` labelled `B`). A layout field has to be added (§4.2).
3. **The Switch Pro is not by position on the Android side.** hid-nintendo reports it by position (B bottom = `BTN_SOUTH`). AOSP's `kl/Vendor_057e_Product_2009.kl:22-29` then maps it **by label**: bottom (B) → `BUTTON_B`, right (A) → `BUTTON_A`, top (X) → `BUTTON_X`, left (Y) → `BUTTON_Y`. Thimsy's code assumes the opposite in three places (`nano/NanoButtonMap.cpp:272-284`, `pwm/ThimsyDeviceKeys.java:221`, and bmap plan §4.1).
4. **Xbox pads via xpad are not by position at the evdev level either.** xpad reports X (left) as `BTN_X` = 0x133 = `BTN_NORTH`, and Y (top) as `BTN_Y` = 0x134 = `BTN_WEST` (`/tmp/ctl-audit/src/xpad.c:706-709`, `767-770`; `bionic/libc/kernel/uapi/linux/input-event-codes.h:327-330`). Android's Xbox layouts undo it (`kl/Vendor_045e_Product_028e.kl:19-22`), so apps are fine. But Nano, which reads raw evdev, treats Xbox X as "top", and `standardPadSpec` has the wrong evdev code for Xbox north and west (`nano/NanoButtonMap.cpp:286`).
5. **Nano does not confirm by position today, not even per pad.** It confirms on raw `BTN_SOUTH` and goes back on `BTN_EAST` (`nano/NanoMenuInput.cpp:3716-3717`, `3752-3755`). That is the right button on the Cube (its driver sends printed A as 304) but the **bottom** button on every external pad. In the brief's example, an Xbox pad on the Cube accepts with A (bottom) today, not B (right).
6. **A "Merge Controllers" setting already exists, and it does nothing.** `persist.gammaos.gamepad.merge` has a row (`nano/NanoMenuPS3Menu.cpp:9712`, `nano/NanoMenuPS3Data.h:347`, `nano/NanoMenuSettingsTree.cpp:547-548`). gammapad reads it and only logs it (`gammapad/GamepadManager.cpp:235`, `289`). There is one virtual device for all pads (`gammapad/GamepadManager.h:164`).
7. **gammapad scrambles a DualShock 4.** It looks up only `Vendor_XXXX_Product_XXXX.kl`, never the `_Version_` files (`gammapad/KeyLayoutParser.cpp:26-29`). hid-sony marks the DS4's mapping with version bit 0x8000 so that Android picks `_Version_8xxx.kl` (`/tmp/ctl-audit/src/hid-sony.c:2875-2883`). gammapad gets the legacy `kl/Vendor_054c_Product_09cc.kl:23-34` instead, so Cross becomes `BTN_X`, Square becomes L1 and L1 becomes L2 (unverified on device, derived from the two files).

Phases (§12):
1. The layout table and the per-device normalisation, host only.
2. Nano's menus: the setting, positions, prompts by primary pad, migration.
3. drastic-nano and Nano's in-process runners.
4. Primary-controller tracking in PhoneWindowManager. Hotkeys by position.
5. Merge Controllers and positional provisioning per emulator.
6. gammapad fixes.
7. Optional: an "Identify controller" flow for unknown pads, and the Switch Pro layout if approved.

---

## 1. How it works today (audit)

### 1.1 Kernel drivers: what each family sends at evdev

Face buttons, as raw codes (304 `BTN_SOUTH`/`BTN_A`, 305 `BTN_EAST`/`BTN_B`, 307 `BTN_NORTH`/`BTN_X`, 308 `BTN_WEST`/`BTN_Y`):

| Family | Driver (this kernel) | Bottom | Right | Top | Left | Follows the Linux gamepad spec? | Evidence |
|---|---|---|---|---|---|---|---|
| RG Cube built-in (`retrogame_joypad`, 484b:1101) | vendor `singleadc-joypad` | 305 (printed B) | 304 (printed A) | 307 (X) | 308 (Y) | No: A/B by label, X/Y happen to fit | `nano/ps3xmb/buttonmap/pads.json:11-34`; vendor `.kl` by print (`/tmp/devbtn/cv/cube/vendor/usr/keylayout/Vendor_484b_Product_1101.kl`: 304 `BUTTON_A`, 305 `BUTTON_B`); `mods/27-controller-layout.sh:2-9`; device research level 1 (`work/device-buttons-research.md`) |
| Xbox 360 / One / Series, USB, and 8BitDo in X-input mode | `xpad` | 304 (A) | 305 (B) | 308 (Y) | 307 (X) | No: X/Y by label (north and west swapped) | `xpad.c:706-709`, `767-770`, `902-904`; 8BitDo PIDs `xpad.c:345`, `477` |
| Xbox One S / Series over Bluetooth (02e0, 02fd, 0b13) | `hid-generic` or `hid-microsoft` | 304 | 305 | 308 (Y) | 307 (X) | **Unverified.** HID button order A, B, -, X, Y would give the same as xpad | none in the tree; needs `getevent` |
| DualShock 3 | `hid-sony` (sixaxis keymap) | 304 (Cross) | 305 (Circle) | 307 | 308 | Yes. Version gets bit 0x8000 | `hid-sony.c:370-371`, `2881-2883` |
| DualShock 4 | `hid-sony` (`ds4_keymap`) | 304 (Cross) | 305 (Circle) | 307 (Triangle) | 308 (Square) | Yes. Version gets bit 0x8000 | `hid-sony.c:412-418`, `2881-2883` |
| DualSense, DualSense Edge | `hid-playstation` (backport) | 304 | 305 | 307 | 308 | Yes | `hid-playstation.c:292-295`; `CONFIG_HID_PLAYSTATION=y` |
| Switch Pro (USB and BT), and 8BitDo in Switch mode | `hid-nintendo` (early ACK backport, no IMU) | 304 (B) | 305 (A) | 307 (X) | 308 (Y) | Yes | `hid-nintendo.c:500-503`, ids `:797-804`. The shipped kernel has the same functions (`joycon_request_calibration`, `joycon_set_player_leds`) and no IMU strings, so it matches this version (unverified on device) |
| 8BitDo in D-input or Android mode (2dc8:xxxx), other HID pads | `hid-generic` | depends on the HID descriptor | | | | Often not | SDL's Linux mappings for 8BitDo D-input say `a:b1,b:b0` (`/tmp/bmap-sdl/src/joystick/SDL_gamepad_db.h:497-776`) |

So on this kernel the families split three ways at evdev: spec (PlayStation, Switch Pro), xpad style (Xbox: X/Y swapped), and the Cube's own (A/B swapped).

### 1.2 Key layouts: what Android apps get

Android picks `Vendor_V_Product_P_Version_N.kl`, then `Vendor_V_Product_P.kl`, then `Generic.kl`. Thimsy installs one override in `/product`, which Android searches before `/vendor` (`mods/27-controller-layout.sh:18-25`).

| Family | Layout file | Bottom | Right | Top | Left | By position? |
|---|---|---|---|---|---|---|
| RG Cube | `/product/usr/keylayout/Vendor_484b_Product_1101.kl` (Thimsy, `mods/keylayout/Vendor_484b_Product_1101.kl:7-11`) | `BUTTON_A` | `BUTTON_B` | `BUTTON_Y` | `BUTTON_X` | Yes |
| Xbox (USB/xpad) | `kl/Vendor_045e_Product_028e.kl:19-22` and siblings (`02d1`, `02dd`, `02ea:21-24`, `0b12:19-22`) | `BUTTON_A` | `BUTTON_B` | `BUTTON_Y` | `BUTTON_X` | Yes |
| Xbox Series BT (0b13) | none in AOSP, so `Generic.kl:289-293` | `BUTTON_A` | `BUTTON_B` | `BUTTON_Y` | `BUTTON_X` | Yes, if §1.1's BT row holds (unverified) |
| DualShock 4 | `kl/Vendor_054c_Product_09cc_Version_8111.kl:25-31` (`_8100` same) | `BUTTON_A` | `BUTTON_B` | `BUTTON_Y` | `BUTTON_X` | Yes |
| DualSense | `kl/Vendor_054c_Product_0ce6.kl:20` (`requires_kernel_config CONFIG_HID_PLAYSTATION`), `:25-31` | `BUTTON_A` | `BUTTON_B` | `BUTTON_Y` | `BUTTON_X` | Yes |
| **Switch Pro** | `kl/Vendor_057e_Product_2009.kl:22-29` | `BUTTON_B` | `BUTTON_A` | `BUTTON_X` | `BUTTON_Y` | **No: by label** |
| 8BitDo SN30 Pro, D-input (2dc8:6101) | `kl/Vendor_2dc8_Product_6101.kl:22-28` (it swaps to positions) | `BUTTON_A` | `BUTTON_B` | `BUTTON_Y` | `BUTTON_X` | Yes, for this one PID |
| gammapad's virtual pad (045e:0b13) | `Generic.kl:289-293` | see §1.3 | | | | |

Android's convention is the Xbox one: `BUTTON_A` bottom, `BUTTON_B` right, `BUTTON_X` left, `BUTTON_Y` top. Every family above follows it except the Switch Pro.

### 1.3 gammapad (Controller Enable)

- **Off by default** on the Cube: `persist.gammaos.gamepad.enable` default `0` (`nano/NanoMenuPS3Menu.cpp:9711`), started by `gammapad/gammapad.rc` on `enable=1`. The Cube's vendor `build.prop` sets no gamepad property (checked in `/tmp/devbtn/cv/cube/vendor/build.prop`). Other handhelds turn it on in their vendor `build.prop` (`work/device-buttons-research.md`, cross-cutting finding 2).
- **What it grabs:** every device with `EV_ABS` + `EV_KEY`, a stick or hat, and `BTN_A` or `BTN_GAMEPAD`, plus any name listed in `persist.gammaos.gamepad.devices` (`gammapad/GamepadManager.cpp:478-528`). External pads included. It skips its own virtual devices by `phys` prefix `gammapad-` (`:455-464`). It takes them with `EVIOCGRAB` (`:541-542`), so Nano and drastic-nano see only the virtual pad while it runs.
- **Merging:** one `VirtualGamepad` (`gammapad/GamepadManager.h:164`). All grabbed pads feed one transformer (`gammapad/GamepadManager.cpp:1097`). `merge` is read and logged, nothing else (`:235`, `:289`). **No player 2 is possible** while gammapad runs.
- **Key layouts:** for each source, `Vendor_%04x_Product_%04x.kl` from `/system` then `/vendor`, else `Generic.kl` (`gammapad/KeyLayoutParser.cpp:17-51`). Not `/product`, so the Cube gets the vendor's by-print file. No `_Version_` lookup. `requires_kernel_config` lines are ignored (`parseLine`, `:72-110`). Keycode names are turned back into Linux codes (`BUTTON_A` → `BTN_A`, `:132-135`).
- **The resulting space is "by label":** `BTN_A` means the button labelled A (or Cross). The Cube's printed A (right) → `BTN_A`. The Switch Pro's A (right) → `kl` `BUTTON_A` → `BTN_A`. Xbox A (bottom) → `BTN_A`. DualSense Cross (bottom) → `BTN_A`. The DS4 is scrambled (finding 7 in §0).
- **ABXY Swap** (`persist.gammaos.gamepad.abxy_swap`) swaps `BTN_A`↔`BTN_B` and `BTN_X`↔`BTN_Y`, before `remap_btn` (`gammapad/InputTransformer.cpp:191`, `268-280`). It is **global**: it applies to every merged pad, so it makes the Cube positional and an Xbox pad wrong at the same time.
- **No source identity is published.** The only properties gammapad sets are the mouse and screen-map flags (`gammapad/MouseMode.cpp:413`, `gammapad/ScreenMapMode.cpp:341-356`). The per-source "south" the bmap plan asked for (§1.1 limit, phase 6) is not there.

### 1.4 Nano and drastic-nano (raw evdev)

**Nano reads every `/dev/input/event*`** (`nano/NanoMenuInput.cpp:72-103`), grabbing only with `persist.gammaos.nano.grab_input` (default false, `:84`). It watches hotplug with inotify (`:104-195`). Every event goes through one function, `nextInputEvent(fd, ev)` (`:1468-1478`, called at `:2922`). There is **no per-device translation** of face buttons. The fd → name map is `mInputFdNames`.

**Confirm and back** (main menus, `nano/NanoMenuInput.cpp`):
- `BTN_SOUTH` → `handleSelect()` (`:3716-3717`); `BTN_EAST`/`KEY_ESC`/`KEY_BACK` → `handleBack()` (`:3749-3755`).
- `BTN_WEST` → "Y button (Nintendo layout: BTN_WEST = Y)" actions: cancel scrape, paste, visualizer, ES-DE VIEW MEDIA, photo sort (`:3760-…`).
- `BTN_NORTH` → "X button (Nintendo layout: BTN_NORTH = X)" actions (`:3880`).
- The setup wizard and the PS3 network wizard repeat the same choice (`:3676-3696`).
- Touch prompts press the same codes: `a`→`BTN_SOUTH`, `b`→`BTN_EAST`, `x`→`BTN_WEST`, `y`→`BTN_NORTH` (`:1431-1434`).

What this does per family (gammapad off):

| Pad | Accept | Back | "X action" (`BTN_NORTH`) | "Y action" (`BTN_WEST`) |
|---|---|---|---|---|
| RG Cube | right (A) | bottom (B) | top (X) | left (Y) |
| Xbox | bottom (A) | right (B) | **left** (X) | **top** (Y) |
| DualShock 4 / DualSense | bottom (Cross) | right (Circle) | top (Triangle) | left (Square) |
| Switch Pro | bottom (B) | right (A) | top (X) | left (Y) |

So today Nano is "by label" for Xbox, Nintendo-positional for the Cube, and Xbox-positional for PlayStation and Switch Pro pads.

**Prompts** (`nano/NanoMenuPS3Menu.cpp`):
- `abxySwapApplied()` (`:10943-10948`) is true only when gammapad runs with `abxy_swap`.
- `refreshFaceButtonPrefs()` (`:10950-10957`) reads `persist.gammaos.nano.face_glyphs` (`letters`/`playstation`) and `persist.gammaos.nano.face_swap` (OK Button relabel).
- `abxyText()` (`:10973-10983`) swaps whole-word A↔B, X↔Y in hint strings; 8 call sites.
- `themeButtonText()` (`:10985-11010`) turns canonical `Cross/Circle/Square/Triangle` into letters or PS names; 20 call sites.
- `drawFaceGlyph()` (`:11018-…`) draws role 0 Confirm, 1 Cancel, 2 Square, 3 Triangle, as a ringed letter or a PS shape; `ps3DlgHintG()` (`:11084-…`) uses it for footers.
- The letters are always the Cube's print: Confirm "A", Cancel "B", role 2 "Y", role 3 "X" (`:11030-11034`). No prompt knows which pad is in use.
- **ES-DE theme help bar:** always the Xbox icon set: `button_a_XBOX.svg` … (`nano/NanoThemeEngine.cpp:5429-5458`). The SNES, Switch and PS sets are already in `nano/nano_esde_assets/help/` (`button_a_SNES.svg`, `button_a_switch.svg`, `button_a_PS.svg`, `button_back_PS4.svg` …), unused.
- **Strings:** `ps3xmb/i18n/*.json` has 3631 keys. 76 contain whole-word A/B/X/Y in a prompt sense ("A: Connect | X: Rescan | B: Back", "Press A to begin"), and 26 the PS names ("Cross: Connect | Triangle: Rescan | Circle: Back") (counted on `de.json`). They are written in Cube terms: A = accept, B = back, X = top, Y = left.
- The button map panel's labels already come from the pad in hand (`bmap::PadModel`, bmap plan §4.2). Its Close is "the button that is Back in Nano (the one sending `BTN_EAST`)" (`nano/NanoButtonMapNav.h:115-117`, `navConfigFor`, `nano/NanoButtonMapNav.cpp:294`).

**Nano's in-process runners** (Quick Resume):
- `LibretroRunner`: `BTN_A`→RetroPad A, `BTN_B`→B, `BTN_X`→X, `BTN_Y`→Y, port 0 only (`nano/NanoMenu.cpp:3614-3618`; `nano/LibretroRunner.cpp:553-559`, `:658-662`). Right on the Cube; on an Xbox pad RetroPad A lands on the bottom button.
- `DrasticRunner`: `BTN_SOUTH` → DS A (`nano/NanoMenu.cpp:1881`, `:2466`). Same split.

**drastic-nano:**
- Menu nav: evdev → Android-style code through `kKeyTable` (304→96, 305→97, 307→99, 308→100; `drastic-nano/DrasticPrefs.cpp:842-845`), then `dispatchNav`: 96 → `navAccept`, 97 → `navCancel`, 99 → `navX` (`drastic-nano/InputMap.cpp:502-517`, routed at `:914-927`). So accept = raw `BTN_SOUTH`, as in Nano.
- Footer: fixed text "L/R: tabs  Up/Down: move  A: select  Left/Right: adjust  B: close" and "Press any key to bind  B: cancel" (`drastic-nano/OverlayMenu.cpp:4241-4251`). It reads neither `face_glyphs` nor `abxy_swap`.
- Game buttons: the keymap seeds A=96 (evdev 304), and an unbound code falls back to `BTN_SOUTH`→DS A, `BTN_EAST`→DS B (`drastic-nano/InputMap.cpp:459-465`, bmap plan §3.2). On the Cube that puts DS A on the right button; on an external pad, on the bottom button.

**Existing swap settings:**

| Property | Row | Effect today |
|---|---|---|
| `persist.gammaos.gamepad.abxy_swap` | Gamepad Settings → Buttons → ABXY Swap (`nano/NanoMenuPS3Menu.cpp:2705`, `:9717`); Quick Menu → Controller (`:1934`); Settings tree (`nano/NanoMenuSettingsTree.cpp:555-556`) | gammapad only (§1.3). With gammapad on it also moves Nano's confirm to the bottom button and flips the prompts. Off without gammapad |
| `persist.gammaos.nano.face_swap` | OK Button (`:2708`, `:9751`) | Relabels Confirm/Cancel only. Input unchanged |
| `persist.gammaos.nano.face_glyphs` | Button Prompts (`:2707`, `:9749`) | Letters or PlayStation shapes |
| `persist.gammaos.gamepad.merge` | Controllers → Merge Controllers (`:2688`, `:9712`) | None (§1.3) |

### 1.5 PhoneWindowManager, `ThimsyDeviceKeys`, `buttonMapKeycode`

- `ThimsyDeviceKeys.buttonMapKeycode(setting, vendor, product, gammapad, swap, gpVid, gpPid, remap)` (`pwm/ThimsyDeviceKeys.java:193-232`). `FACE_SOUTH/EAST/NORTH/WEST` are positions. For any device that is not gammapad's virtual pad it returns `{BUTTON_A, BUTTON_B, BUTTON_Y, BUTTON_X}[face]` (`:221`). That is wrong for the Switch Pro, whose bottom button is `BUTTON_B` (§1.2). For the virtual pad it starts from the Cube's print (305/304/307/308), applies the swap and `remap_btn`, then `Generic.kl` (`:222-232`). An external pad merged into the virtual pad is resolved as if it were the Cube (bmap plan §1.1, known limit).
- Called by `nanoIsButtonMapButton` (`pwm/PhoneWindowManager.java:3018-3030`).
- The universal hotkeys are **Android keycode names** (`persist.gammaos.hotkeys.save = BUTTON_R1` …), compared with `event.getKeyCode()` (`pwm/PhoneWindowManager.java:3060-3083`). The defaults are shoulders, so faces only matter when a user picks one; then "BUTTON_A" is the bottom button on the Cube and Xbox and the right button on a Switch Pro.
- Java and C++ are held to the same cases by `nano/test/fixtures/devkeys/cases.json` (`button_map` at `:91`), run by `nano/test/unit/NanoButtonMapNano_test.cpp:165` and `NanoDeviceKeys_test.cpp:74`.

### 1.6 Controller identification today, and what SDL adds

Today:
- `pads.json`: one entry, the Cube (`rg_cube`, 484b:1101), with per-position label, evdev and Android codes and axes.
- `bmap::standardPadSpec(dev)` (`nano/NanoButtonMap.cpp:254-311`): by vendor only. 045e → Xbox labels, 054c → PlayStation, 057e → Switch Pro, else generic ("Bottom", "Right" …). All of them get evdev South=304, East=305, North=307, West=308 and Android `BUTTON_A/B/Y/X`. Two bugs: Xbox north/west evdev (should be 308/307, §1.1), and Switch Pro Android codes (should be `BUTTON_B/A/X/Y`, §1.2).
- `bmap::padModelFor` (`nano/NanoButtonMap.cpp:1236-1251`) picks the pad: the virtual pad = the Cube through gammapad; a `pads.json` match; else `standardPadSpec`.
- `device-keys.json` and `NanoDeviceKeys` (`nano/NanoDeviceKeys.h:13-100`) pick the handheld by build properties, but carry no layout.

What SDL (zlib licence; `controller_list.h` carries the Valve zlib notice, `/tmp/ctl-audit/sdl/controller_list.h:1-20`) can add:
- **Type by VID/PID**, about 640 entries: Xbox 360 166, Xbox One 193, PS3 80, PS4 61, PS5 15, Switch Pro 9, "Switch input only" (third-party Switch pads) 20, and the "label differs from the protocol" cases: `XInputPS4Controller` (PS-labelled pads speaking X-input) 10 and `XInputSwitchController` 1 (counted in `controller_list.h`). `SDL_GetGamepadTypeFromVIDPID(vendor, product, name, forUI)` (`/tmp/ctl-audit/sdl/SDL_joystick.c:3009-3097`) also names HORI/PowerA Switch clones that report 0000:0000 (`:3013-3021`). The `forUI` flag is the split this plan needs: labels from one answer, input mapping from another (`:3075-3091`).
- **Face style by type**: Sony → shapes; Switch → `BAYX`; GameCube → `AXBY`; else `ABXY` (`/tmp/bmap-sdl/src/joystick/SDL_gamepad.c:1875-1892`, labels `:3576-3682`).
- **Linux evdev mappings by GUID** (bus, VID, PID, version) for pads whose HID descriptor is out of order, such as the 8BitDo D-input family (`SDL_gamepad_db.h:497-776`). They index buttons in SDL's own key order, so using them needs SDL's Linux key-ordering rule copied too (unverified effort).
- What SDL **can't** fix: an 8BitDo in X-input mode presents as an Xbox 360 pad (045e:028e). Its print is Nintendo but its IDs say Xbox. Only the user can say (§14, Q6).

### 1.7 Emulators today

With gammapad off, the built-in pad's Android codes are by position (§1.2), so most shipped configs are already by position on the Cube. The table says where each one lands with the Cube, and what changes for an external pad. Sources: bmap plan §3 (per emulator, at the shipped tag) and the payload.

| Emulator | Config and default | Cube (gammapad off) | External pad | Per device or global |
|---|---|---|---|---|
| RetroArch 1.22.2 | Player 1 binds pinned to keycodes in `RA.cfg:1435` `a=97`, `:1440` `b=96`, `:1576` `x=100`, `:1580` `y=99` (rewritten by `mods/10-update-emulators.sh:186-208`). Player 2 binds `nul` (`RA.cfg:1584-1729`), so player 2 uses the pad's autoconfig. `input_player1_joypad_index=0`, `player2=1` (`:1491`, `:1640`); `input_max_users=5` (`:300`). `menu_swap_ok_cancel_buttons=false` (`:2989`), so RetroArch's own menu confirms with RetroPad A, the right button | by position | Positional pads fine as player 1 (the pinned codes are Android positions). Switch Pro as player 1: RetroPad B on its right button (wrong). As player 2 a Switch Pro has **no autoconfig** in the 226 shipped profiles (`/tmp/bmap-payload/list.txt`), so the "Android Gamepad" fallback applies (by keycode, wrong for it; unverified) | Per port; autoconfig by name and VID:PID |
| PPSSPP 1.20.4 | `controls.ini`, Cross 96, Circle 97 (`mods/27-controller-layout.sh:28-70`) | by position | any positional pad fine; Switch Pro swapped | Global, every pad is device 10 |
| Flycast 2.7 | defaults A/B/X/Y = 96/97/99/100 (DC A bottom), per-device mapping files | by position | fine; Switch Pro swapped | Per device name |
| ARMSX2 2.9.2 | `pad.map.*`, identity defaults (Cross 96) | by position | fine; Switch Pro swapped | Global keycodes; players by `PadRouter` (§9) |
| Vita3K 0.2.1 | SDL indices, identity; Android→SDL 96→SOUTH | by position | fine; Switch Pro swapped (SDL on Android uses keycodes) | OR'd into port 1 |
| WatermelonDS 0.8.0.rc3 | defaults A=97, B=96 | by position (DS A right) | fine; Switch Pro swapped | Global for keys |
| SeedlessDS 1.0.1 | defaults A=96, B=97 | **DS A on bottom (wrong)** | wrong for every pad | Global |
| DraStic r2.6.0.4a | defaults A=96, B=97 | **DS A on bottom (wrong)** | wrong | Global, profile 0 |
| drastic-nano | seed A=96 = evdev 304 | by position (DS A right) | **DS A on bottom** (raw `BTN_SOUTH`) | Global, raw evdev |
| M64Plus FZ 3.0.335 | "Android Gamepad": N64 A=96, B=99 | by position | fine; Switch Pro swapped | Per player profile; no player map = all player 1 |
| Dolphin 2609-197 | **no pad binds at all**; Hotkeys provisioned with a template (`mods/hotkeys/dolphin.json`) | not playable until mapped | same | Per pad section, `Device = Android/<n>/<name>` |
| Azahar 2126.2 | **no binds** until Auto-map; Auto-map uses the Xbox table when east reports 97 | not mapped | same | Global |
| hakuX `a7cdb5a` | A/B/X/Y = 96/97/99/100 (Xbox A bottom) | by position | fine; Switch Pro swapped | Per unit, then per model profile |
| ScummVM v2026.3.0 | JOY_A left click | n/a (not a console) | n/a | Global |
| Nano's Quick Resume `LibretroRunner` / `DrasticRunner` | raw `BTN_A`→RetroPad A / `BTN_SOUTH`→DS A (§1.4) | by position | **A/B on the wrong buttons** | Port 0 only |

**Provisioning today** (`nano/NanoHotkeys.h`, `.cpp`): runs at Nano start (`nano/NanoMenu.cpp:921`) and when the overlay rises, skipping the app in front (`nano/NanoMenuOverlay.cpp:522`). Formats: `ini`, `retroarch_cfg`, `xml_prefs` (`int`, `set`, `string`), `flycast_mapping`, `melonds_mapper` (`nano/NanoHotkeys.cpp:614-618`). Modes: `add` (default; fills a missing or empty key), `append`, `ifUnset`, `set` (`:161-182`). A missing file is created only from a rule's `template` (`:603-612`). Every shipped rule writes **hotkeys only**, never pad buttons (`mods/hotkeys/*.json`). So the mechanism can write positional profiles, but it needs (a) `set` for keys it owns, (b) new formats for RetroArch reservations and Flycast/ARMSX2 player assignment, and (c) a way to tell its own earlier values from the user's (§11).

### 1.8 Settings UI today

- **Gamepad Settings** (Settings → Gamepad Settings, `nano/NanoMenuPS3Data.h:528`) opens `buildGamepadSubmenu` (`nano/NanoMenuPS3Menu.cpp:2669-2683`): Controllers, Sticks & D-Pad, Buttons, Calibrate & Test, Vibration, Button Mapping, Touch & Mouse, Hotkeys.
  - Controllers: Controller Enable, Merge Controllers, Hide Source Device, Devices to Capture, Virtual Device Name (`:2685-2692`). These are all gammapad settings.
  - Buttons: ABXY Swap, Button Prompts, OK Button (`:2703-2709`).
- **Quick Menu → Controller**: Controller Bindings, ABXY Swap, DPAD/Analog Swap, sensitivity, inverts (`:1929-1938`).
- **Settings tree** (the Minima/ES-DE settings view): `gos_gamepad` with the same rows (`nano/NanoMenuSettingsTree.cpp:531-590`).
- **Thimsy Toolbox** (`nano/NanoMenuPS3Data.h:391-418`) holds device tweaks (Anbernic Button Opens Quick Menu …). It is not the place for controller rows.
- Bound rows live in the settings table (`nano/NanoMenuPS3Menu.cpp:9704-9752`) with descriptions in `kGamepadCh` (`nano/NanoMenuPS3Data.h:345-359`).

---

## 2. The model

### 2.1 Position is the meaning, the label is display

- Every button has a **position** (`bmap::Phys`: `South East North West L1 R1 …`, `nano/NanoButtonMap.h:40`). Meanings attach to positions: "accept", "SNES A", "Cross".
- Every pad has **labels** per position, as printed: the Cube `B A X Y` (S E N W), Xbox `A B Y X`, PlayStation `Cross Circle Triangle Square`, Switch `B A X Y`.
- Each device also has a **normaliser**: raw evdev code → `Phys` (and Android keycode → `Phys`). It is the per-family table in §4, with the bugs in §1.6 fixed.
- Nothing else is allowed to read a raw face code as a meaning. Today `BTN_SOUTH` means "accept" in Nano and drastic-nano, and `BUTTON_A` means "the bottom button" in PhoneWindowManager. Both go.

### 2.2 Three controllers to keep apart

| Term | What it is | Used for |
|---|---|---|
| **Built-in pad** | The handheld's own pad (`retrogame_joypad` on Anbernic; `pads.json` first entry) | Default primary when nothing is connected |
| **Primary controller** | The most recently connected external pad, else the built-in pad (§8) | **Prompts** in every Thimsy menu; player 1 owner when Merge is Off |
| **Holding controller** | The pad that sent the triggering press (`bmap_dev`, or the event's fd) | The button map overlay, and any one-off label tied to a press |
| **Players** | Emulator ports | Merge On: every pad is player 1. Merge Off: primary = player 1, the next pad = player 2 (§9) |

### 2.3 Menu roles

"Nintendo" below is the effective menu layout: on the RG Cube, ABXY Swap Off.

| Role | Nintendo (accept right) | Xbox (accept bottom) |
|---|---|---|
| Accept | East (right) | South (bottom) |
| Back | South | East |
| Top action (today's `BTN_NORTH`: rescan, options) | North | North |
| Left action (today's `BTN_WEST`: paste, sort, cancel scrape, VIEW MEDIA) | West | West |

Recommendation: only accept and back swap. The top and left actions stay on their positions in both settings, because the existing code and strings tie them to positions on the Cube (rescan = top, paste = left), and the brief says menus are fixed by position. This keeps the Cube with the setting On behaving exactly as today.

Worked example (the brief's): RG Cube, setting On, external Xbox pad. Right (Xbox B) accepts, bottom (Xbox A) goes back, and the footer shows "Ⓑ Select  Ⓐ Back" with Xbox letters. The same pad on an Xbox-layout handheld with the setting Off: A accepts and shows "Ⓐ".

---

## 3. Settings

### 3.1 The two new settings

| Row | Property | Values | Default | Where |
|---|---|---|---|---|
| **ABXY Swap** (menus only, §16) | `persist.gammaos.nano.menu_abxy_swap` (new) | `0`, `1` | `0`: the handheld's own layout (§4.2) | Where ABXY Swap is today: Gamepad Settings → Buttons, Quick Menu → Controller, the Settings tree `gos_gamepad` |
| **Merge Controllers** | `persist.gammaos.gamepad.merge` (reused, §3.3) | `1`, `0` | `1` | Gamepad Settings, a new first section **Menus & Players** (see §3.4) |

- Accept is on the right when the handheld is Nintendo-layout and ABXY Swap is Off, or Xbox-layout and ABXY Swap is On. Because the swap is relative to the handheld, a card moved to another handheld follows that handheld.
- The old `persist.gammaos.gamepad.abxy_swap` value is not carried over: users turned it on to fix game buttons, not to choose a menu layout (§11).
- Description for ABXY Swap: "Swaps accept and back in Thimsy's menus. Off: the handheld's own layout (RG Cube: A, the right button, accepts). Game buttons always follow position and are not affected."
- Description for Merge: "On: a connected controller and the built-in controls both play as player 1. Off: the connected controller is player 2."
- Both strings go through `trDyn()`.

### 3.2 Readers

| Reader | Accept setting | Merge setting |
|---|---|---|
| Nano (home, overlay instance, wizard, OSK, button map panel) | yes | for provisioning and the Controllers screen |
| drastic-nano | yes | no (DS is single-player; every pad drives it) |
| PhoneWindowManager | no (no menus of its own) | yes, for the primary tracker's player hint (§8) |
| gammapad | no | yes, once §6.6 lands |

A small shared helper `nanopads::acceptRight(devkeysMap, prop)` (C++) and `ThimsyDeviceKeys.acceptRight` (Java) combine the handheld's layout with ABXY Swap, so all readers agree. Held to `cases.json`.

### 3.3 Why reuse `persist.gammaos.gamepad.merge`

It already exists, defaults to `1`, has the right name and does nothing (§1.3). Reusing it gives one switch for both layers: Thimsy's per-emulator player assignment, and gammapad's merge once it honours it. The old row moves out of the gammapad "Controllers" list into the new section, with the new description. Open question Q4 if the user prefers a separate property.

### 3.4 Where the rows go

Gamepad Settings gets a first entry, **Menus & Players**, holding:
1. Merge Controllers
2. Primary Controller: read-only status line ("Xbox Wireless Controller (external)") with the labels it will use. A selectable "Auto / Built-in only" option is open (Q5).
3. Button Prompts (moved from Buttons, see §11 for what it becomes)

ABXY Swap stays in the Buttons section and the Quick Menu → Controller list, with the new description.

### 3.5 How they interact

| Handheld | ABXY Swap | Primary pad | Accept button | Prompt shows | Game buttons |
|---|---|---|---|---|---|
| RG Cube (Nintendo) | Off (default) | built-in | right | Cube "A" | by position |
| RG Cube | Off | Xbox (Bluetooth or USB) | right | Xbox "B" | by position: SNES A on Xbox B, Xbox-game A on Xbox A |
| RG Cube | Off | DualSense | right | Circle | by position |
| RG Cube | Off | Switch Pro | right | Switch "A" | by position |
| RG Cube | On | built-in | bottom | Cube "B" | by position (unchanged) |
| RG Cube | On | Xbox | bottom | Xbox "A" | by position (unchanged) |
| Xbox-layout handheld | Off (default) | built-in | bottom | its "A" | by position |
| Xbox-layout handheld | Off | Switch Pro | bottom | Switch "B" | by position |
| Xbox-layout handheld | Off | 8BitDo in X-input | bottom | "A" as Xbox, though the pad prints B there (§1.6 limit) | by position |

The swap never needs changing when a pad connects or disconnects. A Nintendo-layout external pad on an Xbox-layout handheld follows the handheld: bottom accepts, and the prompt honestly says "B". The setting is a property of the user's habit, not of the pad.

---

## 4. The layout table

### 4.1 Controller families

A new data file replaces the vendor switch in `standardPadSpec`: `nano/ps3xmb/buttonmap/families.json`, read by `bmap::padSpecFor(dev)`. Order of matching: `pads.json` (the handheld), the user's taught pads (§12 phase 7), exact VID:PID, the SDL type table, name rules, then generic.

| Family id | Match | Labels S / E / N / W | Shoulders, Start, Select | Evdev S / E / N / W (this kernel) | Android S / E / N / W | Glyphs |
|---|---|---|---|---|---|---|
| `rg_cube` (handheld) | 484b:1101 on an Anbernic | B / A / X / Y | L1 R1 L2 R2, Start, Select | 305 / 304 / 307 / 308 | A / B / Y / X (Thimsy `.kl`) | letters, Nintendo |
| `xbox` | SDL `XBox360`/`XBoxOne` types, 045e:* | A / B / Y / X | LB RB LT RT, Menu, View | 304 / 305 / **308 / 307** (xpad) | A / B / Y / X | letters, Xbox colours |
| `xbox_bt` | 045e:02e0, 02fd, 0b13 (and other BT PIDs) | A / B / Y / X | as above | 304 / 305 / 308 / 307 (**unverified**) | A / B / Y / X | as above |
| `ps3` | SDL `PS3` (054c:0268 and clones) | Cross / Circle / Triangle / Square | L1 R1 L2 R2, Start, Select | 304 / 305 / 307 / 308 | A / B / Y / X (`_Version_8xxx.kl`) | PS shapes |
| `ps4` | SDL `PS4` (054c:05c4, 09cc, clones) | as PS3 | Options, Share | 304 / 305 / 307 / 308 | A / B / Y / X | PS shapes |
| `ps5` | SDL `PS5` (054c:0ce6, 0df2) | as PS3 | Options, Create | 304 / 305 / 307 / 308 | A / B / Y / X | PS shapes |
| `switch_pro` | 057e:2009, SDL `SwitchPro` | B / A / X / Y | L R ZL ZR, +, − | 304 / 305 / 307 / 308 | **B / A / X / Y** (AOSP `.kl`, by label) | letters, Nintendo |
| `switch_hid` | SDL `SwitchInputOnly` (HORI, PowerA …), name rules | B / A / X / Y | as above | descriptor-dependent (**unverified**) | via `Generic.kl` (**unverified**) | letters |
| `8bitdo_dinput` | 2dc8:* not in xpad's list | B / A / X / Y | per model | descriptor-dependent; SDL Linux mapping | per `.kl` (only 6101 has one) | letters |
| `xinput_nintendo_print` | user-marked X-input pads (Q6) | B / A / X / Y | as Switch | as xpad | as Xbox | letters |
| `generic` | anything else | Bottom / Right / Top / Left | L1 R1 L2 R2, Start, Select | assume spec 304 / 305 / 307 / 308 | A / B / Y / X | neutral position icons |
| gammapad virtual | `device_vid/pid` while gammapad runs | the source pad's (§6.6) | | "by label" space (§1.3) | through `Generic.kl` | |

Each entry is `{"id","match":{…},"labels":{"south":"B",…},"evdev":{"south":304,…},"android":{"south":"BUTTON_B",…},"glyphs":"nintendo|xbox|playstation|generic"}`, the same shape as a `pads.json` button list so `padSpecFromJson` reads both. The SDL list is converted once, offline, by a script under `nano/tools/` into a VID:PID → family file with SDL's notice kept in `NOTICE`; it is data, not linked code.

### 4.2 Handheld layouts

`device-keys.json` gets `"face_layout": "nintendo" | "xbox"` per device, with `default` = `"nintendo"` (the Cube's). `NanoDeviceKeys::Map` and `ThimsyDeviceKeys` gain the field; `cases.json` gains cases. The handheld's pad spec stays in `pads.json`; a handheld whose built-in pad isn't `retrogame_joypad` gets its own `pads.json` entry, keyed by the `device-keys.json` id.

| `device-keys.json` id | Layout | Status |
|---|---|---|
| `anbernic-rgcube` | nintendo | verified (`pads.json:11-34`, Thimsy `.kl`) |
| `anbernic-t820`, `anbernic-mtk`, `anbernic-t618`, `anbernic-rg405v`, `anbernic-rgvita`, `anbernic-rgvitapro`, `anbernic-rgrotate`, `anbernic-rgds` | nintendo | likely (Anbernic prints A on the right); **unverified** per model |
| `ayaneo-pocketairmini`, `ayaneo-pocketmicro` | xbox | likely; **unverified** |
| `allwinner-a133p`, `gkd-350h-ultra`, `retroid-rp4pro`, `retroid-rpclassic`, `retroid-duo`, `gamemt-e6max`, `mangmi-airx`, `ktpocket-ktr1`, `zpg-a1-unicorn` | to confirm | **unknown**: needs the device's photos or the vendor's key layout, plus whether its driver reports by print like the Cube's |

Only the Cube ships today. The table matters when Thimsy runs elsewhere; until each row is confirmed, an unconfirmed device takes the `default`.

---

## 5. Normalisation fixes per controller family

| Family | Layer | Today | Fix |
|---|---|---|---|
| RG Cube | evdev (Nano, drastic-nano) | Nano treats 304 (printed A, right) as `BTN_SOUTH` | Normaliser maps 305→South, 304→East (already in `pads.json`; just use it for menus) |
| RG Cube | gammapad | vendor by-print `.kl` (`KeyLayoutParser.cpp:17-20`) | Look in `/product/usr/keylayout` first, so gammapad gets Thimsy's positional file (§6.6) |
| Xbox (xpad, and BT if confirmed) | evdev | North/West swapped; `standardPadSpec` claims 307 is top (`NanoButtonMap.cpp:286`) | Family table: N=308, W=307 |
| Xbox | Android | correct | none |
| DS3 / DS4 | gammapad | legacy `.kl` (finding 7) | Version-aware lookup in `KeyLayoutParser::parse`: read `EVIOCGID` version and try `_Version_%04x` first, as Android does |
| DS3 / DS4 / DualSense | evdev, Android | correct | none |
| Switch Pro | Android | by label (`kl/Vendor_057e_Product_2009.kl:22-29`) | Two options (Q1): (a) leave Android alone and correct it in every Thimsy consumer and in per-device emulator configs; (b) a Thimsy `/product/usr/keylayout/Vendor_057e_Product_2009.kl` by position, like the Cube's. (b) fixes the global-bind emulators too; it changes what every Android app sees for this pad |
| Switch Pro | Thimsy code | `standardPadSpec` and `ThimsyDeviceKeys` assume `BUTTON_A` bottom | Family table; `ThimsyDeviceKeys.faceKeycode(face, vid, pid)` reads the same table (§6.4) |
| Switch-style HID, 8BitDo D-input | evdev, Android | descriptor-dependent | SDL's Linux GUID mappings where present; otherwise the Identify flow (§12 phase 7) |
| 8BitDo X-input | labels | looks like Xbox | User marks it (Q6); input is already positional |
| gammapad virtual | identity | the source is lost | Publish the last source per event batch (§6.6) |

---

## 6. Components to change

### 6.1 Shared pad model (Nano, drastic-nano, host tests)

`nano/NanoButtonMap.h/.cpp` (already in `drastic_nano_shared_srcs`, bmap plan §1.5):
- `PadSpec padSpecFor(const DeviceId&, const Families&)` replaces `standardPadSpec`; `standardPadSpec` stays as the generic fallback, with the North/West and Switch Pro fixes.
- `Families loadFamiliesFrom(dataDir)` for `families.json` and the SDL-derived VID:PID file.
- `Phys physForEvdev(const PadModel&, int code)` and `Phys physForKeycode(const PadModel&, int keycode)` (inverses of `PadModel::input`).
- `DeviceId` gains `version` (for `_Version_` matching and SDL GUIDs) and `uniq`.

New `nano/NanoPads.h/.cpp` (pure, host-testable):
- `enum class MenuRole { None, Accept, Back, Top, Left }`; `MenuRole menuRole(Phys, bool acceptRight)`.
- `int menuCode(Phys, bool acceptRight)`: the canonical code Nano's existing switches expect: Accept → `BTN_SOUTH`, Back → `BTN_EAST`, Top → `BTN_NORTH`, Left → `BTN_WEST`. Non-face buttons unchanged.
- `bool acceptRight(const devkeys::Map&, const std::string& prop)`.
- `Primary choosePrimary(const std::vector<PadInfo>& connected, …)` (§8).
- `std::string promptText(const std::string& in, const PadModel& primary, bool acceptRight)` (§7).
- Player-assignment resolver for §9: `PlayerPlan plan(const std::vector<PadInfo>&, bool merge)`.

### 6.2 Nano input

`nano/NanoMenuInput.cpp`:
- `openInputDevices()` (`:72-103`) and the hotplug path (`:104-270`): on open, read `EVIOCGID` (with version), `EVIOCGNAME`, `EVIOCGUNIQ`, and build a `PadModel` per fd (`mInputPads[fd]`), next to `mInputFdNames`.
- `nextInputEvent(fd, ev)` (`:1468-1478`): after reading a real event, if `ev.type == EV_KEY` and the code is a face button of a known pad, rewrite `ev.code = menuCode(physForEvdev(pad, code), acceptRight)`. Replayed touch events (`touchHelpPrompt`, `:1431`) are already canonical and skip it.
- Exceptions that must see raw codes: Test Controller (Calibrate & Test), the hotkey capture rows, and the chord code that already resolves through `bmap::buttonMapEvdev`. They read `mLastRawCode`, set before the rewrite.
- Mark the fd as "last used" for §8.
- `menu_abxy_swap` is read on start and re-read when the setting row changes (no polling).

### 6.3 Nano prompts

`nano/NanoMenuPS3Menu.cpp`:
- `refreshFaceButtonPrefs()` (`:10950`): take the primary `PadModel` and `acceptRight` instead of `face_swap` and `abxySwapApplied()`.
- `drawFaceGlyph(role, …)` (`:11018`): role → `Phys` (§2.3) → primary's label → draw. Letters ring for `letters` glyph sets; the PS shapes for `playstation`; Xbox letters may get their colours. The generic family draws a small position icon (a diamond with one dot filled).
- `themeButtonText()` (`:10985`) and `abxyText()` (`:10973`) become wrappers over `nanopads::promptText`.
- `ps3DlgHintG()` (`:11084`) needs no change beyond `drawFaceGlyph`.
- `nano/NanoThemeEngine.cpp:5429-5458`: `esdeHelpIconPath` and `esdeHelpCustomKey` pick the icon by the label the role resolves to and the family's set (`button_<label>_SNES|switch|PS|XBOX.svg`). ES-DE `a` = accept, `b` = back, `x` = left, `y` = top in Nano's mapping (`:1431-1434`).
- `nano/NanoButtonMapNav.cpp:294` (`navConfigFor`): `back` = the Back role's `Phys`, not "the one sending `BTN_EAST`".

### 6.4 PhoneWindowManager

`pwm/ThimsyDeviceKeys.java`:
- `faceKeycode(face, vendor, product, gammapadState)` replaces the hard-coded `{BUTTON_A, BUTTON_B, BUTTON_Y, BUTTON_X}` (`:221`) with a small family table (the Switch Pro row at least), loaded from the same `families.json` the C++ reads, or generated from it at build time. `buttonMapKeycode` calls it.
- `acceptRight(prop, map)` for §3.2.

`pwm/PhoneWindowManager.java`:
- `nanoHotkeyButton` (`:3060-3067`) accepts `FACE_*` values and resolves them per device with `faceKeycode`, as the button map already does.
- A new `ThimsyPadTracker` (§8), registered as an `InputManager.InputDeviceListener`.

### 6.5 drastic-nano

- `drastic-nano/InputMap.cpp`: per-fd `PadModel` (it already loads `pads.json` through `padSpecs()`, `:519-530`). Before `dispatchNav` (`:926`), turn the evdev code into a role and call `dispatchNav` with the role's canonical code. `legacyDsBit` (`:459-482`) uses `Phys` (South → DS B, East → DS A, North → DS X, West → DS Y) so DS A is the right button on every pad.
- The keymap seed (`drastic-nano/DrasticPrefs.cpp:147-158`) stores evdev-derived codes; a new seed version re-expresses the face entries by position (§11).
- `drastic-nano/OverlayMenu.cpp:4241-4251`: footer through `promptText` with the primary pad (from §8's properties) and `acceptRight`.
- `drastic-nano/DnButtonMap.cpp`: close on the Back role.

### 6.6 gammapad

`gammapad/KeyLayoutParser.cpp`:
- `kBaseDirs` (`:17-20`): add `/product/usr/keylayout` first.
- `parse` (`:23-70`): take the version and try `Vendor_%04x_Product_%04x_Version_%04x.kl` first.
- Honour `requires_kernel_config` only as Android does, or skip files that carry it when the kernel lacks the option (low priority: this kernel has the options).

`gammapad/InputTransformer.cpp`:
- Remove the ABXY swap from input (`:268-274`). Each source is normalised to position (Android's convention: `BTN_A` bottom, `BTN_B` right, `BTN_X` left, `BTN_Y` top) from the layout table: the Cube by its `/product` layout, Xbox as is, the Switch Pro's label-based `.kl` undone. No setting changes game buttons.

`gammapad/GamepadManager.cpp`:
- Publish `sys.gammaos.gamepad.last_src=<vid>:<pid>:<name>` when the source of the last face-button press changes (`:1097`, where the source fd is known). This lets Nano and PhoneWindowManager label the pad in hand while gammapad runs.
- `merge=0`: one virtual device per source (second `VirtualGamepad`, distinct `phys` and PID). This is the only way to have player 2 with Controller Enable on. Sizeable; phase 6.

### 6.7 Nano's in-process runners

- `nano/NanoMenu.cpp:3614-3618`: map `Phys` to RetroPad by position (South → B, East → A, North → X, West → Y) through the fd's `PadModel`.
- `nano/NanoMenu.cpp:1881`, `:2466`: DS A from East.

### 6.8 Provisioning

`nano/NanoHotkeys.cpp` grows a second rule set, `mods/pads/*.json` (installed like `mods/hotkeys/`), applied by the same `provision(skipPkg)`. New formats (§9): `retroarch_reservation`, `retroarch_remap_port`, `flycast_maple`, `armsx2_router`, `m64_playermap`, `dolphin_pad`. Rules carry an `owner` marker so a later run can tell Thimsy's values from the user's (§11).

### 6.9 Settings rows and strings

- `nano/NanoMenuPS3Menu.cpp:9704-9752` (bound rows), `:2669-2709` (sections), `:1929-1938` (Quick Menu).
- `nano/NanoMenuPS3Data.h:345-359` (descriptions).
- `nano/NanoMenuSettingsTree.cpp:531-590`.
- New strings through `trDyn()`; `test/i18n_check.py` covers the keys.

---

## 7. Prompt rendering

1. **Which pad:** the primary (§8). The button map panel keeps the holding pad.
2. **Which button:** role → `Phys` (§2.3, with the Accept setting).
3. **Which label:** the primary pad's label for that `Phys` (§4.1).
4. **How drawn:**

| Glyph set | Accept example (setting On) | Drawn as |
|---|---|---|
| `nintendo` (Cube, Switch, 8BitDo) | "A" | ringed letter (today's style) |
| `xbox` | "B" | ringed letter, optional Xbox colour |
| `playstation` | Circle | the existing vector shapes |
| `generic` | right | a diamond of four dots, the active one filled, plus "Right" in text where room allows |

5. **Text prompts:** `promptText(in, primary, acceptRight)` rewrites, whole word only, the canonical tokens the strings use today: `A`→Accept, `B`→Back, `X`→Top, `Y`→Left, and `Cross`→Accept, `Circle`→Back, `Triangle`→Top, `Square`→Left. Each then becomes the primary's label for that role's `Phys`. No retranslation is needed, since the English keys don't change. It is applied only where `abxyText`/`themeButtonText` are applied today (28 call sites), never to general text: whole-word "A" also starts "A file with that name already exists."
6. **Mixed glyph sets in one string** ("Press X or O to close") are rewritten as two roles.
7. **ES-DE help bar:** icons chosen by label and family (§6.3); a theme's `customButtonIcon` override still wins, matched on the family's key (`button_a_SNES` …).
8. **drastic-nano footer:** the same function, linked through `drastic_nano_shared_srcs`.
9. **When the primary changes** (hotplug), prompts redraw on the next frame. No animation.

The **Button Prompts** setting stops being a free choice (the user's decision 4: prompts always follow the primary's print). See §11 and Q3.

---

## 8. Primary-controller tracking

### 8.1 Where

**PhoneWindowManager**, as `ThimsyPadTracker` (new file next to `ThimsyDeviceKeys.java`). system_server is up before Nano, survives Nano restarts, gets `InputDeviceListener` callbacks for every add and remove, and has each device's Android **descriptor**, which Flycast and ARMSX2 key player assignment by (§9). It publishes:

| Property | Value |
|---|---|
| `sys.gammaos.pads.primary` | `<vid hex>:<pid hex>:<version hex>:<descriptor>:<name>` (the `bmap_dev` format plus version) |
| `sys.gammaos.pads.primary_family` | family id (§4.1) |
| `sys.gammaos.pads.list` | up to four pads, `;`-separated, in connection order, built-in first |

Nano and drastic-nano match `primary` to their evdev fds by VID, PID, version and name (`EVIOCGID`, `EVIOCGNAME`); two identical pads are told apart by `EVIOCGUNIQ` where the driver fills it (Bluetooth MAC; unverified for xpad). Nano keeps a local fallback (the same rule over its own fds) for the early boot window before system_server publishes.

### 8.2 Rules

1. **Connected beats built-in:** a newly connected external gamepad (source `GAMEPAD` or `JOYSTICK`, with face buttons) becomes primary at once.
2. **Several externals:** the most recently connected is primary. When it disconnects, the previous one still connected takes over; none left → built-in.
3. **Last-used is not a rule** by default: pressing the built-in pad while an external is connected doesn't move the prompts. Otherwise a handheld resting in a dock with a bumped button would flicker. An optional "Primary follows last used" is Q5.
4. **Reconnects** (a Bluetooth pad waking) count as connections, so the pad becomes primary again. Android gives a new device id each time; the tracker keys by descriptor.
5. **Not pads:** keyboards, mice, the vendor's `virtual_gamepad` touch device (`nano/NanoMenuInput.cpp:3178-3181`), the DualSense touchpad and motion nodes, and gammapad's own virtual pad (it stands for its source, §6.6) are never primary.
6. **gammapad running:** the primary is still tracked by physical device (the tracker sees the hidden sources only if `hide_source=0`; otherwise it reads gammapad's published source list, §6.6). Until that lands, with gammapad on the primary is the built-in pad, and the prompts say so.

### 8.3 Edge cases

- A pad that connects while a game runs becomes primary at once. Merge On means it already drives player 1 in emulators that merge (§9). Merge Off: provisioning applies on the next launch; the running app is skipped as today (`nano/NanoMenuOverlay.cpp:522`).
- Primary removed while a Nano dialog is up: the footer redraws with the built-in labels; the meaning of each position doesn't change.
- Two controllers of the same model: the first connected keeps player 2 (Merge Off); prompts look the same.

---

## 9. Merge Controllers

### 9.1 What each emulator needs

| Emulator | Players today | Merge On (default) needs | Merge Off needs | Evidence |
|---|---|---|---|---|
| RetroArch | one device per port, by detection order; P1 binds pinned to keycodes | Both pads on core port 1: `input_remap_port_p2 = 0`. That key is read **only from `.rmp` remap files** (`/tmp/bmap-retroarch/ra/configuration.c:6140`, `:6253`; mapping `input/input_driver.c:6676-6700`). So: a provisioned core remap per shipped core, or accept that P2's pad is ignored by single-player cores | Primary on port 1 through device reservation: `input_player1_reserved_device = "<vid>:<pid> "`, `input_player1_device_reservation_type = 2` (`configuration.c:1577`, `:4017`; `tasks/task_autodetect.c:427-520`). Built-in on port 2 likewise. The pinned P1 binds must stop pinning, or be repeated as the primary's autoconfig, so a Switch Pro as P1 is positional | |
| PPSSPP | single player; any pad is device 10 | nothing | nothing (single player) | bmap plan §3.2 |
| Flycast | port by detection order: first joystick A, next B (`/tmp/bmap-flycast/shell/android-studio/flycast/src/main/java/com/flycast/emulator/periph/InputDeviceManager.java:203-206`); override `emu.cfg [input] maple_<descriptor> = port` (`core/input/gamepad_device.cpp:32`, `:790-795`; descriptor passed at `InputDeviceManager.java:219`) | `maple_<desc> = 0` for every connected pad | primary `0`, built-in `1` | |
| ARMSX2 | first pad to press = P1, next = P2 (`/tmp/bmap-armsx2/…/input/PadRouter.kt:5-12`) | `ARMSX2.xml` `pad_router_all_p1 = true` (`PadRouter.kt:83`, prefs file `MainActivityRuntime.kt:2708`) | `pad_router_pinned = {"<descriptor>":0, …}` (`PadRouter.kt:59`, `:315-347`) | |
| Dolphin | `GCPad1`/`GCPad2` sections, one `Device` each | `GCPad1` binds as `|` alternatives naming both devices (Dolphin expressions take full qualifiers; bmap plan §3.2) | `GCPad1 Device` = primary, `GCPad2 Device` = built-in, and `SIDevice1 = 6` | bmap plan §3.2; device id rules **unverified** |
| M64Plus FZ | no player map = every pad player 1 | nothing | a player map (prefs key **unverified**) | bmap plan §3.2 (`PlayerMap.java:82-86`) |
| Vita3K | OR'd into port 1 outside PSTV mode | nothing | PSTV mode only; ignore | bmap plan §3.2 (`ctrl.cpp:340-356`) |
| Azahar, WatermelonDS, SeedlessDS, DraStic, drastic-nano, ScummVM | single player; global binds | nothing | nothing | bmap plan §3.2 |
| hakuX | `controllers.json` `slots` | slot rules **unverified** | **unverified** | bmap plan §3.2 (`Controllers.kt:208`) |
| Nano Quick Resume runners | port 0 only | nothing | none (single port) | `nano/LibretroRunner.cpp:660` |
| gammapad on | one virtual pad | inherent | not possible until §6.6 | §1.3 |

### 9.2 How it is applied

- `nanopads::plan(pads, merge)` returns the assignment; per-emulator writers turn it into config edits, as provisioning rules (§6.8). The writers are pure functions over file text and are host-tested.
- When: at Nano start, when the overlay rises (skipping the app in front), and when the tracker's `sys.gammaos.pads.list` changes while no emulator is in front.
- RetroArch saves `retroarch.cfg` on exit (`config_save_on_exit`, bmap plan §3.1), so its edits must be made while it is not running, which the skip rule gives.

### 9.3 Recommendation for RetroArch Merge On

RetroArch is the main emulator, and its merge needs remap files. Recommended: provision `config/remaps/<library_name>/<library_name>.rmp` holding only `input_remap_port_p1 = "0"` and `input_remap_port_p2 = "0"` for each core in `retropad.json`, created only where no remap exists, and marked as Thimsy's. Whether a `.rmp` with only those keys leaves the binds at their defaults is **unverified** (check `input_remapping_load_file` before building). A user's own remap replaces it (lookups don't stack, bmap plan §3.1), so Merge On is lost for that core: acceptable, and noted in the setting's help.

---

## 10. Emulator positional mapping per console

Positions: S bottom, E right, N top, W left.

| Console | Mapping by position | Shipped today (Cube) | Notes |
|---|---|---|---|
| NES, Game Boy, GBA, Virtual Boy, Lynx, WonderSwan | A = E, B = S | yes (RetroArch cores, bmap plan §3.1 table) | |
| SNES, DS, 3DS | A = E, B = S, X = N, Y = W | RetroArch yes; WatermelonDS yes; drastic-nano yes; **SeedlessDS and DraStic no** (A on S); Azahar not mapped | Fix SeedlessDS and DraStic defaults by provisioning (§11) |
| PlayStation 1/2, PSP, Vita | Cross = S, Circle = E, Square = W, Triangle = N | yes | |
| Xbox (hakuX), Dreamcast | A = S, B = E, X = W, Y = N | yes | |
| PC Engine | I = E, II = S | yes | |
| Neo Geo Pocket | A = S, B = E | yes (`mednafen_ngp`) | |
| **N64** | A = S, B = W, C buttons on the right stick, Z = L2, L = L1, R = R1 | yes (RetroArch, M64Plus FZ) | Recommendation: keep |
| **GameCube** | Two readings: by geometry, A (the big centre button) = S, B = W, X = E, Y = N; by label on a Nintendo pad, A = E, B = S, X = N, Y = W | Dolphin has no binds | **Recommend geometry** (A = S): A is the main button on GameCube and sits lowest-centre, B is to its lower left. Also matches SDL's GameCube face style `AXBY` (`SDL_gamepad.c:1888`). Q7 |
| **Genesis 3-button** | A = W, B = S, C = E | yes (GPGX, PicoDrive) | |
| **Genesis 6-button** | bottom row A = W, B = S, C = E; top row X = L1, Y = N, Z = R1; Mode = Select | yes (GPGX default, bmap plan §3.1) | Recommendation: keep the core default |
| **Saturn** | Yaba: A = S, B = E, X = W, Y = N, C = L1, Z = R1, L/R = L2/R2. Beetle Saturn: same but C and Z on opposite shoulders | as the cores ship | **Recommend** making both cores match the Genesis 6-button rule (A W, B S, C E, X L1, Y N, Z R1) through core remaps, so a Sega fighting game feels the same on both systems and both cores. Needs per-core `.rmp`; Q8 |
| **3DO** | A = W, B = S, C = E (same as Genesis 3) | yes (`opera`) | |
| **Neo Geo (AES/MVS)** | FBNeo default: A = S, B = E, C = W, D = N | yes | Alternative "arcade row" A = W, B = N, C = R1, D = … is less common on pads. Recommendation: keep FBNeo's |
| **Arcade 6-button (CPS)** | LP W, MP N, HP R1; LK S, MK E, HK L1 per FBNeo/MAME | yes (bmap plan §3.1) | Recommendation: keep |
| Atari 2600/5200/7800, ColecoVision, MSX | primary fire = S, secondary = E | yes | |
| Wii Remote (sideways), Classic Controller | Classic: SNES rule. Sideways remote: 2 = E, 1 = S | not mapped (Dolphin) | Provisioned with the GameCube template |

The rule for every row: the emulator receives Android positions (`BUTTON_A` = S, etc.) from every pad, and maps those to console buttons as above. Once §5 is done for a pad family, the same config works for all pads.

---

## 11. Migration

| Today | After |
|---|---|
| `persist.gammaos.gamepad.abxy_swap` (gammapad input swap, global) | Retired from input (§6.6). Cleared once at the first start of the new build. Not copied to `menu_abxy_swap`, because users set it to fix game buttons on the Cube; menus start at the handheld's layout |
| `face_swap = 1` (OK Button: relabel only) | Retired. It contradicted input. `face_swap` alone never changed input, so leave `menu_abxy_swap` at `0` and clear `face_swap` |
| `face_glyphs = playstation` | Q3. Default proposal: retire; the primary's print decides. Optionally keep a "Built-in pad prompts: Letters / PlayStation" override for the built-in pad only |
| `persist.gammaos.gamepad.merge` (no effect) | Reused as Merge Controllers. Its value is kept: users who set 0 get Merge Off |
| Hotkeys stored as `BUTTON_A` … | Faces are rare (defaults are shoulders). A stored face keycode becomes the matching `FACE_*` for the built-in pad's layout (with gammapad off, `BUTTON_A` is the bottom button = `FACE_SOUTH`), shoulders stay keycodes. Next to `bmapMigrateSetting()` (`nano/NanoMenu.cpp:911-920`) |
| drastic-nano keymap (`persist.gammaos.drastic_nano.key.*`, evdev-derived) | Face entries re-expressed by position once (seed version bump), so DS A stays on the right button of every pad. Non-face entries untouched |
| SeedlessDS, DraStic DS A/B on the bottom/right | Provision `k_4`/`k_5` (SeedlessDS) and DraStic's `_KeyMapConfigs_0_3`/`_2` to A = 97, B = 96 with mode `ifUnset` plus an owner marker, so a user's own change is never overwritten. Q9 asks whether to correct existing installs |
| Provisioned values in general | Each written key gets a sidecar record (`/data/system/nano_pads_owned.json`: file, key, value). A later run changes only keys whose current value still equals what Thimsy wrote |
| README/HOTKEYS wording ("in its menus A still confirms; ABXY Swap stays off", `README.md:98-107`; HOTKEYS.md:70 on `buttonMapKeycode`) | Rewritten when phase 2 lands |

---

## 12. Phases

| Phase | Scope | Host-testable | Effort |
|---|---|---|---|
| 1. Data and model | `families.json` (+ SDL-derived VID:PID file and its tool), `face_layout` in `device-keys.json`, `padSpecFor`, North/West and Switch Pro fixes in the model, `NanoPads` (roles, `menuCode`, `acceptRight`, `promptText`, `choosePrimary`, `plan`), Java mirror and `cases.json` | all | 4-5 days |
| 2. Nano menus | Per-fd `PadModel`, `nextInputEvent` rewrite, raw-code exceptions, the two rows and the new section, prompts by primary (glyphs, text, ES-DE icons), button map panel Back, migration of `abxy_swap`/`face_swap`/`face_glyphs`/hotkeys, Nano-local primary fallback, docs | helpers yes; UI on device | 4-6 days |
| 3. drastic-nano and in-process runners | drastic-nano roles, DS buttons by position, footer, keymap seed bump; `LibretroRunner`/`DrasticRunner` by position | `drastic-nano/tests` and the unit tests | 2 days |
| 4. Primary tracking and PWM | `ThimsyPadTracker` and properties; `faceKeycode`; hotkeys by `FACE_*`; Nano/drastic-nano read the properties | the resolver yes; listener on device | 2-3 days |
| 5. Merge and emulator provisioning | `mods/pads/*.json`, new formats, owner sidecar, RetroArch reservation and core remaps, Flycast, ARMSX2, Dolphin GC template (positional + players), M64Plus, SeedlessDS/DraStic A/B | writers yes, with fixtures | 5-7 days |
| 6. gammapad | `/product` and `_Version_` lookup, per-source swap, `last_src`, optionally `merge=0` as two virtual pads | parser yes (new host test) | 2-3 days (+3-4 for two pads) |
| 7. Unknown pads, optional | "Identify Controller" in Gamepad Settings: press bottom, right, top, left; saves `{vid,pid,version,name} → evdev per Phys` to `/data/system/nano_xmb/buttonmap/pads.user.json`, read by phase 1's matcher. The Switch Pro `/product` layout if Q1 says yes | logic yes | 2-3 days |

Phases 1-3 give the user's menu behaviour with any common pad. Phase 4 makes prompts correct while games run and over the overlay. Phase 5 is the emulator side.

---

## 13. Tests

**Host, `gammaos_nano_unit_tests`** (new `NanoPads_test.cpp`, extended `NanoButtonMap_test.cpp`):
- T1 Family matching: VID:PID, version and name fixtures → family id, for the Cube, Xbox 360 (045e:028e), Xbox Series USB (045e:0b12) and BT (045e:0b13), DS4 v2 with version 0x8111, DualSense, Switch Pro, HORI Switch clone at 0000:0000 with its name, 8BitDo 2dc8:6101, an unknown pad.
- T2 Normalisation: for each family, evdev code → `Phys` and Android keycode → `Phys` for all four faces. Pins finding 4 (Xbox 307 = West) and finding 3 (Switch Pro `BUTTON_A` = East).
- T3 Roles: the 2 settings × 6 families → accept/back/top/left `Phys`, and `menuCode` → the canonical codes; the Cube with the setting On must equal today's behaviour exactly.
- T4 `promptText`: every i18n key containing a prompt token (the 76 + 26 found) × {Cube, Xbox, PS, Switch} × {On, Off} → expected strings; negative cases ("A file with that name already exists.", "A-B Repeat", "Cross Color Reduction Filter") unchanged.
- T5 `choosePrimary`: connect/disconnect sequences, two externals, reconnect with a new device id, non-pad devices ignored.
- T6 `acceptRight`: `""`/`0`/`1` × each `device-keys.json` entry; the same cases run in Java through `cases.json`.
- T7 Player plans and writers: `plan()` for Merge On/Off with 1-3 pads; each writer on fixture files (RetroArch cfg and `.rmp`, Flycast `emu.cfg`, `ARMSX2.xml`, Dolphin `GCPadNew.ini`, M64Plus prefs) → expected text; owner sidecar: a user-changed key is left alone.
- T8 Migration: property sets before → after, including `abxy_swap` with and without gammapad running.

**Host, gammapad:** a new `gammapad_keylayout_tests` for `KeyLayoutParser`: `/product` precedence, `_Version_` preference with fixture `.kl` files, the DS4 legacy-vs-version case.

**Host, drastic-nano:** extend `drastic-nano/tests` for role dispatch and DS bits per family.

**Host, i18n:** `gammaos_nano_i18n_check` for the new strings.

**Device checklist:**
1. `getevent -lp` and `getevent -l` for each pad family on the Cube, recording the four face codes and `EVIOCGID` version: Xbox Series USB and BT, Xbox 360, DS4, DualSense, Switch Pro USB and BT, 8BitDo SN30 Pro in X/S/D/A modes, a HORI/PowerA Switch pad. This confirms or corrects every "unverified" row in §1.1 and §4.1.
2. Nano with each pad, setting On and Off: accept/back/top/left positions in XMB, DSi, Minima, ES-DE themes, the setup wizard, the OSK, the Quick Menu over a game, the button map panel.
3. Prompts follow the most recently connected pad, fall back on disconnect, and the Cube's own prompts are unchanged with nothing connected.
4. drastic-nano menu and DS buttons with the Cube and an Xbox pad.
5. Each emulator in §9.1 with Merge On and Off, one external pad: who is player 1 and 2, and console buttons by position (§10).
6. gammapad on: the Cube positional for apps with the swap off; a DS4 merged is correct after §6.6.
7. Hotkeys set to a face button work on the same position from the Cube and a Switch Pro.

---

## 14. Risks and open questions

| Risk | Impact | Mitigation |
|---|---|---|
| Unverified kernel behaviour (Xbox BT, Switch-style HID, hid-nintendo on device) | A family's normaliser is wrong | Device checklist item 1 before phase 2 ships; the Identify flow covers what can't be known |
| Users used to today's behaviour with an external pad (bottom confirms on the Cube) | Muscle memory changes | It is the user's decision 3. The setting's help says how to get bottom-accept |
| Overwriting emulator settings | A user's own binds lost | Owner sidecar; `ifUnset`/owned-only writes; the skip rule for the running app |
| RetroArch merge needs remap files | Clutter in `config/remaps`; a user remap disables merge for that core | Only where none exists; documented |
| Android descriptors for Flycast/ARMSX2 | Nano can't compute them from evdev | Published by `ThimsyPadTracker` (§8.1) |
| gammapad on hides sources | Prompts and players can't follow the physical pad | §6.6 `last_src`; until then the built-in pad is primary while gammapad runs, stated in the Controllers screen |
| 8BitDo X-input misidentified | Xbox labels on a Nintendo-printed pad | Q6 |
| Two identical pads | Can't tell apart without `uniq` | Connection order; `EVIOCGUNIQ` where present |
| Prompt text rewriting false positives | Wrong letters in ordinary text | Applied only at today's 28 hint call sites; T4 negative cases |
| SDL table drift | New pads unknown | The tool re-imports on an SDL bump; generic fallback is safe |

**Open questions for the user**

1. **Switch Pro on the Android side.** AOSP maps it by label (§1.2), so in every emulator that binds keycodes globally (PPSSPP, Azahar, ARMSX2, WatermelonDS, SeedlessDS, Vita3K, M64Plus) its A and B land swapped. A Thimsy `/product` layout for 057e:2009, like the Cube's, fixes all of them at once but changes what Android apps see for that pad. Is a per-device layout allowed under "leave Android apps alone"?
2. **"Thimsy's own UI":** Nano and drastic-nano only, or also Thimsy-built Android apps (GammaScreenMapper, JoystickLedPicker, ShaderControl, LaunchGuardControl, SecondaryDisplayControl)? The plan assumes Nano and drastic-nano.
3. **Button Prompts setting:** retire it (prompts always follow the primary's print), or keep a Letters/PlayStation override for the built-in pad?
4. **Merge Controllers property:** reuse `persist.gammaos.gamepad.merge` (recommended, §3.3), or a new property and leave gammapad's alone?
5. **Primary controller:** add "Primary follows last used" and/or "Built-in only" options, or keep the single rule (most recently connected external)?
6. **X-input pads with Nintendo print** (8BitDo in X mode): accept Xbox labels, or offer "This controller is labelled like Nintendo" in the Controllers screen?
7. **GameCube:** geometry (A bottom, recommended) or Nintendo label positions (A right)?
8. **Saturn:** make Yaba Sanshiro and Beetle Saturn match the Genesis 6-button rule through core remaps (recommended), or keep each core's default?
9. **Existing installs:** should provisioning correct SeedlessDS's and DraStic's A/B (and Dolphin/Azahar's missing binds) on existing setups, or only fresh ones?
10. **Emulator menus:** should ABXY Swap also drive RetroArch's `menu_swap_ok_cancel_buttons` and PPSSPP's confirm button? Not covered by decision 2; the plan leaves them unchanged.
11. **Other handhelds' layouts:** confirm the §4.2 table (only the Cube is verified).

**Findings outside this feature** (each a separate fix)
1. gammapad scrambles DualShock 3/4 (legacy `.kl`, §1.3).
2. gammapad's ABXY Swap is global, so it breaks an external positional pad when it fixes the Cube.
3. `standardPadSpec` has Xbox north/west evdev codes swapped (`nano/NanoButtonMap.cpp:286`), affecting the button map's Y (table toggle) and `FACE_NORTH/WEST` for Xbox pads.
4. `ThimsyDeviceKeys.buttonMapKeycode` and `standardPadSpec` treat the Switch Pro as positional at the Android level (`pwm/ThimsyDeviceKeys.java:221`, `nano/NanoButtonMap.cpp:272-284`).
5. No RetroArch autoconfig for the Switch Pro in the 226 shipped profiles.
6. The "Merge Controllers" row has no effect today.
7. SeedlessDS and DraStic put DS A on the bottom button by default.

## 15. Decisions (2026-11-15)
User answers:
- **Switch Pro:** fix it in Thimsy only. No system-wide layout file; Nano, drastic-nano and the emulators Thimsy provisions correct the label/position mapping. Android apps see the pad unchanged.
- **8BitDo in X-input mode:** show Xbox labels by default; a per-controller label choice in the Controllers settings overrides it.
- **GameCube:** A on the bottom button (geometry).
- **Existing installs:** update only settings Thimsy wrote itself; leave user remaps alone.

Defaults chosen pending objection:
- "Thimsy's own UI" means Nano and drastic-nano.
- Button Prompts is kept as an override for the built-in pad only; prompts otherwise follow the primary controller.
- Merge Controllers reuses `persist.gammaos.gamepad.merge`.
- Primary controller follows the most recently connected external pad; no extra options yet.
- Saturn: both cores follow the Genesis 6-button layout via core remaps.
- RetroArch's and PPSSPP's own menus are not driven by the setting.
- Other handhelds' layouts (§4.2) stay unverified until confirmed.

## 16. Decisions (2026-11-16)
User: "The button assignments should be based on position and the abxy swap should really only control the menus." Today the Cube needs ABXY Swap On for games to be right by position, and a Bluetooth Xbox pad needs it Off.
- **Game buttons by position for every pad, always.** gammapad drops its input swap and normalises each source by the layout table (§6.6). Nothing the user sets moves game buttons.
- **ABXY Swap controls menus only.** Same name and places, new property `persist.gammaos.nano.menu_abxy_swap`, relative to the handheld's layout (§3.1). It replaces the proposed "Accept with Right Button" row.
- The old gammapad `abxy_swap` value is cleared, not migrated (§11).
