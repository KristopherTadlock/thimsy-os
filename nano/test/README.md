# Nano host tests

Everything here runs on the build host: no device, no flash. The modules are declared at the end
of `../Android.bp` (and `gammaos_sharecfg_tests` in `../../gammaos-sharefs/Android.bp`); the
host tests are in `TEST_MAPPING` presubmit.

| Module | What it checks |
|---|---|
| `gammaos_nano_unit_tests` | `test/unit/*_test.cpp` against the pure sources: NanoTouch, NanoBidi, NanoUtf8Case, NanoJson, NanoI18n, NanoNdsBanner, NanoBiosCatalog, NanoEsdeTheme, NanoLaunch, NanoAppFiles, NanoBiosUtil, NanoScummvmList (ScummVM's game list as the system's games), NanoDisplayTarget (the display Nano draws on, the panel beside a monitor in dual screen mode, and the panel kept as display 0 while a monitor mirrors it), drastic-nano's DsScreenLayout (a DS screen at 4:3 in each window), `NanoFrameFlag.h`, the OTA version compare, the button map's model and readers (NanoButtonMap, NanoButtonMapReaders, fixtures in `test/fixtures/buttonmap/`), and RetroArch's config rewrite, launch config and hotkey provisioning (NanoRaConfig, NanoHotkeys, fixtures in `test/fixtures/retroarch/`) |
| `gammaos_nano_json_fastmath_tests` | the NanoJson tests with NanoJson built `-O2 -ffast-math`, as in gammaos-nano (non-finite checks must not fold away) |
| `gammaos_nano_archive_tests` | `test/archive/`: NanoArchive zip/7z round trips and hostile archives |
| `gammaos_nano_i18n_check` | `test/i18n_check.py`: every `ps3xmb/i18n/*.json` has every key, the same placeholders as the English key, no duplicate keys |
| `nano_bootlogo_golden_test` | `test/bootlogo/`: `nano-bootlogo` draws `bootscreen/thimsy.svg` with Roboto at 1280x720 and 640x480, with and without a status line, exactly as the checked-in `.sha256` files say |
| `gammaos_nano_buttonmap_tests` | `test/buttonmap/NanoButtonMapLayout_test.cpp`: the button map's layout at 11 screen sizes (panel rule, font floor, nothing clipped or overlapping, every row shown), and the navigation and touch state machine (NanoButtonMapNav) |
| `nano_buttonmap_golden_test` | `test/buttonmap/`: `nano-buttonmap` draws each `scenes/*.json` with Roboto (Noto Sans Hebrew for the RTL scene) at 7 sizes, exactly as `goldens.sha256` says |
| `gammaos_sharecfg_tests` | share password round trip, `\040` mountinfo paths, property length limits |
| `gammaos_nano_*_fuzzer` | libFuzzer targets in `test/fuzz/`: JSON, NDS banner, BIOS zip matcher, ES-DE theme, archive |

The C++ tests build with `-Wall -Werror` and ASan + UBSan (`gammaos_nano_test_defaults`).

## Build and run

From the tree root, after the usual `source build/envsetup.sh` and `lunch`:

```sh
m gammaos_nano_unit_tests gammaos_nano_json_fastmath_tests gammaos_nano_archive_tests \
  gammaos_nano_i18n_check nano_bootlogo_golden_test gammaos_sharecfg_tests \
  gammaos_nano_buttonmap_tests nano_buttonmap_golden_test
atest --host gammaos_nano_unit_tests gammaos_nano_json_fastmath_tests gammaos_nano_archive_tests \
  gammaos_nano_i18n_check nano_bootlogo_golden_test gammaos_sharecfg_tests \
  gammaos_nano_buttonmap_tests nano_buttonmap_golden_test
```

Or run the binaries directly, which is quicker while iterating (gtest flags such as
`--gtest_filter=EsdeTheme.*` work):

```sh
out/host/linux-x86/nativetest64/gammaos_nano_unit_tests/gammaos_nano_unit_tests
out/host/linux-x86/nativetest64/gammaos_nano_archive_tests/gammaos_nano_archive_tests
out/host/linux-x86/nativetest64/gammaos_sharecfg_tests/gammaos_sharecfg_tests
```

Fixtures are found next to the binary (`<bin dir>/test/fixtures`, where Soong installs `data`);
set `NANO_TEST_FIXTURES` to point a binary somewhere else, e.g. at `test/fixtures` in the source.
The Python and shell tests find their data next to themselves or in the source tree, so
`python3 test/i18n_check.py` and `test/bootlogo/bootlogo_golden_test.sh` also work from here.

RootWindowContainer's copy of the rule for which `retroarch.cfg` RetroArch launches pass
(`services/core/java/com/android/server/wm/NanoRetroArchConfig.java`) runs the same cases as
`RaConfig.LaunchConfigSharedCases` (`test/fixtures/retroarch/launch_config_cases.tsv`). From
`frameworks/base`, with any JDK:

