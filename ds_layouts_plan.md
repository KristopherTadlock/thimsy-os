# DS layouts for the Cube screen and monitors (WatermelonDS, SeedlessDS)

Status: proposed 2026-11-27. Research only: no code was changed, nothing was built, no device was touched, and no code from the APKs was run. Every technical claim is marked **[C]** (confirmed from source, the APK, the Thimsy patch or a device capture), **[I]** (inferred) or **[U]** (unverified; needs the device).

Request (owner, 2026-11-27): "Azahar has layout profiles that scale dynamically with resolution. I am not seeing the same options in WatermelonDS. It looks like it is fixed pixel. I want to create layouts that are suitable for the internal display and for a variety of other resolutions in WatermelonDS and SeedlessDS. How do I do that?"

Background: `external_display_modes_plan.md` (Docked and Dual), `dual_screen_43_plan.md` (Swap Screens), `controller_layout_plan.md` (provisioning), `user_emulators_plan.md`, `HOTKEYS.md`.

Evidence used:
- WatermelonDS source at tag 0.8.0.rc3 (commit 9bd6a0d, a fork of rafaelvcaetano/melonDS-android), SeedlessDS at tag 1.0.1 (commit cb80196) and Azahar at tag 2126.2 (sparse), cloned as data into `out/dp-evidence/test43/dslayouts/src/`.
- The APKs pulled from the owner's Cube, unzipped into `out/dp-evidence/test43/dslayouts/wm_unz`, `sd_unz` and `seedless_unz`; DEX string tables in `out/dp-evidence/test43/dslayouts/strs/`. The manifests say 0.8.0.rc3 and 1.0.1. The source's layout constants are all in the DEX files (`layouts.json`, `mainScreenLayoutDto`, `uiSize`, `Built-in Screen`, `dual_screen_preset`; `ctrl_layout`, `top_offx`, `_DefaultLayout`, `_ScreenLayout2`, `_DualScreenPreset`, `_DsSameSize`) **[C]**. That the logic matches the tags is **[I]**. The SeedlessDS APK was built from 3bd4645, one commit after 1.0.1, with no layout change **[C]**.

Citations: `W:` is `out/dp-evidence/test43/dslayouts/src/watermelonds/app/src/main/java/me/magnum/melonds/`, `WR:` is `.../watermelonds/app/src/main/res/`, `S:` is `.../src/seedlessds/app/src/main/java/com/seedlessds/app/`, `A:` is `.../src/azahar/`, and `P:` is a line in `patches/thimsy/0001-thimsy.patch`.

## 1. How to do it today

### 1.1 The short answer

- **Azahar's layouts are not really "profiles that scale".** Its Original, Large Screen, Side by Side and Hybrid layouts are worked out from the window size every time, so one choice fits 720x720, 1080p and 4K. Its **Custom** layout is fixed pixels, like WatermelonDS (`A:src/common/settings.h:580-606`, `A:src/core/frontend/framebuffer_layout.cpp:57-389`) **[C]**.
- **WatermelonDS** has two kinds of layout:
  - The built-in **Default** layout is rebuilt for any size, but you can't edit it, and on the Cube's square screen it is poor (section 1.2).
  - A **custom layout** is in fixed pixels. But one custom layout holds a separate copy, a "variant", for each screen size. You make the layout once, then open the layout editor once at each resolution and save. WatermelonDS then picks the variant that matches the current size, and switches by itself when you dock or undock **[C code, I on the device]**.
  - **A variant is used only at exactly its own size.** A 1920x1080 variant is not scaled to 1280x720, 2560x1440 or 3840x2160, even though they are all 16:9. At a size with no variant, WatermelonDS uses its own Default layout, not yours **[C]**. So one layout per aspect ratio is not enough: you need one variant per resolution.
  - Refresh rate does not count: 1080p60 and 1080p120 share one variant **[C]**.
  - In Dual Screen, the **Dual screen presets** scale one DS screen to each display automatically, at any monitor resolution, as long as each display holds exactly one DS screen **[C]**.
- **SeedlessDS** already works the way you want, like Azahar's preset layouts **[C]**:
  - You pick an arrangement (side by side, large top screen, stacked...), and the screens are worked out from the window size every time.
  - Your own nudges in "Edit controls" are stored as fractions of the screen, so they also fit every resolution.
  - The limit: there is **one** setting for all screens, and the best arrangement for the square Cube screen (stacked) is not the best for a 16:9 monitor (big top screen). SeedlessDS has a **Second layout** and a hotkey to swap between the two. That is the way to have one layout for the Cube and one for monitors. You press the swap hotkey each time you move between the Cube and a monitor, in Docked and in Dual Screen, because the stacked layout also asks Android for a portrait window (section 1.3).
  - It has no per-resolution or per-display layouts, and no layout import or export **[C]**.

| | Azahar | WatermelonDS | SeedlessDS |
|---|---|---|---|
| Layouts that scale to any size | Yes: Original, Large, Side by Side, Hybrid | Only the built-in Default (not editable), and the Dual presets | Yes: every arrangement, and your nudges |
| Your own exact layout | Custom (pixels, one size) | Custom layouts, one pixel variant per size, chosen by exact size | No pixel layout; nudges are proportional |
| Different layout per screen | No (one Custom) | Yes: variants per size and per display pair | No; two layouts and a swap hotkey |
| Cycle hotkey | Cycle Layouts | Cycle layout | Cycle layouts, Swap layouts 1·2 |

### 1.2 WatermelonDS, step by step

