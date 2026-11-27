# Linux build: implementation design

Date: 2026-11-25. Status: design only; nothing in it has been built or run, except the patch regeneration check in section A.3, run once on the fetched tree. Written autonomously while the owner was away; every choice is logged in section I and in `docs/linux/DECISIONS.md` ("Implementation design"). A three-lens design review (architecture, testing, risk) was applied the same day; section K lists each finding and what was done, and the sections below already carry the changes.

This is the design the next phases follow: the refactor of Nano for portability (plan Phase 1), the tests, the Android build chain's changes, and the Linux build (plan Phase 2), up to the point where a build would run. It follows `linux_handheld_plan.md` (the plan) and `docs/linux/DECISIONS.md`. Where it refines a decision there, it says so. Claims not checked against code or a source are marked *(guess)*.

Sources read for it: the plan (sections 0 to 17), `DECISIONS.md`, `android_launcher_plan.md` (sections 1 and Phase 0 results), the applied Nano tree at `work/android-launcher/upstream/frameworks/base/cmds/gammaos-nano` (its `Android.bp`, `NanoMenu.h`, `NanoMenuDrm.h`, `NanoDisplayTarget.h`, `NanoVideo.h`, `NanoMenuUtils.h`, `main.cpp`, `test/`), `tools/android-launcher/`, `tests/run.sh`, `tests/util.py`, `tests/unit/test_nano_units.py`, `tests/fixtures/nano_gtest/`, and `mods/`.

---

## 0. Facts this design rests on

Counted on the applied tree (GammaOS `0bc531e2d` plus the patch), 2026-11-25:

| Fact | Value |
|---|---|
| Files in the Nano folder | 2,110; 15 MB without `ps3xmb/` (69 MB, 1,515 files), `nano_esde_themes/` (3.9 MB) and `nano_esde_assets/` (1.6 MB) |
| Patch files under `cmds/gammaos-nano/` | 486 of 841: 342 new, 2 deleted (`NanoHalChime.cpp`, `.h`), the rest modified; 7 binary (6 test archives, 1 fixture `retroarch.cfg`) |
| Patch files under `ps3xmb/` | 173 (106 `daijishou`, 43 `controllers`, 21 `i18n`, 3 `buttonmap`), 6.9 MB |
| The Nano block in the patch | one contiguous run of sections, lines 2468 to 163621; the patch's sections are sorted by path |
| `property_get` and typed forms | 922 uses in 58 files; `property_set` 599 in 40; `android::base::GetProperty`/`SetProperty` and typed forms 153 in 15 |
| `__system_property_find`/`_serial`/`_wait` | 60 uses in 8 files (`NanoMenu::PropEdge`) |
| `ALOG*` | 1,490 uses in 70 files; `__android_log_*` 21 in 10 |
| `system(` / `popen(` / `exec*` / `fork(` | 117 in 33 / 38 in 18 / 4 in 3 / 7 in 4 |
| `"/data/system` / `"/storage/` / `/mnt/media_rw` / `"/system/etc` / `/system/fonts` | 191 in 52 / 142 in 28 / 73 in 28 / 43 in 19 / 19 in 3 |
| Other absolute roots (lines in `.cpp` and `.h`, counted at the review) | `"/data/data` 127, `"/data/local` 25, `"/data/user` 25, `"/data/misc` 4, `"/data/gammaos_ota` 4, `"/system/bin` 47, `"/vendor/bin` 4, `"/storage/self` 27, `"/mnt/shares` 17, `"/proc/self` 30, `/dev/block` 7 (`NanoScreenCal.cpp` opens `/dev/block/by-name/baseparameter` read-write and `pwrite`s it) |
| Direct `fork` and `exec` | `NanoMenuEsdeMenu.cpp:838-843` (`/system/bin/nsenter`, `execv`), `NanoOtaCheck.cpp:185-198` and `544-557` (`execvp`), `NanoSdMove.cpp:264-282` (`execv`); no `posix_spawn` |
| `uptimeMillis` / `elapsedRealtime*` / `systemTime` | 237 in 26 / 60 in 10 / 76 in 8 |
| Time outside those (lines) | `steady_clock` 81, `clock_gettime` 42, `usleep(` 154, `nanosleep` 2, `sleep_for` 3; frames are paced by `usleep(16666)` |
| `NanoMenu::threadLoop` | lines 1315 to 6329 of `NanoMenu.cpp`: one-time set-up (`mallopt`, `mlockall` at 1435-1449), then its own frame loop `while (!exitPending() && !mExitRequested)` at 4060, then teardown and `IPCThreadState::self()->stopProcess(); return false;`. It runs once per process, not once per frame. `readyToRun` (437-1227) takes the DRM path only `if (sDrmActive)`, which `drmEarlySplash()` in `NanoMenuDrm.cpp` sets; `threadLoop`, `readyToRun`, `onFirstRef` and `binderDied` are private in `NanoMenu.h` |
| Other binaries built from Nano's files | `drastic-nano` compiles the filegroup `drastic_nano_shared_srcs` (19 files: `NanoMenuDrm.cpp`, `DrasticRunner.cpp`, `DrasticGpu3d.cpp`, `DrasticSettings.cpp`, `FakeJNI.cpp`, `NanoJson.cpp`, `NanoI18n.cpp`, `NanoBidi.cpp`, `NanoDeviceKeys.cpp`, the four `NanoButtonMap*.cpp`, `NanoAppFiles.cpp`, `NanoBiosUtil.cpp`, `NanoBiosCatalog.cpp`, `NanoSystemsHeal.cpp`, `NanoEsdeSystemName.cpp`, `NanoDisplayTarget.cpp`); its own `Android.bp` and `main.cpp` sit in the patch outside the Nano block (patch lines 369 to 1912). `gammaos-nano-scrape` compiles 6 (`NanoScrapeWorker`, `NanoScrapeQueue`, `NanoScraper`, `NanoJson`, `NanoI18n`, `NanoScummvmList`) and `gammaos-nano.rc` starts it on `sys.gammaos.nano.scrape_run`. The `cc_test_host` modules compile more |
| Sources outside the Nano folder | `NanoMenuShares.cpp` includes `<share_config.h>` (`libgammaos_sharecfg`, `cmds/gammaos-sharefs`); `NanoMenuRgbFollow.cpp` includes `GammaRgbProcess.h` (`libgammargb_process_headers`); the unit tests use `:drastic_nano_layout_srcs` and `include_dirs: ["frameworks/base/cmds/drastic-nano"]` |
| Android NDK and libc extras in shared files | `android/imagedecoder.h` and `android/bitmap.h` (`NanoThemeEngine.cpp`, 56 `AImageDecoder` uses: the ES-DE theme's image decoding), `aaudio/AAudio.h` (`NanoAudio.cpp` in callback mode with `requestStart` and `requestPause`; `NanoMenuPS3Menu.cpp`), `media/NdkMedia*.h` (reached from `NanoMenu.h` through `NanoTsDemux.h`), binder, AIDL and HIDL light and health; Bionic-only `mallopt(M_PURGE_ALL)` and `M_CACHE_COUNT_MAX`, `strlcpy`, `getprogname`, `ANDROID_PRIORITY_*`, `SLOGE_IF` |
| `gammaos-nano.rc` | 18 `on property:` triggers (`scrape_run`, `dosleep`, `dowake`, `park_restart`, `fixedperf`, `app_launched`, `minimal_boot` and others) |
| `.gitignore` of the fork | ignores `*.7z`: the six archive fixtures under `test/fixtures/archive/` would be left out of `nano/` silently. `nano/.gitignore` (upstream) ignores the ScreenScraper developer credentials (`screenscraper_dev.local.txt`, `NanoScraperDevCreds.gen.h`); `NanoScraperDevCreds.h` builds without them (`__has_include`) |
| `utils/Mutex` | not used (Nano uses `std::mutex`) |
| `namespace android` | 179 files |
| `NanoMenu` | `class NanoMenu : public Thread, public IBinder::DeathRecipient`, with `sp<SurfaceComposerClient>`, `sp<IBinder>`, `sp<SurfaceControl>`, `sp<Surface>` members; `NanoMenu.h` includes `sys/system_properties.h`, `utils/Thread.h`, `binder/IBinder.h`, `android/sensor.h`, `android/looper.h` |
| SurfaceFlinger use | `NanoMenu.cpp` 48, `NanoMenuOverlay.cpp` 48, `NanoMenuRender.cpp` 33, `NanoMenuPS3Clock.cpp` 23 lines |
| DRM state (`sDrm*`, `drmMode*`) | `NanoMenuDrm.cpp` 382; read from shared files too: `NanoMenuRender.cpp` 63, `NanoMenu.cpp` 60, `NanoMenuPS3Clock.cpp` 24, `NanoMenuShaders.cpp` 22, `NanoMenuPS3Menu.cpp` 19, `NanoMenuEffects.cpp` 11, `NanoScreenCal.cpp` 10 |
| C++ standard | C++17 is enough: `std::clamp` (16), `std::optional` (1); no `<span>`, `<format>`, `<bit>`, concepts, `<=>`, `from_chars`, `std::filesystem`. Soong builds it with its default (gnu++17 on Android 14 *(guess, from memory)*) |
| Host tests | `gammaos_nano_unit_tests` (24 production files plus `:drastic_nano_layout_srcs` and `test/unit/*_test.cpp`, ASan and UBSan, `-Wall -Werror`), plus archive, file job, BIOS e2e, button map, fastmath, i18n, two golden tests and five fuzzers. The tests use `TEST`, `TEST_F` (101), `SCOPED_TRACE` (24), `EXPECT_FLOAT_EQ` (23), `GTEST_SKIP` (8), `EXPECT_NEAR`; no gmock matchers |
| Platform headers of the host-tested files | counted transitively, few need none: `NanoButtonMap.h` includes `NanoMenuUtils.h`, which includes `android-base/properties.h`, `cutils/properties.h` and `utils/Log.h`, so the button-map files and their tests need shims. Libraries: `openssl/md5.h` (`NanoButtonMapReaders`), zlib (`NanoNdsBanner`, `nano_test_zip.h`), OpenSSL and zlib (`NanoBiosCatalog`), tinyxml2 (`NanoEsdeTheme`), `ziparchive/zip_archive.h` (`NanoEsdeSystemName_test.cpp`, `NanoEsdeTheme_test.cpp`). The exact per-test closure is computed by G1c, not hand-counted |
| Host compilers | Windows: Python 3.12, no compiler. WSL `Ubuntu` (20.04, checked at the review): g++ 9.4, make, Python 3.8.2, glibc 2.31, git 2.25.1, `unshare`, `prlimit` and `flock` present, no cmake, **no development packages**: no `zlib.h`, `openssl/`, `EGL/`, `GLES2/`, `libdrm/`, `gtest/` under `/usr/include`. Git Bash on Windows has no `flock`. `tests/unit/test_nano_units.py` already compiles `NanoTypec` and `NanoMonitorMode` with `g++ -std=c++17 -Wall -Wextra -Werror` against a 95-line GoogleTest stand-in |
| git | Windows 2.37.2 (no `merge-tree --write-tree`, which needs 2.38; its system config sets `diff.astextplain.textconv` and `core.autocrlf true`, and its system gitattributes map `*.doc`, `*.pdf` and others to `astextplain`); WSL 2.25.1 (no `git ls-tree --format`, which needs 2.36, and which `fetch_nano.sh:31` uses) |

g++ 9.4 supports every C++17 feature Nano uses. Whether all of Nano compiles under g++ was not checked: compiling would have competed with the AOSP job for the shared WSL2 VM's memory, and the development headers it needs are not installed and can't be downloaded now. Every C++ target beyond the dependency-free ones is therefore "written, not compiled" until step G10 (section G).

---

## Principles

1. **Merge-friendly first.** `nano-144` keeps moving. Every step prefers a change that adds files over one that edits Nano's existing files, and an edit that is mechanical over one that is not.
2. **The seam is cut at the lowest boundary that leaves call sites alone.** In order: header shims (`compat/`), link-time substitution (another implementation of the same functions), filesystem mapping on the Linux image (`/data/system`, `/system/etc`, `/system/fonts` as links), and only then interfaces in `NanoPlatform.h`, where behaviour really differs.
3. **Android behaviour does not change** in any step that is not explicitly an Android change, and every step says which Android files it touches (section H).
4. **Every commit leaves the tests passing** that can run on this machine: Python on Windows, g++ 9.4 in WSL `Ubuntu` at `nice -n 19`, `-j1`. While an Android build may be running, only the small C++ targets run (`check-compat`, `check-unit-0`, `libthimsy`, `thimsy-session`, `thimsy-padd`), each through the memory guard of C.3; the heavy ones are deferred to step G10 and reported as skipped, never as passing.
5. **One list per fact.** A source file is listed once (`nano/build/sources.txt`), a Buildroot option once (a fragment), a device fact once (`device.json`), and every other list is generated from it or checked against it.

---

## A. Source layout in the fork

### A.1 Options

| Option | For | Against |
|---|---|---|
| (1) The patch stays the only source; `fetch_nano.sh` applies it into `work/` for CMake and Buildroot | No duplicate; the AOSP flow is untouched; no new tooling | Every refactor commit is a diff of a diff (unreviewable for a change across 100 files); editing needs the applied tree, regenerated by hand; CMake and Buildroot need a fetch and apply step before any build; git blame and history of Nano's files are lost in the fork; parallel agents can't touch the same 11 MB file |
| (2) **Nano's source as real files in `nano/`, the patch's Nano block regenerated from it** | Normal history, review and blame; CMake and the Buildroot recipe build from the repository; parallel work on separate files; the AOSP flow still reads the same patch file | Two representations: needs a regenerator, an importer for `nano-144`'s patch changes, and a consistency test |

**Chosen: (2)** (decision I1). The regeneration was tried on the fetched tree (A.3) and reproduces the committed patch's Nano block byte for byte, apart from four executable bits that a Windows checkout loses and the tool takes from git's index. The cost is two tools and a test; the gain is that the refactor becomes reviewable.

This refines decision 7 (`DECISIONS.md` D7: "the patch stays Nano's source of truth until the seam has settled and the fork has merged back"). On the `linux` branch, `nano/` is what people edit and the patch is generated from it, always in step (the consistency test fails otherwise). On `nano-144`, the patch stays the source until the merge. The AOSP flow consumes the same file in the same form on both branches.

### A.2 What is in `nano/` and what stays fetched

`nano/` is the folder `frameworks/base/cmds/gammaos-nano` as the patch leaves it, minus the fetched assets. The rule (decision I2):

- **Fetched, never committed:** a file under `ps3xmb/`, `nano_esde_themes/` or `nano_esde_assets/` that the current patch does not add or modify. Those are upstream GammaOS's binary assets (about 72 MB) and are read from the base commit.
- **Committed:** everything else: all code, `Android.bp`, scripts, docs, tests and fixtures (the 7 binary fixtures included), the vendored `liba52/`, `nanosvg/`, `rcheevos/`, and every file under the three asset folders that the patch adds or modifies (173 files, 6.9 MB today). About 22 MB in all.
- **Overlay:** for a path under the three asset folders, a file in `nano/` wins over the base's; a base file there that the patch deletes is listed in `tools/nano-tree/upstream.json` (`"deleted"`; empty today). Outside the asset folders, `nano/` is complete: a base file that is not in `nano/` is deleted by the patch (today `NanoHalChime.cpp` and `.h`).
- **Metadata stays out of `nano/`,** so it never reaches the patch: `tools/nano-tree/upstream.json` (`repo`, `base` commit `0bc531e2d64698316f28cedc8992ffa0e44f1a76`, `path`, the three asset globs, `abbrev: 11`, `deleted`, `fork_only`, `siblings`) and `tools/nano-tree/block.sha256` (the SHA-256 of the Nano block `regen` last wrote). On the fork `upstream.json` is the only place the base is written; `fetch_nano.sh` becomes a wrapper that reads it. There is no `sync.json`: the `nano-144` commit last merged is `git merge-base linux nano-144`, which git already knows.
- **Bytes are kept exact:** `.gitattributes` gets `/nano/** -text` (no line-ending normalisation; diff still treats text as text). Executable bits live in git's index (`git update-index --chmod=+x`), since Windows checkouts lose them; 4 files have them today.
- **Ignored files:** the root `.gitignore`'s `*.7z` rules would drop the six archive fixtures without a message, so G0 adds `!/nano/**/*.7z` and `!/nano/**/*.7z.[0-9][0-9][0-9]` after them. `nano/.gitignore` keeps ignoring the scraper credentials, and `import-tree` skips every path `git check-ignore` matches, so a developer's local credentials never reach `nano/`.
- **Fork-only files stay out of the patch.** `upstream.json`'s `fork_only` globs list what Android never compiles: `platform/linux/**`, `platform/host/**`, `test/host/**`, `compat/stubs/**`, `build/**`, `CMakeLists.txt`, `cmake/**`, `third_party/wpa_ctrl/**`, and `LICENSE` and `NOTICE` (I41's licence files, gfix). `regen` leaves them out, so a Linux or host commit does not rewrite the 11 MB patch. What does reach the patch, and so the AOSP tree, is what Android compiles or what the launcher plan's app reads from the patched tree: the shared files, `compat/include/**` and `compat/src/**` (plan 4.3: "the seam and the compat shims land in the patch"), and `NanoPlatform.h` and `platform/firmware/**` once P4 starts. Two rules keep new patch files inert for Soong: no new `Android.bp` anywhere under `nano/` except the existing one, and no new file under a directory `Android.bp` globs (`test/unit/*_test.cpp` is globbed into `gammaos_nano_unit_tests`; host-only tests go to `nano/test/host/`).
- **Siblings.** Nano reads three things outside its folder (section 0): `libgammaos_sharecfg`'s `share_config.h`, `GammaRgbProcess.h` and `drastic-nano`'s `DsScreenLayout`. `upstream.json`'s `siblings` lists their paths; `compose` exports them from the base plus the patch's sections into `OUT/_siblings/`, so the host build and the tests find them. They are not copied into `nano/`; the files that need the first two are `[firmware]` for v1 (C.1).

### A.3 The regeneration, as tried