```sh
mkdir -p /tmp/racfg && javac -d /tmp/racfg services/core/java/com/android/server/wm/NanoRetroArchConfig.java \
  cmds/gammaos-nano/test/java/NanoRetroArchConfigCheck.java && java -cp /tmp/racfg \
  com.android.server.wm.NanoRetroArchConfigCheck cmds/gammaos-nano/test/fixtures/retroarch/launch_config_cases.tsv
```

One screen at a time (`services/core/java/com/android/server/display/GammaosScreenArrangement.java`:
which display is display 0, which are switched off, the old external modes' cleanup, and each
monitor's mode: Auto, stored modes and trials) runs on the host the same way, from
`frameworks/base`:

```sh
mkdir -p /tmp/screens && javac -d /tmp/screens \
  services/core/java/com/android/server/display/GammaosScreenArrangement.java \
  cmds/gammaos-nano/test/java/GammaosScreenArrangementCheck.java && java -cp /tmp/screens \
  com.android.server.display.GammaosScreenArrangementCheck
```

Thimsy's `tests/unit/test_screen_arrangement.py` runs it too.

A fuzzer: `m gammaos_nano_json_fuzzer`, then run
`out/host/linux-x86/fuzz/x86_64/gammaos_nano_json_fuzzer/gammaos_nano_json_fuzzer -max_total_time=60`.

### When a golden changes on purpose

`test/bootlogo/bootlogo_golden_test.sh --bless` rewrites the `.sha256` files from the current
`nano-bootlogo` output (`--keep DIR` keeps the frames to look at first).
`test/buttonmap/buttonmap_golden_test.sh --bless --keep DIR` does the same for the button map
(after `m nano-buttonmap`); look at the PNGs in DIR before keeping the new hashes. The real-theme hashes in
`test/unit/NanoEsdeTheme_test.cpp` change only together with a deliberate change to how themes
resolve; the test prints the new values.

## ES-DE theme limits

`NanoEsdeTheme` refuses a theme (the engine then falls back to the default theme) once one load
needs more than 512 files, 16 MB of theme files, a 64 KB substituted value or 16 MB of variable
substitution in total. An include of a file already being parsed (a cycle) is skipped. Theme
paths (images, fonts, sounds, includes) must stay inside the theme set folder.

The limits were sized against the real themes available, over every variant x colour scheme x
aspect x system (the same loop as `EsdeRealThemes` in `test/unit/NanoEsdeTheme_test.cpp`):

| Theme | Files per load | Bytes | Variables | Longest value | Substituted bytes | Worst load |
|---|---|---|---|---|---|---|
| Slate (bundled) | 7 | 43 KB | 55 | 50 | 2.5 KB | 1.4 ms |
| Colorful (simplified) | 6 | 63 KB | 39 | 1051 | 5.0 KB | 1.2 ms |

Every limit is at least 60x the largest value measured. `EsdeRealThemes` also checks that both
themes resolve exactly as they did before the limits existed (element count and a hash of every
property); Colorful is read from the thimsy build cache, or `NANO_TEST_COLORFUL_ZIP`.

## esde_parser_check

`esde_parser_check.cpp` is a standalone dump tool for one real theme set, outside the Soong
modules. It resolves the gamelist grid / textlist / system carousel so the theme-resolution
layering can be checked without a build+flash cycle:

```sh
c++ -std=c++17 -DESDE_HOST_TEST \
    -I frameworks/base/cmds/gammaos-nano \
    -I frameworks/base/cmds/gammaos-nano/test \
    -I external/tinyxml2 \
    frameworks/base/cmds/gammaos-nano/test/esde_parser_check.cpp \
    frameworks/base/cmds/gammaos-nano/NanoEsdeTheme.cpp \
    external/tinyxml2/tinyxml2.cpp \
    -o /tmp/esde_parser_check

/tmp/esde_parser_check <themeDir> [system] [variant] [colorScheme] [aspect] [screenAspect]
```

`themeDir` holds `capabilities.xml` and `theme.xml`. `screenAspect` is width/height; `aspect`
can be `automatic` (nearest declared ratio) or an explicit id like `16:9`. Pointed at Art Book
Next it checks the theme's own documented numbers: per-system `${systemCoverSize}` include
substitution and comma-listed aspect matching (`snes` at 4:3 gives grid `itemSize 0.33125 x
0.33125`), the default cover size for an unknown system (`0.2476562 x 0.3302083`), aspect
adaptivity, and that colour schemes are not wildcards (`ffffff55` vs `444444`). It prints
`== OK (0 failures) ==` on success. `test/log/log.h` is the logging stub that lets it link without
liblog; `tools/esde-parsecheck` loads every theme in a folder across its full capability matrix.