**What to know first.**
- **The Cube's square screen counts as landscape.** Thimsy tells apps a 720x720 screen is landscape (`P:172876-172886`, `gammaSquareIsLandscape` at `P:173394-173408`) **[C]**. WatermelonDS reads that (`W:ui/emulator/EmulatorActivity.kt:4017-4023`) **[C]**.
- So its **Default layout on the Cube** is its landscape one: the top screen at 475x356 in the top-left corner and the bottom screen at 245x183 next to it, with the lower half of the screen empty (`W:impl/layout/DefaultLayoutProvider.kt:167-223`, 66% of the width for the top screen) **[C code, I on the device]**. A custom layout is worth making.
- On a monitor at 1920x1080 the Default is top 1267x950 and bottom 653x489, both at the top edge **[C arithmetic on the same code]**.
- **Make every variant in the mode you will play in.** The variant key is the window size, the orientation, the screen insets and the display(s) the app sees (`W:domain/model/layout/UILayoutVariant.kt`). The editor saves into the variant for the size and displays it is running on (`W:ui/layouteditor/LayoutEditorViewModel.kt:231-262`) **[C]**. Why it matters: in Dual Screen with no Dual variant, WatermelonDS reuses your Docked variant at the same size but swaps in the Default's screens (bottom on the monitor, top on the Cube), so you see your buttons with the Default's screens; and in Docked, a Dual-only variant at the same size can be picked, because the last size-only fallback ignores displays (`W:impl/layout/UILayoutProvider.kt:156-185`) **[C]**.
- **Keep the layout's own orientation at "Follow system"** (editor > Start > properties). Setting Portrait because the screen is square changes the variant key and the orientation the app asks for (`LayoutConfiguration.orientation`) **[I]**.
- **The editor works with the controller**, which matters on the monitor, which has no touch (`W:ui/layouteditor/LayoutEditorManagerView.kt:447-495`, `LayoutEditorActivity.kt:135-141`, `LayoutEditorView.kt:125-139`) **[C code; U that the keys reach it on Thimsy]**:
  - D-pad: move the selected item in steps of 1% of the screen (selects one if none is selected).
  - L1 / R1: select the previous or next item.
  - L2 / R2: shrink or grow the selected item in 2.5% steps. Screens keep their 4:3 shape. They work only while the size panel is showing (`LayoutEditorManagerView.kt:500-504`). The panel hides after a typed edit and after a deselect (`:302-304, 683-686`), so press L1 or R1 to select the item again **[C]**.
  - A: the item's menu: **Position** (type exact X and Y, `W:ui/layouteditor/ui/LayoutComponentPositionDialog.kt`), **Size**, **Center horizontal**, **Center vertical**, **Delete** (`LayoutEditorManagerView.kt:535-553`) **[C]**. Center horizontal and Center vertical give the centred X and Y in the tables below in one press. Don't use Size on a screen: it drives the size slider meant for buttons (`:627-631, 663-667`) **[I]**. With nothing selected, A opens the add menu for that display (`:711-731`).
  - B: deselect, or go back. Start: the editor menu (Save, properties, background).
  - **Typed size:** with a screen selected, tap the **Width** or **Height** number on the size panel and type the size in pixels; the other side follows at 4:3 (`LayoutEditorManagerView.kt:169-193, 331-333, 357-359, 651-658`) **[C]**. The controller can't reach those numbers (the D-pad is taken by the editor), so on the monitor that needs a USB mouse **[I that a mouse works]**; with the controller alone, use L2/R2.
  - The Cube's D-pad reaches Android as a hat axis (`HOTKEYS.md:206`). Android normally turns a hat into D-pad keys for the app in focus **[I]**, and the Cube's L2/R2 may arrive as trigger axes rather than L2/R2 keys **[U]**. Test T2 checks both. If either fails, resizing on the monitor needs a mouse.
- **Reset in the editor clears every variant of that layout**, not just the current one (`W:ui/layouteditor/LayoutEditorViewModel.kt:220-229`) **[C]**. Don't use it once you have several variants.
- **Docked mode has no touch.** The Cube's touchscreen does nothing while the monitor is the screen (`README.md:1634-1635`) **[C]**. In Docked, the bottom (touch) screen can only be touched with a USB mouse **[I]**; I found no stick-as-pointer setting in WatermelonDS **[U]**. For games that need the touch screen, play in Dual Screen (touch on the Cube) or on the Cube alone.
- **Mirror mode** (the third External Display mode, `P:45672`) keeps the Cube's screen as display 0 at 720x720 and shows the same picture on the monitor (`dual_screen_43_plan.md:277`) **[C design]**. WatermelonDS then uses your 720x720 variant, and the monitor shows that square picture. It should see no second display **[I; test T7]**.

**One-time setup, and the Cube's screen (720x720, no monitor).**
1. WatermelonDS > Settings > Input > **Soft input behaviour**. The default, "Hide console buttons when a controller is connected", already hides the touch D-pad and buttons, because the Cube's pad counts as a controller (`W:impl/SharedPreferencesSettingsRepository.kt:1641-1643`, `W:ui/emulator/input/ConnectedControllerManager.kt:130-136`) **[C code, I that the pad counts]**. The small utility buttons (hinge, soft-input toggle, mic, fast forward) stay. Choose "Always invisible" for a clean picture, or delete them in your layout. The bottom screen still takes touch either way **[I]**.
2. Settings > Input > **Layouts** > **New layout**. The editor opens at once on a new layout, at the Cube's size, starting from the Default for this size (`W:ui/layouts/ui/LayoutsScreen.kt:97-100`, `W:ui/layouteditor/LayoutEditorViewModel.kt:90-95`) **[C]**.
3. Arrange the two screens by touch, with A > Center horizontal for exact centring. Two good starting points (x, y, width x height, in pixels):
   - **Stacked:** top screen at 120,0, 480x360; bottom screen at 120,360, 480x360.
   - **Big top, small bottom:** top screen at 40,0, 640x480; bottom screen at 200,480, 320x240.
4. Start > properties: name it, for example "Thimsy" (orientation stays Follow system) **[I where the name is set]**. Start > **Save**.
5. Back in Layouts, tap "Thimsy" so it is the selected layout (`W:ui/layouts/ui/LayoutsScreen.kt:250-288`) **[C]**. To change this variant later, use ⋮ > **Edit** on the Cube's screen.

**Each monitor resolution (Docked mode).**

6. Plug in the monitor in Docked mode. In Nano's Output Resolution, pick 1280x720. (It can't be changed over a running game, `README.md:1686-1688`.)
7. Open WatermelonDS > Settings > Input > Layouts > ⋮ > **Edit** on the **same** "Thimsy" layout. It starts from the Default for this size, because this size has no variant yet.
8. Arrange with the controller: L1/R1 to pick a screen, L2/R2 for size (after a typed edit, L1/R1 again first), A > Center horizontal / Center vertical or A > Position for exact X and Y. With a USB mouse, tap the Width number to type the size. Start > Save.
9. Repeat steps 6-8 at 1920x1080 (one variant covers 60 and 120 Hz), 2560x1440 and 3840x2160 (4K30 and 4K60 should share one, if Android reports the same size **[I]**).
   - In **Dual Screen**, 4K30 is not offered, and a kept mode under 50 Hz is replaced by Auto (`README.md:1689-1691`) **[C]**. So a Dual variant at 4K exists only at 4K60 (4:2:0), and a Docked 4K30 choice runs at Auto (usually 1080p) in Dual.
   - 4K60 runs through the 4:2:0 composition path (`README.md:1714-1741`). Neither DS emulator has been checked for frame pacing there **[U; test T10]**.

Suggested numbers (x, y, width x height). They are the same designs at each size **[C arithmetic]**:

| Layout | 1280x720 | 1920x1080 | 2560x1440 | 3840x2160 |
|---|---|---|---|---|
| Big top screen, small bottom screen in the lower-right corner | top 0,0 960x720; bottom 960,480 320x240 | top 0,0 1440x1080; bottom 1440,720 480x360 | top 0,0 1920x1440; bottom 1920,960 640x480 | top 0,0 2880x2160; bottom 2880,1440 960x720 |
| Side by side | 0,120 and 640,120, each 640x480 | 0,180 and 960,180, each 960x720 | 0,240 and 1280,240, each 1280x960 | 0,360 and 1920,360, each 1920x1440 |
| Stacked | 400,0 and 400,360, each 480x360 | 600,0 and 600,540, each 720x540 | 800,0 and 800,720, each 960x720 | 1200,0 and 1200,1080, each 1440x1080 |