On the fetched cache (`work/android-launcher/upstream/.git`, which holds the base commit's Nano blobs), with a temporary index:

```sh
export GIT_INDEX_FILE=$TMP/index
git read-tree --prefix=frameworks/base/cmds/gammaos-nano/ <base>:frameworks/base/cmds/gammaos-nano
git add -A frameworks/base/cmds/gammaos-nano            # the applied tree
git diff --cached --binary --abbrev=11 <base> -- frameworks/base/cmds/gammaos-nano
```

The output was 161,154 lines and 9,954,914 bytes, the same as the patch's Nano block; the only differences were four `new file mode 100755` lines written as `100644` (`nano-typec-guard.sh`, `test/bootlogo/bootlogo_golden_test.sh`, `test/buttonmap/buttonmap_golden_test.sh`, `test/buttonmap/rgba2png.py`). The tool therefore stages each file with `git update-index --add --cacheinfo <mode>,<blob>,<path>`, the mode read from the fork's index (`git ls-files -s nano/`), never from the filesystem. `--abbrev=11` matches today's patch; git lengthens an abbreviation that is ambiguous in the cache's object store, which the consistency test would catch *(guess that it stays at 11 for this set)*.

That trial used porcelain `git diff` on Windows git, whose output follows user and system config. The tool pins everything instead:

- **Objects.** The fork's blob ids are not in the cache's object store, so every git command of `regen` runs with `GIT_ALTERNATE_OBJECT_DIRECTORIES=<fork>/.git/objects`.
- **Command.** `git -c core.quotepath=off -c core.compression=-1 diff-index --cached -p --binary --abbrev=11 --no-renames --no-textconv --no-ext-diff --no-color --diff-algorithm=myers --src-prefix=a/ --dst-prefix=b/ <base>`, under `GIT_CONFIG_NOSYSTEM=1`, `GIT_CONFIG_GLOBAL=/dev/null` (ignored by git 2.25, which reads `HOME`) and an empty `HOME`, so no `astextplain`, no `noprefix`, no rename detection and no user attributes apply.
- **Binary sections.** The 7 `GIT binary patch` sections are zlib output, whose bytes depend on the zlib build and `core.compression`. When a binary file's post-image blob id equals the one in the committed patch's `index` line, `regen` copies the committed section verbatim; only a changed binary file is re-encoded.
- **Check.** `regen --check` compares text sections byte for byte and binary sections by the decoded blob id.
- **Second git.** The trial is repeated under WSL's git 2.25 before G0 is called done; if the two gits disagree on a text section, Windows git 2.37 is declared the reference and the WSL run of `test_regen_matches` is skipped with that reason.

### A.4 The tools: `tools/nano-tree/`

`tools/nano-tree/nano_tree.py`, Python 3.8-compatible (WSL's Python is 3.8; no `str.removeprefix`, no `match`, `from __future__ import annotations` where types are used), standard library plus the `git` command:

| Command | Does |
|---|---|
| `fetch [--from-cache DIR]` | Makes the base cache `work/nano-base/` (a blob-filtered repository at the base commit, as `fetch_nano.sh` does, listing blobs with plain `git ls-tree -r` parsed in Python, since `--format` needs git 2.36). `--from-cache work/android-launcher/upstream` adopts an existing cache with no network and no transport: it makes the new cache with `objects/info/alternates` pointing at the old cache's objects, then `git update-ref refs/thimsy/base <base>` (a local `git fetch` of an unadvertised SHA from a ref-less, shallow promisor repository is refused, or tries the network) |
| `compose OUT` | Writes the full applied folder: base files under the asset globs, overlaid by `nano/`; deleted paths left out; siblings under `OUT/_siblings/`. What CMake's asset install, the Buildroot recipes and the old tests read |
| `regen [--check]` | Builds the Nano block as in A.3 (leaving out `fork_only` paths) and splices it between the committed patch's prefix and suffix. It refuses to overwrite a patch whose Nano block no longer hashes to `block.sha256` (someone or a merge changed it outside `regen`) and points to `import-patch`. `--check` compares instead of writing; exit 1 with the first differing line |
| `import-patch REF` | Three-way import of `nano-144`'s Nano changes into `nano/` (A.5) |
| `merge-patch` | What the merge driver runs (A.5) |
| `import-base NEW` | Moves `nano/` to a new GammaOS base (A.5) |
| `import-tree DIR` | Copies an applied AOSP folder (the developer's `frameworks/base/cmds/gammaos-nano`) into `nano/` under the A.2 rule, with modes, skipping paths `git check-ignore` matches; the path for changes made in the AOSP tree |
| `install-driver` | Writes the merge drivers' lines into `.git/config` (`nanopatch` for the patch, `nanoblock` for `block.sha256`; git does not version them); `status` warns when they are missing |
| `merge-block` | What the `nanoblock` driver runs for `block.sha256` (A.5) |
| `export-assets OUT` | Installs Nano's runtime assets the way `Android.bp`'s `prebuilt_etc` modules do (`srcs` globs to `sub_dir`), from the composed tree, for the Linux image and the host E2E root |
| `status` | Base commit, cache state, last sync, whether `regen --check` passes |

Modules: `nano_tree/patchfile.py` (split a patch into sections, find the Nano block, check it is contiguous and sorted, splice), `gitcache.py` (the cache, alternates and temporary indexes, with the pinned environment of A.3), `compose.py`, `regen.py`, `importer.py`, `androidbp.py` (a small Blueprint reader: modules, filegroups, string lists, `srcs`, `local_include_dirs`, `sub_dir`, comments; used by `export-assets`, the source-list test and the Android-surface test), `upstream.json`, `block.sha256`. `tools/android-launcher/fetch_nano.sh` becomes a wrapper (`nano_tree.py fetch` then `compose`), so the launcher plan's flow keeps working; `ndk_probe.py` is unchanged.

### A.5 Importing `nano-144`'s changes

`nano_tree.py import-patch nano-144`:

1. Old block: the Nano block of `git show $(git merge-base linux REF):patches/thimsy/0001-thimsy.patch`. New block: the same at `REF`.
2. Each side's base is read from that side's commit: `upstream.json` where it exists, else the `BASE=` line of `tools/android-launcher/fetch_nano.sh` (how `nano-144` records it today). In the cache, with temporary indexes: tree O = old base plus old block, tree T = new base plus new block (`git apply --cached`), tree M = `compose` of `nano/`. When the two bases differ, the command stops and points to `import-base` first.
3. Per path: unchanged between O and T, skip; changed only in T, take T's; changed in both, `git merge-file -p M O T` (per file, because git 2.37 has no `merge-tree --write-tree`); added or deleted on one side, follow that side; both sides different, a conflict. Binary files conflict whenever both sides changed them.
4. Conflicts are left in `nano/` with markers and listed; exit 1. Otherwise `regen` runs.

**Merging the branches.** A plain `git merge nano-144` can merge the patch with no conflict when the two sides changed different sections (one section per file, sorted by path), and `nano/` would then lack `nano-144`'s changes. So nothing is left to a hand procedure: `.gitattributes` gets `patches/thimsy/0001-thimsy.patch merge=nanopatch -diff linguist-generated`, and `nano_tree.py install-driver` registers the driver (`merge.nanopatch.driver = <python> tools/nano-tree/nano_tree.py merge-patch %O %A %B %P`, with the interpreter that ran `install-driver`, since Windows has no `python3`). Git runs it on every merge that touches the patch, conflict or not. It takes `nano-144`'s prefix and suffix (the fork never edits the patch outside the Nano block; test A.6 holds it to that), runs `import-patch` against the incoming side, then `regen`, and leaves the result for the merge commit; a conflict in `nano/` fails the merge with the list. `-diff` keeps reviews on `nano/`, not on the generated patch. If the driver is not installed, `regen --check` and the cache-free consistency test (A.6) fail the merge commit, and `regen` itself refuses to overwrite the merged block (A.4), so `nano-144`'s work can't be dropped by "fixing" the check. Merging `linux` back into `nano-144` later is the same in reverse, after which `nano-144` edits `nano/` too and the AOSP developer flow becomes: edit in the AOSP tree, `import-tree`, `regen`, commit. **Merging two fork branches** (a step branch into `linux`, where both sides have `nano/` and both regenerated the patch) is different: git merges `nano/` itself, so the `nanopatch` driver does not import anything. It identifies HEAD, the incoming commit (`GITHEAD_<sha>`, `MERGE_HEAD`, or the branch whose patch is the incoming blob) and their single merge base, checks them against the driver's three versions, merges the three `nano/` trees from git's objects (not the working tree, since merge-ort runs drivers before it writes it), merges the patch's framework sections as text, and regenerates the block; `block.sha256` has its own driver (`merge=nanoblock`, `nano_tree.py merge-block`) that computes the same result (kept for a day under `.git/nanotree-merge/`, since git may run the two drivers in either order). If `nano/` or `upstream.json` conflicts, the sides are on different GammaOS bases, or the commits can't be told for certain (a rebase or cherry-pick), both drivers keep this side's files and exit 1; after resolving `nano/`, `regen --force` finishes it (gfix).

**Moving the GammaOS base.** Upstream is 408 commits past `0bc531e2d` (`BACKPORTS.md`), so `nano-144` may move its base. `nano_tree.py import-base NEW`: fetch NEW's Nano folder into the cache; three-way merge upstream's changes between the old and the new base into `nano/` per file (`git merge-file`, as in step 3), for every path outside the asset globs; for the asset globs, only `nano/`'s overlay files that upstream also changed are merged, the rest follow the new base; update `upstream.json`; `regen`. A merge of a `nano-144` that moved its base runs `import-base` first, then `import-patch`.

### A.6 Consistency and round-trip tests

**Where the tests live.** `tests/` is what `scripts/build.sh` runs in `quick` mode before every Android release build, so fork-only checks must not sit there, or a Linux-side test could block the Cube's build after the merge back. Only the patch-consistency tests go in `tests/` (they skip where there is no `nano/`). Everything else of A, B, C and H goes in `tools/nano-tree/tests/`, which `tests/run.sh quick` does not discover; `tests/run.sh nano` runs it.

| Test | Checks | Where, how long |
|---|---|---|
| `tools/nano-tree/tests/test_nano_tree.py` | patch splitting, splicing, the contiguity check, `androidbp.py` on a fixture `Android.bp`, the A.2 rule on a tiny synthetic tree, `fork_only` handling, mode handling, binary sections copied verbatim, the three-way rules of A.5 and `import-base` on synthetic old, new and ours trees (made with `git init` in a temporary directory), the merge driver on a scratch repository where the two sides edit different files | Windows and WSL, seconds |
| `tests/unit/test_nano_patch_consistent.py` | **cache-free**: for each section of the Nano block, the post-image id in `index ..<id>` prefix-matches `git hash-object --no-filters nano/<path>`; every added or modified path exists in `nano/` and every `nano/` path outside `fork_only` has a section or equals the base (the base side checked only with the cache); `block.sha256` matches; the prefix and suffix equal those of the patch at `git merge-base linux nano-144` (`test_outside_block_unchanged`: the fork has not edited framework parts) | seconds; skips where `nano/` does not exist (`nano-144` before the merge) |
| `tests/e2e/test_nano_patch.py` `test_regen_matches` | `regen --check` passes: the committed patch is exactly what `nano/` produces | needs `work/nano-base/` (skips with a message otherwise); about 10 to 30 s *(guess)* |
| `test_patch_applies_to_base` | the committed Nano block applies to the base with `git apply --cached --check`, and the resulting tree equals `compose` minus `fork_only` | as above |
| `test_import_round_trip` | a synthetic `nano-144` change (one hunk edited, one file added, one deleted, made in a scratch clone) imported into a scratch copy of `nano/` gives back exactly the changed block | as above, under a minute |
| `tools/nano-tree/tests/test_nano_files.py` | the set of paths `git ls-files nano/` lists equals the non-asset paths of the patch's Nano block plus the base's untouched non-asset files plus `fork_only` files; for every block path, `git check-ignore --no-index nano/<path>` prints nothing; both credential names are ignored under `nano/`; the executable bits in the index equal the patch's `new file mode 100755` lines | seconds (the base part with the cache) |

`import-tree` and `import-patch` run the `test_nano_files.py` set check themselves after staging and fail with the missing list.

The existing `tests/unit/test_nano_units.py` and `test_screen_arrangement.py` read Nano's sources from the AOSP tree or from the patch; they gain `nano/` as the first place to look.

---

## B. The platform seam

### B.1 Four layers, in the order they land

| Layer | Mechanism | Android build | Linux and host builds | Edits to Nano's existing files |
|---|---|---|---|---|
| 1. Header shims | `nano/compat/include/`: Android's header names, Thimsy's implementations | not on the include path; the real headers | first on the include path | none |
| 2. Inert stubs | `nano/compat/stubs/include/`: types and functions that let firmware-only code compile and do nothing (binder, SurfaceFlinger client, sensors, AHardwareBuffer) | not on the include path | on `nano_core`'s include path only; a ratchet test makes the stubbed surface only shrink | none |
| 3. Link-time and filesystem substitution | the Linux and host libraries define the same free functions as a firmware file they leave out (the DRM buffer back end on Linux, all of `NanoMenuDrm.h`'s API on the host); `system`, `popen`, `pclose` and the `exec` family wrapped at link time (and `ioctl` on the host, for fake input devices); Android paths mapped by links on the Linux image | unchanged | substituted | none, except the one mechanical split of B.5 |
| 4. Interfaces | `nano/NanoPlatform.h`: abstract services, one implementation per platform | `platform/firmware/`: today's code, moved | `platform/linux/`, `platform/host/` | call sites move, area by area |

Layers 1 to 3 give a Linux binary with **no edits to Nano's existing files**, so the Android build is unchanged and `nano-144` merges stay one-sided. Layer 4 is the comprehensive refactor; it edits shared files under section H's rules, one area per commit.

### B.2 Layer 1: `nano/compat/` (shims)

Written fresh with Android's signatures (no AOSP code copied; Apache-2.0 headers like the rest of Nano). Behaviour mirrors Android where Nano depends on it.

| Header | Provides | Backed by |
|---|---|---|
| `cutils/properties.h` | `property_get`, `property_set`, `property_get_bool`, `property_get_int32`, `property_get_int64`, `PROPERTY_VALUE_MAX` (92), `PROPERTY_KEY_MAX` | the config store (B.3). Android's limits kept: `property_set` refuses a value over 91 bytes for keys not starting `ro.` and returns -1, because Nano splits long lists into continuation keys for that limit (`nanoPkgListRead`) |
| `android-base/properties.h` | `android::base::GetProperty`, `SetProperty`, `GetBoolProperty`, `GetIntProperty<T>`, `GetUintProperty<T>` | the same store |
| `sys/system_properties.h` | `prop_info` (opaque), `__system_property_find`, `__system_property_serial`, `__system_property_wait`, `PROP_VALUE_MAX` | the store's per-key change counter and condition variable |
| `log/log.h`, `utils/Log.h`, `android/log.h` | `ALOGV/D/I/W/E`, `SLOGV/D/I/W/E`, `_IF` forms, `LOG_ALWAYS_FATAL(_IF)`, `LOG_FATAL_IF`, `__android_log_print`, `__android_log_write`, `__android_log_buf_print`, priorities; `ALOGV` compiled out unless `LOG_NDEBUG` is 0, as on Android | `nanocompat_log()`: one line to stderr (`<ms> <prio> <tag>: msg`); on the Linux image also a ring file `/run/thimsy/log/nano.log` (256 KiB, two halves) |
| `utils/RefBase.h`, `utils/StrongPointer.h` | `RefBase` (`incStrong`, `decStrong`, `getStrongCount`, `onFirstRef`, `onLastStrongRef`, `createWeak`), `sp<T>`, `wp<T>` with `promote()` | atomics; `onFirstRef` on the first strong reference, as Android does |
| `utils/Thread.h`, `system/thread_defs.h` | `android::Thread` (`run(name, priority)`, `requestExit`, `requestExitAndWait`, `join`, `isRunning`, `exitPending`, `getTid`, virtual `readyToRun` and `threadLoop`), `PRIORITY_DISPLAY`, `ANDROID_PRIORITY_NORMAL`, `ANDROID_PRIORITY_BACKGROUND` and the other constants | `pthread`; `run` holds a strong reference for the thread's life and loops `threadLoop()` while it returns true and no exit is pending, as Android's does. Priority is applied with `setpriority`; `PRIORITY_DISPLAY` maps to nice -4 on Linux *(guess at the value Nano wants without SCHED_FIFO)* |
| `utils/Errors.h` | `status_t`, `OK`, `NO_ERROR`, `UNKNOWN_ERROR` and the rest Nano uses | constants |
| `utils/SystemClock.h`, `utils/Timers.h` | `uptimeMillis`, `elapsedRealtime`, `elapsedRealtimeNano`, `systemTime(clock)`, `nsecs_t`, `ns2ms`, `ms2ns`, `seconds_to_nanoseconds` | `clock_gettime`. No fake clock: Nano also reads `steady_clock` and `clock_gettime` directly about 120 times and paces itself with `usleep`, and libstdc++'s `steady_clock::now` can't be wrapped, so a fake clock would control only part of Nano's time. Host scenarios run in real time (D.3) |
| `android-base/file.h`, `unique_fd.h`, and any other libbase header G4 finds | `ReadFileToString`, `WriteStringToFile`, `unique_fd` | plain POSIX |
| `private/android_filesystem_config.h` | the `AID_*` constants Nano uses (`AID_SYSTEM` 1000, `AID_MEDIA_RW` 1023, `AID_SDCARD_RW` 1015, `AID_EVERYBODY` 9997 and the rest G4 finds) | constants; on Linux everything runs as root, so ownership calls become harmless |
| `android/imagedecoder.h`, `android/bitmap.h` | `AImageDecoder_createFromBuffer`, `_createFromFd`, `_getHeaderInfo`, `_setTargetSize`, `_setAndroidBitmapFormat`, `_decodeImage`, `_delete`, `AImageDecoderHeaderInfo_*`, `ANDROID_BITMAP_FORMAT_RGBA_8888`: the subset `NanoThemeEngine.cpp` calls | a **real** decoder, not a stub, because the ES-DE theme (decision 9's v1 UI on Linux) decodes every image through it: libpng, libjpeg-turbo, libwebp and giflib, all Buildroot packages and Ubuntu `-dev` packages; HEIF and AVIF return `ANDROID_IMAGE_DECODER_UNSUPPORTED_FORMAT`, and the theme falls back as it does for a broken file |
| `nanocompat/bionic.h` (force-included with `-include` in Linux and host builds, so no Nano file is edited) | `M_PURGE_ALL`, `M_CACHE_COUNT_MAX` and the other Bionic `mallopt` parameters, as values glibc's `mallopt` does not know (it returns 0 and does nothing; `M_PURGE_ALL` could later call `malloc_trim(0)`); inline `strlcpy` and `strlcat` when `__GLIBC__` is older than 2.38; `getprogname()` as `program_invocation_short_name` | glibc 2.31 in WSL lacks all of them; a ratchet test lists the Bionic-only identifiers it finds in shared files and fails on a new one |

`String8` and `String16` (7 uses in 4 files) get shims only if a file `nano_core` compiles needs them (G4 decides).

**Control keys.** Some property writes are messages to the framework, not settings. A launch is a handshake of about ten keys followed by Nano exiting. Since G7-launch-a every site builds its writes as a `LaunchTuple`, in the order it always wrote them, and hands it to `commitLaunch()` (`NanoLaunchCommit.h`; 69 calls in nine files), which passes it to `Launcher::commit` (B.6). For example, threadLoop's DrasticSf launch writes `launch_app`, `launched_pkg`, `launch_intent`, the ROM path (`setLaunchRomPath`), `launch_core`, `return_recent`, `service.bootanim.nano_retroarch` and `drop_input`, then sets `mExitRequested`; its DraStic launch ends with `handoff_fired`, `pending_exit` and `do_launch`. Some of those keys are read back as state elsewhere (`NanoControlCenter.cpp:1105` and `1838`, `NanoButtonMap.cpp:1360`; line numbers at wave 5). So the store never lets a handler swallow a write: **the store writes first, and handlers observe** (`addObserver`, B.3). The trigger table is derived, not hand-listed: `tools/nano-tree/nanotree/controlkeys.py` reads every `on ... property:` trigger of `nano/*.rc`, the rc triggers elsewhere in the patch that name one of Nano's keys, the framework listeners (Nano's `props.txt` keys found as string literals in the patch's other sections), and `ctl.*`, `sys.powerctl` and `service.bootanim.*`; `tools/nano-tree/control-keys.json` gives each one a disposition on Linux: `routed` (to what), `store-only` (state read back by Nano; nothing else happens), `refused` (logged once) or `not-reached` (nothing on Linux writes the key) (G9-8). A routed row names its route kind: `session:<verb>` (a `<seq>:<verb>` request in `sys.thimsy.session.request`, seq one past the key's current number, G9-14), `session-key` (`thimsy-session` watches the key itself), `launcher` (the launch handshake), `hook:<action>` (the family hook, run in the background), `first-frame` and `rc-reset` (only the rc's reset of the key, as for `dowake=1`). A row may add `value` (the triggering value), `reset` (the rc's setprop, written before the action) or `source: manual` (a listener the patch does not show, such as `do_launch`'s). `tools/nano-tree/tests/test_control_keys.py` fails when a trigger has no row or a row is stale, and checks the C++ table `kControlRows` in `nano/platform/linux/ControlKeys.cpp` (kept by hand, G9-9) against the JSON, the session verbs against `linux/docs/session.md`, and that every `session-key` is one `thimsy-session` reads. The rows: the launch handshake is `launcher`, one request debounced over 3 s on `do_launch=1` or `service.bootanim.nano_retroarch=1` (G9-12). It is still the Linux launch protocol after G7-launch: the Linux `Launcher::commit` writes the keys as the firmware does, and the trigger's write reaches the launcher through this table (`controlkeys::setLaunchHandler`, set after `install()`). Only a trigger written inside Nano's own `commit()` is a launch; one from another process is logged and refused (G7-launch-b-14). The launcher queues the request and sets `sys.gammaos.nano.app_launched=1`, so threadLoop's exit path does not wait for it. Nano then exits, and `main_linux.cpp` starts the game (B.7, REV4-1). `dosleep=1` is `session:sleep` and `park_restart=1` `session:restart-nano` (each reset to 0, as the rc does), `dowake=1` only the reset. `service.bootanim.nano_action=reboot` and `=shutdown` are `session:reboot` and `session:poweroff` (a row for an exact value overrides the key's catch-all row). Nano itself no longer writes them on Linux: since G7-power the last step of `NanoMenu::prepareShutdown` (the menu's Reboot and Power Off, the PS3 quick actions, the 7 s power hold, the DRM-sleep timeout) is `Power::powerAction`. The firmware's writes the `nano_action` key as before; the Linux one sends the `reboot` or `poweroff` session request itself (G7-power-9). The two rows are kept for a write from elsewhere, and Nano never writes `sys.powerctl` itself; `sys.powerctl` and `scrape_run` are `session-key` (`scrape_run` starts `thimsy-nano-scrape`, C.2); `fixedperf=1`/`0` are `hook:fixedperf-on`/`-off`; `drm_active=1` (Nano's splash on the panel) is `first-frame`: `sys.thimsy.nano.first_frame=1` and `sys.boot_completed=1`, once per process (G9-11; the session never writes `sys.boot_completed`); settings are one `persist.*` row, `store-only`; `ctl.*`, `ota.prestage`, `drastic_nano.start`, the Bluetooth pairing keys, the other `nano_action` values (`android`, `recovery`, `safemode`) and the overlay triggers are refused while the Quick Menu and the DS player are out of scope. On the host the fakes record them, and the E2E test asserts on them. The launcher plan's app needs the same table (`android_launcher_plan.md`, Phase 0 results).

### B.3 The config store: `libthimsy/`

A new top-level component, **MIT** (decision I5), C++17, no dependencies beyond libc: used by Nano's compat layer on Linux and host, by `thimsy-session`, `thimsy-padd` and the `thimsy-prop` command. MIT lets Nano (Apache-2.0) and the MIT Linux tools link it without a licence question; it is all Thimsy-written.

```
libthimsy/
  LICENSE (MIT)  Makefile (host: make check)  CMakeLists.txt
  include/thimsy/config.h      ConfigStore
  include/thimsy/atomic_file.h write-temp, fsync, rename, fsync dir
  include/thimsy/watch.h       inotify wrapper with a test seam
  include/thimsy/uevent.h      NETLINK_KOBJECT_UEVENT message parser (pure)
  include/thimsy/json.h        a small JSON reader and writer for device.json and volumes.json
  src/*.cpp
  tools/thimsy-prop.cpp        thimsy-prop get|set|list|watch|boot
  testing/gtest/gtest.h        the GoogleTest stand-in (section D.1), MIT
  tests/*_test.cpp
```

The store's contract (the plan, 5.2: "the same key names in a config store"):

| Namespace | Where | Writes |
|---|---|---|
| `persist.*` | one file, `/storage/.config/thimsy/persist.props`, mode 0600: `key=value` lines, sorted, values escaped (`\n`, `\\`) | read-modify-write under `flock` on `persist.lock`, temp file (created 0600), `fsync`, `rename`, `fsync` of the directory. Rare: Nano is the main writer |
| `ro.*` | `/run/thimsy/props/ro/<key>`, written once at boot by `thimsy-prop boot` from `/usr/share/thimsy/device.json`, `thimsy/install.json` and the build's `/usr/share/thimsy/build.props` | refused after boot, as on Android |
| everything else (`sys.*`, `debug.*`, `vendor.*`) | `/run/thimsy/props/<key>`, one file per key (tmpfs) | write temp and rename: atomic per key, no lock, any number of writers |

```cpp
namespace thimsy {
class ConfigStore {
 public:
  struct Roots { std::string persistFile, runDir; };          // tests pass a temp dir
  static std::unique_ptr<ConfigStore> open(const Roots&);
  std::string get(std::string_view key, std::string_view def = {}) const;   // any thread
  bool set(std::string_view key, std::string_view value);    // any thread; Android's limits
  uint32_t serial(std::string_view key) const;               // bumps on every change seen
  bool waitChange(std::string_view key, uint32_t oldSerial, int timeoutMs);
  using Observer = std::function<void(std::string_view key, std::string_view value)>;
  void addObserver(std::string_view keyOrPrefix, Observer);  // called after the value is stored; never vetoes
  void startWatching();     // one inotify thread for the run dir and persist file
  void stopWatching();
};
bool isSecretKey(std::string_view key);   // *pw*, *pass*, *token*, *secret*, *ssdev*: redacted when printed
}  // namespace thimsy
```

Threading: `get` and `set` take a `std::shared_mutex`; the watcher thread updates the cache and serials when another process writes; `waitChange` waits on a condition variable; observers run after the store's lock is released. The process-wide store for Nano's shims is created by the platform before any Nano thread starts and lives to exit.

**Secrets.** On Android, `persist.*` values such as `persist.gammaos.scraper.ssdevid` and `.ssdevpw` live on encrypted userdata; on Linux they sit in a plain file on a removable card. `persist.props` is 0600, `thimsy-prop list` and the control-key log print `<redacted>` for `isSecretKey` keys, and the plan says that a removed card exposes these values (plan 5.2).

### B.4 Layer 2: inert stubs

`nano/compat/stubs/include/` declares what firmware-only code needs to compile inside shared files. The list is not written by hand: `tools/nano-tree/include_census.py` lists every `#include` reachable from each `[core]` file (quoted and angle includes, through `local_include_dirs`), classifies each header as standard, third-party library, shim (layer 1) or Android-only, and writes `tools/nano-tree/include-census.json`; `test_nano_stubs.py` fails when an Android-only header reached from `[core]` has neither a shim nor a stub. Known from the review's census: `binder/IBinder.h` (`IBinder` with a nested `DeathRecipient` and `binderDied(const wp<IBinder>&)`), `binder/ProcessState.h`, `binder/IServiceManager.h`, `binder/IPCThreadState.h` (`stopProcess` does nothing: there is no binder thread pool), `android/binder_manager.h`, `gui/*.h` (`SurfaceComposerClient` and its `Transaction`, `SurfaceControl`, `Surface`: the methods Nano calls, each doing nothing and returning `*this`, null or an error), `ui/*.h` value types, `utils/String16.h` (`readyToRun`'s `sfName`), `android/sensor.h` and `android/looper.h` (opaque types; `ASensorManager_getInstance` returns null), `android/hardware_buffer.h` and `android/native_window.h` (opaque; allocation fails), `android/performance_hint.h` (no session), the AIDL and HIDL light and health headers (`ILight`, `IHealth`: no service found), `aaudio/AAudio.h` (opening a stream fails) and `media/NdkMedia*.h`, including `NdkMediaDataSource.h` (every create returns null), the last two behind `Cap::MediaPlayers` off. Every stubbed function logs once at first call on a debug build. The ratchet test (`tools/nano-tree/tests/test_nano_stubs.py`) counts declarations under `stubs/` and fails if the count grows past `tools/nano-tree/stubs-budget.txt`; layer 4 lowers the budget as it moves code out.

Stubs, not edits, for two reasons: `NanoMenu` derives from `Thread` and `IBinder::DeathRecipient` and holds `sp<>` members of SurfaceFlinger types, so its header can't be made portable without touching the most-edited file in `nano-144`; and the SurfaceFlinger paths in `NanoMenu.cpp`, `NanoMenuRender.cpp` and `NanoMenuPS3Clock.cpp` are dead on Linux, where DRM is the only display.

### B.5 Layer 3: substitution

- **Display.** A full substitute for `NanoMenuDrm.cpp` (3,577 lines, also compiled into `drastic-nano`) would have to define about 45 extern globals and emulate the AHardwareBuffer ring that shared render loops drive directly (`NanoMenuRender.cpp:7430-7475`, `NanoMenu.cpp:2560-2747`, `NanoMenuInput.cpp:1110-1114`), and it would drift silently from every `nano-144` change to the pair. So the Linux build **compiles `NanoMenuDrm.cpp`'s KMS and flip logic too** and substitutes only its buffer back end: `drmAllocAhbTarget` (line 1100) and the AHardwareBuffer and gralloc-metadata calls (25 `AHardwareBuffer` references). Step G9a moves that back end, verbatim, into `NanoMenuDrmBuffers.cpp` behind a small header (`NanoMenuDrmBuffers.h`: allocate a render target, import it as an EGL image and a DRM framebuffer, free it), and adds the new file to both `gammaos-nano`'s `srcs` and `drastic_nano_shared_srcs`, which are inside the Nano block. It is the one edit to an existing Nano file before P4: a mechanical move reviewed by H.3's rule, an Android-surface change with its `android-surface:` line, and a candidate to cherry-pick to `nano-144` as soon as the AOSP job is done, so later `nano-144` edits to the DRM file merge cleanly. `nano/platform/linux/display/DrmBuffersGbm.cpp` implements the same header with GBM buffer objects and `EGL_EXT_image_dma_buf_import`. The rotation contract (plan 5.3) sets `sDrmRotationDeg` and `sDrmGlRotation`, so the shared code's existing GL rotation path does the rotation. Section B.8 writes down the ring contract the back end must honour. If the split proves not to be mechanical (G9a's first check), the fallback is a full substitute in `DrmLinux.cpp`, with B.8's contract and a symbol-set test that compares what `NanoMenuDrm.h` declares with what the substitute defines (`nm` of the object when a compiler exists, a header parse otherwise).
- **Display on the host.** The host has no DRM device, so `nano/platform/host/DrmHost.cpp` defines all of `NanoMenuDrm.h`'s API (the same symbol-set test covers it): `drmEarlySplash` sets `sDrmActive` and one `DrmDisplay` of the scenario's size, the ring targets are plain GL texture ids from the GL stub, and every flip (`drmFlipAll`, `drmFlipRingSlot`) is a present: it counts and passes through the frame barrier of D.3.
- **Shell and processes.** The Linux and host executables link with `-Wl,--wrap=system,--wrap=popen,--wrap=pclose,--wrap=execv,--wrap=execvp,--wrap=execve` (no `posix_spawn` in Nano today; a ratchet adds it to the list if it appears). `platform/linux/ShellGate.cpp` defines the wrappers. Since OA-4's migration step 1 (`docs/linux/ARCHITECTURE-AUDIT.md`, step W6-safety) the gate **refuses by default**: no Android command runs on Linux, even when a program of the same name exists. A command or program runs only when every simple command in it matches an exact shape on the gate's short, temporary allow-list (`kAllow`: Linux-meaningful shapes with Linux-meaningful arguments, each naming its call site and marked `TODO(OA-4 step 4): remove`; today one entry, the setup wizard's `TZ=<zone> date -d @<epoch> +%z` at `NanoMenuSetupWizard.cpp:318`), with no redirection but to `/dev/null` or `2>&1` and only `;`, `&&`, `||` or newlines between commands. The routes that only touch Thimsy's own property store run in the gate and are logged once per shape as "routed (OA-4 step 5)": `setprop`, `getprop`, and `reboot` and `svc power reboot|shutdown` through `sys.powerctl`, which `thimsy-session` acts on; a route behind a wrapper or an assignment, piped or mixed with another command is refused. Everything else is refused with exit status 127 and no output (an `exec*` refusal returns -1 with `ENOENT` instead of exiting, G9-5: each of Nano's exec sites calls `_exit(127)` right after, and a caller that did not fork keeps running): Android tools (`am`, `cmd`, `settings`, `svc`, `pm`, `dumpsys`, `appops`, `input`, `screencap`, `ime`, `locksettings`, `gammaos-net`, `nsenter`, ...), `/system/bin/<tool>` (no longer rewritten to the Linux tool), `/vendor/...`, `restorecon` and `chcon` (no longer successful no-ops: there is no SELinux, every caller ignores or only logs the result, and reporting success was a lie), wrappers (`timeout` with any of its options, `nice`, `env`, `su`, `time`, `stdbuf`, `chrt`, `taskset`, `ionice`), shell keywords, and every Linux program not on the list, `curl` included (the image has no `curl` program; migration step 4's `Http` uses the libcurl Nano links). Every refusal is counted (`refusalCount()`, exec refusals in forked children included, through a shared anonymous mapping made before `main`) and logged once per command shape (`refusedShapes()`: the command word and its subcommand, past wrappers, such as `am force-stop`, `cmd display` or `/system/bin/chown`; at most 128 shapes, then a full table is logged once). Only Nano's own process logs: a forked child, whose stderr is often `/dev/null` (`runArgv`) or a side file (`esdeRunProc`), records the shape, and Nano logs it on its next `system()`, `popen()`, `pclose()`, `refusalCount()` or `refusedShapes()` call. The mapping's lock holds its holder's pid, so a child killed while holding it (`runArgv`'s timeout) does not wedge it. The allow-list's pattern classes (`{zone}`, `{digits}`) accept no character `sh` expands, since a pass hands the caller's own string to `sh`; the lexer treats a carriage return as a word character, as `sh` does. The platform's own process starts never go through the gate (OA-5): `Launcher.cpp` calls `shellgate::platformExecve`, `actions/Run.cpp` its raw `execve`, `ControlKeys.cpp` `posix_spawn`. `platform/linux/tests/shellgate_test.cpp` decides one string of every command shape Nano's Linux-compiled shared code sends today (`docs/linux/audit-2026-11-26/inv_shell.md`) and fails when the set that passes, routes or is refused changes. The fork-and-exec sites (`esdeRunProc` in `NanoMenuEsdeMenu.cpp`, `runArgv` in `NanoSdMove.cpp`; `NanoOtaCheck.cpp`'s are compiled out on Linux) are listed in a ratchet in `tools/nano-tree/tests/test_shell_sites.py` (G9-21: the B.5 ratchets live in their own files, not G0's `test_nano_ratchets.py`), which also requires every process-starting function Nano calls to be in `nano/platform/linux/linker-wraps.txt`, holds the allow-list to its reviewed entries, each with its call site and its removal TODO, and caps the program starts (a ceiling shared with H.4's Linux ratchets), so a new site fails the test until it has a row. The image links `/system/bin` to a curated `/usr/libexec/thimsy/android-bin/` (empty in the alpha); a wrapper there would also need an allow-list entry. On the host the gate is **record-only**: it never executes anything, returns the scenario's scripted result, and the E2E test fails on an unexpected command (`nano/test/host/scenarios/README.md` lists the scenarios that still script Android command strings, for steps 3 and 6). `--wrap` rewrites undefined references in every object and static archive in the link (`nano_core`, `nano_compat`, `libthimsy.a`, any static third-party archive), and leaves shared libraries alone. That is intended for `nano_core`; the static third-party archives linked into `thimsy-nano` are listed in `nano/build/sources.txt`'s header (none in the alpha: libcurl, OpenSSL and the image libraries link as shared libraries) so their calls are known to pass through the gate. Linux builds don't use LTO for Nano at first *(guess that GCC LTO and `--wrap` mix badly)*.
- **Paths.** The Linux root (read-only EROFS) carries links, so Nano's path literals work unedited. `/data` as a whole links to one writable tree, `/storage/.config/thimsy/data/`, which first boot fills with `system/` (the state files, the plan's "same file names"), `local/tmp/`, `data/`, `user/0`, `misc/`, `gammaos_ota/` and a link `media/0` to `/storage/emulated/0`. `/sdcard` and `/storage/self/primary` link to `/storage/emulated/0` (the second inside the data partition, made at first boot, since `/storage` is that partition's mount). `/system/etc` links to `/usr/share/thimsy/system/etc` (where `thimsy-data` installs the allowed assets, E.3), `/system/fonts` to `/usr/share/fonts/thimsy` (where `thimsy-fonts` installs files under the Android names `NanoMenuRender.cpp`'s `fontPaths` expects), `/system/bin` to the curated directory above. `/mnt/shares` and `/vendor/bin` are not created: their users are `[firmware]` (Shares) or refused by the gate. `thimsy-session` makes `/mnt/media_rw/<id>` a link to `/storage/<id>` for each mounted drive. `tools/nano-tree/nanotree/pathroots.py` extracts every absolute-path prefix literal under a real top-level root in `nano/`'s shared files (test and compat files left out; under `/storage` only `emulated` and `self` are named, any other name is a volume) and `tools/nano-tree/path-roots.json` gives each a disposition (`link`, `same` where Linux has the path itself, `gate`, `dropped` with its `[firmware]` user, `kernel` for `/proc`, `/sys`, `/dev`, `not-reached`, or `not-a-path` for a suffix Nano appends to a folder; G9-18); `tools/nano-tree/tests/test_path_roots.py` fails on a root with no row and checks the `link` rows against `post-build.d/10-android-links.sh`, so a new root arriving from `nano-144` fails loudly. The host E2E builds the same layout in a jail (D.3).
- **Raw block devices.** On Android, `NanoScreenCal.cpp` opens `/dev/block/by-name/baseparameter` read-write and `pwrite`s it, and Nano runs as root on Linux. Rule: the Linux root never creates `/dev/block/by-name` (neither mdev rules nor post-build make it), and `thimsy-session` starts Nano in its own mount namespace in which every internal-storage block node (an `mmcblk*` whose `device/type` is `MMC`, and its partitions) has `/dev/null` bind-mounted read-only over it, so neither Nano nor a game it launches can open the eMMC; dropping `CAP_SYS_RAWIO` alone would not stop a root `open()`. `NanoScreenCal.cpp` stays `[core]` (its DRM parts are shared) but its baseparameter path finds no node; a ratchet lists the shared files that name `/dev/block` or `mmcblk` paths (one today; sysfs `/sys/dev/block` does not count, G9-19), so a new one is reviewed (`test_path_roots.py`, and `test_nano_stubs.py`'s `block-paths.txt`); `10-android-links.sh` fails the build if the root has `/dev/block`, `/vendor` or `/mnt/shares` (G9-20). This is the SD-only principle (plan 1) applied to Nano itself.

### B.6 Layer 4: `NanoPlatform.h`

Small interfaces in `nano/NanoPlatform.h`, implemented in `platform/firmware/`, `platform/linux/` and `platform/host/`. Names follow plan 4.1: `platform/firmware` is today's Android firmware code, `platform/android` is reserved for the launcher plan's app (decision I3; the task's "nano_platform_android" is `nano_platform_firmware` here).

**Declared when used, not ahead.** G5 declared only `Platform`, `createPlatform`, `install` and `get`, with no service in it. Each G7 step then declared its own interface, sized to the code it moves, in the same commit as the move (I27). The sketch below is what waves 4 and 5 declared, shortened: comments are cut and some parameters are elided. `nano/NanoPlatform.h` is the reference. The first draft's shape is in the history. Its members that no moved code used are not declared: `Display::open`, `mode`, `present`, `setPanelPower` and `hotplugPending`; `Audio`'s volume; `Volumes::list`, `onChange` and `eject`; `Power::sleep` and `setBusy`; `SystemActions`' `Action` enum, `run` and `supported`; `InputPolicy::isBuiltInPad` and `deviceMatchKeys`; and `Cap` with `Platform::has`, which wave 6 declared when it first hid rows by them (W6-caps, below). `Clock` is dropped (B.2: no fake clock).

**Who may call `get()`.** `drastic-nano`, `gammaos-nano-scrape` and the `cc_test_host` modules compile some of Nano's files and never call `install()`. Rule, held by a ratchet in `test_nano_ratchets.py`: no file in `drastic_nano_shared_srcs`, in `gammaos-nano-scrape`'s `srcs` or in any `cc_test_host` module's `srcs` (read from `Android.bp` by `androidbp.py`) may include `NanoPlatform.h` or call `nanoplat::`. Those files are frozen-API for the seam: they reach platform behaviour through layers 1 to 3 (property shims, link-time substitution), or take what they need as parameters from a caller outside the set. Platform code calls into them, never the reverse. So `drastic-nano/Android.bp` and `main.cpp`, which sit outside the Nano block, never need an edit; if one ever does, it is a declared exception to `test_outside_block_unchanged`, landed on `nano-144` first.

```cpp
// nano/NanoPlatform.h (C++17), as declared after G7-net. Namespace nanoplat; no Android or Linux
// system header included. Shortened: see the header for every comment.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace android { struct LaunchTuple; }   // NanoLaunchCommit.h (G7-launch-a)

namespace nanoplat {

class Display {                        // render thread only (G7-display)
 public:
  virtual ~Display() = default;
  // readyToRun()'s DRM splash decision: takes the panel unless this process presents through
  // SurfaceFlinger; returns the early DRM fd still held, or -1 (sDrmActive says whose the panel is)
  virtual int startScanout(int earlyDrmFd, bool overlayMode, bool (*forceSfPath)()) = 0;
  virtual bool releaseForChild() = 0;  // Linux: finish the GPU, wait for the flip, planes off, free the
                                       // ring, drop master; firmware: does nothing, false
  virtual bool reacquire() = 0;        // Linux: master back (retrying), modeset, ring rebuilt; firmware: false
};

struct LaunchRequest {                 // the handshake's keys when its trigger is written (G7-launch-b)
  enum class Kind { Core, Package };
  Kind kind = Kind::Core;
  std::string package, core, rom, system, intent;   // launch_app, launch_core, launch_rom (else
                                                    // launch_game), launch_system, launch_intent
  std::string trigger;                              // the key whose write fired the launch
};
struct ChildExit { int pid = 0; int status = 0; bool signaled = false; };

class Launcher {
 public:
  virtual ~Launcher() = default;
  virtual bool nanoExitsOnLaunch() const = 0;   // the intended contract: firmware true, Linux false
                                                // (resident Nano); nothing reads it yet
  virtual void commit(const android::LaunchTuple&) = 0;   // commitLaunch(): one site's writes, in order
  virtual bool launch(const LaunchRequest&, std::string* error) = 0;   // firmware: always false;
                                                // Linux: checks and queues (started after the menu ends)
  virtual bool childRunning() const = 0;
  virtual std::optional<ChildExit> pollChild() = 0;   // non-blocking; reported once
  virtual void requestChildQuit() = 0;                // SIGTERM to the child's process group
};

struct AudioFormat { const char* name = ""; int rate = 0, channels = 0; bool lowLatency = false; };
class AudioStream {                    // made paused; destroying it stops and closes it (G7-audio)
 public:
  virtual ~AudioStream() = default;
  virtual bool start() = 0;
  virtual void pause() = 0;
  virtual int sampleRate() const = 0;  // what the output agreed to
  virtual int channelCount() const = 0;
};
class Audio {                          // a stream factory: NanoAudio keeps several streams open
 public:
  virtual ~Audio() = default;
  static constexpr int kFillContinue = 0, kFillStop = 1;          // AAudio's callback results
  using Fill = std::function<int(int16_t* frames, int count)>;   // on the platform's audio thread
  using OnDisconnect = std::function<void()>;                     // the device went away
  virtual std::unique_ptr<AudioStream> open(const AudioFormat&, Fill, OnDisconnect) = 0;  // null: failed
};

class Volumes {                        // any thread; may touch the filesystem (G7-volumes)
 public:
  virtual ~Volumes() = default;
  virtual std::string cardRoot() = 0;                      // NanoMenu::sdCardRoot
  virtual bool isSlot(const std::string& storagePath) = 0; // sdIsSlot
  virtual bool present(const std::string& id) = 0;         // esdeStoreFollowCard
  virtual bool absent(const std::string& id) = 0;          // sdVolumeAbsent
  virtual std::vector<std::string> writableFolders(const std::string& vol, const std::string& rel) = 0;
};

class Power {                          // G7-power
 public:
  virtual ~Power() = default;
  virtual void readBattery(int& percent, bool& charging) = 0;   // pollBattery(), render thread
  virtual bool setBrightnessViaHal(int level) = 0;  // firmware: the lights HAL; Linux: false
  virtual void mirrorBrightness(int level) = 0;     // syncBrightnessToAndroid(); does not block
  virtual int savedBrightness() = 0;                // readAndroidBrightness(); -1 when none
  virtual void powerAction(const char* action) = 0; // prepareShutdown()'s last step: "reboot",
                                                    // "shutdown" (firmware also recovery, safemode, android)
};

class SystemActions {                  // input and worker threads; may block (G7-power)
 public:
  virtual ~SystemActions() = default;
  virtual void setWifiEnabled(bool on) = 0;
  virtual void forgetWifiNetwork(int savedNetId) = 0;
  virtual void setBluetoothEnabled(bool on) = 0;
  virtual void connectBluetooth(const std::string& mac) = 0;
  virtual void setTimeFormat(bool h12) = 0;
  virtual void setTimeZone(const std::string& tz) = 0;
};

struct WifiNetEntry { std::string ssid, bssid; int rssi = -127, security = 0, savedNetId = -1;
                      bool connected = false; };                 // NanoMenu::WifiNetEntry's fields
struct BtDevEntry { std::string name, address; bool bonded = false, connected = false; int cod = 0; };
class Network {                        // G7-net: methods named after their call sites; each blocks
 public:
  virtual ~Network() = default;
  enum WifiLevel { kWifiLevel_Unknown = 0, kWifiLevel_Off, kWifiLevel_Disconnected, kWifiLevel_Connected };
  enum BtLevel { kBtLevel_Unknown = 0, kBtLevel_Off, kBtLevel_On, kBtLevel_Connected };
  // NanoMenuNet.cpp's poll thread
  virtual void autoEnableWifi() = 0;
  virtual void pollState(WifiLevel&, int& wifiBars, std::string& wifiSsid, BtLevel&, int& btConnected) = 0;
  // the Wi-Fi screens (results: WifiRadio, WifiLists, WifiAddresses, WifiMacAndIp, WifiLink structs)
  virtual WifiRadio wifiListRadio() = 0;
  virtual WifiLists wifiLists() = 0;
  virtual void startWifiScan() = 0;
  virtual bool connectSavedWifi(int savedNetId) = 0;
  virtual void joinWifi(const std::string& ssid, int security, const std::string& password, bool force) = 0;
  enum class JoinCheck { Pending, Connected, WrongPassword };
  virtual JoinCheck checkWifiJoin(const std::string& ssid) = 0;
  virtual bool wifiRadioEnabled() = 0;                     // render thread, bounded
  virtual WifiAddresses wifiAddresses(bool connected) = 0; // render thread, bounded
  virtual WifiMacAndIp wifiMacAndIp() = 0;                 // render thread, bounded
  virtual WifiLink wifiLink() = 0;
  // the Bluetooth screens and wizard (btWiz*)
  virtual BtList btListState() = 0;
  virtual std::vector<BtDevEntry> discoverBt() = 0;
  virtual bool pairBt(const std::string& mac) = 0;
  virtual bool unpairBt(const std::string& mac) = 0;       // input handling, bounded
  virtual bool btRadioOn() = 0;
  virtual std::vector<BtDevEntry> bondedBt() = 0;
  virtual void setBtRadio(bool on) = 0;
  virtual std::vector<BtDevEntry> btWizScan() = 0;
  virtual bool btWizPair(const std::string& a, const std::string& p) = 0;
  virtual void btConfirmInbound(const std::string& a, int v) = 0;
  virtual bool btWizConnect(const std::string& a) = 0;
  virtual bool btWizDisconnect(const std::string& a) = 0;
  virtual bool btWizUnpair(const std::string& a) = 0;
};

class InputPolicy {                    // per device, because the firmware grabs per fd at open (G7-input)
 public:
  virtual ~InputPolicy() = default;
  enum class Opened { AtStart, Hotplugged, Swept };   // openInputDevices, IN_CREATE, the sweep
  virtual void deviceOpened(int fd, const char* path, Opened where) = 0;   // render thread
  virtual void deviceClosing(int fd) = 0;                                  // render thread
  virtual void silenceGamePad(bool on) = 0;   // any thread; Linux: padd's intercept; firmware: nothing
};

struct Platform {                      // each platform derives from it; createPlatform sets every member
  virtual ~Platform();
  std::unique_ptr<Display> display;  std::unique_ptr<Launcher> launcher;
  std::unique_ptr<Audio> audio;      std::unique_ptr<Volumes> volumes;
  std::unique_ptr<Power> power;      std::unique_ptr<SystemActions> actions;
  std::unique_ptr<InputPolicy> input; std::unique_ptr<Network> net;
};

struct PlatformArgs { int argc = 0; char** argv = nullptr; };
std::unique_ptr<Platform> createPlatform(const PlatformArgs&);  // once per platform library; null: failed
void install(std::unique_ptr<Platform>);                       // main(), before any Nano thread
Platform& get();                                                // after install(); never null
}  // namespace nanoplat
```

**Ownership and threading.** `main()` calls `createPlatform`, then `install`, before `NanoMenu` is made; the services live until exit (`get()` returns a reference, so call sites never test for null; it aborts with a message if `install()` was never called, which the rule above keeps out of the binaries that don't call it). Which platform is linked is a link-time choice (one `createPlatform` per library), not a registry. The settings store is not an interface here: Nano reaches it through the property shims (layer 1), and Linux and host back the shims with `libthimsy`'s store. Paths are not an interface yet: the filesystem mapping (B.5) covers Linux and the host jail; a `Paths` interface (state, asset, font and storage roots) is added when the launcher plan's app needs it, which can't use links.

**What each area moves.** The firmware implementation of each interface is today's code, moved verbatim into `platform/firmware/<Area>.cpp`, with the call site replaced by `nanoplat::get().<area>-><call>`. As done in waves 4 and 5 (`docs/linux/DECISIONS.md`, "Wave 4" and later); what a step left at its call site is in its decisions:

| Area | Firmware code that moved (from) | Linux implementation | Host fake |
|---|---|---|---|
| Display | `readyToRun`'s DRM splash decision in `NanoMenu.cpp` (`Display.cpp`; G7D-1). Not moved: the headless pbuffer EGL set-up, the SurfaceFlinger window surface and `NanoMenuRender.cpp`'s present (G7D-2, G7D-3: `NanoMenu.h` is frozen), and `NanoMenuDrm.cpp`, which is in `drastic_nano_shared_srcs` and is handled at layer 3 (B.5) | `platform/linux/display/`: the hand-off (`releaseForChild`, `reacquire`) and the rotation contract (`PanelOrientation.cpp`, G7D-11), on `NanoMenuDrm.cpp`'s KMS code with the GBM back end | `DrmHost.cpp` (B.5) plus `FakeDisplay`, which records what it did |
| Launch | the handshake writes. G7-launch-a, a behaviour-neutral commit, gathered them from `NanoMenu.cpp`'s `threadLoop`, `NanoMenuXmb.cpp`, `NanoMenuPS3Menu.cpp`, `NanoMenuOverlay.cpp`, `NanoControlCenter.cpp`, `NanoMenuInput.cpp`, `NanoMenuSearch.cpp` and `NanoMenuState.cpp` into `commitLaunch(const LaunchTuple&)` in a new `NanoLaunchCommit.cpp` (69 calls; every other property write stays in place between two commits). G7-launch-b moved `commitLaunch`'s writes into `Launcher::commit` (`Launcher.cpp`); the firmware's `launch()` refuses, and it never has a child. Nano's exit after a launch is unchanged on every platform | `platform/linux/Launcher.cpp`: `commit` writes the keys; the control keys' trigger queues the request (B.2). After the menu thread ends, `main_linux.cpp`'s `leaveQueued()` runs `Display::releaseForChild`, `fork`, `setsid`, signals reset, descriptors above 2 closed, `execve` (RetroArch on the matching Linux core until plan 6.4's emulator definitions exist). It writes `sys.thimsy.game.pid`, clears the handshake and exits, leaving the game to `thimsy-session` (REV4-1, REV4-3). The resident path (`pollChild`, `requestChildQuit`, `reacquire`) is kept for later | `FakeLauncher`: writes the keys, records each trigger as a `launch` event; `pollChild` reports an exit after a scripted delay |
| Audio | `NanoAudio.cpp`'s AAudio output from `NanoAudioPlayer` and `NanoSfxPlayer` (one builder, callback mode, start, pause, stop, close; `Audio.cpp`). Not moved: the player's gain, the system volume (G7-audio-2) and `NanoBootChime.cpp` | `AudioPull.cpp` (the testable pull core) and `AudioAlsa.cpp` (the ALSA `default` PCM, a plug over dmix per `/etc/asound.conf`; not compiled here: needs alsa-lib headers) | `FakeAudio`: pulls and counts frames |
| Volumes | the `/storage` and `/mnt/media_rw` scans of `NanoSdMove.cpp` (`sdCardRoot`, `sdIsSlot`, `sdVolumeAbsent`) and `NanoEsdeStore.cpp` (`esdeStoreFollowCard`'s; `Volumes.cpp`). `NanoSdMove.cpp` keeps one-line forwarders (G7V-1) | `platform/linux/Volumes.cpp`: reads `/run/thimsy/volumes.json`, written by `thimsy-session`, when its serial changed | `FakeVolumes`: a scripted list from the host config |
| Power | `NanoMenuSystem.cpp`'s battery read, lights-HAL brightness, Settings brightness mirror and read, and `prepareShutdown`'s `nano_action` write (`Power.cpp`). Not moved: `NanoControlCenter.cpp` (already sysfs) and sleep (already a session request through `dosleep`) (G7-power-3 to -6) | `platform/linux/Power.cpp`: `/sys/class/power_supply`; no HAL (`NanoBacklight.h` writes the backlight on every platform); `powerAction` is a `reboot` or `poweroff` session request (G7-power-8, -9) | `FakePower`: records |
| System actions | the Wi-Fi radio, forget, Bluetooth radio and connect, time-format and time-zone commands of `NanoMenuSettings.cpp` and `NanoMenuSetupWizard.cpp`, as six typed methods (`SystemActions.cpp`; G7-power-1). `runCmd` got external linkage (G7-power-2) | `platform/linux/actions/SystemActions.cpp`: Wi-Fi on and off through `persist.thimsy.wifi.enabled` and `D30net`, forget through the `WpaCtrl` client (`actions/WpaCtrl.cpp`, Thimsy's own MIT control-socket client, not hostap's vendored `wpa_ctrl`; G7-power-10), Bluetooth through `persist.thimsy.bluetooth.enabled`, `D35bluetooth` and `bluetoothctl`, and the time zone link (G7-power-11 to -13). Programs run through `actions/Run.cpp` (fork, `setsid`, no inherited descriptors) | `FakeActions`: records calls |
| Network | the Wi-Fi and Bluetooth queries and operations of the Settings screens (`NanoMenuSettings.cpp`) and of the HUD's poll (`NanoMenuNet.cpp`), with the parsers of their output (`Network.cpp`; G7-net). `runCmdUnlocked` and `shellQuote` got external linkage. `NanoNetBridge` stays at the call sites. Not moved: `NanoControlCenter.cpp`'s `wifi_on` read, `NanoBtStable.cpp` and the airplane commands | `platform/linux/actions/Network.cpp`: Wi-Fi through `WpaCtrl` (`STATUS`, `SIGNAL_POLL`, `LIST_NETWORKS`, `SCAN_RESULTS`, `SCAN`, `SELECT_NETWORK`, `ADD_NETWORK`/`SET_NETWORK`, `REMOVE_NETWORK`, `SAVE_CONFIG`; gateway from `/proc/net/route`, DNS from `/etc/resolv.conf`), Bluetooth through `bluetoothctl` (not run while the daemons are not meant to be up: no Bluetooth in device.json, the switch off, or no bus socket), and `setBtRadio` (the PS3 wizard) as the whole switch that `setBluetoothEnabled` is (the key, `D35bluetooth`, power on or off); every call time-bounded, the render- and input-thread ones to a few seconds | `FakeNetwork`: the host config's scripted networks and devices |
| Input policy | the home's per-device `EVIOCGRAB` silencing in `NanoMenuInput.cpp` (`openInputDevices`, `checkInputHotplug`'s `IN_CREATE`, `IN_DELETE` and sweep; `InputPolicy.cpp`; G7-input-1). Not moved: `NanoMenu.cpp`'s exit-time grab of every fd and its destructor's release; not `NanoDeviceKeys.cpp`, which is in `drastic_nano_shared_srcs`: it keeps reading `ro.product.*` through the property shim, and on Linux `thimsy-prop boot` writes those match keys from `device.json` (layer 1) | `platform/linux/InputPolicy.cpp`: never grabs padd's pad; `silenceGamePad` connects to padd's tap, then sets `sys.thimsy.padd.intercept` (G7-input-4 to -6). No site calls it yet | `FakeInput`: grabs as the firmware does and records |

Evdev reading stays in shared code: the kernel interface is the same on both platforms (plan 5.1), and on Linux Nano reads padd's single pad. Plan 4.3's "input source" interface is therefore not cut. On the host, input reaches Nano without an input seam: the jail's `/dev/input/eventN` are FIFOs, and the host link wraps `ioctl` (`platform/host/FakeEvdev.cpp`) to answer `EVIOCGNAME`, `EVIOCGID`, `EVIOCGBIT`, `EVIOCGABS` and `EVIOCGRAB` for those descriptors from the scenario's device description (D.3); every other `ioctl` passes to `__real_ioctl`.

**Settings rows and `Cap`.** Hiding a row by a missing `Cap` needs edits to `NanoMenuSettingsTree.cpp` and the XMB menu builders that no area owns, so it was its own step. Planned as G7-caps after the alpha, it landed as W6-caps (OA-4 migration step 3, `docs/linux/ARCHITECTURE-AUDIT.md`). `NanoPlatform.h` declares `enum class Cap` and `Platform::has(Cap)`, and the settings dispatcher `nanoplat::Settings` (`supports`, `read`, `write`). `readSettingValue` and `writeSettingValue` are thin forwarders to it, and their Android code and post-write hooks moved to `platform/firmware/Settings.cpp`. The firmware supports every setting and has every `Cap`, so Android is unchanged. Linux (`platform/linux/Settings.cpp`) answers from a keyed table that fails closed: Nano's own preferences, the read-only device name (`ro.product.model`), and the system settings it implements itself (`font_scale`, `screen_off_timeout`); every other key is unsupported, and a write to it does nothing and returns false. Linux claims no `Cap` yet (`platform/linux/Capabilities.cpp`). Rows Linux hides: in the legacy settings tree, every row bound to an unsupported setting, the Reset Wi-Fi & Bluetooth, Factory Reset and USB mode actions, and a category left empty; in the XMB, data rows, quick-menu leaves, gamepad leaves and mouse leaves bound to an unsupported setting, and the Power submenu's Recovery, Safe Mode and Boot Android. An XMB submenu whose rows all hide is still listed (it opens empty); that is a follow-up. The host fake (`platform/host/FakeSettings.cpp`, `FakeCapabilities.cpp`) supports and has everything unless the host config's `settings` and `caps` say otherwise.

### B.7 The order, and which files change when

| Pass | Lands | Existing Nano files changed | Android binary |
|---|---|---|---|
| P0 | `nano/` as files, `tools/nano-tree/` (G0) | none | identical (same patch bytes) |
| P1 | `compat/` shims, `libthimsy/`, host build of the 24 tested files (G1) | none | identical: new files only, none listed in `Android.bp` |
| P2 | `compat/stubs/`, every `nano_core` file written for the host, the Linux display back end and shell substitution, `platform/linux/` skeleton, CMake (G4, G6, G9) | one: G9a's mechanical split of `NanoMenuDrm.cpp`'s buffer back end into `NanoMenuDrmBuffers.cpp`, added to `gammaos-nano` and `drastic_nano_shared_srcs` (B.5) | same code, moved; an Android-surface change under H's rules |
| P3 | `NanoPlatform.h` (no services yet), `platform/host/`, the headless E2E (G5) | none: `main_linux.cpp` links `nano_platform_linux` and `main_headless.cpp` links `nano_platform_host`, each with its own `createPlatform`; the firmware `createPlatform` does not exist yet | identical |
| P4 | the interfaces, one area per commit (G7), **G7-main first** | G7-main: `main.cpp` calls `install(createPlatform())` as its first statement, its boot steps move into `NanoBootSequence.cpp`, and `Android.bp` adds `NanoBootSequence.cpp`, `NanoPlatform.cpp` and `platform/firmware/Platform.cpp` (an empty firmware platform) to `gammaos-nano`'s `srcs`. Then per area, the files in B.6's table, each commit adding its own `platform/firmware/<Area>.cpp` to `srcs` (and G7-launch-a `NanoLaunchCommit.cpp`). Done in waves 4 and 5 for Display, Launch, Audio, Volumes, Power and system actions, Input policy and Network; none of it compiled with the Android toolchain yet (H.7) | changes; behaviour the same (section H). Every P4 commit updates the Android surface with an `android-surface:` line |

So the first refactor pass (P0 to P3) edits one existing Nano file, by a move. Edits start at P4, after the Linux alpha can already be built on layers 1 to 3. G7-main moved `main.cpp`'s portable boot steps into a shared `nanoBootSequence(void (*platformSteps)())` (`NanoBootSequence.cpp`, with `main.cpp`'s `LOG_TAG`): `setpriority(ANDROID_PRIORITY_DISPLAY)`, the start log line, `sys.gammaos.nano.drm_active=0`, then the callback, then `new NanoMenu` and `run("GammaOSNano", PRIORITY_DISPLAY)`. `main.cpp` passes `firmwareBootSteps()` (DraStic's patch, preload, init and QR watcher; binder's thread pool), so the firmware's order and thread are exact (G7-main-1). `main_linux.cpp` calls it with no callback rather than copying the steps.

`nano/platform/linux/main_linux.cpp` (G9-17, G7-main, REV4-1) first sets up the platform, before any thread: `nanocompat::setLogFile("/run/thimsy/log/nano.log")`, `nanocompat::init` on `/storage/.config/thimsy/persist.props` and `/run/thimsy/props` with watching on (a failure is logged and Nano runs on default settings), then `nanolinux::controlkeys::install()` (the shell gate needs no set-up: it is linked in with `--wrap`). Then `createPlatform()` (`LinuxPlatform.cpp`; null: exit 2), `install()`, and `controlkeys::setLaunchHandler(launchFromHandshake)`, so a trigger written before the launcher exists is only logged (G7-launch-b-15). It defines `main.cpp`'s `gEarlyDrmFd` as -1, as `main_headless.cpp` does (left to the `[dropped-linux]` stub generator it would be 0, stdin). Against `main.cpp`'s path:

| `main.cpp` step | `main_linux.cpp` |
|---|---|
| `install(createPlatform())`, the first statement | after the set-up above (store and control keys first) |
| `--overlay` (the in-game SurfaceFlinger overlay) | refused: exit 0 (no SurfaceFlinger), after `install()` as in `main.cpp` |
| drastic-nano relaunch (`auto_relaunch`) | left out: the DS player is out of scope |
| `overlay_home` stray-instance guard | left out: no overlay, so no stray |
| early DRM master grab on `/dev/dri/card0` | left out: no HWComposer to beat; `readyToRun`'s `drmEarlySplash` (through `Display::startScanout`) opens the card itself (`gEarlyDrmFd` stays -1, as on Qualcomm) |
| `nanoBootSequence(firmwareBootSteps)` | `nanoBootSequence(nullptr)`: the priority, `drm_active=0`, `NanoMenu` and its thread are shared; `firmwareBootSteps()` (DraStic, binder) is left out |
| `IPCThreadState::joinThreadPool()` | `NanoMenu::join()`: on Android the process ends in `threadLoop`'s `stopProcess()`; the layer-2 `stopProcess` only logs, so `main` goes on when the thread ends. `run()`'s status is not checked (G7-main-7) |
| (Android's framework starts the launched activity as the home exits) | `nano.clear()` (`~NanoMenu` releases and closes the pads), then `leaveQueued()`: the launch the control keys queued is started in its own session and left running with `sys.thimsy.game.pid` set, and the handshake is cleared (B.6's Launch row). `controlkeys::uninstall()`, then exit |

Nano exits after every launch on Linux, as Android's home does (REV4-1), so threads `~NanoMenu` leaves behind (the render watchdog, the scan workers) end with the process instead of outliving the menu for the game's length. The exit status tells `thimsy-session` how the menu ended (REV4-2; `linux/docs/session.md`): 0 (`kExitLaunchOver`) after a launch, started or not, 1 (`kExitNoLaunch`) when the menu ended without one, 2 when the platform did not start. The session restarts Nano when the game ends, or at once after a 0 with no game running, and holds back a launch loop with the crash back-off: cycles under 10 s in which Nano was on screen under 2 s before the launch (or never), so no one had time to choose (an auto-launch); a user trying games that fail at once is not held back. `Launcher::nanoExitsOnLaunch()` still says false for Linux: a resident Nano that hands the panel over and polls the child is the intended contract, not yet built.

SIGPIPE stays at its default, as on Android. `tools/nano-tree/tests/test_main_linux.py` fingerprints `main.cpp` from `firmwareBootSteps()` to the end, so a change there (a merge from `nano-144`) fails until this table is reviewed. It also checks the set-up order, that the platform is installed first, `nanoBootSequence()`'s order and tag, and `gEarlyDrmFd` in every main (G7-main-6). `main_headless.cpp` does not call `nanoBootSequence()` yet: it still mirrors `main.cpp` by hand and cites old line numbers (DECISIONS.md, open items from wave 4).

### B.8 The DRM buffer back end's contract

What `NanoMenuDrm.cpp` and the shared render loops expect of a render target, so `DrmBuffersGbm.cpp` honours the rules the AHardwareBuffer version does. Checked by G9a against the moved code (`NanoMenuDrmBuffers.cpp`) and its callers in `NanoMenuDrm.cpp`; the first draft, read from `NanoMenuDrm.h` alone, had the ring depth, the free path and the zero-copy condition wrong and missed the EGL gate. `NanoMenuDrmBuffers.h` carries a short form. `DrmHost.cpp` replaces all of `NanoMenuDrm.h`'s API, so only the ring and scanout rules apply to it.

- **The interface is one function.** `drmAllocAhbTarget(eglDpy, w, h, target, label)`, in `NanoMenuDrmBuffers.cpp` since G9a. There is no free function: `NanoMenuDrm.cpp` frees inline, in `drmStop()`'s `releaseSlot` (`scanFbo` back into `glFbo`, the FBO, the texture, the EGL image through `sEglDestroyImageKHR`, `RMFB` of `drmFbId`, `GEM_CLOSE` of `drmGemHandle` on `sDrmFd`, `AHardwareBuffer_release(ahb)`, `w = h = 0`) and in `drmSetupZeroCopy()`'s two unwind loops (the FBO, the texture, the EGL image, `AHardwareBuffer_release`, then `= {}`; no `RMFB` or `GEM_CLOSE`, so on Android a ring that fails part-way keeps the earlier slots' framebuffers until the fd closes: pre-existing, left alone). The flip path's CPU-read fallback calls `AHardwareBuffer_describe`, `_lock` and `_unlock`. So a back end that is not AHardwareBuffer defines those four NDK functions for its own handles (link-time substitution, B.5); the layer-2 `android/hardware_buffer.h` stub declares them, and G4's stubs must not also define them.
- **Allocate.** At the size `drmSetupZeroCopy` passes: the primary display's mode size (twice the height in AFBC mode), and for the secondary ring the first non-primary display's mode size, so always the panel's native orientation; rotation is the GL path's (`sDrmRotationDeg`, `sDrmGlRotation`, `sDrmRotMat`). R8G8B8A8 in memory (`DRM_FORMAT_ABGR8888`), linear, except in AFBC mode (`sDrmAfbcMode`: `rk356x`, drastic-nano's Low Latency, two displays), where the import carries gralloc's AFBC modifier and pitch. On success the target has `ahb` non-null (the flip path reads `ahb != nullptr` as "this slot exists"), `eglImage`, `glTexture`, `glFbo` (a complete colour attachment), `w`, `h`, and `drmFbId` and `drmGemHandle` (both 0 when the DRM import failed, which is still success); `scanFbo` is untouched (the turned-panel resolve owns it); `GL_FRAMEBUFFER` is left bound to 0 and `GL_TEXTURE_2D` to the new texture. On failure: false, nothing owned, `ahb` null. Called on the thread whose EGL context is current.
- **The ring.** `AHB_RING_DEPTH` (5, not 3) targets per ring. `sAhbRingPrimary` is all or none, and `sDrmZeroCopy` becomes true only once all five exist; without it `drmFrameEnd`'s non-zero-copy branch does nothing, so on Linux, which has no SurfaceFlinger fallback, nothing is shown. `sAhbRingSecondary` is all or none when there is more than one display; without it the other displays mirror the primary. `sAhbTarget` and `sAhbTargetSecondary` are macros for slot `sRingRenderIdx`; the render loops draw into that slot and `drmFlipRingSlot(idx)` presents slot `idx`. Five slots cover render N, fence N-1, primary scanout N-2, secondary scanout N-3 (frame sync) and a spare, so a slot is never drawn into while on screen. The back end only allocates.
- **Scanout.** A target with `drmFbId != 0` is page-flipped directly (the PRIME path needs the primary's and, when there is one, the secondary's framebuffer). The buffer's first row is GL's y = 0, so `drmSetupZeroCopy` folds a Y-flip into `sDrmRotMat` when `sAhbRingPrimary[0].drmFbId != 0`; any EGL image of a dma-buf bound to an FBO stores rows that way. A target with `drmFbId == 0` is valid on Android only: the flip path then waits for the GPU, CPU-reads the buffer through `AHardwareBuffer_describe`, `_lock` and `_unlock`, and copies it into the display's dumb buffer. **A Linux back end fails the allocation rather than return `drmFbId == 0`**, and its `AHardwareBuffer_lock` may simply fail. In AFBC mode a back end that can't allocate AFBC fails too.
- **Fences.** Not the back end's: the render loops put an EGL native fence in `sAhbRingSyncPrimary[idx]` (`EGL_ANDROID_native_fence_sync`) and `drmFlipRingSlot` waits on it (`eglClientWaitSyncKHR` on the PRIME path; in AFBC mode the fence fd becomes the atomic commit's `IN_FENCE_FD`). With no fence the PRIME path does not wait; callers outside the ring use `drmFrameEnd`, whose `glFinish` makes the GPU's writes visible before the flip. Mesa offers `EGL_ANDROID_native_fence_sync` on DRM drivers *(guess for panfrost; checked in G10)*.
- **The EGL gate (a Linux blocker G9a found).** Before allocating anything, `drmSetupZeroCopy` (which stays in `NanoMenuDrm.cpp`) returns with zero-copy off unless `eglGetProcAddress` resolves `eglCreateImageKHR`, `glEGLImageTargetTexture2DOES` and `eglGetNativeClientBufferANDROID`, and `EGL_EXTENSIONS` lists `EGL_ANDROID_image_native_buffer` and `EGL_ANDROID_get_native_client_buffer`. Mesa's GBM platform offers neither Android name *(from memory)*. So G9 satisfies the gate without editing `NanoMenuDrm.cpp`: `thimsy-nano` links with `-Wl,--wrap=eglGetProcAddress,--wrap=eglQueryString` (beside B.5's shell wraps), and the wrappers answer for those three names only (the client-buffer function can return null: only the substituted `drmAllocAhbTarget` would call it) and pass everything else through. If that proves fragile, the fallback is a P4 move of the gate into the back end (a `drmBuffersAvailable(eglDpy)` call), an edit under H.3.
- **Teardown and re-acquire.** `drmStop` frees the slots on `eglGetCurrentDisplay()`, so the context must still be current, and closes `drmGemHandle` on `sDrmFd`: a back end whose buffer's GEM handle has another owner (a `gbm_bo` made on `sDrmFd`, whose handle a PRIME import would return again) leaves `drmGemHandle` 0 and closes the buffer in its `AHardwareBuffer_release`. `drmReacquireForHome` reopens `/dev/dri/card0` and runs `drmEarlySplash` and `drmSetupZeroCopy` again, so `sDrmFd` can change between allocations: a back end keys its allocator (the `gbm_device`) on the current `sDrmFd`.

---

## C. The build

### C.1 One source list

`nano/build/sources.txt` lists every compiled file once, in sections:

```
[core]          shared files compiled on every platform (Android.bp's gammaos-nano srcs minus [firmware]),
                including the media, overlay and SD-move files that compile against the layer-2 stubs:
                NanoMenuVideo.cpp, NanoMenuPhotos.cpp, NanoMenuMusic.cpp, NanoMenuOverlay.cpp,
                NanoSdMove.cpp, NanoMenuRadio.cpp, NanoMenuIptv.cpp, NanoMenuPdf.cpp, NanoMenuMtp.cpp ...
[drm]           NanoMenuDrm.cpp: firmware and Linux (with their buffer back ends), not the host (DrmHost.cpp)
[firmware]      Android firmware only: main.cpp, DrasticRunner.cpp, DrasticGpu3d.cpp, DrasticSettings.cpp,
                FakeJNI.cpp, NanoMenuDrasticPark.cpp, NanoMenuDrmBuffers.cpp (after G9a), NanoBootChime.cpp,
                NanoBtStable.cpp, NanoMenuShares.cpp and NanoMenuRgbFollow.cpp (their headers live outside
                Nano's folder; v1 has no Shares or RGB follow on Linux), LibretroRunner.cpp (until Phase 5) ...
[dropped-linux] a subset of [firmware]: every file that defines anything [core] or [drm] names (NanoMenu::
                methods, free functions, variables; 10 of the 12 today), for which the Linux and host
                builds link generated stubs (below). Generated by the census (G4-5)
[linux]         platform/linux/**.cpp except platform/linux/tests/ (main_linux.cpp among them), and
                NanoPlatform.cpp until G7-main moves it to [core]
[host]          platform/host/**.cpp, test/host/headless/main_headless.cpp, NanoPlatform.cpp (the same)
[scrape]        gammaos-nano-scrape's six files, for thimsy-nano-scrape (C.2)
[test-unit]     the 24 host-tested production files and test/unit/*_test.cpp, without DsScreenLayout_test.cpp
```

The `[core]` and `[firmware]` split is made in G4 by a script, not by compiling (the headers to compile against are not installed, section 0): `include_census.py` (B.4) marks a file `[firmware]` when it reaches an Android-only header that has neither a shim nor a stub and that G4 decides not to stub. Compiling decides nothing until G10, which may move a file and records why.

**No hand-written NanoMenu stubs.** The firmware and media files define hundreds of `NanoMenu::` methods declared in `NanoMenu.h` (6,629 lines, the most edited header): `NanoMenuVideo.cpp` 147, `NanoMenuPhotos.cpp` 108, `NanoMenuMusic.cpp` 71, `NanoMenuOverlay.cpp` 60, `NanoSdMove.cpp` 24 and more. Hand-copied stubs would break on every signature change in `nano-144`. So those files compile on Linux against the layer-2 stubs (AMediaCodec, AAudio and the rest fail at run time), and `NanoSdMove.cpp` is settled as `[core]` here and in B.6. `[dropped-linux]` is every `[firmware]` file that defines something a `[core]` or `[drm]` file names, not only `NanoMenu::` methods (G4-5): `NanoMenu.cpp` calls into the libretro and DS runners, the boot chime, Bluetooth's helper and RGB follow. A false positive costs nothing: `nano/test/host/stubgen/gen.py` generates, at build time from `nm` of the whole link, stubs only for the symbols still undefined there that a `--from` file defines (or a `--scope` prefix matches), with return types read from the declarations, each logging once and returning a value-initialised zero; a missing or ambiguous declaration, a template or an operator is an error, never a guess (G4-6). Nothing is committed, and `test_nano_sources.py` fails if a `dropped/` directory with hand-written stubs appears. What `[core]` needs a real value for is defined by the platform instead, so the generator never sees it: `gEarlyDrmFd` (-1) by each main, `nanoRgbFollowSample()` (a no-op) by `DrmHost.cpp` and `DrmBuffersGbm.cpp`, and `NanoMenuDrmBuffers.cpp`'s functions by the GBM back end (CMake refuses a `[linux]` without it).

`tools/nano-tree/tests/test_nano_sources.py` checks, with `androidbp.py`: every file in `Android.bp`'s `gammaos-nano` `srcs` is in exactly one of `[core]`, `[drm]` and `[firmware]`; `[dropped-linux]` is a subset of `[firmware]`; `[linux]` and `[host]` are the platform folders' files (B.6); `[scrape]` equals `gammaos-nano-scrape`'s `srcs`; every `[test-unit]` production file is in `gammaos_nano_unit_tests`' `srcs`; every listed file exists. `Android.bp` stays authoritative for Android; `sources.txt` only partitions it.

### C.2 CMake (for the Linux image and the app later)

`nano/CMakeLists.txt` (minimum 3.16; Buildroot provides host CMake; not runnable on this machine today):

| Target | Kind | Sources | Links |
|---|---|---|---|
| `nano_compat` | static | `compat/src/*.cpp` | `libthimsy` (via `add_subdirectory(../libthimsy)` in the repository, `find_package(thimsy)` in Buildroot) |
| `nano_stubs` | static | `compat/stubs/src/*.cpp` (B.4's layer 2) | `nano_compat`; exports the stub include directory and `-DEGL_NO_PLATFORM_SPECIFIC_TYPES` (`stubs.mk`'s `NANOSTUBS_CPPFLAGS`) to everything that links it |
| `nano_core` | static | `[core]`, minus the files an OFF feature switch leaves out | `nano_stubs`, `nano_compat`; FreeType, libpng, libjpeg-turbo, libwebp, giflib (the image decoder shim), zlib, tinyxml2, libcurl, OpenSSL (libcrypto), EGL, GLESv2, libdrm (headers), nanosvg (vendored), pthread |
| `nano_platform_linux` | static | `[linux]` minus `main_linux.cpp` and `ShellGate.cpp`, plus `[drm]`: `ControlKeys.cpp`, `NanoPlatform.cpp`, and `DrmBuffersGbm.cpp` with `EglWraps.cpp` in place of `NanoMenuDrmBuffers.cpp` | `nano_core`, `nano_stubs`, `nano_shellgate`, gbm, EGL, GLESv2, libdrm, alsa-lib |
| `nano_shellgate` | static | `platform/linux/ShellGate.cpp` (B.5's wrappers) | `nano_compat` |
| `nano_platform_host` | static | `[host]` minus the main | `nano_core`, `nano_stubs` |
| `thimsy-nano` | executable | `main_linux.cpp`, the generated `[dropped-linux]` stubs | `nano_platform_linux` and `nano_core` in one link group; `-Wl,--wrap=system,--wrap=popen,--wrap=pclose,--wrap=execv,--wrap=execvp,--wrap=execve`, and the EGL gate's two (B.8): the list is read from `nano/platform/linux/linker-wraps.txt` |
| `thimsy-nano-scrape` | executable | `[scrape]` | `nano_compat`, `nano_shellgate`, libcurl, OpenSSL, zlib; the same wrap. `gammaos-nano.rc` starts the scraper as its own small process on `scrape_run`; on Linux the control-key table routes that key to `thimsy-session`, which starts this binary, so a scrape beside a game stays a few MB as on Android (decision I25) |
| `nano-headless` | executable | `main_headless.cpp`, the generated stubs and the GL stub's generated part | `nano_platform_host` and `nano_core` in one link group, `nano_glstub`; the same wrap plus `--wrap=ioctl` |
| `nano_glstub` | static | `test/host/glstub/glue.c` (the hand-written part) | none |
| `nano_unit_tests` | custom target, CTest | `[test-unit]`: one program per `test/unit/*_test.cpp` with its closure (`nano/build/unit-closure.txt`) | GoogleTest if `find_package(GTest)` finds it, else the stand-in |

`nano_platform_firmware` is not a CMake target: it exists only in `Android.bp`, which remains the only build of the Android firmware (decision I4). CMake reads `nano/build/sources.txt` itself (`file(STRINGS)` and a small section parser in `nano/cmake/sources.cmake`), so there is no second copy of the lists to compare. Flags: `-std=gnu++17`, and the `gammaos-nano` `cflags` of `Android.bp` (`-DGL_GLEXT_PROTOTYPES`, `-DEGL_EGLEXT_PROTOTYPES`, `-O3`, `-ffast-math`, `-fno-math-errno`, `-fno-trapping-math`, `-Wall`), read from `Android.bp` at configure time by `nano/cmake/androidbp_flags.cmake` *(or written into `sources.txt`'s header and checked by the test, if CMake parsing proves awkward)*. `-Werror` is off for device builds at first (GCC warns differently from Clang) and on for the host tests. Feature switches: `NANO_WITH_MEDIA=OFF`, `NANO_WITH_AC3=OFF` (drops `liba52`'s GPL code on Linux, plan 3.3), `NANO_WITH_PDF=OFF`, `NANO_WITH_ARCHIVE=OFF` (`NanoArchive.cpp` and `NanoOtaCheck.cpp`, which need libziparchive and the LZMA SDK). An OFF switch leaves its `[core]` files out of `nano_core` without moving them in `sources.txt`, and what they define gets generated stubs, as `[dropped-linux]` does (`nano/cmake/features.cmake`; G6-5); `NANO_WITH_MEDIA=ON` is refused until a media back end exists. `NANO_PLATFORM` picks the executables: `linux` (the default, Buildroot's), `host`, `all` or `none` (the libraries, the scraper and the unit tests); a platform whose entry points are missing from `[linux]`/`[host]` stops at configure with a message (G6-6).

Where the build differs from the table's first draft (G6), and why:

- **The generated stubs are compiled into each executable**, not into a platform library: they are made from `nm` of the whole link, the main included (`nano/cmake/stubgen.cmake`), so a library holding them would depend on itself. For the same reason each main is a one-file static library (`nano_linux_main`, `nano_host_main`), so `nm` reads every input as an archive (G6-3).
- **A link group.** `nano_core` and the platform library call each other (`[core]` calls `NanoMenu::` methods that `[drm]` or `DrmHost.cpp` define), so the executables link those two archives inside `-Wl,--start-group`/`--end-group` (G6-4).
- **`nano_shellgate`** is split out of the platform library so `thimsy-nano-scrape` links the same shell gate for its `--wrap` set; `NANO_BUILD_SCRAPE` needs it listed in `[linux]` (G6-7).
- **`nano_unit_tests` is a custom target** over one program per test, production files built once as object libraries: each test's closure differs, a program whose headers are missing is skipped alone (C.3), and one that needs asset folders is disabled when `NANO_DATA` is empty (G6-10).
- **`NANO_WITH_ARCHIVE`** is a fourth feature switch (above); the host Makefile's `headless` and `syntax-core` still compile those files (G10).
- **`NANO_PLATFORM`** replaces building every platform at once (above).
- The stub generators run on the build host's `python3` (3.8 or later) and `CMAKE_NM` (G6-12).

### C.3 The host build usable now: `nano/test/host/Makefile`

For g++ 9.4 and make, with no CMake: a plain Makefile that reads `sources.txt` (a 10-line `awk` turns sections into make variables), builds out of tree (`O=$HOME/.cache/thimsy/nano-host`, so nothing is written to NTFS), and has targets:

| Target | Builds and runs | Needs | Time at `-j1`, `nice -n 19` |
|---|---|---|---|
| `check-compat` | `libthimsy` and `compat/` tests | g++ only | under a minute *(guess)* |
| `check-unit-0` | the `*_test.cpp` programs whose closure needs no outside library, through the shims, with the stand-in | g++ only | 2 to 5 min first, seconds after *(guess)* |
| `check-unit` | every `*_test.cpp` program | zlib, OpenSSL, tinyxml2, FreeType, a zip reader for the two tests that include `ziparchive/zip_archive.h` | 3 to 8 min *(guess)*; **deferred (G10)** |
| `syntax-core` | `g++ -fsyntax-only` of every `[core]` file, **no sanitizers** | as `check-unit`, plus EGL, GLES, libdrm and the image libraries' headers | 10 to 20 min *(guess)*; **deferred (G10)** |
| `headless` | `nano-headless` | as `syntax-core` | 20 to 40 min first *(guess: `NanoMenuPS3Menu.cpp` is 1.1 MB)*; **deferred (G10)** |

**One program per test.** The tests are not one program: each `test/unit/X_test.cpp` builds into its own binary with its production-file closure, recorded in a generated table (`nano/build/unit-closure.txt`, from `include_census.py` and the `gammaos_nano_unit_tests` list) with each program's header needs. A program whose headers are missing prints `SKIP X_test: missing <header>`; the rest build and run. So one missing library skips only the tests that need it.

**Memory guard.** WSL `Ubuntu` shares the 13 GB WSL2 VM with the AOSP build, and `nice` does not limit memory. Every target first runs `linux/tools/build_guard.sh --light` (E.8): it refuses `check-unit`, `syntax-core` and `headless` whenever an Android build may be running, and refuses any target when `MemAvailable` is under the floor. Every compiler and test process runs under `prlimit --as=3221225472` (3 GiB) with `echo 1000 > /proc/self/oom_score_adj`, so a runaway `cc1plus` dies first and alone. The Makefiles of `libthimsy`, `thimsy-session` and `thimsy-padd` follow the same rule and build out of tree under `$HOME/.cache/thimsy/<component>`, as this one does.

Flags: `-std=gnu++17 -O0 -g -Wall -Werror -fsanitize=address,undefined` for test programs, `-include nanocompat/bionic.h` everywhere, plus GCC-only suppressions kept in one variable (`-Wno-class-memaccess -Wno-stringop-truncation -Wno-format-truncation` to start *(guess at the needed set)*), so no Nano file is edited for a GCC warning. Each target probes its headers first (`echo '#include <zlib.h>' | g++ -E -x c++ -`) and prints `SKIP <target>: missing <header>` instead of failing; the Python wrapper turns that into a skipped test. The packages to install once downloads are allowed are listed in `nano/test/host/DEPS.md` (Ubuntu: `zlib1g-dev libssl-dev libtinyxml2-dev libfreetype-dev libpng-dev libjpeg-turbo8-dev libwebp-dev libgif-dev libcurl4-openssl-dev libegl-dev libgles-dev libdrm-dev libgbm-dev libasound2-dev libgtest-dev`).

`NanoArchive` (libziparchive and the LZMA SDK) stays out of the host suite until a portable zip back end exists (later; the Linux build needs one anyway).

---

## D. Tests

Every test runs in seconds to minutes with Python 3.12 on Windows or g++ 9.4 in WSL at `nice -n 19`, `-j1`, behind the memory guard (C.3). New Python tests use only the standard library and must run on 3.8 as well. Commands:

```
Windows:  py -3.12 -I -m unittest discover -s tests -t .                       (quick; gates the Android build)
          py -3.12 -I -m unittest discover -s tools/nano-tree/tests -t tools/nano-tree
          py -3.12 -I -m unittest discover -s linux/tests -t linux
WSL:      nice -n 19 make -C nano/test/host -j1 check-compat check-unit-0
          nice -n 19 make -C libthimsy -j1 check
          nice -n 19 make -C linux/src/thimsy-session -j1 check      (and thimsy-padd)
After G10: THIMSY_TEST_MODE=slow tests/run.sh nano                   (adds the headless E2E)
```

`tests/run.sh` gains `nano`, `linux` and `cxx` modes that discover only their own directories (`tools/nano-tree/tests`, `linux/tests`, the Makefiles' wrappers); the default stays `quick`, which keeps discovering `tests/` only. That also keeps the fork's new tests away from the existing suite's 3.9-only code (`scripts/esde_config.py:36` uses `str.removeprefix`), which is Android-side and not changed here.

### D.1 Unit tests

| Suite | Kind | Covers |
|---|---|---|
| Nano's existing host tests | C++, `check-unit-0` and `check-unit` | the 24 production files, one program per test, with the stand-in or GoogleTest |
| `libthimsy/tests` | C++ | store: get, set, Android's value limit, `ro.*` refusal, persist round trip and its 0600 mode, crash safety (a writer killed between temp and rename leaves the old file), two processes writing different keys, serials and `waitChange`, observers (called after the store, never vetoing), secret redaction; atomic file; uevent parser against captured messages; JSON |
| `nano/test/host/compat` | C++ | `property_get` and typed forms against Android's documented behaviour (bool spellings `1/y/yes/on/true`, int parse and range, defaults); `Thread` run and exit semantics, `onFirstRef` timing, `sp`/`wp`; log macros (`LOG_NDEBUG`, `_IF`, `SLOG*`); `bionic.h`; the image decoder shim on small PNG, JPEG, GIF and WebP fixtures (once the libraries exist) |
| `linux/src/thimsy-session/tests`, `thimsy-padd/tests` | C++ | section F |
| `tools/nano-tree/tests/`: `test_nano_tree.py`, `test_nano_files.py`, `test_nano_sources.py`, `test_nano_stubs.py`, `test_nano_ratchets.py`, `test_android_surface.py`, `test_control_keys.py` | Python | sections A.6, C.1, B.4, B.5, B.2, H |
| `linux/tests/*` | Python | every tool in section E |

**The GoogleTest stand-in** grows from today's 95 lines to cover what Nano's tests use: `TEST_F` with `SetUp` and `TearDown`, `SCOPED_TRACE`, `EXPECT_FLOAT_EQ` and `ASSERT_FLOAT_EQ` (4 ULPs, as GoogleTest), `EXPECT_NEAR`, `GTEST_SKIP`, `EXPECT_LT`/`LE`/`GT`/`GE` and the `ASSERT_` forms, `--gtest_filter` (simple globs), and output in GoogleTest's format followed by the old stand-in's summary line (`N tests, M failed`), which `tests/unit/test_nano_units.py` asserts on. It lives in `libthimsy/testing/gtest/gtest.h` (MIT, written fresh); `tests/fixtures/nano_gtest` stays for the existing test until G1 points it at the new one. With GoogleTest installed, the Makefile uses the real one.

### D.2 E2E: the Android chain

- The existing `tests/e2e/` (mods, packing, sandbox) unchanged.
- The patch round trip of A.6 (`regen --check`, apply to base, import round trip, prefix and suffix unchanged, modes).
- `test_android_surface.py` (section H).

### D.3 E2E: Nano on the host, headless

`nano-headless` runs `nano_core` over `nano_platform_host` with `nano_glstub` (every GLES 2 and EGL entry point Nano calls, generated from the link's undefined symbols by `nano/test/host/glstub/gen.py`; ids increment, compiles succeed, `glReadPixels` returns zeros, `glGetError` returns 0). No GPU, no window. Golden screenshots need a real renderer (Mesa llvmpipe) and come later. It is written in G5 and first built in G10 (its headers are not installed now, section 0).

**The driver runs Nano as Nano runs.** `NanoMenu::threadLoop` is not a per-frame function: it holds the whole session (set-up, its own frame loop, teardown, `stopProcess`), and `threadLoop` and `readyToRun` are private. So `main_headless.cpp` does what `main.cpp` does: it installs the host platform (`createPlatform`, `install`), creates `NanoMenu` and calls `run()` through the compat `android::Thread`, on real time, then joins the thread. It still mirrors `main.cpp`'s steps by hand rather than calling `nanoBootSequence()` (B.7). On the host, `readyToRun` takes the DRM path because `DrmHost.cpp`'s `drmEarlySplash` sets `sDrmActive` (B.5); without it, it would reach the SurfaceFlinger branch and fail with "No displays found".

**Stepping.** Every flip in `DrmHost.cpp` is a present, and each present passes a **frame barrier**: it increments `present_count` and, when the scenario asks for it, blocks until the driver grants the next frame. So a scenario can stop Nano between frames, inspect the store, files and fakes, inject input, and release N frames. Between barriers Nano runs on real time; its helper threads (scan, net poll) are real threads. `expect` steps wait on a condition with a wall-clock deadline (`within_ms`), polling the store, the files and the fakes; there is no fake clock (B.2).

**Input.** The jail's `/dev/input/event0..N` are FIFOs that the runner creates and holds open `O_RDWR`, so Nano's `O_RDONLY | O_NONBLOCK` open succeeds and it never reads end-of-file. A `press` step writes `struct input_event` records (key down, `SYN_REPORT`, key up after `hold_ms`) with the host's current time. `FakeEvdev.cpp` (the host's `--wrap=ioctl`, B.6) answers the identity and capability ioctls for those descriptors from the scenario's `devices` entry (name, bus, vendor, product, key and axis bits), so Nano classifies the fake pad as it would padd's. Pressing needs neither root nor `/dev/uinput`.

**The jail.** Nano writes `/data/...` and reads `/system/etc/...`, `/proc/self`, `/proc/meminfo` and `/proc/asound`, and ASan reads `/proc/self/maps`. The runner (`tests/e2e/nano_jail.py`) builds a root in a temporary directory with the Linux image's layout (B.5's links; assets from `nano_tree.py export-assets`; a fixture drive at `/storage/1234-ABCD` with `ROMs/` and `ES-DE/`) and runs `nano-headless` in it with `unshare --user --map-root-user --mount --pid --fork --mount-proc` and `chroot`. Inside the namespace it mounts a tmpfs at the jail's `/usr`, binds `/usr/bin`, `/usr/lib`, `/usr/lib64` (if present) and `/usr/libexec` individually, binds `/lib`, `/lib64`, `/bin`, `/etc` and `/dev/null`, remounts every bind read-only (`mount -o remount,bind,ro`), and only then creates `/usr/share/thimsy/system/etc` and `/usr/share/fonts/thimsy` on the tmpfs. A probe at the review showed unprivileged `unshare --user --map-root-user --mount` works in WSL `Ubuntu` (uid 0 inside, tmpfs mount allowed), which confirms I17's guess. There is **no real-root fallback**: if user namespaces are unavailable, the test skips with a message. Running as real root would put the WSL distro's files behind Nano's shell paths, and with ASan an `mlockall` could try to lock its shadow memory in the shared VM. Scenarios boot with `persist.gammaos.nano.mlockall=0` (`NanoMenu.cpp:1447`) for the same reason.

**Scenarios** are JSON lines in `nano/test/host/scenarios/`:

```json
{"boot": {"props": {"persist.gammaos.nano.theme": "xmb", "persist.gammaos.nano.mlockall": "0"},
          "devices": [{"node": "event0", "name": "Thimsy Gamepad", "bus": 6, "vendor": 21576, "product": 1}]}}
{"expect": {"present_count_at_least": 1}, "within_ms": 10000}
{"press": "DPAD_RIGHT"}
{"press": "A", "hold_ms": 80}
{"expect_launch": {"kind": "Core", "core": "snes9x", "rom_endswith": "/ROMs/snes/Test Game.sfc"}, "within_ms": 5000}
{"child_exit": {"status": 0, "after_ms": 500}}
{"expect_file": {"path": "/data/system/nano_esde_played.txt", "contains": "Test Game"}, "within_ms": 3000}
{"expect_no_shell": ["am", "settings"]}
```

First scenarios: boot to the home and present a frame; open a system and launch a game (core and standalone); a launch's state files written; a setting changed and persisted to `persist.props`; a drive appearing (`FakeVolumes`) adds its library; the quit request reaching the launcher. Written so far (`nano/test/host/scenarios/README.md`): `boot_home`, `navigate`, `launch_game` (core only) and `setting_persisted`, their key sequences first guesses marked `TODO(G10)`; the drive and quit scenarios are not written. Since G7-launch-b, `commitLaunch` reaches the host's `FakeLauncher`, which writes the handshake keys as the firmware does and records a `launch` event at the trigger's write; `expect_launch` waits for that event. Nano's launch sites still exit after a launch, so `child_exit` waits for Nano to exit and starts it again in the same jail, as `thimsy-session` would. The same scenario is meant to pass before and after each G7 step, which is P4's behaviour check on the host; none has run yet (G10, with the merged platform). Deadlines are generous (seconds, not frames) so a slow `-O0` ASan build under `nice` does not flake; a scenario that needs exact frame counts uses the barrier instead.

Runs: `slow` mode only (the build is long the first time); each scenario takes seconds.

### D.4 E2E: the Linux build, static

Nothing here runs Buildroot. All are Python under `linux/tests/`, seconds each unless stated:

| Test | Checks |
|---|---|
| `test_defconfig.py` | `linux/tools/defconfig.py` merges each target's fragments (E.4) in order; the committed `configs/thimsy_*_defconfig` equal the merge; no option set twice with different values except where a later fragment is declared an override (`# override:` comment); every `BR2_PACKAGE_THIMSY_*` and `BR2_PACKAGE_LIBRETRO_*` symbol exists in a `package/*/Config.in`; every `$(BR2_EXTERNAL_THIMSY_PATH)/...` path exists; **no target named `*_release` or marked `"publish": true` in `targets.json` includes `dev.fragment`**, and every release target sets a locked root password, no adbd, no serial getty and dropbear off or key-only. Upstream Buildroot symbols are checked only when `--buildroot DIR` (a Buildroot tree at the pinned version) is given |
| `test_packages.py` | `linux/tools/lint_packages.py`: each package has `Config.in`, `<pkg>.mk`, `<pkg>.hash` (except local sites), `_VERSION`, `_SITE`, `_LICENSE`, `_LICENSE_FILES`; naming; `Config.in` sourced from `package/Config.in`; a core's commit equals its entry in `mods/retroarch-cores.src.lock` (E.3); `thimsy-data`'s install list is generated from `ASSETS.md`'s KEEP rows and holds no path `ASSETS.md` marks REPLACE or ASK (the Rodin fonts, the XMB icons with the PSN logo, the wave captures, the DSi sprites and sounds, the Mirsany font); Buildroot's own `utils/check-package` runs too when a Buildroot tree is given |
| `test_rcs.py` | init scripts parse (`sh -n`, through Git Bash or WSL's `sh`), have no bashisms from a pattern list, are executable in the index, and run in the order E.5 states; the synchronous part of `rcS` starts nothing that section 9's budget forbids (a list); `S00base` and `D60firstboot` run against a fake `/sys` and `/proc/cmdline` (two disks that both have a `data` partition, and an eMMC whose `device/type` is `MMC`) and pick only the boot disk's partitions |
| `test_layout.py`, `test_gpt_check.py` | each family's `genimage.cfg` (E.6), read statically: names, type GUIDs, order, 1 MiB alignment, the raw idbloader ending before the first partition, no entry of a type vold mounts in the written image (the `games` entry is made at first boot); `linux/tools/gpt_check.py`, which `post-image.sh` runs on every real image, against small GPT images the test writes itself |
| `test_fit.py` | `linux/tools/fit.py` writes each family's `.its` exactly as the golden file; when `dtc` and `mkimage` are on `PATH`, it builds the FIT and reads the nodes back |
| `test_build_guard.py` | `linux/tools/build_guard.sh` (E.8) refuses with a fake `/proc/meminfo` below the threshold, with the lock held, with a fake `wsl.exe -l --running` that lists `thimsy-aosp`, and with a high fake `/proc/loadavg`; `--light` refuses the heavy targets; `--force` without its typed confirmation is refused. Runs in WSL; skips where `flock` is missing (Git Bash) |
| `thimsy_import/test_*.py` | section E.7, once G2c runs |
| `qemu/test_qemu_boot.py` | written, not run (E.9) |

---

## E. The Linux tree

### E.1 Layout

Per plan 3.2 and decision 6, refined. FlipBase's subtree merge (decision 19, at the start of Phase 2a) brings its files into the same `linux/` prefix; where Thimsy has already written a file at one of FlipBase's paths, the merge keeps Thimsy's and FlipBase's stays in history, **except licence and notice files**: FlipBase's `LICENSE` (its MIT copyright notice, which MIT requires in every copy), any `COPYING` and its `NOTICE` are kept as files (`LICENSE.FlipBase`, `NOTICE`), and `linux/LICENSE` says that the BaseOS-derived files FlipBase's notice names are not covered by it (plan risk 12; open question 26).

```
linux/
  LICENSE (MIT)  LICENSE.FlipBase  NOTICE (FlipBase's, kept at the merge)  README.md
  external.desc            name: THIMSY
  external.mk              include $(sort $(wildcard $(BR2_EXTERNAL_THIMSY_PATH)/package/*/*.mk))
                           plus the gpl/package recipes
  Config.in                source "$BR2_EXTERNAL_THIMSY_PATH/package/Config.in"
  build.sh                 entry point (E.8)
  configs/
    fragments/*.fragment   E.4
    targets.json           which fragments make which defconfig
    thimsy_v8_0_defconfig  thimsy_v8_2_defconfig  thimsy_rk3566_hw_defconfig
    thimsy_qemu_aarch64_hw_defconfig                 generated by tools/defconfig.py, committed
  board/
    common/
      rootfs-overlay/      etc/inittab, etc/init.d/{rcS,rcK,S*}, etc/init.d/deferred/D*, etc/thimsy/,
                           usr/libexec/thimsy/, usr/share/thimsy/
      post-build.sh        runs post-build.d/* in order (each file has one owner, section G)
      post-build.d/        10-android-links.sh (the B.5 links), 20-build-props.sh, 30-permissions.sh
      post-image.sh        runs post-image.d/* in order
      post-image.d/        10-erofs.sh (root), 20-data.sh (data ext4, mkfs.ext4 -d), 30-genimage.sh (per-device
                           cards through genimage), 40-gpt-check.sh (tools/gpt_check.py on each card)
      users.table  device.table
    rk3566/
      common/              per-family rcS hooks (governors, ALSA UCM, sleep), overlay
      uboot/               boot.cmd, uboot.fragment (FlipBase's options, the rgxx3 board)
      linux.fragment       Thimsy's kernel fragment (plan 3.2)
      genimage.cfg         E.6
      devices/             anbernic-rg-arc-d.json, anbernic-rg-arc-s.json: device.json overrides
    qemu-aarch64-virt/
      linux.fragment  genimage.cfg  run-qemu.sh  devices/qemu-virt.json
  thimsy/<family>/overrides/     Thimsy's own data on generated data
  generated/<family>/            written by thimsy-import (by hand for RK3566 in Phase 2a): defconfig
                                 fragment, DTB list, firmware list, product catalog, device.json facts,
                                 ROCKNIX commit, hash guards
  package/
    Config.in
    libthimsy/  thimsy-nano/  thimsy-data/  thimsy-session/  thimsy-padd/  thimsy-fonts/
    thimsy-gpu-pack/        packs Mesa's driver files from a tier tree into a GPU-pack EROFS (decision 25)
    retroarch/  libretro/libretro.mk (the shared macro)  libretro-<core>/ (one per core, E.5)
    ppsspp/  scummvm/  sdl3/ and sdl2-compat/ (only if the pinned Buildroot lacks them)
  src/                     small C tools (FlipBase's four files arrive with the merge)
    thimsy-session/  thimsy-padd/          section F
  tools/
    defconfig.py  lint_packages.py  check_rcs.py  build_guard.sh  assets_allowlist.py
    gpt_check.py  fit.py
    thimsy-import/         E.7 (deferred, section G)
  tests/
    run.sh  test_*.py  thimsy_import/  qemu/  fixtures/
  docs/                    card-layout.md, boot-chain.md, init.md, families/rk3566.md
  gpl/
    COPYING (GPL-2.0)  README.md
    generated/<family>/linux/patches/  generated/<family>/linux/config  generated/<family>/dts/
    board/<family>/linux/patches/      FlipBase's RK3566 patches and Thimsy's (Phase 2a)
    board/<family>/uboot/patches/      FlipBase's U-Boot series, the RG353PS fork's two fixes
    package/                           recipes ported from GPL sources (rocknix-joypad later)
    patches/<package>/                 BR2_GLOBAL_PATCH_DIR: patches to third-party packages
```

`gpl/patches/` (decision I9) holds Thimsy's patches to third-party packages (the RetroArch `drm_ctx.c` rotation patch, the SDL3 KMSDRM rotation patch, Batocera-style PPSSPP and RetroArch DRM fixes). Each patch carries its upstream's licence (GPL-3.0 for RetroArch, zlib for SDL), as plan 3.3 says a patch does; `gpl/README.md` says so, so `gpl/` is "copyleft and third-party patches", not GPL-2.0 only.

`libthimsy/` and `nano/` stay at the repository's top level. Buildroot's `legal-info` saves no source for a `_SITE_METHOD = local` package (it only warns), so the recipes do not build them from the working tree: `linux/build.sh` first writes `git archive` tarballs of the tracked files at `HEAD` (`libthimsy-<sha>.tar.gz`, `nano-<sha>.tar.gz`, `nano-assets-<sha>.tar.gz`) into the download directory, and the recipes use `_SITE_METHOD = file` with those names, so `legal-info` saves them and nothing untracked (a developer's scraper credentials, build outputs) can reach an image. A public release also publishes `git archive` of the whole repository at its tag. At a later `git subtree split --prefix=linux`, those sites become git sites at a pinned commit.

### E.2 Boards

- **RK3566** (`board/rk3566/`), wave 0: the ARC-D (by the SD-only route Phase 0 proves) and the ARC-S. One hardware pack; per-device `device.json` overrides. U-Boot: mainline `anbernic-rgxx3-rk3566_defconfig` plus FlipBase's series and the two RG353PS fixes (plan 3.2), built from rkbin's DDR blob as TPL; the BL31 per decision 18. Kernel: ROCKNIX's RK3566 version at the tag chosen at Phase 2a's start (Q30's rule), the hand import under `gpl/generated/rk3566/`, FlipBase's generic patches under `gpl/board/rk3566/linux/patches/`, `board/rk3566/linux.fragment` on top.
- **QEMU aarch64 virt** (`board/qemu-aarch64-virt/`), test only (decision I8): the v8.0 tier root unchanged, a hardware pack with a mainline `defconfig` plus `linux.fragment` (virtio block, GPU, input, net built in; EROFS), no U-Boot (QEMU's `-kernel`), and a `mesa-swrast` GPU pack (softpipe, no LLVM) so Nano draws on `virtio-gpu` without virgl. It replaces AMD64 as the first QEMU target: it boots the same aarch64 tier root the devices run, where an AMD64 target would need a third userland. AMD64 stays in the plan for later.

### E.3 Packages

| Package | Site | Builds | Notes |
|---|---|---|---|
| `libthimsy` | file: `git archive` of `libthimsy/` (E.1) | CMake: static library into staging, `thimsy-prop` into the target | MIT |
| `thimsy-nano` | file: `git archive` of `nano/` (E.1) | CMake targets `thimsy-nano` and `thimsy-nano-scrape` | depends on `libthimsy`, `mesa3d` (GBM, EGL, GLES), `libdrm`, `freetype`, `libpng`, `jpeg` (libjpeg-turbo), `libwebp`, `giflib`, `zlib`, `tinyxml2`, `libcurl`, `openssl`, `alsa-lib`. `_LICENSE = Apache-2.0, GPL-2.0+ (NanoOskKanji.h, derived from SKK-JISYO), Zlib (nanosvg), MIT (rcheevos)`, plus GPL-2.0 for `liba52` only when `NANO_WITH_AC3=ON` (off on Linux). With the Kanji table built in, the binary is distributed as a GPL-3.0-compatible combination with its full source published *(guess, not legal advice; plan 3.3's question stays open for the owner)*. `nano/NOTICE` lists `liba52/`, `NanoAc3.*` and `NanoOskKanji.h` with their licences |
| `thimsy-data` | file: `export-assets` output, filtered | installs to `/usr/share/thimsy/system/etc/` from an **allowlist** that `linux/tools/assets_allowlist.py` generates from `ASSETS.md`'s KEEP rows, never by mirroring `Android.bp`'s `prebuilt_etc` modules; Nano falls back where an asset is missing (the replacement work, such as the Rodin font, lands under `ASSETS.md`'s plan) | licences per `ASSETS.md`; the controller art is CC BY-NC 4.0 |
| `thimsy-session`, `thimsy-padd` | local `src/...` (in `linux/`, which `legal-info`'s published `linux/` tree covers) | CMake | MIT; section F |
| `thimsy-fonts` | upstream font releases, pinned with hashes | installs to `/usr/share/fonts/thimsy` under Android's file names | Apache-2.0 and OFL; replacements for DroidSans and NotoColorEmoji (plan 5.2) |
| `thimsy-gpu-pack` | none | packs the driver files of one Mesa build into `gpu-<driver>.erofs` | decision 25 |
| `retroarch` | GitHub `libretro/RetroArch` at the Android build's RetroArch version *(the version to read from the Android build's APK)* | autotools: KMS, EGL, GLES 3, ALSA, `libudev` (libudev-zero), no X11, Wayland, Qt or PulseAudio | GPL-3.0; patches in `gpl/patches/retroarch/` |
| `libretro-<core>` | GitHub, per core | the shared macro in `package/libretro/libretro.mk`: `$(eval $(libretro-core-package))` with per-core `_MAKE_OPTS` (for example YabaSanshiro's `platform=arm64_cortex_a53_gles3`, plan 6.2) | each core's own licence; non-commercial cores marked honestly for `legal-info` |
| `ppsspp` | GitHub, 1.20.4 (the Android app's) | CMake: GLES, EGL, SDL through `sdl2-compat`, no Vulkan on Mali | GPL-2.0+ |
| `scummvm` | GitHub, the Android app's version | configure: SDL back end, GLES 2 | GPL-3.0+ |

**Cores.** The alpha set (8, decision 10's "RetroArch only"): `snes9x`, `genesis_plus_gx`, `picodrive`, `mgba`, `sameboy`, `mesen`, `fceumm`, `pcsx_rearmed`. The v1 set adds plan 6.2's Linux column: `fbneo`, `mame2003_plus`, `mamearcade` (2 GB images), `stella`, `prosystem`, `a5200`, `gearcoleco`, `bluemsx`, `mednafen_vb`, `mednafen_ngp`, `mednafen_pce_fast`, `mednafen_wswan`, `mednafen_lynx`, `fake08`, `opera` (Medium), `yabasanshiro` (Medium), `mupen64plus_next` (`GLES3=1`), `melondsds`, `flycast` (Medium). That is 27 cores, not the plan's "10 to 15": the core table of open question 10 grew it.

**Core commits.** `mods/retroarch-cores.lock` pins buildbot binaries by SHA-256, not source commits, so "built from the commit the lock pins" (plan 6.2) needs a new file: `mods/retroarch-cores.src.lock` (core, repository, commit), written by `scripts/core_sources.py` from each locked Android build's embedded version string (`library_version` usually carries the short git hash *(guess, per core)*), read from the build cache's downloads. Until it is generated (it needs the cache, and the lookup of full hashes needs the network), the core recipes carry `_VERSION = TODO` and `lint_packages.py` reports them as pending, not passing (decision I10).

### E.4 Defconfigs and fragments

Buildroot reads one defconfig per tree. Thimsy keeps fragments as the source and commits the merged defconfigs that Buildroot reads (`make BR2_EXTERNAL=linux thimsy_v8_2_defconfig` works as usual):

| Fragment | Holds |
|---|---|
| `common.fragment` | aarch64, glibc toolchain (Buildroot's own, built once and reused as the shared SDK), BusyBox init, libudev-zero, `BR2_PER_PACKAGE_DIRECTORIES`, ccache, `BR2_GLOBAL_PATCH_DIR` to `gpl/patches`, no initramfs, EROFS host tools, `BR2_ROOTFS_OVERLAY`, post-build and post-image scripts |
| `tier-v8_0.fragment`, `tier-v8_2.fragment` | the CPU tuning (`BR2_cortex_a53`, `BR2_cortex_a55`) and the userland: `thimsy-nano`, `libthimsy`, `thimsy-session`, `thimsy-padd`, `thimsy-fonts`, `thimsy-data`, alsa-utils, wpa_supplicant, dropbear, exfatprogs, e2fsprogs |
| `gpu-panfrost.fragment`, `gpu-swrast.fragment` | Mesa with the driver set, host LLVM only for Panfrost's shader compiler |
| `apps-alpha.fragment`, `apps-v1.fragment` | RetroArch and the cores (E.3), PPSSPP and ScummVM in v1 |
| `hw-common.fragment` | kernel and U-Boot pinning style, `linux-firmware` selection rules (decision 24) |
| `hw-rk3566.fragment`, `hw-qemu-aarch64.fragment` | kernel version and config sources, DTBs, U-Boot board |
| `dev.fragment`, `release.fragment` | serial console, adbd gadget, dropbear by default; or none of them: no adbd, no serial getty, dropbear off (key-only when a user turns SSH on), the root password locked (`BR2_TARGET_ENABLE_ROOT_LOGIN` off; Buildroot's default root password is empty) |

`targets.json`: `thimsy_v8_0` = common + tier-v8_0 + gpu-panfrost + gpu-swrast + apps-alpha + dev; `thimsy_v8_2` = common + tier-v8_2 + gpu-panfrost + apps-alpha + dev; `thimsy_v8_0_release` and `thimsy_v8_2_release` = the same with `release` instead of `dev`, marked `"publish": true`; `thimsy_rk3566_hw` = common + hw-common + hw-rk3566 + `generated/rk3566/defconfig.fragment`; `thimsy_qemu_aarch64_hw` = common + hw-common + hw-qemu-aarch64. `build.sh --publish` refuses a target that is not marked for publishing, and decision 16's public alpha is built from the release targets only. `defconfig.py` merges them with Buildroot's `merge_config.sh` semantics (later wins; `# override:` marks an intended change) and writes the defconfigs.

### E.5 Init and the 3 s boot

FlipBase's `/init` mounts `proc`, `sys` and `devtmpfs` and execs BusyBox `init`.

```
/etc/inittab
  ::sysinit:/etc/init.d/rcS
  ::respawn:/usr/libexec/thimsy/session        # thimsy-session, which starts Nano at once
  ::shutdown:/etc/init.d/rcK
  ::ctrlaltdel:/sbin/reboot
```

`rcS` runs `S*` in order; together they must stay well under the boot budget's share (*(guess)* 100 ms on RK3566; measured by the boot clock from the first build):

| Script | Does |
|---|---|
| `S00base` | `/run` and `/tmp` as tmpfs; the boot disk resolved first (the disk holding `root=PARTUUID=` from `/proc/cmdline`, else the disk of the mounted root), then the data partition found by GPT name (`PARTNAME=data` in `/sys/class/block/*/uevent`) **on that disk only**, never on an `mmcblk*` whose `device/type` is `MMC`, and mounted at `/storage` (ext4, `noatime`); `/run/thimsy/{props,log}`; on first boot the config tree, the `/data` tree of B.5 (`/storage/.config/thimsy/data/...`) and `/storage/emulated/0`, `/storage/self/primary` |
| `S01props` | `thimsy-prop boot`: `ro.*` from `device.json`, `install.json` and `build.props` |
| `S02padd` | `thimsy-padd &` (background; Nano opens the pad when it appears) |
| `S03modules` | the family's out-of-tree modules from `device.json` (none on RK3566) |

Everything else waits for Nano's first frame: `thimsy-session` starts Nano, waits for `sys.thimsy.nano.first_frame=1` (set by Nano's boot-instrumentation hook, plan Phase 2b), then runs `/etc/init.d/deferred/D*` in the background: `D10cards` (coldplug scan, `fsck`, mount at `/storage/<VOLID>`; the uevent listener then takes over), `D20sound` (`alsactl restore`, once `device.json`'s `audio.card_index` card is there), `D30net` (`wpa_supplicant` only if Wi-Fi is on, `udhcpc`), `D35bluetooth` (`dbus-daemon` and `bluetoothd` only on a device whose `device.json` has `"bluetooth": true` and only if Bluetooth is on), `D40time` (`ntpd`), `D50ssh` (dev images), `D60firstboot` (on the boot disk only, when its GPT has no entry 5: create entry 5 over the free space typed Linux reserved, `mkfs.exfat -L THIMSY` on it, then retype it Microsoft basic data, each step re-run safely after a power cut; E.6). `rcK` stops the game's process group, syncs and unmounts.

### E.6 Card layout and tooling

Partition types per plan 8.2 and open question 8; names and the data type decided here (decision I7):

| Entry | Name | Type | Type GUID | RK3566 size (spare) |
|---|---|---|---|---|
| raw | idbloader | (no entry) | at sector 64 | before LBA 16384 |
| 1 | `uboot` | Linux reserved | `8DA63339-0007-60C0-C436-083AC8230908` | 8 MiB (8 MiB) |
| 2 | `boot` | Linux reserved | `8DA63339-0007-60C0-C436-083AC8230908` | 40 MiB (40 MiB) |
| 3 | `root` | Linux root (arm64) | `B921B045-1DF0-41C3-AF44-4C6F280D3FAE` | 1 GiB (1 GiB) *(guess)* |
| 4 | `data` | Linux home | `933AC7E1-2EB4-4F13-B844-0E14E2AEF915` | 256 MiB *(guess)* |
| 5 | `games` | Microsoft basic data | `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7` | not in the written image (which ends 1 MiB after `data`): created at first boot from 2 MiB after `data` over the free space, with exFAT `THIMSY`, or re-adopted from an earlier install |

**The image's tail** (review finding device-safety-2): the written image ends 1 MiB after `data` (RK3566: at 2409 MiB, `size` in `genimage.cfg`), its backup GPT in its last 33 sectors, and `D60firstboot` starts `games` 2 MiB after `data` (2410 MiB). Writing the image again over a used card therefore leaves the old library's exFAT whole, and D60 re-adopts an exFAT labelled `THIMSY` that it finds at that start (entry 5 over its extent, basic data, no format); anything else there is formatted. `gpt_check.py` refuses an image that does not end exactly 1 MiB after its last entry, and a booted card whose games entry starts earlier. `linux/docs/init.md` has the details.

Spares stay unallocated until an update writes them (plan 5.2). The exFAT filesystem is made on the device, because exFAT can't be grown and creating it on the PC is what the installer avoids (plan 8.2); the name `games` and label `THIMSY` are Phase 3's to confirm (plan 7: the data-partition label).

**No unformatted basic-data entry, ever.** An entry typed basic data with no filesystem is what Android's vold reports as unmountable, and its format wizard repartitions the whole card (plan risk 13); on the ARC-D, Android may see the card before Linux first boots, and Windows offers to format such a partition as soon as the image is written (an NTFS format would then stop first boot from making exFAT). So the written image has entries 1 to 4 only and leaves the rest of the card unallocated. `D60firstboot` creates entry 5 typed Linux reserved, makes the exFAT, then retypes it basic data: at every moment, a basic-data entry has a filesystem. **This keeps Windows' prompt away, not Android's** (review finding device-safety-3, open for the owner): vold makes no volume for entries 1 to 4 (Linux reserved, arm64 root, Linux home), so until first boot the card scans with no volume at all, and SystemUI's `StorageNotification` then posts "Unsupported SD card" with the same `StorageWizardInit` format flow. Setting the card up there erases Thimsy; the installer and the user docs must say not to. A fix in the image (a small basic-data entry that already has a filesystem, so vold mounts something) needs gpt_check.py's vold rule changed and is the owner's decision. The GPT is rewritten with FlipBase's `gptgrow.c` (taken at the merge; *(guess that it can add an entry as well as grow one)*, else util-linux `sfdisk`), on the boot disk only (E.5).

- **The alpha's images come from Buildroot's `genimage`**, as decision 13 says (I6 is withdrawn): `board/<family>/genimage.cfg` per family, run by `post-image.d/30-genimage.sh`. `linux/tools/gpt_check.py` (a small, independent GPT reader: protective MBR, both headers and their CRCs, entry array CRC, names, type GUIDs, alignment, the vold rule) checks every image `genimage` writes and fails the build on a breach. `.bmap` files come from Buildroot's host `bmap-tools` *(guess that the pinned Buildroot packages it)*. The Python card assembler, `card.py`, is written only when the installer's Rust crate needs test vectors (Phase 4).
- `linux/tools/fit.py`: writes the `.its` (zstd kernel, the device's DTB, bootargs, no hash nodes, plan 8.2) and calls `mkimage`; the 512-byte header in front of the FIT follows FlipBase's `tools/mkfit.py`, taken at the merge *(format not yet read)*.

### E.7 `thimsy-import`

`linux/tools/thimsy-import/`, Python 3.8, standard library only:

```
thimsy_import/
  cli.py           thimsy-import --rocknix DIR --tag TAG --family RK3566 [--out linux/] [--check]
  rocknix.py       the tree: paths, tag, commit
  configxml.py     projects/ROCKNIX/config.xml: products, sub-images, DTB names, fdt and overlays
  options.py       devices/<FAMILY>/options: KEY="value" lines read as data, never executed
  linuxmk.py       packages/linux/package.mk: the family's case branch as text, PKG_VERSION, patch dirs
  patches.py       the flattened series in scripts/unpack's order, ROCKNIX's file names, provenance lines
  kconfig.py       the base config, CONFIG_EXTRA_FIRMWARE blocks, the conformance check of Thimsy's fragment
  firmware.py      kernel-firmware.dat, filtered by the firmware policy (decision 24)
  quirks.py        quirk files' KEY="value" facts into device.json rows; no script text copied
  inputplumber.py  capability YAML read for pad facts (a small reader for the YAML subset used)
  devicejson.py    device.json writer and its schema check (schema/device.schema.json)
  guards.py        hash guards per family case block (generated/<family>/guards.json)
  emit.py          deterministic writes to generated/<family>/ and gpl/generated/<family>/
```

`device.json` (one per product; the facts plan 5.3 and 8.4 need): `id`, `family`, `model` (device-tree `model`), `dtb`, `ram_mb`, `panel` (`width`, `height`, `rotation`, `rotation_source`), `touch`, `pad` (`producer`: `padd` or a kernel joypad; `sources` with match rules; `buttons` map to the normalised layout; `face_buttons` 4 or 6; `sticks`; `labels`; `face_layout`; `hotkey`), `audio` (`card`, `playback` paths), `leds`, `fans`, `power` (power-key node, wake sources), `profile_default`, `status`.

**Deferred, and fixtures from real shapes** (decision I11, revised at the review): the importer is not built against guessed shapes. G2c waits until ROCKNIX's real files can be read: first by reading them over the web (GitHub's file pages for `projects/ROCKNIX/config.xml`, a family's `options`, `packages/linux/package.mk`, a `kernel-firmware.dat`, a quirk file), which is allowed now, then by a real clone when fetching is allowed. The fixture tree under `linux/tests/fixtures/rocknix-mini/` (two families, three products, eight small patches with and without `From:` lines, a licensed and a proprietary firmware entry) is then cut down from those shapes, each fixture file naming the ROCKNIX path and commit it imitates. Phase 2a's hand import of RK3566 does not need the importer. Expected outputs are golden files under `fixtures/expected/`.

Tests: the import equals the golden output; a second run writes identical bytes; every patch carries its provenance line and keeps its name; a changed case block stops the import (guard); the proprietary firmware is left out and reported; a fragment that flips a Thimsy-critical option fails the conformance check; `--check` reports differences without writing; no quirk script text appears in any output (a test greps for shell syntax).

### E.8 `build.sh` and the guard

`linux/build.sh <target>` (a tier, a GPU pack, a hardware pack or `card <device>`) runs Buildroot in Docker in the `thimsy-linux` WSL distro (decision "Build distro"). Before anything else it runs `tools/build_guard.sh`, which refuses (exit 3) when:

- the lock `/mnt/wsl/thimsy-build.lock` is held (`flock -n`); `/mnt/wsl` is shared by every WSL2 distro in the VM, so a Linux build and any build that takes the same lock exclude each other;
- `MemAvailable` in `/proc/meminfo` is under 9 GB: the WSL2 VM's memory is shared by all distros, so a running AOSP build shows up there even though its processes are in another distro *(guess at the threshold: the AOSP build's peak leaves less)*;
- `docker ps` lists a container named like the Android build's (`thimsy-*build*`);
- `wsl.exe -l --running` (through WSL interop, from inside the `thimsy-linux` distro or WSL `Ubuntu`) lists `thimsy-aosp`, where the Android build runs natively and where neither the lock nor `docker ps` can see it *(guess that interop is on in both distros; if `wsl.exe` can't run, the guard refuses and says so)*;
- the VM-wide one-minute load in `/proc/loadavg` (all distros share one kernel) is above 8, half the threads.

`--light` is the mode the C++ test Makefiles use (C.3): it allows only the small targets while any of the Android checks fires. `--force` asks for a typed confirmation (`--force=I-checked-no-android-build-runs`) and skips only the memory and load checks; it never skips the lock or the distro and container checks. The Android build's scripts are not changed to take the lock now (the running job must not be touched); adding the lock to `scripts/build.sh` is scheduled for right after the current AOSP job ends (step G10). Nothing in this design runs `build.sh`.

### E.9 The QEMU boot test (written, not run)

`linux/tests/qemu/test_qemu_boot.py`, run only with `THIMSY_QEMU=1` and a built `qemu-virt` card:

```
qemu-system-aarch64 -M virt -cpu max -smp 4 -m 1024 -nographic
  -kernel Image -append "root=/dev/vda3 rootfstype=erofs ro init=/init console=ttyAMA0"
  -drive file=card.img,if=virtio,format=raw -device virtio-gpu-pci -device virtio-keyboard-pci
  -qmp unix:qmp.sock,server,nowait
```

It waits (600 s under TCG) for the serial markers `thimsy: rcS done`, `thimsy-session: nano started`, `nano: first frame`; then sends Enter through QMP `send-key`, which padd maps to `BTN_SOUTH` through `qemu-virt.json`'s pad section, and waits for Nano's log line for the opened menu; then `sys.powerctl` through `thimsy-prop` over the serial shell, and a clean power-off. It also prints the boot clock's timeline.

---

## F. `thimsy-session` and `thimsy-padd`

**Language: C++17** for both (decision I12), not C as the plan sketched. They share `libthimsy` (store, uevent parser, JSON, atomic files) with Nano's Linux platform, the same compiler, and the same test stand-in. Both are built without exceptions in their I/O loops' hot paths *(style)*, link statically to `libthimsy`, and stay small (*(guess)* 1,500 to 2,500 lines each).

**Shape:** each is a pure core (state machines and policy, with time and I/O injected) plus a thin I/O layer behind interfaces; the core is tested on the host, the I/O layer on the device.

### F.1 `thimsy-session`

Responsibilities (plan 5.2): start and supervise Nano; drives and volumes; hotplug; power, volume and lid keys; sleep and wake; the quit hotkey; the deferred start of services; requests from Nano.

```
linux/src/thimsy-session/
  CMakeLists.txt  Makefile (host check)
  src/main.cpp                  wiring; one epoll loop
  src/core/KeyMachine.*         power short press (sleep), long press (power-off when Nano is not running,
                                FlipBase's pwrkeyd idea), volume, lid; clock injected
  src/core/SleepPlan.*          the ordered steps: SIGSTOP the game's group, stop wpa_supplicant, write
                                mem_sleep and state, and the reverse; per-family hooks from device.json
  src/core/VolumePolicy.*       which partitions are drives (plan 7: per partition; eMMC (device/type MMC)
                                and the boot disk's partitions out, except the boot disk's games
                                partition; the boot disk resolved as S00base does), volume id from the
                                filesystem serial, mount point /storage/<VOLID>, fsck first
  src/core/Supervisor.*         Nano's start, restart and back-off; the game's process group from
                                sys.thimsy.game.pid; deferred services after first frame
  src/core/Requests.*           sys.thimsy.session.request: sleep, poweroff, reboot, quit-game, eject:<id>,
                                scrape (starts thimsy-nano-scrape, as gammaos-nano.rc's scrape_run trigger)
  src/core/NanoJail.*           Nano's mount namespace: /dev/null over the eMMC's block nodes (B.5)
  src/io/                       Netlink, Evdev (KEY_POWER from rk805-pwrkey, volume, SW_LID), Sysfs,
                                Blkid (reads superblocks itself: exFAT, FAT, ext4 serials and labels),
                                Mounter (mount(2), fsck through exec), Proc
  tests/                        core tests with fakes; tests/e2e/ (below)
```

**Interfaces with Nano:** only the config store and one file. Nano reads `/run/thimsy/volumes.json` (written atomically; `sys.thimsy.volumes.serial` bumps on change) and writes `sys.thimsy.session.request`; the compat control table (B.2) turns Nano's existing power writes (`service.bootanim.nano_action=reboot|shutdown`; Nano never writes `sys.powerctl`) into requests. Session writes `sys.thimsy.hud.volume` when a volume key changes the level, for Nano's slider.

**Host tests:** the pure cores with a fake clock and scripted inputs (a long press at 2.9 s and 3.1 s; a lid closing during a game; a card removed mid-mount; a partition on the root device with and without the games name; a second card that also has a `data` partition; an eMMC whose `device/type` is `MMC`; two cards with the same serial, plan 7 row 9). **Host E2E:** `thimsy-session --root DIR --fake-hw` runs the real binary against a directory tree that stands in for `/sys`, `/dev` and `/run` (power-supply and backlight files, a uevent replay file, a FIFO for key events, a fake `mount` and `fsck` that log), and the test asserts the files it writes (`mem_sleep`, `state`, `brightness`, `volumes.json`) and the commands it ran. No root needed.

### F.2 `thimsy-padd`

Responsibilities (plan 5.3): open and grab the source nodes `device.json` names; merge them into one uinput pad with the Thimsy identity; apply remaps, axis inversion and calibration; intercept mode; force feedback to the vibrator; hide the source nodes.

```
linux/src/thimsy-padd/
  src/core/PadSpec.*      device.json's pad section: sources, button map, face buttons, sticks, inversion,
                          calibration, hotkey
  src/core/Merger.*       SourceEvent{source, type, code, value, timeNs} -> virtual pad events; held-state
                          table; intercept on: a release for every held button and axes to centre, then
                          nothing forwarded; intercept off: resume from the current state
  src/core/Calibrate.*    deadzone and range per axis
  src/core/Rumble.*       FF upload, play and erase -> vibrator commands (pwm-vibrator, pm8xxx)
  src/io/EvdevSource.*    open, EVIOCGBIT capability check, EVIOCGRAB, unlink the node after opening
  src/io/UinputSink.*     UI_DEV_SETUP, UI_ABS_SETUP, FF requests
  src/io/Control.*        sys.thimsy.padd.intercept through libthimsy's watch
  tools/padd-replay.cpp   reads a trace ("<ms> <source> <type> <code> <value>" lines, and "intercept 1|0")
                          and prints the events the core emits
```

**The identity** (decision I13): name `Thimsy Gamepad`, bus `BUS_VIRTUAL` (0x06), vendor 0x5448, product 0x0001, version 1. On `BUS_VIRTUAL` the ids can't collide with a USB device's in SDL's GUID; no USB vendor id is claimed. The layout follows the kernel's `Documentation/input/gamepad.rst`: `BTN_SOUTH`/`EAST`/`NORTH`/`WEST`, `BTN_TL`/`TR`/`TL2`/`TR2`, `BTN_SELECT`, `BTN_START`, `BTN_MODE`, `BTN_THUMBL`/`THUMBR`, the d-pad as `ABS_HAT0X`/`HAT0Y`, sticks as `ABS_X`/`Y`/`RX`/`RY`. The ARC's C and Z become `BTN_THUMBR` and `BTN_THUMBL`, as ROCKNIX maps them (plan 5.3); the facts live in its `device.json`. One `gamecontrollerdb` line and one RetroArch autoconfig file (`board/common/rootfs-overlay/usr/share/thimsy/input/`) serve every built-in pad.

**Host tests:** the cores with scripted events (a button held across intercept is released within the same batch; an axis returns to centre; a remap; an inverted axis; calibration bounds; a rumble effect uploaded and played maps to the right vibrator command). **Host E2E:** `padd-replay` against golden traces in `tests/traces/` (the ARC's three sources; a `rocknix-joypad`-style single source; an external pad ignored). uinput, grabbing and unlinking run as root on a device only: that is Phase 0's (c) item and open question 4.

---

## G. Work breakdown

Each step lists the files it owns, what it depends on, and what "done" means.

**How parallel steps work.** Each agent works in its own worktree (`git worktree add ../thimsy-os-linux-<step> -b step/<step> linux`), never in the shared one, so a step's tests can't pass because of another step's uncommitted files. The orchestrator stages only the step's owned paths (`git add -- <owned paths>`, never `git add -A`), merges or cherry-picks the step branches into `linux` one at a time, lets the merge driver regenerate the patch (A.5), and before pushing re-runs the runnable tests in a clean worktree checked out at the new commit. Every Makefile builds out of tree under `$HOME/.cache/thimsy/<component>`, so no object file lands in the repository.

**What "done" means while the AOSP job runs.** The WSL distro has no development packages and nothing may be downloaded (section 0). A step whose checks need them is done as **"written, not compiled"**: its report lists each skipped check with the reason (`missing <header>`, `deferred: heavy target`), and the orchestrator records it as skipped, never as passing. Until G10, the checks that count are: the Python tests on Windows, `check-compat`, `check-unit-0`, `libthimsy`, `thimsy-session` and `thimsy-padd` in WSL behind the memory guard.

**Stage 0 (serial)**

| Step | Owns | Done when |
|---|---|---|
| **G0** `nano/` and the tools | `nano/**` (the import), `tools/nano-tree/**` (with `tests/`, `upstream.json`, `block.sha256`, `props.txt`, `control-keys.json`, `path-roots.json`), `tools/android-launcher/fetch_nano.sh` (wrapper), `.gitattributes` (`/nano/** -text`; the patch's `merge=nanopatch -diff linguist-generated`), `.gitignore` (`/work/nano-base/`, `/work/nano-assets/`, the `!/nano/**/*.7z` re-includes), `tests/unit/test_nano_patch_consistent.py`, `tests/e2e/test_nano_patch.py`, `tests/unit/test_nano_units.py` and `test_screen_arrangement.py` (read `nano/` first) | `nano/` holds A.2's set (the six `.7z` fixtures included) with the index modes set; `regen --check` passes under Windows git and again under WSL's git 2.25 (or Windows git is declared the reference, A.3); the merge driver is installed and its scratch-repository test passes; all A.6 tests pass on Windows; the ratchet baselines (H) are recorded; the old tests still pass |

**Stage 1a (parallel after G0)**

| Step | Owns | Done when |
|---|---|---|
| **G1a** libthimsy | `libthimsy/**` | store (with observers, 0600 persist file, redaction), atomic file, watch, uevent, JSON, `thimsy-prop`, the gtest stand-in; `make -C libthimsy check` passes in WSL behind the guard. JSON: `libthimsy` keeps its own small reader because `NanoJson` is Apache-2.0 and Nano-specific (its fast-math notes), while the MIT tools should stay single-licence (decision I5) |
| **G2d-skeleton** recipe skeletons | `linux/package/Config.in`, `linux/package/*/Config.in` (one symbol per package of E.3, no recipes yet) | the symbols exist, so G2a's test can check against them |
| **G2a** Linux skeleton (after G2d-skeleton) | `linux/LICENSE`, `README.md`, `external.*`, `Config.in`, `configs/**` (release targets included), `tools/defconfig.py`, `tools/build_guard.sh`, `build.sh`, `linux/tests/run.sh`, `test_defconfig.py`, `test_build_guard.py`, `gpl/COPYING`, `gpl/README.md` | the defconfigs regenerate from fragments; no publish target includes `dev`; the guard tests pass in WSL |
| **G2b** card checks | `linux/tools/{gpt_check,fit}.py`, `board/*/genimage.cfg`, `linux/tests/test_{layout,gpt_check,fit}.py` | E.6's checks pass on both boards' `genimage.cfg` |
| **G2e** init | `linux/board/common/rootfs-overlay/**`, `linux/board/common/post-build.sh` and `post-image.sh` (the runners only), `linux/board/common/post-build.d/20-*`, `30-*`, `linux/board/rk3566/common/**`, `linux/tools/check_rcs.py`, `linux/tests/test_rcs.py`, `linux/docs/init.md` | E.5's scripts, boot-disk scoping included; `test_rcs.py` passes |

**Stage 1b (after G1a)**

| Step | Owns | Done when |
|---|---|---|
| **G1b** compat shims | `nano/compat/include/**` (with `nanocompat/bionic.h` and the image decoder header), `nano/compat/src/**`, `nano/test/host/compat/**`, `nano/test/host/compat/compat.mk` (included by G1c's Makefile) | B.2's shims; `check-compat` passes in WSL (the image decoder's tests skip until its libraries exist) |
| **G2d** recipes | `linux/package/**` beyond the skeletons, `linux/gpl/package/**` (empty), `linux/tools/lint_packages.py`, `linux/tools/assets_allowlist.py`, `linux/tests/test_packages.py`, `scripts/core_sources.py`, `tests/unit/test_core_sources.py` | every package of E.3 written; lint passes except the cores' pending commits, reported as such; the asset allowlist excludes every REPLACE and ASK row |
| **G3a** padd | `linux/src/thimsy-padd/**` | F.2's host tests and replay goldens pass in WSL |
| **G3b** session | `linux/src/thimsy-session/**` | F.1's host tests and fake-hardware E2E pass in WSL |

**Stage 1c (after G1b)**

| Step | Owns | Done when |
|---|---|---|
| **G1c** host build | `nano/build/sources.txt`, `nano/build/unit-closure.txt`, `nano/test/host/Makefile`, `nano/test/host/DEPS.md`, `tools/nano-tree/include_census.py`, `tools/nano-tree/tests/test_nano_sources.py`, `tools/nano-tree/tests/test_nano_host.py` (the Python wrapper), `tests/run.sh` (the `nano`, `linux` and `cxx` modes), the repoint of `tests/unit/test_nano_units.py` at `libthimsy/testing` | `check-unit-0` builds and passes every test program whose closure needs no outside library; each other program is listed with its missing header; `test_nano_sources.py` passes |

**Stage 2**

| Step | Owns | Done when |
|---|---|---|
| **G4** `nano_core` for the host (serial, one agent) | `nano/compat/stubs/**`, `tools/nano-tree/stubs-budget.txt`, `tools/nano-tree/include-census.json`, `nano/build/sources.txt` (the `[core]`/`[drm]`/`[firmware]` split), `nano/test/host/stubgen/**`, `tools/nano-tree/tests/test_nano_stubs.py`. **No existing Nano file is edited** | the split is made from the include census; every Android-only header reached from `[core]` has a shim or a stub; the stub budget is recorded. Written, not compiled: `syntax-core` runs in G10 |
| **G9a** DRM back-end split | `NanoMenuDrm.cpp` (the moved lines only), new `NanoMenuDrmBuffers.cpp` and `.h`, `Android.bp` (`gammaos-nano` `srcs` and `drastic_nano_shared_srcs`), the surface baseline, B.8's contract checked against the moved code | the move passes H.3's review rule; the cache-free consistency test passes; the commit carries its `android-surface:` line and is flagged for cherry-pick to `nano-144` after the AOSP job. If the split is not mechanical, the step stops and G9 takes the full-substitute fallback (B.5) |
| **G6** CMake (after G4) | `nano/CMakeLists.txt`, `nano/cmake/**` (with `sources.cmake`), `libthimsy/CMakeLists.txt` | the CMake files read `sources.txt` directly; configuring needs CMake and is not run here |
| **G9** Linux platform, layer 3 (after G9a) | `nano/platform/linux/display/**` (`DrmBuffersGbm.cpp`), `nano/platform/linux/ShellGate.cpp`, `nano/platform/linux/ControlKeys.cpp`, `nano/platform/linux/main_linux.cpp`, `linux/board/common/post-build.d/10-android-links.sh`, `tools/nano-tree/tests/test_control_keys.py` | the shell gate's and the control table's tests pass; the symbol-set test passes for `DrmHost.cpp` and any substitute. Written, not compiled: `-fsyntax-only` against libdrm, GBM and EGL runs in G10 |

**Stage 3**

| Step | Owns | Done when |
|---|---|---|
| **G5** the seam and the headless E2E | `nano/NanoPlatform.h` (no services), `nano/NanoPlatform.cpp` (`install`, `get`), `nano/platform/host/**` (with `DrmHost.cpp` and `FakeEvdev.cpp`), `nano/test/host/glstub/**`, `nano/test/host/headless/**`, `nano/test/host/scenarios/**`, `tests/e2e/nano_jail.py`, `tests/e2e/test_nano_headless.py` | written, not compiled; the jail's runner passes its own tests (layout, read-only binds, FIFO devices) in WSL without Nano; the scenarios run in G10 |
| **G7-main** (first P4 step) | `main.cpp` (`install(createPlatform())`, the shared `nanoBootSequence()`), `Android.bp` (`NanoPlatform.cpp`, `platform/firmware/Platform.cpp`), `platform/firmware/Platform.cpp` | the Android surface updated with its `android-surface:` line (H.1); `main_linux.cpp` calls `nanoBootSequence()` |
| **G7-display** | `nano/platform/firmware/Display.cpp`; edits in `NanoMenu.cpp` (`readyToRun`), `NanoMenuRender.cpp`; `nano/platform/linux/display/Display.cpp`; `platform/host/FakeDisplay.cpp`; the `Display` declaration | H's rules met; headless scenarios unchanged |
| **G7-launch-a** (after G7-display: both edit `NanoMenu.cpp`) | new `NanoLaunchCommit.cpp`; edits gathering the handshake in `NanoMenu.cpp`, `NanoMenuXmb.cpp`, `NanoMenuPS3Menu.cpp`, `NanoMenuOverlay.cpp`, `NanoControlCenter.cpp`, `NanoMenuInput.cpp`, `NanoMenuSearch.cpp`, `NanoMenuState.cpp` (not `NanoButtonMap.cpp`, which only reads the keys and is in `drastic_nano_shared_srcs`) | behaviour unchanged: the same keys written in the same order (a scenario asserts the order) |
| **G7-launch-b** | `platform/{firmware,linux,host}/Launcher*.cpp`; `commitLaunch` swapped to `nanoplat::get().launcher`; the `Launcher` declaration | as above; `expect_launch` reads the fake launcher |
| **G7-audio** | `platform/*/Audio*.cpp`; edits in `NanoAudio.cpp`; the `Audio` declaration (pull model) | as above |
| **G7-volumes** | `platform/*/Volumes*.cpp`; edits in `NanoEsdeStore.cpp`, `NanoMenuFileExplorer.cpp`, `NanoSdMove.cpp`; the `Volumes` declaration | as above |
| **G7-power and actions** | `platform/*/Power*.cpp`, `platform/*/actions/**`; edits in `NanoMenuSystem.cpp`, `NanoControlCenter.cpp`, `NanoMenuNet.cpp`, `NanoMenuSettings.cpp`, `NanoMenuSetupWizard.cpp`; `nano/third_party/wpa_ctrl/`; the two declarations | as above |
| **G7-input** | `platform/*/InputPolicy*.cpp`; edits in `NanoMenuInput.cpp` (grab) only; the `InputPolicy` declaration | as above |
| **G7-caps** (landed as W6-caps) | edits in `NanoMenuSettingsTree.cpp` and the XMB menu builders that hide rows by `Cap` and by the settings dispatcher's `supports`; the `Cap` values they use; `platform/*/Settings*.cpp` and `Capabilities*.cpp` | rows of missing features hidden on Linux; Android unchanged |

The G7 steps touch disjoint call-site files except where two areas meet in one file (`NanoMenu.cpp`, `NanoControlCenter.cpp`, `NanoMenuInput.cpp`): those are serialised in the order listed. `NanoMenu.h` is not edited by any G7 step: services are reached through `nanoplat::get()`, not new members. No G7 step edits a file in `drastic_nano_shared_srcs`, `gammaos-nano-scrape`'s `srcs` or a host test's `srcs` to call `nanoplat::` (B.6's rule, a ratchet).

**Stage 4**

| Step | Owns | Done when |
|---|---|---|
| **G8** the Linux image's assembly, written | `linux/board/common/post-image.d/**`, `linux/board/rk3566/post-image.d/**`, `linux/board/qemu-aarch64-virt/**`, `linux/tests/qemu/**` | everything a `build.sh` run needs exists and passes the static tests; **no build is run** |
| **G2c** importer (deferred) | `linux/tools/thimsy-import/**`, `linux/tests/thimsy_import/**`, `linux/tests/fixtures/rocknix-mini/**` | when ROCKNIX's real files have been read (E.7); E.7's tests pass |

**Queued until the AOSP job has finished and downloads are allowed**

| Step | Does | Done when |
|---|---|---|
| **G10** verification | installs `nano/test/host/DEPS.md`'s packages in WSL `Ubuntu`; runs `check-unit`, `syntax-core`, `-fsyntax-only` of `nano/platform/linux/` and `headless` with its scenarios, behind the guard; moves any file G4 placed wrongly, with the reason; adds the build lock to `scripts/build.sh`; runs `regen --check` under both gits again | every check that was "written, not compiled" has run and passes, or has a listed failure with its fix step |

---

## H. Risks to the Android build, and the rules that limit them

The Cube's image can't be rebuilt now, so byte-identical Android output can't be shown by a build. What can be shown:

1. **Passes P0 to P3 change no byte of anything Soong compiles into the image, except G9a's move.** The patch changes only by adding files that no `Android.bp` module lists and no glob matches (and `fork_only` files don't reach it at all, A.2). `tools/nano-tree/tests/test_android_surface.py` makes this mechanical without a committed manifest. It reads the surface from `nano/Android.bp` with `androidbp.py`: every module that compiles or installs Nano's files, inside the Nano block or outside it (`gammaos-nano`, `gammaos-nano-scrape`, `nano-bootlogo`, the library modules, the filegroups `drastic_nano_shared_srcs` and `drastic_nano_layout_srcs`, `drastic-nano` itself, `nano-seedrng`, `nano-powerwatch`, the `cc_test_host` modules), their `srcs`, the headers they include transitively (quoted includes, and angle includes resolved through `local_include_dirs` and `include_dirs`), every `prebuilt_etc` source, `Android.bp` itself, `gammaos-nano.rc` and the shell scripts installed. It then compares, path by path, the patch's sections for those paths at `HEAD` with the same sections at `git merge-base linux nano-144`, both read from git objects, so no cache and no baseline file is needed. Every difference must be covered by a commit since the merge base whose message carries an `android-surface:` line naming the step and its review; the test lists the uncovered paths. Normal Android edits arriving from `nano-144` are on both sides of the comparison and cost nothing.
2. **The host compiles what Android compiles.** Every `[core]` file compiles on g++ (G10); the 24 tested files run their tests on the host. Clang's `-Werror` set differs from GCC's: once downloads are allowed, WSL gets `clang` and the Makefile gains a `syntax-clang` target with Soong's warning flags for every file of the Android surface.
3. **P4 edits are moves, not rewrites.** The firmware implementation of an area is the old code moved verbatim; the call site becomes one call. Review rule: `git diff --color-moved=zebra --color-moved-ws=allow-indentation-change` must show every removed line of the old code as moved, apart from the wrapper's own lines, which are listed in the commit message. A new behaviour on Android is never part of a P4 commit. **Every P4 commit, and G9a, is an Android-surface change**: it carries its `android-surface:` line, and G7-main comes first, so `install()` and the firmware platform exist before any call site uses them (B.7). No P4 commit may leave the Android build unlinkable: each adds its own `platform/firmware/<Area>.cpp` to `srcs` in the same commit.
4. **Ratchets** (`tools/nano-tree/tests/test_nano_ratchets.py`, `test_linux_ratchets.py`, `test_shell_sites.py` and `test_nano_stubs.py`, seconds). "Shared files" are the C and C++ files of `nano/` outside the fork-only globs and outside `platform/**` (`platform/firmware` is Android's own implementation, compiled only there). The set of property key literals in shared files equals its baseline (`tools/nano-tree/props.txt`; a deliberate change updates it, and a key arriving from `nano-144` is added by the merge driver's run, not by hand); shared files outside `compat/` (the shim layer) have no `__ANDROID__`, `__linux__`, `__BIONIC__`, `__GLIBC__` or `NANO_PLATFORM_*` conditional (budget 0); the stub budget only shrinks; every absolute-path root in shared files has a row in `path-roots.json` (B.5); every `gammaos-nano.rc` trigger and framework listener has a row in `control-keys.json` (B.2); no file in `drastic_nano_shared_srcs`, `gammaos-nano-scrape`'s `srcs` or a `cc_test_host` module's `srcs` includes `NanoPlatform.h` or names `nanoplat::` (B.6); the Bionic-only identifiers in shared files equal their list (B.2); the shared files that name `/dev/block` or `mmcblk` equal their list (B.5). **The Linux ratchets** (OA-4's migration step 0; ceilings in `tools/nano-tree/linux-ratchets.json`) count, over the Linux-compiled shared code (`nanotree.ratchets.linux_shared_sources`: `sources.txt`'s `[core]`, `[drm]` and `[scrape]` less the feature-switch files, which Linux turns off, plus the top-level `nano/*.h` and `*.inc`; never `platform/**`), what OA-4 says shared code must not hold: program starts (`system`, `popen`, `fork`, `exec*`, `posix_spawn`, per function), string literals whose first program is an Android tool, Android-only `#include`s beyond a portability allow-list (log, clocks, properties, RefBase, Thread, android-base file and strings), Android path literals per root (`/data`, `/storage`, `/system`, `/mnt`, `/sdcard`, ...), device and SoC literals (`fde60000`, `sprd_`, `soc-thermal`, `gpu-thermal`, `vop@`, `route-dsi`, `baseparameter`, `rfkill0`, `panel0-backlight`, `lcd-backlight`, `ums`, `sc98`, `sc99`, `sharkl`, `rk356x`), and Android-named `NanoPlatform.h` members (`ViaHal`, `mirror*`, `android::LaunchTuple`, `forceSfPath`). Each ceiling may only shrink, towards 0: the test fails when a count rises (the new platform work belongs behind a `nanoplat` interface) and when it falls (lower the ceilings with `py -3.12 tools/nano-tree/tests/test_linux_ratchets.py --lower`, which never raises one). `system`/`popen` call sites are a ceiling, not a floor: `test_shell_sites.py` reads the same spawner ceilings, and fork-and-exec sites may not grow without a row (B.5). These tests live outside `tests/`, so they never gate an Android release build (A.6).
5. **One host scenario set for both sides.** The headless scenarios (D.3) run before and after each G7 step, so an area's move keeps Nano's observable behaviour (launches, writes, settings) the same on the host.
6. **The patch outside Nano is untouched** (A.6), so framework, SELinux and init changes can't slip in through the fork.
7. **Before the merge into `nano-144`:** an AOSP build from the merged tree, with `drastic-nano`, `gammaos-nano-scrape` and the host test modules linked, and the owner's device smoke test (a DS game started through `drastic-nano` included), after the current AOSP job has finished. That is the only real proof, and it gates the merge, not the fork's progress.
8. **Residual risks:** GCC accepts code Clang rejects, or the reverse (point 2 narrows it); a moved block depends on a header's inclusion order (the AOSP build catches it); the regenerator's output differs on a git version or zlib build (A.3 pins the options and copies binary sections; the consistency test catches the rest before a commit, not after).

---

## I. Decisions made in this design

All by Claude (autonomous; for owner audit), 2026-11-25. Also listed in `docs/linux/DECISIONS.md` under "Implementation design".

| Id | Decided | Why | Would change it |
|---|---|---|---|
| I1 | Nano's source as real files in `nano/` on the fork; the patch's Nano block regenerated from it and committed; the patch remains what the AOSP flow consumes. Refines D7 for the fork | the refactor must be reviewable and buildable without a fetch-and-apply step; regeneration was shown byte-exact (A.3) | the owner preferring the patch as the only source until the merge |
| I2 | Fetched, not committed: the files under `ps3xmb/`, `nano_esde_themes/`, `nano_esde_assets/` that the patch doesn't touch; everything else committed (about 22 MB) | keeps 72 MB of upstream binary assets out of history while every Thimsy-made file is a normal file | a wish to build without the base cache (then commit the assets, or publish an asset tarball) |
| I3 | Directory and target names follow plan 4.1: `platform/firmware` (today's Android firmware code), `platform/android` reserved for the launcher plan's app | the two plans already use these names; the task's "nano_platform_android" would collide with the app | the launcher plan renaming |
| I4 | `Android.bp` stays the only build of the Android firmware; CMake builds Linux, host and later the app; `sources.txt` partitions `Android.bp`'s list and a test keeps them in step | Soong can't consume CMake; one list per fact | none foreseen |
| I5 | A new top-level `libthimsy/` (MIT) for the config store and small shared helpers, used by Nano's Linux and host shims, `thimsy-session`, `thimsy-padd` and `thimsy-prop`. It keeps its own small JSON reader rather than reusing `NanoJson` (review) | one store implementation across Nano (Apache-2.0) and the MIT Linux tools, with no licence question; `NanoJson` is Apache-2.0 and Nano-specific (its fast-math notes), and the MIT tools stay single-licence | the owner preferring the store inside `nano/`, or Apache-2.0 for the tools |
| I6 | **Withdrawn at the review.** The alpha's images come from Buildroot's `genimage`, as D13 said; `linux/tools/gpt_check.py` checks each written image against the vold rule; `card.py` is written in Phase 4, when the installer's crate needs test vectors | `card.py` and `bmap.py` were built before anything needed them; a small GPT checker keeps the testable part | the installer's crate needing vectors sooner |
| I7 | GPT names `uboot`, `boot`, `root`, `data`, `games`; data typed Linux home; exFAT made on first boot with label `THIMSY` | the plan left the data type open between home and server data; exFAT can't grow, and the installer avoids making filesystems | Phase 3 fixing the label; FlipBase's tools needing its names (then rename at the merge) |
| I8 | The first QEMU target is aarch64 `virt` (with a `mesa-swrast` GPU pack), not AMD64 | it boots the devices' own tier root, so one userland is tested; AMD64 needs a third | wave plans needing an x86 target |
| I9 | `linux/gpl/patches/<package>/` holds Thimsy's patches to third-party packages, each under the patched package's licence | they are copyleft or third-party material, kept out of the MIT area | none foreseen |
| I10 | `mods/retroarch-cores.src.lock` (core, repository, commit), generated from the Android lock's binaries; until then the core recipes are marked pending | the existing lock pins binaries, not sources, so the plan's "same commit" rule has nothing to read | the buildbot exposing source commits directly |
| I11 | `thimsy-import` waits (G2c, deferred) until ROCKNIX's real files have been read, over the web now or by a clone later; its fixtures are cut down from those real shapes, each naming the path and commit it imitates (revised at the review) | fixtures built on guessed shapes would test the guesses; Phase 2a's hand import doesn't need the tool | none foreseen |
| I12 | `thimsy-session` and `thimsy-padd` in C++17, as pure cores plus thin I/O layers | they share `libthimsy`, the compiler and the test stand-in with Nano; host-testable cores | a size or start-time problem on the device (then C for padd's I/O loop) |
| I13 | padd's identity: `Thimsy Gamepad`, `BUS_VIRTUAL`, vendor 0x5448, product 0x0001, version 1; kernel gamepad layout | no USB vendor id is claimed; SDL's GUID stays unique and stable | an emulator that ignores `BUS_VIRTUAL` pads *(test in Phase 0)* |
| I14 | Android paths on Linux through links in the read-only root, not by editing path literals: `/data` as a whole to `/storage/.config/thimsy/data/`, `/sdcard` and `/storage/self/primary` to `/storage/emulated/0`, `/system/etc`, `/system/fonts`, `/system/bin` (a curated directory), `/mnt/media_rw/<id>`; every absolute root in shared files has a disposition in `path-roots.json`, held by a ratchet (widened at the review); a `Paths` interface only when the app needs it | zero edits to shared files; the state keeps its file names (plan 5.2); the census found roots the first list missed (`/data/data`, `/data/local`, `/data/user`, `/storage/self`, `/system/bin`) | the app's needs arriving first |
| I15 | Android shell commands and programs in Nano are routed on Linux by `--wrap=system,popen,pclose,execv,execvp,execve` through a gate that translates or refuses them; on the host the gate only records (widened at the review) | zero edits at the call sites; nothing Android-only runs as root on Linux; three files fork and exec directly, which a `system`-only gate missed | `--wrap` failing with the toolchain's LTO (then no LTO, or a `nanocompat_system` rename in P4) |
| I16 | Inert stubs of binder, SurfaceFlinger, sensor and AHardwareBuffer types for the Linux and host builds, with a shrinking budget | `NanoMenu`'s base classes and members need them, and editing `NanoMenu.h` would conflict with `nano-144` | P4 moving those members out sooner |
| I17 | The headless E2E runs Nano on its real thread and real time in a user-namespace jail (`--pid --mount-proc`, read-only binds, a tmpfs `/usr`) laid out like the Linux root, with a GLES stub, `DrmHost.cpp`, a frame barrier, FIFO input devices answered by a wrapped `ioctl`, and no real-root fallback (revised at the review) | `threadLoop` holds the whole session, a fake clock can't reach Nano's direct clock calls, and real root would expose the WSL distro; unprivileged user namespaces were confirmed to work in WSL `Ubuntu` | user namespaces becoming unavailable (then the test skips) |
| I18 | `build.sh` and the C++ test Makefiles refuse while the build lock is held, `MemAvailable` is under 9 GB, an Android build container runs, `wsl.exe -l --running` lists `thimsy-aosp`, or the VM's load is above 8; `--light` lets only the small test targets through; `--force` needs a typed confirmation and skips only the memory and load checks; compilers run under a 3 GiB `prlimit` with `oom_score_adj` 1000 (widened at the review) | the AOSP and Linux builds share one 13 GB VM, and the AOSP build runs natively in `thimsy-aosp`, which neither the lock nor `docker ps` sees | measured peaks |
| I19 | The Android build chain needs no reimplementation: only the patch's producer changes in the fork (`nano_tree.py regen`, run by a merge driver on every merge), and `scripts/build.sh` is unchanged apart from taking the build lock after the current AOSP job (G10) | the AOSP flow already consumes one patch file; nothing in it depends on how the file is made | the merge changing how `nano-144` developers work (then `import-tree` becomes their routine) |
| I20 | New Python tools and tests must run on 3.8 as well as 3.12; the existing suite under `tests/` is not held to 3.8 (`scripts/esde_config.py` uses 3.9's `str.removeprefix`), so `tests/run.sh`'s new modes discover only their own directories (clarified at the review) | WSL `Ubuntu` has 3.8 and runs the C++ tests' wrappers | moving the host tests to a newer distro |
| I21 | Patch merges: `.gitattributes` gives the patch `merge=nanopatch -diff linguist-generated`, and the driver (`nano_tree.py merge-patch`, registered by `install-driver`) runs `import-patch` and `regen` on every merge, conflict or not; `regen` refuses to overwrite a Nano block it did not write (`block.sha256`); `sync.json` is dropped for `git merge-base linux nano-144` | a plain `git merge` can merge the patch with no conflict and leave `nano/` without `nano-144`'s changes; a hand procedure would be forgotten | git gaining a better hook for this |
| I22 | Fork-only files stay out of the patch (`upstream.json` `fork_only`: `platform/linux/**`, `platform/host/**`, `test/host/**`, `compat/stubs/**`, `build/**`, CMake files, `third_party/wpa_ctrl/**`, `LICENSE`, `NOTICE`); `compat/include/**`, `compat/src/**`, `NanoPlatform.h` and `platform/firmware/**` do reach it | Linux and host commits no longer rewrite the 11 MB patch or ship code Soong never builds; the launcher plan's app still finds the shims in the patched tree (plan 4.3) | the launcher app building from `nano/` directly (then `compat/` can be fork-only too) |
| I23 | Fork-only tests live in `tools/nano-tree/tests/` and `linux/tests/`, outside the `tests/` that gates Android release builds; only the cache-free patch-consistency test stays in `tests/`. The Android surface is computed relative to `git merge-base linux nano-144` from the patch's own sections, with no committed manifest | Linux-side tests must not block the Cube's build after the merge back; a manifest would change with every normal Android edit | none foreseen |
| I24 | No file compiled by `drastic-nano`, `gammaos-nano-scrape` or a host test module may include `NanoPlatform.h` or call `nanoplat::`; those files reach platform behaviour through layers 1 to 3; `NanoDeviceKeys.cpp` is not edited (its `ro.product.*` reads are served on Linux by `thimsy-prop boot`) | those binaries never call `install()`, and `drastic-nano`'s own files sit outside the Nano block, which the fork may not edit | a feature that truly needs a service there (then a declared exception landed on `nano-144` first) |
| I25 | The scraper runs on Linux as its own binary, `thimsy-nano-scrape`, built from `gammaos-nano-scrape`'s six files and started by `thimsy-session` when Nano writes `sys.gammaos.nano.scrape_run` | the plan makes the scraper a v1 feature (5.4), and a separate process keeps a scrape beside a game to a few MB, as on Android | memory proving no issue on Linux (then in-process) |
| I26 | The Linux build compiles `NanoMenuDrm.cpp` and substitutes only its buffer back end, after a mechanical split (G9a) into `NanoMenuDrmBuffers.cpp`, the one edit to an existing Nano file before P4, flagged for cherry-pick to `nano-144`; the host substitutes all of `NanoMenuDrm.h`'s API in `DrmHost.cpp`, under a symbol-set test; B.8 writes down the ring contract | a full Linux substitute of a 3,577-line, actively tuned file would drift silently; the host has no DRM device | the split not being mechanical (then the full substitute with the same test and contract) |
| I27 | `NanoPlatform.h` interfaces are declared in the G7 step that moves their code, sized to it; `Clock` is dropped; `deviceModel()` becomes `deviceMatchKeys()`; `Audio` takes the pull shape of NanoAudio's AAudio callback; G7-main is the first P4 step; hiding rows by `Cap` is its own step (G7-caps) after the alpha | interfaces designed ahead of use would be frozen wrong; P4 commits must each leave the Android build linkable | none foreseen |
| I28 | Launching: a behaviour-neutral commit first gathers the handshake into `commitLaunch(const LaunchTuple&)`, then the seam swaps that one function (done: G7-launch-a, then G7-launch-b's `Launcher::commit`). The `do_launch` handshake stays the Linux launch protocol: the Linux `commit` writes the keys, and the trigger, observed by the control keys, is a launch only when written inside Nano's own `commit` (G7-launch-b-14); Nano then exits and leaves the game to `thimsy-session` (REV4-1, B.7). Control-key handlers observe after the store writes and never consume; the trigger table is derived from `gammaos-nano.rc` and the framework listeners, with a disposition per trigger | the keys are written from nine files and some are read back as state; a consuming handler would break those readers | none foreseen |
| I29 | No hand-written `NanoMenu::` stubs: the media, overlay and SD-move files compile on Linux against the layer-2 stubs; a file that truly can't compile gets stubs generated at build time from the link's undefined symbols; `[dropped-linux]` is a subset of `[firmware]` | hundreds of methods would drift with every `NanoMenu.h` change on `nano-144` | none foreseen |
| I30 | `android/imagedecoder.h` and `android/bitmap.h` get a real Linux and host implementation on libpng, libjpeg-turbo, libwebp and giflib; HEIF and AVIF are unsupported on Linux v1 | the ES-DE theme, the Linux v1 UI (D9), decodes every image through `AImageDecoder`; system libraries avoid vendoring and are Buildroot packages | a theme set needing HEIF or AVIF (then libheif or libavif) |
| I31 | Bionic-only libc extras (`mallopt` parameters, `strlcpy`, `strlcat`, `getprogname`) come from a force-included `nanocompat/bionic.h`, with a ratchet on Bionic-only identifiers | glibc 2.31 lacks them; force-including edits no Nano file | none foreseen |
| I32 | `NanoMenuShares.cpp` and `NanoMenuRgbFollow.cpp` are `[firmware]` for v1; `compose` exports the sibling sources Nano reads from outside its folder for the host build and tests | their headers live in other modules of the AOSP tree; neither feature is in v1 on Linux | Shares or RGB follow becoming Linux features |
| I33 | A GammaOS base move is handled by `nano_tree.py import-base NEW` (three-way, per file); `import-patch` reads each side's base from that side's commit; on the fork `upstream.json` is the only place the base is written | upstream is 408 commits ahead, so `nano-144` may move its base, and a fixed base would break the import | none foreseen |
| I34 | `regen` uses pinned plumbing (`diff-index` with explicit options, no system or user config, the fork's objects as alternates) and copies unchanged binary sections verbatim; `--from-cache` uses alternates, not a fetch; if Windows and WSL git disagree, Windows git 2.37 is the reference | porcelain diff follows config, and zlib output differs between builds; a local fetch of an unadvertised SHA is refused | none foreseen |
| I35 | Heavy C++ targets (`check-unit`, `syntax-core`, `headless`) are deferred to G10, after the AOSP job and once downloads are allowed; steps that need them are "written, not compiled" and reported as skipped; the `[core]`/`[firmware]` split comes from an include census | the WSL distro has no development packages, downloads are not allowed, and large compiles could get an AOSP process OOM-killed | the packages being installed sooner |
| I36 | Each parallel agent works in its own git worktree; the orchestrator stages only owned paths, merges serially and re-tests in a clean worktree; stages re-cut so each file has one owner (`post-build.d/`, `post-image.d/`) | tests on a shared working tree can pass on another step's uncommitted files | none foreseen |
| I37 | Release targets (`thimsy_v8_*_release`, marked for publishing) use `release.fragment`: no adbd, no serial getty, dropbear off or key-only, root password locked; `build.sh --publish` refuses any other target | every target included the dev fragment, and Buildroot's default root password is empty, while D16 makes the alpha public | none foreseen |
| I38 | `thimsy-data` installs from an allowlist generated from `ASSETS.md`'s KEEP rows, never by mirroring `Android.bp`; "no REPLACE or ASK asset in the image" joins D16's public-alpha conditions and Phase 2's exit | the Rodin font, PS3 and DSi assets are marked non-redistributable, and the plan already said the Rodin replacement must land before any Linux image ships | `ASSETS.md`'s rows changing |
| I39 | The written image has no `games` entry; `D60firstboot` creates it typed Linux reserved, makes the exFAT, then retypes it basic data | an unformatted basic-data entry invites Android's format wizard (risk 13) and Windows' format prompt | Phase 3's device tests finding a better order |
| I40 | `S00base`, `D60firstboot` and `thimsy-session` act only on the boot disk (resolved from `root=PARTUUID=` or the mounted root) and never on an eMMC; Nano runs in a mount namespace with `/dev/null` over the eMMC's nodes; `/dev/block/by-name` is never created | a second card or an eMMC with a `data` partition would otherwise be mounted and written, and Nano's screen-calibration code writes a raw Android partition | none foreseen |
| I41 | Source publication: the recipes build `libthimsy`, `nano` and the assets from `git archive` tarballs (`_SITE_METHOD = file`), so `legal-info` saves them; a public release also publishes `git archive` of the whole repository; `THIMSY_NANO_LICENSE` names the GPL-2.0+ Kanji table; `nano/NOTICE` lists the GPL parts | `legal-info` saves no source for local sites, and Nano's source must be published beside a public alpha | the owner's answer on the Kanji table (plan 3.3) |
| I42 | At FlipBase's subtree merge, licence and notice files are kept (`LICENSE.FlipBase`, `NOTICE`), not replaced by Thimsy's; refines D19 | MIT requires FlipBase's notice in every copy | none foreseen |
| I43 | Phase 2's rotation fallback: for the alpha (RetroArch only), RetroArch's own rotation settings if the `drm_ctx.c` patch fails *(guess that they cover the menu)*; for SDL standalones (v1), build against SDL3 directly, else the emulator leaves the set until Phase 5, where the owner's per-game compositor fallback (Q27) becomes available; refines Q27 and Q28 | Q27's compositor fallback does not exist before Phase 5, and Q28's `display: kiosk` is a Phase 5 spike (Q29) | the Phase 5 spike landing earlier |
| I44 | The alpha's library lives on the boot card's `games` drive, chosen by default; the 256 MiB data partition (`/storage/emulated/0`) holds state and small files only; refines D10 | D10's "library on internal storage" would put ROMs on a 256 MiB partition | Phase 3 sizing the data partition for a library |
| I45 | Public Linux images do not carry the ScreenScraper developer credentials: images are built from tracked files only, `import-tree` skips ignored files, and a test keeps both credential names ignored under `nano/`; the scraper then needs the user's own account *(until the owner decides, as android_launcher_plan D13 asks for the APK)* | a developer's local credentials would otherwise be compiled into a public image | the owner deciding to ship them |
| I46 | On Linux `persist.props` is mode 0600, and keys that look like secrets are redacted in `thimsy-prop list` and in logs; the plan notes that a removed card exposes them | Android keeps these values on encrypted userdata; the card is removable | encrypting the data partition later |
| I47 | CMake reads `nano/build/sources.txt` itself (`file(STRINGS)`), so there is no second list to compare | one list per fact (principle 5) | none foreseen |

---

## J. What is not verified

- No C++ was compiled: whether `nano_core` compiles with g++ 9.4 is checked in G10. The feature census (section 0) says C++17 suffices. WSL `Ubuntu` has no development packages (checked at the review), so only the dependency-free targets can run before G10.
- CMake, Buildroot symbols, FlipBase's file formats (FIT header, card layout script, U-Boot script, what `gptgrow.c` can do) and ROCKNIX's file shapes were not read in this pass; each is marked where used. Buildroot's `legal-info` skipping sources of `local` packages, and its host `bmap-tools` package, are taken from the review and memory, not re-read in Buildroot's source.
- B.8's contract was checked by G9a against the moved code, by reading, and the split was mechanical (`git diff --color-moved` clean). Not checked: that the moved code compiles on its own (no Android toolchain here; the AOSP build of H.7 does), and the Mesa behaviour B.8 relies on (the EGL gate's wraps, `EGL_ANDROID_native_fence_sync`), until G10.
- `regen` under WSL's git 2.25 has not been run (A.3); nor has the merge driver.
- Whether WSL interop (`wsl.exe` from inside a distro) works in `thimsy-linux` and WSL `Ubuntu`, which E.8's guard relies on. Unprivileged user namespaces in WSL `Ubuntu` were confirmed by the review's probe.
- Whether Nano treats a FIFO input device answered by the wrapped `ioctl` exactly as a real pad (D.3); G10's first scenario shows it.
- The core commits for E.3.

---

## K. Review

A design review on 2026-11-25 looked at this document through three lenses (architecture, testing, risk) and raised 52 findings, many of them the same problem seen from two or three lenses. Each was re-checked against the applied Nano tree, the repository and the two machines' tools before the design changed; the re-check column says what was confirmed (counts are this pass's own, which differ a little from the review's where the pattern differs). All changes were decided by Claude (autonomous; for owner audit) and are logged as decisions I21 to I47 (section I and `docs/linux/DECISIONS.md`), with amendments to I5, I6, I11, I14, I15, I17, I18, I19 and I20. "Done" means the design above now says it; nothing was built.

### K.1 Architecture

| # | Sev. | Finding | Re-checked | What was done |
|---|---|---|---|---|
| A1 | high | `nanoplat::get()` in files that `drastic-nano`, `gammaos-nano-scrape` and the host tests compile, none of which calls `install()`; `drastic-nano`'s own files are outside the Nano block | yes: `drastic_nano_shared_srcs` has 19 files, `NanoMenuDrm.cpp` and `NanoDeviceKeys.cpp` among them; the scraper's 6 files; `gammaos_nano_unit_tests`' 24 | Done. B.6 rule and ratchet: those files never include `NanoPlatform.h` or call `nanoplat::` (I24); `NanoDeviceKeys.cpp` is no longer edited (G7-input); the DRM file is handled at layer 3; H.1's surface covers every module that compiles Nano's files; `sources.txt` gains `[scrape]` and `[drm]`; an edit to `drastic-nano` would be a declared exception landed on `nano-144` first |
| A2 | high | The headless driver can't call `threadLoop()` per tick; the fake clock misses most timing; no way to inject presses | yes: `threadLoop` 1315 to 6329 with its own loop at 4060 and `stopProcess`; `steady_clock` 81, `clock_gettime` 42, `usleep(` 154 | Done. D.3 rewritten: Nano runs on its real thread and real time; a frame barrier in `DrmHost.cpp`'s flips; `within_ms` deadlines; `Clock` and the fake clock dropped. Input: FIFO devices plus a host `--wrap=ioctl` that answers the evdev identity ioctls, chosen over an input seam (no shared-file edit) and over `/dev/uinput` (needs real root) (I17) |
| A3 | high | A plain `git merge nano-144` can merge the patch silently; `regen` could then drop `nano-144`'s work | yes: one section per file, sorted, so different files merge textually | Done. Merge driver `merge=nanopatch` on every merge; `regen` refuses to overwrite a block it did not write (`block.sha256`); a cache-free consistency test (post-image ids against `git hash-object --no-filters`); `sync.json` replaced by `git merge-base` (A.4 to A.6, I21) |
| A4 | high | A full `DrmLinux.cpp` re-implements a large, tuned API and drifts silently | yes: 45 `extern` lines in `NanoMenuDrm.h`; `drmFrameEnd` skips the non-zero-copy path; `NanoMenuDrm.cpp` 3,577 lines, `drmAllocAhbTarget` at 1100 | Done in the fork: G9a splits the buffer back end out by a mechanical move, Linux compiles the KMS code with a GBM back end, B.8 writes the ring contract, a symbol-set test covers `DrmHost.cpp` and the fallback. "Land it on `nano-144` first" is changed to "flag it for cherry-pick to `nano-144` after the AOSP job": the `nano-144` tree must not be touched while its build runs (I26) |
| A5 | medium | Regen's byte-exactness depends on git config, attributes and zlib | yes: Windows git's system config sets `diff.astextplain.textconv` and `core.autocrlf`; 7 binary sections; WSL git is 2.25.1; the patch has no rename sections, so `--no-renames` keeps it byte-exact | Done. Pinned plumbing under no system or user config; binary sections copied verbatim when unchanged; `--check` compares binary sections by blob id; a second trial under WSL git before G0 is done (A.3, I34) |
| A6 | medium | Hand-written `[dropped-linux]` stubs for hundreds of `NanoMenu::` methods; `main_linux.cpp` a hand copy of `main.cpp` | yes: `NanoMenuVideo.cpp` 147, `NanoMenuPhotos.cpp` 108, `NanoMenuMusic.cpp` 71, `NanoMenuOverlay.cpp` 60, `NanoSdMove.cpp` 24; `main.cpp` 1,220 lines | Done. Those files compile in `[core]` against the stubs; stubs for a truly dropped file are generated from the link (C.1, I29). `main.cpp`'s boot steps move into a shared `nanoBootSequence()` in G7-main, in the fork rather than "on `nano-144` first", for the same reason as A4 |
| A7 | medium | Control keys are a handshake; 18 rc triggers missing; a consuming handler breaks readers | yes: `NanoMenu.cpp:2047-2055` and `4040-4048`; readers at `NanoControlCenter.cpp:1103-1106`, `1835` and `NanoButtonMap.cpp:1360`; 18 `on property:` lines | Done. Observers after the store writes, never consuming (B.3); the trigger table derived from the rc file and the framework listeners, with a disposition each and a test; the `do_launch` handshake is the launch protocol until G7-launch (B.2, I28) |
| A8 | medium | The Launch area's file list is wrong (`NanoMenuEsdeMenu.cpp` writes no launch key) | yes: counts 44, 17, 10, 8, 6, 5, 2, 2, 1, and 0 in `NanoMenuEsdeMenu.cpp` | Done. B.6's row corrected; G7-launch split into `-a` (gather the handshake into `commitLaunch`, behaviour-neutral) and `-b` (swap it), serialised after G7-display. The gathering commit is made in the fork, not first on `nano-144` (see A4) |
| A9 | medium | `Audio` is a blocking `write()`, NanoAudio is pull-mode with start and pause | yes: `setDataCallback` at 258, `requestStart` at 297 and 466, `requestPause` at 475 | Done. The sketch takes the pull shape, and the interface is declared only in G7-audio, sized to the moved code (B.6, I27) |
| A10 | medium | Interfaces designed and frozen ahead of use; `Clock` duplicates hooks; `deviceModel()` has two meanings; `Cap` row hiding has no owner | yes: G5 froze eight interfaces; no G7 step owned `NanoMenuSettingsTree.cpp` | Done. Declared per G7 step; `Clock` dropped; `deviceMatchKeys()`; G7-caps owns the row hiding, after the alpha (I27) |
| A11 | medium | Linux-only code goes into the Android patch, and parallel steps would conflict on it | yes: A.2's last bullet; G0 to G9 all add under `nano/` | Done. `fork_only` exclusion list (I22); the patch is marked `-diff linguist-generated`. "Regenerate once per integration" became: the merge driver regenerates on every merge, so parallel steps never conflict on the patch, and a step regenerates only when it touches an included path |
| A12 | medium | The surface manifest and props baseline tax every Android edit; tests in `tests/` gate the Android build | yes: `scripts/build.sh:58` runs `tests/run.sh quick` | Done. The surface is computed against the `nano-144` merge base from the patch's own sections, with no manifest; fork-only tests moved to `tools/nano-tree/tests/` and `linux/tests/`; only the patch-consistency test stays in `tests/` (A.6, H.1, I23). The props baseline stays, but a key arriving from `nano-144` is added by the merge driver's run |
| A13 | medium | Path links and the shell gate leave holes (`/data/data`, `/data/local`, `/system/bin`; fork and exec sites) | yes: see section 0's roots row and exec row | Done. `/data` mapped whole; `/storage/self/primary`; curated `/system/bin`; `exec*` wrapped; a path-roots inventory with a ratchet (B.5, I14, I15) |
| A14 | medium | Sources outside Nano's folder; no Linux scrape binary | yes: `share_config.h` in `NanoMenuShares.cpp`, `GammaRgbProcess.h` in `NanoMenuRgbFollow.cpp`, the unit tests' `drastic-nano` include dir | Done. `upstream.json` `siblings` exported by `compose`; Shares and RGB follow `[firmware]` for v1 (I32); a `thimsy-nano-scrape` target started by `thimsy-session` (C.2, I25) |
| A15 | medium | A GammaOS base move is not handled | yes: `BACKPORTS.md` 408 commits ahead; `fetch_nano.sh:15` `BASE=` | Done. Each side's base read from its commit; `import-base NEW`; `upstream.json` the only base on the fork (A.5, I33) |
| A16 | low | Pieces built before they are needed: importer, `card.py`, `bmap.py`, a second JSON, a second copy of the source lists | yes: I11's guessed shapes; I6 | Done for four: G2c deferred until real shapes are read (I11); `genimage` for the alpha with a small GPT checker (I6 withdrawn); CMake reads `sources.txt` (I47). **Skipped** for JSON: `libthimsy` keeps its own reader, because `NanoJson` is Apache-2.0 and Nano-specific while the MIT tools stay single-licence (I5 now says so) |
| A17 | low | `git ls-tree --format` needs 2.36; B.7 wording; where `[dropped-linux]` belongs | yes: `fetch_nano.sh:31`; WSL git 2.25.1 | Done. Plain `ls-tree -r` parsed in Python (A.4); B.7's P3 row corrected; `[dropped-linux]` is a subset of `[firmware]`, checked by the source test |

### K.2 Testing

| # | Sev. | Finding | Re-checked | What was done |
|---|---|---|---|---|
| T1 | high | The headless driver can't reach private `threadLoop`/`readyToRun`; `readyToRun` takes the SurfaceFlinger path on the host | yes: `NanoMenu.h:357-365` private; `readyToRun` uses DRM only `if (sDrmActive)` | Done with option 1 (Nano's real thread through the compat `Thread::run`) plus `DrmHost.cpp`, owned by G5, which sets `sDrmActive` and counts flips. Option 2 (a step mode inside `exitPending()`) was not taken: the frame barrier gives the same stepping without changing the compat `Thread`'s semantics |
| T2 | high | No development library is installed in WSL and downloads are not allowed | yes: no `zlib.h`, `openssl/`, `EGL/`, `GLES2/`, `gtest/`; glibc 2.31 | Done. "Written, not compiled" with skips reported, never counted as passing; the `[core]` split from an include census; G10 queued for after the AOSP job (G, I35) |
| T3 | high | Heavy host compiles could exhaust the shared VM | yes: `.wslconfig` 13 GB per the plan; `NanoMenuPS3Menu.cpp` 1.1 MB | Done. The guard's `--light` mode in every Makefile; `prlimit --as` 3 GiB; `oom_score_adj` 1000; `syntax-core` without sanitizers; only small targets while the AOSP job may run (C.3, I18) |
| T4 | high | P4's order breaks the Android build before G7-main | yes: `install()` and the firmware sources arrived only in G7-main | Done. G7-main is the first P4 step; each later area commit adds its own firmware file and carries an `android-surface:` line (B.7, H.3) |
| T5 | high | `drastic-nano` ignored | yes (A1) | Done (A1). The surface scan also resolves angle includes through `local_include_dirs` and `include_dirs`. The alternative (allowlisting an edit to `drastic-nano/main.cpp`) is not needed |
| T6 | high | The stub list misses headers that core files include; image decoding would be lost | yes: `NanoThemeEngine.cpp` includes `android/imagedecoder.h` and `bitmap.h`, 56 `AImageDecoder` uses | Done. The stub inventory comes from an include census checked by `test_nano_stubs.py`; the image decoder is a real shim, backed by libpng, libjpeg-turbo, libwebp and giflib rather than a vendored `stb_image.h` (nothing may be downloaded now, and themes need WebP, which `stb_image` lacks) (B.2, B.4, I30) |
| T7 | medium | Bionic-only libc extensions | yes: `mallopt(M_PURGE_ALL, 0)` at `NanoMenu.cpp:1435`, `SLOGE_IF` at 409; `strlcpy`, `getprogname` and `M_CACHE_COUNT_MAX` users found | Done. Force-included `nanocompat/bionic.h`, `SLOG*` and `ANDROID_PRIORITY_*` in the shims, a ratchet (B.2, I31) |
| T8 | medium | The shell gate misses fork and exec | yes (A13) | Done (A13), with `/system/bin` linked to a curated directory and a ratchet on fork-and-exec sites |
| T9 | medium | Path mapping covers only some Android paths | yes (A13) | Done. `/data` whole, `/storage/self/primary`, `/system/bin`, the inventory test. `/mnt/shares` is **not mapped**: its users are `[firmware]` for v1 (I32), which the inventory records |
| T10 | medium | The jail has no `/proc`; `/usr` can't take the link targets; no input path; real root with ASan is dangerous | yes: `unshare` present in WSL; `mlockall` gated at `NanoMenu.cpp:1447`; `openInputDevices` opens `O_RDONLY \| O_NONBLOCK` and caches `EVIOCGNAME` only on success | Done. `--pid --fork --mount-proc`; a tmpfs `/usr` with individual binds; FIFO input held open `O_RDWR`; `mlockall=0`; no real-root fallback (D.3) |
| T11 | medium | The fake clock can't control Nano's time | yes (A2) | Done (A2). Fake clocks stay in the pure cores of `libthimsy`, `thimsy-session` and `thimsy-padd`, where time is injected |
| T12 | medium | `*.7z` in `.gitignore` drops six fixtures | yes: `.gitignore` lines 4 and 5 | Done, with a narrower re-include than `!/nano/**` (`!/nano/**/*.7z` and the split-volume form), so the root's other rules still apply; `import-tree` and `import-patch` check the staged set against the block; a check-ignore test (A.2, A.6) |
| T13 | medium | Regen and the cache tools fail against the real cache (missing objects, unadvertised SHA, git versions) | yes: the cache is shallow, ref-less and a `blob:none` promisor | Done. Alternates for the fork's objects; `--from-cache` by alternates and `update-ref`; pinned options; Windows git the reference if the two disagree (A.3, A.4, I34) |
| T14 | medium | `check-unit-0`'s scope and counts are wrong | yes: 24 production files; `NanoMenuUtils.h` pulls in property and log headers | Done. One program per test with a computed closure, skips naming the header; section 0 corrected; G1c after G1a and G1b (C.3, G) |
| T15 | medium | Stage ownership overlaps or has gaps | yes: `board/common/**` owned twice; `tests/run.sh` unowned; G2a needs G2d's symbols | Done. Stages 1a, 1b, 1c re-cut; `post-build.d/` and `post-image.d/` give each script one owner; G1c owns `tests/run.sh` and the repoint; G6 after G4 (G) |
| T16 | medium | Parallel agents on one working tree; objects written into the repository | yes: only `nano/test/host` had an out-of-tree rule; `.gitignore` has no object rules | Done. A worktree per agent, owned-path staging, serial merges and a clean-worktree re-test; every Makefile builds under `$HOME/.cache/thimsy/<component>` (G, I36) |
| T17 | low | Hand-written dropped stubs; `NanoSdMove.cpp` placed in two sections | yes (A6) | Done (A6); `NanoSdMove.cpp` settled as `[core]` in C.1 and B.6 |
| T18 | low | Stand-in output vs `test_nano_units.py`; 3.8 rule vs the existing suite; `flock` in Git Bash | yes: the test asserts `N tests, 0 failed`; `esde_config.py:36` uses `removeprefix`; no `flock` in Git Bash | Done. The stand-in keeps the summary line; new modes discover only their own directories and I20 no longer claims the old suite runs on 3.8; `test_build_guard` skips without `flock`. `esde_config.py` is **not changed**: it is Android-side and outside this work |
| T19 | low | B.7 P3 and B.5's `--wrap` statement inaccurate | yes | Done. Both reworded; static archives in the link are listed so their calls are known to pass the gate (B.5, B.7) |

### K.3 Risk

| # | Sev. | Finding | Re-checked | What was done |
|---|---|---|---|---|
| R1 | high | `drastic-nano` and the host tests would link-fail or crash on a null platform | yes (A1) | Done (A1) with the frozen-API rule. The other option, a lazily built firmware default inside `get()`, was not taken: it would link the firmware services (AAudio and the rest) into `drastic-nano` and the host tests. H.7 adds a `drastic-nano` link and a DS game to the pre-merge AOSP check |
| R2 | high | Heavy compiles in the shared VM with no guard | yes (T3) | Done (T3), with `oom_score_adj` 1000 so the test dies first |
| R3 | high | The Linux image would ship assets `ASSETS.md` marks non-redistributable | yes: `thimsy-data` mirrored `prebuilt_etc`; plan line 455 already required the Rodin replacement first | Done. An allowlist from `ASSETS.md`'s KEEP rows and a lint check (E.3, D.4); the condition added to D16 and to Phase 2's exit in the plan (I38) |
| R4 | high | Every target includes the dev fragment; Buildroot's root password is empty | yes: E.4 used `release.fragment` nowhere | Done. Release targets, a `test_defconfig.py` assertion, `build.sh --publish` refusal, D16 naming the release build (E.4, I37) |
| R5 | medium | An unformatted basic-data `games` entry invites Android's format wizard and Windows' format prompt | yes: E.6 and plan risk 13 | Done with both suggestions combined: no entry 5 in the written image; first boot creates it reserved, formats, then retypes (E.6, I39) |
| R6 | medium | `S00base` and `D60firstboot` not scoped to the boot card | yes: E.5's `PARTNAME=data` across every block device | Done. Boot-disk resolution, eMMC refused, fake-`/sys` tests (E.5, F.1, I40) |
| R7 | medium | No procedure for a base move; the base written twice | yes (A15) | Done (A15). On `nano-144`, `fetch_nano.sh` keeps its `BASE=` line until the merge back; the importer reads either |
| R8 | medium | The shell gate misses fork and exec; `--wrap` statement wrong | yes (A13, T19) | Done (A13, T19); the gate passes through `__real_*` |
| R9 | medium | `legal-info` saves no source for local packages; Nano's GPL-derived Kanji table under an Apache-2.0 label | yes: `NanoOskKanji.h:1-3`; E.1's local sites | Done. `git archive` tarballs as `file` sites, the whole repository archived at a release tag, an honest `_LICENSE`, `nano/NOTICE` (E.1, E.3, I41). The `NANO_WITH_KANJI=OFF` switch was **not taken**: `NanoOskKanji.h` is a quoted include from the same directory, which no include-path shim can replace, so the switch would need a shared-file edit; publishing the full source with an honest licence covers the same obligation *(guess, not legal advice)* |
| R10 | medium | "Thimsy's version wins" would drop FlipBase's MIT notice | yes: D19's wording; G2a writes `linux/LICENSE` first | Done. Licence and notice files are kept at the merge (E.1); D19 amended (I42) |
| R11 | medium | The guard can't see an Android build in `thimsy-aosp`; `--force` removes the checks | yes: the AOSP job runs natively in a WSL distro | Done. `wsl.exe -l --running` and VM load checks; `--force` typed and narrower; the lock added to `scripts/build.sh` in G10 (E.8, I18) |
| R12 | low | The rotation fallback contradicts itself across Q27, Q28 and Q29 | yes: `pending_answers.md` Q27; DECISIONS Q28 and Q29 | Done. The Phase 2 fallback order is stated (I43) in DECISIONS and the plan |
| R13 | low | Decision entries overridden without amendment | yes: D26 against I8, D13 against I6, D6 against I5 and I9, D10 against E.6 | Done. "Refined by" lines on D6, D7, D10, D13, D19 and D26 in DECISIONS and the plan; `gpl/` described as per-patch licences; the alpha's library on the `games` drive (I44). D13 is no longer contradicted, since I6 is withdrawn |
| R14 | low | The scraper developer credentials could reach a public image | yes: `nano/.gitignore`; `NanoScraperDevCreds.h` builds without them via `__has_include` | Done. `import-tree` skips ignored paths; images build from tracked files; a test; a decision that public Linux images carry no credentials until the owner says otherwise (I45) |
| R15 | low | `persist.*` credentials in a plain file on a removable card | yes: `persist.gammaos.scraper.ssdevid` and `.ssdevpw` | Done. Mode 0600, redaction, a note in plan 5.2 (B.3, I46) |
| R16 | low | The root fallback could alter the WSL distro | yes (T10) | Done. Read-only binds, a record-only host gate, no real-root fallback (B.5, D.3) |