**Dual Screen (monitor + Cube screen).** In Dual Screen the game runs on the monitor and the Cube's screen is WatermelonDS's second display (`external_display_modes_plan.md:42`) **[C design, U on the device]**. Two ways:
- **(a) Easiest, scales by itself:** Settings > Video > **Dual screen presets** > **"Internal: Top, External: Bottom"**. Then Keep DS ratio, and Integer scale if you like, and the alignment for each display. This works at every monitor resolution with no per-resolution work (`W:ui/emulator/EmulatorViewModel.kt:2618-2700`) **[C]**.
  - Limit: the preset rescales a display only when it holds exactly one DS screen (`applyScreenScaleToLayout`, `EmulatorViewModel.kt:2668`), then swaps screens so the right one is on each display (`W:ui/emulator/EmulatorActivity.kt:1779-1813`) **[C]**. That holds for the Default and for your Docked variant reused in Dual (WatermelonDS puts the Default's screens into it, `W:impl/layout/UILayoutProvider.kt:156-176`). It does not hold for a custom Dual variant with both screens on one display, which is then left as it is **[C]**. In the rare case the size-only fallback (`UILayoutProvider.kt:179-185`) hands Dual a Docked variant with both screens on the monitor, the top screen could show on both displays **[I]**.
  - "Internal" means the display WatermelonDS runs on, which in Thimsy's Dual Screen is **the monitor**; "External" is the Cube's screen. So this puts the top screen on the monitor and the touch screen on the Cube **[C code, I on Thimsy]**.
  - The presets are off by default (`W:impl/SharedPreferencesSettingsRepository.kt:1395-1396`) **[C]**. Without them, the Default puts the **bottom** screen on the monitor and the top screen on the Cube (section 4.2).
- **(b) Your own layout:** leave the presets at Disabled. In Dual Screen at each monitor resolution, Edit the same layout again. The editor shows the Cube's part on the Cube's screen (`ExternalLayoutEditorPresentation`) **[C code, I on Thimsy]**.
  - It opens on the Default for an external display: the **bottom** screen on the monitor and the top screen on the Cube (`W:impl/layout/DefaultLayoutProvider.kt:62-69`) **[C]**.
  - On the monitor: select the bottom screen (L1/R1), A > Delete; then, with nothing selected, A opens the add menu for that display: add the top screen and size it.
  - On the Cube: delete its top screen by touch, add the bottom screen from its add menu, and place it by touch.
  - Edit both displays. A display you leave untouched is saved empty and refilled from the Default when the game runs (`W:ui/layouteditor/LayoutEditorViewModel.kt:254-256`, `UILayoutProvider.kt:88-108`) **[C]**.
  - Save. This is again one variant per monitor resolution.
- Settings > Video > **External display mode** must stay "WatermelonDS" (the default). With "Mirror", the editor still saves dual variants, but the game never uses them (`WR:xml/pref_video.xml:103-110`, `W:ui/emulator/EmulatorActivity.kt:1549-1561`) **[C]**.

**Check and keep.**

10. Start a game in each mode and check your layout is used, not the Default. If you see the Default, the editor's size differed from the game's (section 6, test T3).
11. Settings > System > **Backup layout** into a folder on the card, for example `emulator_config/watermelonds`. This writes `external_layout.json` (`W:impl/SettingsBackupManager.kt:475-514`) **[C]**. It matters, because Thimsy does not copy WatermelonDS layouts to the card (section 5).
    - Or let WatermelonDS keep a backup by itself: Settings > System > **Save internal config as a file** (off by default, `WR:xml/pref_system.xml:54-59`). After every layout save it rewrites `melonDualDS.opts`, which holds layouts, backgrounds, controller config, settings and ROM data, in the save-file folder, or the first ROM folder when no save folder is set (`W:impl/SettingsBackupManager.kt:84-101, 148-164`; `W:impl/InternalLayoutsRepository.kt:168`) **[C]**. It needs a ROM or save folder set inside WatermelonDS on the card; Nano sets neither **[C no match in the patch]**. After a reinstall, picking that folder offers Restore (`W:ui/settings/fragments/RomsPreferencesFragment.kt:84-110`, `restoreMirrorFrom`) **[C]**.
    - Back up before you swap cards or reinstall. NanoEmuSync gives the card's copy priority for any file "never synced on this card" (a reinstall, another card) (`P:17344-17346, 17416`) **[C]**, so an older card copy of WatermelonDS's `shared_prefs` can put back an old `input_layout_id` over what you set in the app.
12. To restore: Settings > System > **Restore layout**, then force-stop WatermelonDS (Android Settings > Apps) before you touch any layout. Restore replaces all layouts and does not reload them; the app keeps the old list in memory and its next save writes it back over the restored file (`W:impl/InternalLayoutsRepository.kt:120-133`) **[C code, I effect]**.

**Shortcut for the 16:9 sizes (optional, for PC editing).**
- Make the 1920x1080 variant properly. Make the other three once in the editor too (any small change, then Save), so their keys are exactly what the app records.
- Backup layout. On a PC, replace the rects in the 1280x720, 2560x1440 and 3840x2160 variants with the 1080p ones times 2/3, 4/3 and 2, rounded to whole numbers. Leave the variant keys alone.
- Restore layout, then force-stop WatermelonDS.

**Per game and cycling (optional).**
- A game can have its own layout (game settings in WatermelonDS; stored as `RomConfig.layoutId` in `files/rom_data.json`, `W:domain/model/rom/config/RomConfig.kt:11`) **[C]**.
- Settings > Input > **Layouts for cycling**, plus a button mapped to **Cycle layout**, switches between several layouts in game, like Azahar's Cycle Layouts. With no selection, cycling is off (`WR:values/strings.xml:1148-1151`) **[C]**.

**What WatermelonDS can't do.**
- Scale a custom layout to another size, even at the same aspect ratio **[C]**.
- Edit the Default layout (only custom layouts have Edit, and the Default is never saved, `W:impl/InternalLayoutsRepository.kt:155-158`) **[C]**.
- Offer Azahar-style preset arrangements (Large, Side by Side) for one display **[C]**.
- The string "Layouts can not be edited while a dual screen preset is active" exists (`WR:values/strings.xml:658`), but no code uses it, so the editor still opens **[C]**. While a preset is live, the preset places the screens in Dual Screen.

### 1.3 SeedlessDS, step by step

**What to know first.**
- **Screen layout** (`_DefaultLayout`) and **Second layout** (`_ScreenLayout2`) each pick one of these. All are worked out from the window size each time (`S:emu/DsGlView.kt:412-475`) **[C]**:

| Value | Name in Settings | 720x720 (Cube) | 1920x1080 |
|---|---|---|---|
| 0 | Landscape 1:1 (the default) | two 360x270 side by side | two 960x720 side by side |
| 1 | Landscape x:1 (top screen in 72% of the width, small bottom screen 24% wide in the lower-right corner) | 518x389 + 173x130 | 1382x1037 + 461x346 |
| 2 | Vertical (stacked) | two 480x360 | two 720x540 |
| 3 | Integer scale | looks like 1:1 (no own geometry) | |
| 4 | Fullscreen | two halves, stretched | two halves, stretched |

- **Vertical asks Android for a portrait window, in every mode.** `applyOrientation()` requests portrait whenever Screen layout is Vertical, whatever the preset or display, and runs again after every settings change, Swap layouts and Cycle layouts included (`S:emu/EmulatorActivity.kt:195-208, 224, 259-270`) **[C]**.
  - On a monitor, Thimsy ignores app orientation requests and keeps rotation 0 (`P:172998-173003, 173083-173090`) **[C]**. On Android 14 that normally letterboxes an app that asks for portrait into a portrait-shaped window, about 607x1080 at 1080p, with a top screen of about 607x455 **[I; test T11]**. This applies in Docked and in Dual Screen: the preset only places the screens, it does not change the request.
  - So **on a monitor, the current Screen layout must not be Vertical**. Swap layouts fixes it live, since the next request is not portrait **[I]**.
  - On the Cube's own square screen the effect is unknown: Thimsy forces it to count as landscape, so it may also letterbox Vertical **[U; test T6]**.
- **Dual screen presets** are on by default: "Top here · bottom on TV" (`_DualScreenPreset=1`, `S:emu/DsSettings.kt:82`) **[C]**. A preset takes effect only while a second display is attached (`live()`, `S:emu/DualScreenPresets.kt:42`) **[C]**. "Here" is the display SeedlessDS runs on, which is **the monitor** in Thimsy's Dual Screen; "TV" is meant to be the Cube's screen. So the default should put the top screen on the monitor and the touch screen on the Cube **[C code, U on Thimsy]**.
  - Caveat: SeedlessDS takes the **first** presentation display that is on and not private, and does not skip its own display (`S:emu/ExternalDisplay.kt:244-246`) **[C]**. Android 14 lists presentation displays by type, external before internal **[I from AOSP]**, and the monitor at display 0 carried `FLAG_PRESENTATION` in an older capture (`out/dp-evidence/dumpsys-display-run2.txt:104`); the patch adds that flag to the Cube's screen in Dual (`P:169353`) but does not strip it from the monitor **[C]**. So in Dual Screen (and Docked) SeedlessDS may open its second screen on the monitor it runs on instead of the Cube **[U; test T7]**.
- **On-screen controls hide when a gamepad is connected**, by default (`_HideControlsWithGamepad`, "Hide when a gamepad is connected", `S:emu/DsSettings.kt:47`, `S:emu/EmulatorScreen.kt:189`) **[C]**; the Cube's pad should count **[I; test T5]**.
- **Trap 1:** picking a preset in its dropdown sets Screen layout to Fullscreen (stretched), Single screen on and Swap off (`apply()`, `S:emu/DualScreenPresets.kt:50-61`) **[C]**. On the Cube alone or in Docked mode, where the preset is not live, you then get two stretched halves. Second layout is not touched.
- **Trap 2:** while a preset is selected, the Screen layout, Second layout and Single screen rows are hidden ("the preset owns them", `S:emu/SettingsScreen.kt:441, 479-517`) **[C]**.
- **Trap 3:** "Same screen size" (on by default) shrinks the bigger image to the physical size of the other, using each display's reported dpi, width and height separately (`S:emu/DualScreenPresets.kt:100-114`; the dpi come from `S:emu/DsGlView.kt:427-433` and `S:emu/ExternalDisplay.kt:61-62`) **[C]**. An older capture shows the Cube's screen reporting 151 x 269 dpi and a monitor 320 x 320 dpi (`out/dp-evidence/dumpsys-display-run2.txt:26, 104`) **[C for that build]**. With those values the top screen on a 1080p monitor becomes about 1526x642, badly stretched **[C arithmetic]**. Turn it off.
- The Edit controls nudges (`shared_prefs/ctrl_layout.xml`) are fractions of the window, so they fit every size. But there is one set for all modes, games and profiles (`S:emu/ControlLayout.kt:5-80`) **[C]**.
- User profiles also split saves and states into their own folder (`S:emu/EmulatorActivity.kt:92-93, 119`, `S:emu/Profiles.kt:45-48`) **[C]**. Don't use them as display profiles.
- **Docked mode has no touch** (`README.md:1634-1635`) **[C]**. To touch the bottom screen when docked: Settings > Physical controller > Rumble and sticks > **Right stick mode: Pointer mode** (`_AnalogStickMode`, `S:emu/SettingsScreen.kt:114, 565-566`; the mapping help lists L1 as "Touch pointer", `S:emu/MappingInfoScreen.kt:66`) **[C setting, U on Thimsy]**, or a USB mouse **[I]**. For touch-heavy games, Dual Screen or the Cube alone is the better mode.
- **Mirror mode:** the apps see the Cube's 720x720 screen as display 0 and the monitor shows the same picture (`dual_screen_43_plan.md:277`) **[C design]**, so the Cube layout is the right one there.

**Steps (one setup for Cube, Docked and Dual).**
1. SeedlessDS > Settings > Screens > Dual screen presets > **Preset: "Disabled (manual layout)"**, so the hidden rows show.
2. **Screen layout: Vertical** (for the Cube). **Second layout: Landscape x:1** (big top screen) or **Landscape 1:1** (for monitors). **Integer scale: off** (on the Cube it makes Vertical 256x192). Rotation: Automatic.
3. Set **Preset** back to **"Top here · bottom on TV"**, for Dual Screen. This sets Screen layout to Fullscreen again (trap 1); step 6 fixes it. If you never use Dual Screen, leave the preset Disabled and skip step 6.
4. Open **DualScreen Presets Config**: Keep DS ratio **on**, Same screen size **off**, Integer scale off, Vertical alignment Centre for both (or Top/Bottom for the Cube's touch screen). Back in Screens: Internal screen delay **0** (it delays the monitor, not the Cube) **[I]**, External display border 0.
5. Settings > Physical controller > **Map gamepad buttons**: map **Cycle layouts** and **Swap layouts 1·2** to spare buttons (L3 and R3, for example). Avoid the keys Thimsy's hotkeys use (F2, F4, F6 on `s_1`-`s_3`, `mods/hotkeys/seedlessds.json`; `HOTKEYS.md`).
6. On the Cube's screen, start a game and press **Cycle layouts** until the screens are stacked: Fullscreen > 1:1 > x:1 > Vertical, 3 presses (cycle order 0>1>2>3>4, `S:emu/EmulatorScreen.kt:192-211`). It is saved. Now layout 1 is Vertical (Cube) and layout 2 is your monitor layout. The pause menu's "Change layout" does the same, but it is hidden while a preset is set (`S:emu/EmulatorScreen.kt:348, 556`) **[C]**, so the hotkey is needed here.
7. **On a monitor (Docked and Dual Screen):** press **Swap layouts 1·2** once at the start of a game, so the monitor layout is current; press it again when you are back on the Cube. In Dual Screen the preset places the screens, but a current Vertical layout would still ask for a portrait window on the monitor (see "Vertical asks Android for a portrait window" above) **[C code, I effect]**. In Mirror mode, keep the Cube layout.
   - If you'd rather never swap: make **Landscape x:1** the Screen layout for every mode (518x389 + 173x130 on the Cube, 1382x1037 + 461x346 at 1080p). It never asks for portrait.
8. Optional: pause menu > **Edit controls**. Drag or shrink the Top and Bottom boxes (25-100% of the fitted size), scale the external screen, hide buttons, Save. This applies in every mode, so keep it small, or press Reset so each mode keeps its clean fit.
9. Optional, per game: game settings store any of the above for one game only (`pg_<hash>.xml`). The Edit controls nudges stay global. Once a game has its own settings, Swap layouts and Cycle layouts in that game change only that game's file, not the global layouts 1 and 2 (`applyDisplayPersist`, `S:emu/EmulatorScreen.kt:192-196`) **[C]**; so do step 6 in a game without its own settings.

**Or edit the settings file on a PC.** NanoEmuSync keeps a copy of SeedlessDS's `shared_prefs` on the card and copies it back before SeedlessDS starts (`P:17336-17348, 17383`) **[C]**. First check that `<card>/emulator_config/seedlessds/app/shared_prefs/seedless_config.xml` exists. If it doesn't, the sync is not running for SeedlessDS on this card; use the in-app steps above. The sync runs only after "Move Emulator Files to microSD" (`emuSdMode`, `P:67163-67174`), and SeedlessDS's part of that move stops when its folder grant fails (`P:68049-68054`), which known bugs B1 and B2 make likely with the Cube's 36-character card id (`scummvm_autoadd_plan.md:416-417`) **[C mechanism, I for SeedlessDS]**. With SeedlessDS force-stopped, edit the file:
```
<int name="_DefaultLayout" value="2" />
<int name="_ScreenLayout2" value="1" />
<int name="_DualScreenPreset" value="1" />
<boolean name="_DsKeepRatio" value="true" />
<boolean name="_DsSameSize" value="false" />
<int name="_ExtDisplayDelay" value="0" />
```
This avoids trap 1. Layout 1 is Vertical here, so press Swap layouts on a monitor (step 7). The card's copy also wins whenever SeedlessDS's own copy has never been synced with this card (a reinstall, another card) (`P:17344-17346`) **[C]**, so an old card copy can quietly bring back old `_DefaultLayout`, `_ScreenLayout2` or `ctrl_layout.xml` values. Before you put in another card or reinstall SeedlessDS, check that the card holding your current settings is the one in the Cube **[I]**.

**What SeedlessDS can't do.**
- Keep a different layout per resolution or per display, or switch by itself when you dock **[C]**.
- Make a screen bigger than its fitted size in Edit controls (25-100%) **[C]**.
- Export or import layouts: "Export data" copies `files/SeedlessDS` folders only, not `shared_prefs` (`S:emu/SystemToolsScreen.kt:234-246`) **[C]**. The card copy kept by NanoEmuSync is the only backup.

## 2. What Thimsy can do (ranked, recommended first)

| # | Option | Effort | Risk | Value |
|---|---|---|---|---|
| 1 | Add WatermelonDS `files/layouts.json` and `files/backgrounds.json` to NanoEmuSync, with a safeguard for a stale card copy | small (2 table lines, `P:17381`, plus the safeguard) | low-medium (a stale card copy wins and would replace every variant) | layouts survive reinstall, reset and moving the card; the synced `input_layout_id` no longer points at a missing layout |
| 2 | Provision display settings: WatermelonDS `dual_screen_preset`; SeedlessDS `_DefaultLayout` / `_ScreenLayout2`, `_DsKeepRatio`, `_DsSameSize`, `_ExtDisplayDelay`; hotkey binds for Cycle layout / Swap layouts | small | low (ifUnset) | right out of the box in all modes, and avoids SeedlessDS trap 1 |
| 3 | Thimsy reference layouts for WatermelonDS: a generated `layouts.json` merge with a variant for every Thimsy size | medium | medium (a bad write can hide all user layouts) | WatermelonDS behaves like Azahar's presets on Thimsy |
| 4 | SeedlessDS: Nano sets `_DefaultLayout` per mode before launch | small-medium | medium (overrides the user) | no swap hotkey needed when docking |
| 5 | Upstream requests to SapphireRhodonite (both apps) | none for Thimsy | none | the real fix: proportional variants |
| 6 | A Thimsy-patched WatermelonDS build | medium | high (pinned APK, updates) | not recommended |
| 7 | Framework: treat the square as portrait for chosen apps | large | medium | not recommended |

### 2.1 Option 1: sync WatermelonDS layouts (recommended first)
- Today NanoEmuSync keeps WatermelonDS `shared_prefs` and `files/controller_config.json` only (`P:17381-17382`) **[C]**.
- Add `{"watermelonds", "me.magnum.melondualds", false, "files/layouts.json", {}}` and the same for `files/backgrounds.json`.
- `files/rom_data.json` (per-game layout ids and other per-game settings) is a decision (D3): it may hold ROM URIs that differ between cards **[U]**.
- The sync is three-way, the card wins a conflict, and it never writes into a running emulator (`P:17336-17348`) **[C]**. Nano force-stops the emulator before it writes at launch (`P:67444-67449`) **[C]**, so WatermelonDS's in-memory copy can't write an old list back over a synced file.
- **Risk:** a file "never synced on this card" takes the card's copy (`P:17344-17346, 17416`) **[C]**. A stale `layouts.json` on a card (an old card, or one written before the owner's edits) would replace every variant without warning. Safeguard, like B6's for `scummvm.ini` (`scummvm_autoadd_plan.md:418`): when the card wins over an app copy that has no base or also changed, keep the app's copy as `layouts.json.bak` and merge by layout id (the card's layouts plus any only in the app's copy).
- Alternative or complement: WatermelonDS's own "Save internal config as a file" (`melonDualDS.opts`, section 1.2 step 11) already mirrors layouts to a ROM or save folder; Nano could turn it on, but it needs a ROM or save folder set in the app, which Nano doesn't set **[C]**.
- Tests: the existing emusync host tests plus a case for each new item, and "card older than the app" (the app's layouts kept in the .bak and merged).

### 2.2 Option 2: provision display settings
- Uses the existing provisioning engine (`nanohotkeys::provision`, `P:23406-23437`; `xml_prefs` and `melonds_mapper` formats, `P:23274, 23330`) with mode `ifUnset`, so the owner's later choices win **[C mechanism]**.
- WatermelonDS, its main prefs file (`me.magnum.melondualds_preferences.xml` **[I]**): `dual_screen_preset` = `internal_top_external_bottom` (the enum name in lower case, `W:impl/SharedPreferencesSettingsRepository.kt:104, 1816`) **[C]**. Already proposed in `external_display_modes_plan.md:461`.
- WatermelonDS `files/controller_config.json`: bind `CYCLE_LAYOUT` (`W:domain/model/Input.kt:37`) and `SWAP_SCREENS` with `melonds_mapper` **[C]**.
- SeedlessDS `seedless_config.xml`: `_DsKeepRatio=true`, `_DsSameSize=false`, `_ExtDisplayDelay=0`, and a layout pair (int and boolean keys, `S:emu/DsSettings.kt:260-299, 357-396`) **[C]**. The pair is D2:
  - `_DefaultLayout=2`, `_ScreenLayout2=1`: best on the Cube (Vertical), but Vertical asks for portrait, so a monitor needs a Swap layouts press each time (section 1.3).
  - `_DefaultLayout=1`, `_ScreenLayout2=2`: x:1 everywhere with no swap needed; Vertical one press away on the Cube. SeedlessDS writes every key once its settings are saved, so `ifUnset` only helps a fresh install. On the owner's Cube SeedlessDS has only BIOS files so far **[C from the task]**.
- SeedlessDS `keymap.xml`: `s_5` (Swap layouts 1·2) and `s_8` (Cycle layouts) to new universal hotkey keys (`S:emu/KeyMap.kt:22-32, 51`) **[C]**. Needs a free key code and a `HOTKEYS.md` entry.
- Depends on device checks: Vertical on the Cube's screen (T6) and on a monitor (T11), and the presentation question in Docked and Dual (T7).

### 2.3 Option 3: Thimsy reference layouts for WatermelonDS
- **Design once as fractions, generate every size.** A host script (in `mods/`, at build time) writes `mods/layouts/watermelonds-layouts.json`: layouts such as "Thimsy Stacked", "Thimsy Big Top", "Thimsy Side by Side", each with a fixed UUID and a variant for 720x720, 1280x720, 1920x1080, 2560x1440 and 3840x2160 (the table in section 1.2). Dual variants are optional, since the presets cover Dual Screen.
- **Variant keys must be captured first** (T1). Expected key per size **[I]**: `uiSize` (W,H); `insets` all 0 (only the display cutout counts, `W:ui/emulator/EmulatorActivity.kt:531-546`); `orientation` `"LANDSCAPE"` (the square too, because of Thimsy); `folds` empty; `mainScreenDisplay` `{id:0, type:"EXTERNAL", width:W, height:H}` (the Cube's screen is named "Built-in Screen (port 0)", not exactly "Built-in Screen", section 4.1); no secondary display.
- **Writer** (a new `melonds_layouts` provisioning format, or a step after emusync's pre-launch sync):
  - runs only while WatermelonDS is not running (it keeps layouts in memory for its whole process, `W:impl/InternalLayoutsRepository.kt:120-133`);
  - parses the existing list and inserts or replaces only Thimsy's UUIDs, keeping every user layout;
  - leaves a Thimsy layout alone once the owner edited it (keep a hash of what Thimsy last wrote; if the app's copy differs, skip);
  - writes through `nanobios::appFileWrite` (atomic, chown, restorecon, `P:8874-8932`), and only if `files/` exists;
  - sets `input_layout_id` with `ifUnset`; `input_cycle_layout_ids` needs multi-item string set support in `applyXmlPrefs`;
  - writes only for a known versionCode (41 = 0.8.0.rc3), because app upgrades can migrate the file (`W:migrations/Migration39to40.kt`), and fails closed.
- **Validation before writing:** round-trip parse; non-null `mainScreenLayoutDto`, `secondaryScreenLayoutDto` and `backgroundMode`; valid UUIDs; enum names exactly as in the app (case matters, `W:impl/dtos/layout/UILayoutVariantDto.kt`). One bad entry makes WatermelonDS load no custom layouts, and its next save overwrites the file (`W:impl/InternalLayoutsRepository.kt:141-172`) **[C]**.
- Needs option 1, so the result reaches the card.
- No action on a resolution change: WatermelonDS picks the variant itself.

### 2.4 Option 4: SeedlessDS layout per mode at launch
- Before launch, Nano reads the display state (`sys.gammaos.ext.state`) and writes `_DefaultLayout` (2 on the Cube, 1 on a monitor) into `seedless_config.xml`.
- Overrides the owner's choice and does not follow a dock change mid-game. Per-game files (`pg_*.xml`) can override it. Needs an owner opt-in (D5).

### 2.5 Option 5: upstream requests
Ask SapphireRhodonite (GPL-3, both apps) for:
- a proportional fallback in `W:impl/layout/UILayoutProvider.kt` `findSimilarLayoutVariant` (`:114-218`): scale the nearest same-aspect variant instead of falling back to the Default. Its comment already says "ignoring size differences", but the code compares `uiSize ==` (`:179-185`) **[C]**;
- matching "Built-in Screen" as a prefix (`W:impl/layout/devicemapper/DefaultLayoutDisplayMapper.kt:14, 19`);
- Azahar-style preset arrangements for one display;
- for SeedlessDS, a layout per display class (square, 16:9).

### 2.6 Options 6 and 7: not recommended
- A patched WatermelonDS conflicts with the pinned upstream APK (sha256 in `mods/added-apps.txt`) and with app updates.
- Configuration is per display, so a per-app "square is portrait" needs app-level overrides in the framework, for little gain.

## 3. File formats and locations

### 3.1 WatermelonDS (`/data/data/me.magnum.melondualds/`)
- `files/layouts.json`: a Gson JSON array of `LayoutConfigurationDto`; **not** a Room database (`W:impl/InternalLayoutsRepository.kt:35, 135-172`) **[C]**:
  ```
  [{id, name, type (DEFAULT|CUSTOM), orientation (FOLLOW_SYSTEM|PORTRAIT|LANDSCAPE), useCustomOpacity, opacity,
    layoutVariants: [{variant: {uiSize:{x,y}, insets:{left,top,right,bottom}, orientation, folds:[],
                                displays:{mainScreenDisplay:{id,type,width,height}, secondaryScreenDisplay:{...}|null}},
                      layout: {mainScreenLayoutDto:{backgroundId, backgroundMode, components:[{rect:{x,y,width,height}, component, alpha, onTop, buttonMode?}]},
                               secondaryScreenLayoutDto:{...}}}]}]
  ```
- `rect` is in whole pixels (`W:domain/model/Rect.kt:6`) **[C]**. Components: TOP_SCREEN, BOTTOM_SCREEN, HYBRID_SCREEN, DPAD, BUTTONS, START, SELECT, L, R, single A/B/X/Y, and utility buttons (hinge, soft input, fast forward, mic, reset, pause, swap screens, quick save/load, rewind, slot-2 analog) (`W:domain/model/layout/LayoutComponent.kt`) **[C]**.
- Buttons in the Default are sized in dp (140 dp D-pad and face buttons, 50 dp L/R, 40 dp small buttons), so they follow density, not resolution **[C]**.
- `shared_prefs/me.magnum.melondualds_preferences.xml` **[I name]**: `input_layout_id` (the global layout), `input_cycle_layout_ids`, `dual_screen_preset`, `external_display_mode` (`W:impl/SharedPreferencesSettingsRepository.kt:105, 1630-1638, 1801-1803`) **[C keys]**.
- `files/rom_data.json`: per-game `layoutId`. `files/backgrounds.json`: layout backgrounds **[C]**.
- Backup and restore: Settings > System > Backup layout / Restore layout, as `external_layout.json` in a folder the owner picks. The "internal layout" pair is hidden (`WR:values/feature_switches.xml:3`). The whole-settings file `melonDualDS.opts` includes layouts when `save_internal_config_as_file` is on (`W:impl/SettingsBackupManager.kt:34, 84-101, 198-200`) **[C]**.

### 3.2 How WatermelonDS picks a variant (`W:impl/layout/UILayoutProvider.kt`) **[C]**
1. An exact key match (`:71-75`): size, insets, orientation, folds and both displays (id, type, width, height).
2. With a second display: the same pair with the screens swapped; then the secondary's id ignored; then the main-only variant with the default screens put back (`:114-178`).
3. Any variant with the same orientation and **exactly the same** `uiSize` (insets may be rotated 180°) (`:179-185`).
4. Otherwise `DefaultLayoutProvider.buildDefaultLayout` (`:82`).

`uiSize` comes from the emulator view's size listener (`W:ui/emulator/EmulatorActivity.kt:594-605`), so a dock or undock picks the new variant live **[C code, I on the device]**.

### 3.3 SeedlessDS (`/data/data/com.seedlessds.app/shared_prefs/`)
- `seedless_config.xml`: global settings of the Default profile; other profiles use `seedless_config__<hash>.xml` (`S:emu/DsSettings.kt:172-176`) **[C]**. Holds every key in section 1.3.
- `pg_<hash>.xml`: per-game overrides, written as a diff (`S:emu/DsSettings.kt:175-195, 453-459`) **[C]**.
- `ctrl_layout.xml`: Edit controls. `<id>_x`, `<id>_y` (dp), `<id>_s` (scale) for dpad, face, lbtn, rbtn, startselect, menu, extra; `top_scale`, `top_offx`, `top_offy`, `bot_scale`, `bot_offx`, `bot_offy` (fractions), `ext_scale`, `hidden`, `background_uri` (`S:emu/ControlLayout.kt:5-80`) **[C]**.
- `profiles.xml` (`_current`, `_list`), `keymap.xml` (`k_N`, `s_N`) **[C]**.
- No layout data in `files/SeedlessDS/user`, and the APK bundles no layout files **[C]**.

## 4. How it plays with Docked, Dual Screen and Swap Screens

### 4.1 What the apps see
- **Handheld:** one display, the Cube's screen, id 0, 720x720, landscape (Thimsy) **[C]**.
- **Docked:** one display, the monitor, id 0, at the chosen mode; the Cube's screen is off (`README.md:1622-1637`) **[C]**. Density is 213 dpi at 1080 lines, scaled by the mode **[C]**.
- **Dual Screen:** the monitor is display 0 and runs the game; the Cube's screen is a second display (id 2 or 3, not fixed) with `FLAG_PRESENTATION` (`P:169349-169360`; `dual_screen_43_plan.md:59-60`) **[C]**. Games launch on display 0 **[C]**. The monitor itself probably keeps `FLAG_PRESENTATION` too (stock for an external display; seen on display 0 in `dumpsys-display-run2.txt:104`) **[I]**.
- **Mirror:** the Cube's screen stays display 0 at 720x720 and the monitor shows the same picture (`P:45672`, `dual_screen_43_plan.md:277`, `configureGammaosMirrorLocked` at `P:169330-169343`) **[C]**. Apps should see one display **[I; T7, T9]**.
- **Touch:** none in Docked (`README.md:1634-1635`) **[C]**; on the Cube in Dual Screen and Mirror.
- **Display names:** the Cube's screen was "Built-in Screen (port 0)" and the monitor "HDMI Screen" in an older capture (`out/dp-evidence/dumpsys-display-run2.txt:26, 103-104`) **[C for that build]**; the patch has no rename **[I current build]**. WatermelonDS calls a display BUILT_IN only if the name is exactly "Built-in Screen", so it sees **every** Thimsy display as EXTERNAL **[C code, I on the current build]**.

### 4.2 Dual Screen
- WatermelonDS Default with an EXTERNAL display: bottom screen and touch buttons on the main display (the monitor), top screen on the second (the Cube) (`W:impl/layout/DefaultLayoutProvider.kt:62-69`) **[C code, I on Thimsy]**. That is upside down for Thimsy; use the preset or a custom Dual variant.
- WatermelonDS picks its second display as the first display that is not the current one, not "HiddenDisplay" or "WebRTC_ScreenCapture", not off, and is non-default or a presentation display (`W:impl/layout/SecondaryDisplaySelector.kt:19-31`) **[C]**.
- With no Dual variant, WatermelonDS reuses the Docked variant at the same size with the Default's screens put in (bottom on the monitor, top on the Cube) (`W:impl/layout/UILayoutProvider.kt:156-176`) **[C]**; the preset then swaps and rescales them.
- SeedlessDS uses the first presentation display that is on and not private, and does **not** skip its own display (`S:emu/ExternalDisplay.kt:237-255`) **[C]**. Android 14's `DisplayManager.getDisplays(DISPLAY_CATEGORY_PRESENTATION)` lists WIFI, EXTERNAL, OVERLAY, VIRTUAL, then INTERNAL displays **[I from AOSP]**. If the monitor at display 0 is itself listed (the older capture shows `FLAG_PRESENTATION` on it), SeedlessDS could open its second screen on the monitor it runs on: in Docked mode the preset would go live, and in Dual Screen the monitor (EXTERNAL) would come before the Cube (INTERNAL), so the bottom screen would not reach the Cube **[U, test T7]**.
- SeedlessDS's Vertical layout asks for portrait in Dual Screen too (section 1.3) **[C code, I effect]**.
- Touch on the Cube in Dual Screen depends on its touchscreen being bound to that display (`external_display_modes_plan.md:405`) **[I]**.

### 4.3 Plugging in mid-game
- WatermelonDS declares `screenSize|smallestScreenSize|screenLayout|orientation` but not density; SeedlessDS lacks `smallestScreenSize` and density **[C manifests]**. Both declare `isGame`, and Thimsy's game guard keeps such apps from being relaunched on a display-0 change (`P:172729-172796`, `thimsyIsGame` at `P:172792`) **[C]**.
- WatermelonDS then updates its displays and size and picks the new variant (`W:ui/emulator/EmulatorActivity.kt:236-252, 1549-1561, 4044-4051`) **[C code, I on the device]**. SeedlessDS recomputes from the new size, but keeps the same arrangement.

### 4.4 Swap Screens (build 43, not shipped)
- With the Cube's screen as main (`dual_screen_43_plan.md`), WatermelonDS's main display is the Cube and the second is the monitor. The swapped-pair match (`W:impl/layout/UILayoutProvider.kt:118-139`) may reuse a Dual variant saved the other way **[I]**. The presets' "Internal" becomes the Cube, so "Internal: Bottom, External: Top" would be the right one then **[I]**.
- A Presentation is not moved by a swap (`dual_screen_43_plan.md:357`) **[C design]**.

## 5. NanoEmuSync
- Covered today (`P:17381-17385`) **[C]**: WatermelonDS `shared_prefs` and `files/controller_config.json`; SeedlessDS `shared_prefs` (so `seedless_config.xml`, `pg_*.xml`, `ctrl_layout.xml`, `keymap.xml`, `profiles.xml`) and `files/SeedlessDS/user` minus `backup` and `savestates`; Azahar `shared_prefs`.
- **Not covered:** WatermelonDS `files/layouts.json`, `files/backgrounds.json`, `files/rom_data.json` **[C]**. So WatermelonDS layouts are lost on a reinstall or reset and don't move with the card, while the synced `input_layout_id` points at a layout that is no longer there **[I]**. Option 1 fixes this.
- The card copy wins a conflict. Any Thimsy writer (options 2-4) must run after the pre-launch sync, so the next sync pushes its result instead of a stale card copy undoing it **[C sync rule, I consequence]**.
- The card copy also wins for a file "never synced on this card" (a reinstall, another card) (`P:17344-17346, 17416`) **[C]**. Today that can already put back an old WatermelonDS `input_layout_id`, or old SeedlessDS `_DefaultLayout`, `_ScreenLayout2` or `ctrl_layout.xml`, over changes made in the app **[I]**. With option 1 it would extend to every WatermelonDS layout, hence its safeguard.
- The sync runs only with emulator files on the card (`emuSdMode`, `P:67163-67174`), and bugs B1/B2 (`scummvm_autoadd_plan.md:416-417`) can stop the card folder grants for SeedlessDS and WatermelonDS with the Cube's 36-character card id **[C mechanism, I for these apps]**. Option 1's value depends on that working.

## 6. Tests and on-device steps

Owner checks first (read-only on Thimsy's side; no adb needed for T1):
- **T1 Variant keys.** Make one WatermelonDS layout at 720x720 and one at 1080p Docked (section 1.2), then Backup layout to the card and read `external_layout.json` on a PC. Confirms `uiSize`, insets, orientation (`LANDSCAPE` expected for the square), display type (`EXTERNAL` expected) and ids. Needed before option 3.
- **T2 Editor on the monitor.** In Docked mode, do the controller keys reach the WatermelonDS editor: the D-pad (a hat axis on the Cube, so it relies on Android's synthesized D-pad keys), L1/R1, L2/R2 (keys, or only trigger axes?), A > Position / Center, Start > Save? Does a USB mouse work, including tapping the Width number?
- **T3 Editor size equals game size.** After saving a variant, start a game at the same mode: your layout, not the Default, must show. Check at 720x720, 1080p and 4K.
- **T4 Dual presets.** WatermelonDS "Internal: Top, External: Bottom" and SeedlessDS "Top here · bottom on TV": top on the monitor, bottom on the Cube, touch works on the Cube, the controller stays with the game.
- **T5 Controller hides touch buttons.** The Cube's pad counts as a gamepad in both apps (touch buttons hidden).
- **T6 SeedlessDS Vertical on the Cube.** It asks for portrait (`S:emu/EmulatorActivity.kt:259-270`) while Thimsy reports the square panel as landscape (`P:172876-172886`). Look for letterboxing: the game should fill 720x720 with two 480x360 screens, not a narrower window. If it letterboxes, use Landscape x:1 on the Cube.
- **T7 SeedlessDS second screen.** In Docked mode: no Presentation on the monitor itself, the preset not live, Screen layout or Second layout used. In Dual Screen: the bottom screen opens on the Cube, not over the game on the monitor. In Mirror: no second screen in either app.
- **T8 Dock and undock mid-game** in both apps: WatermelonDS switches variant; SeedlessDS refits.
- **T9 Display info.** `dumpsys display` on the current build: the Cube's display name and its xdpi/ydpi, the monitor's at each mode, and which displays carry `FLAG_PRESENTATION` in Docked, Dual and Mirror.
- **T10 4K60 (4:2:0).** Both DS emulators at 3840x2160 60 Hz: frame pacing, and the layout used.
- **T11 SeedlessDS Vertical on a monitor.** In Docked and in Dual Screen with Screen layout Vertical: is the window letterboxed to a portrait strip? Does one Swap layouts press restore the full window?

Host tests (for options 1-3):
- emusync item tests for the new WatermelonDS paths, and "card older than the app" for `layouts.json` (the app's copy kept as .bak and merged).
- Provisioning: `ifUnset` keeps an owner value; the right key types; the lowercase enum value.
- Layout generator: every rect is whole, inside the screen and 4:3 for screens; one variant per Thimsy size.
- Layout writer: merge keeps every non-Thimsy layout byte-for-byte; an edited Thimsy layout is left alone; a malformed existing file is left alone; an unknown versionCode writes nothing; the output round-trips through a parser with the app's rules.

## 7. Risks
- **WatermelonDS custom layouts need an exact size match.** Every mode and resolution needs its own variant, or it silently falls back to the Default **[C]**.
- WatermelonDS layouts are not on the card until option 1 ships. Back them up with Backup layout, or turn on "Save internal config as a file" **[C]**.
- A stale card copy wins over the app's copy after a reinstall or a card swap (NanoEmuSync) **[C rule, I effect]**.
- Docked mode has no touch: the bottom screen needs a mouse, or SeedlessDS's stick pointer **[C README, U pointer]**.
- SeedlessDS Vertical asks for portrait, which probably letterboxes it on a monitor in Docked and Dual Screen **[C code, I effect]**.
- SeedlessDS may open its second screen on the monitor itself in Docked and Dual Screen **[U]**.
- Restore without a force-stop can be undone by the app's next save **[C code, I effect]**.
- Reset in the WatermelonDS editor wipes every variant **[C]**.
- External display mode "Mirror" in WatermelonDS makes the editor's variants differ from the game's **[C]**.
- SeedlessDS: re-picking a preset undoes the Screen layout choice; Edit controls nudges apply in every mode; profiles split saves **[C]**.
- SeedlessDS "Same screen size" uses dpi values that are wrong on the Cube and on monitors **[C capture of an older build, I effect]**.
- Option 3: a bad write can make WatermelonDS drop all custom layouts and then overwrite the file **[C]**. Validate, back up to the card, gate on versionCode.
- Unverified on the device **[U]**: the current display names, square = landscape inside WatermelonDS, the window size equal to the display size in each mode (4K included), the editor with a controller on the monitor, SeedlessDS's Presentation in Docked and Dual, Vertical on the Cube and on a monitor, frame pacing at 4K60.

## 8. Owner decisions
- **D1** Sync WatermelonDS `layouts.json` and `backgrounds.json` through NanoEmuSync (option 1), with the .bak-and-merge safeguard for a stale card copy? Recommended yes. Instead, or as well, turn on WatermelonDS's own "Save internal config as a file"?
- **D2** Provision display defaults (option 2): WatermelonDS preset "Internal: Top, External: Bottom"; SeedlessDS Keep ratio on, Same size off, delay 0, and either Vertical first / x:1 second (best on the Cube, Swap layouts on every monitor) or x:1 first / Vertical second (no swap needed on monitors)? `ifUnset` only, or a one-time `set` with an owner marker?
- **D3** Also sync `rom_data.json` (per-game layout choice, and other per-game settings)?
- **D4** Ship Thimsy reference layouts for WatermelonDS (option 3)? If so, which (Stacked, Big top, Side by side, Single), which is the default on the Cube and on monitors, and should Thimsy set `input_layout_id` or only add the layouts?
- **D5** Let Nano pick SeedlessDS's layout per mode at launch (option 4), overriding your choice, or keep the swap hotkey? Option 4 would also keep Vertical off the monitor.
- **D6** Which buttons for the new universal hotkeys "Cycle layout" and "Swap layouts"?
- **D7** Send the upstream requests (option 5) to SapphireRhodonite?
- **D8** For Dual Screen in WatermelonDS: is the automatic preset enough, or do you want custom Dual variants per resolution?
- **D9** Update SeedlessDS to 1.0.3? It adds an "Automatic" dual-screen preset and a pause-menu screen gap (release notes; the `ctrl_layout` keys are unchanged in its source) **[C notes, U behaviour]**.

## 9. Open questions
1. What does the current build call the Cube's screen, and what dpi does it report (T9)?
2. Is the square really LANDSCAPE inside WatermelonDS on the current build (T1)?
3. In Docked and Dual Screen, does `getDisplays(DISPLAY_CATEGORY_PRESENTATION)` include display 0, and before the Cube (T7, T9)?
4. Do controller keys reach the WatermelonDS editor on the monitor (T2)?
5. Are WatermelonDS's dp-sized buttons usable at 4K (density scaled by the mode)? Only matters with touch buttons shown.
6. After Swap Screens ships, should the Dual variants and presets be re-made for main = Cube?

## Sources
- https://github.com/SapphireRhodonite/WatermelonDS (tag 0.8.0.rc3)
- https://github.com/SapphireRhodonite/SeedlessDS (tags 1.0.1, 1.0.3; release notes)
- https://github.com/azahar-emu/azahar (tag 2126.2)
- https://www.joeysretrohandhelds.com/guides/seedlessds-setup-guide/
- https://www.androidauthority.com/nintendo-ds-emulation-watermelonds-seedlessds-3696622/
