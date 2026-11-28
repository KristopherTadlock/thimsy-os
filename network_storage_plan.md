# Network storage plan: the Cube as a client of a network share

Status: research and plan (2026-11-21). **Not to be implemented until approved.** It builds on `usb_storage_plan.md`, where a share becomes one more drive. It is the opposite direction to `file_sharing_plan.md`, which is a server on the Cube; the two can share the discovery code.

## Requirement (user, 2026-11-21)

- **ES-DE files, emulator settings and ROMs on a network drive** (an SMB server on the home network, with the Cube as the client), implemented natively.
- **The menus show the ROMs on the share** and load artwork and everything else from it, as if it were a microSD card.
- **Move emulators' installed content to the share to free internal space:** Vita3K's installed games, and Eden's games and content (Eden is user-installed, not shipped).
- **Cache large disc images on the device while they stream,** without a long wait at start-up.
- Any other features that fit.

## Summary

- **Thimsy already has a network-share client, inherited from GammaOS:** `frameworks/base/cmds/gammaos-sharefs`.
  - It covers SMB2/3 (libsmb2), NFSv3/v4 (libnfs), WebDAV and FTP (libcurl), with one root FUSE process per share and up to 4 shares.
  - It's configured in **Settings > Network Shares** in Nano (`NanoMenuShares.cpp`) and in the GammaShares document provider (`packages/apps/GammaShares`).
  - It mounts each share at `/mnt/shares/<name>` and bind-mounts it so apps see `/storage/<name>`.
  - It's undocumented in the README.
- **What it lacks for this goal:**
  - **Security:** the SMB library has known memory bugs, and share passwords are stored weakly.
  - **Correctness:** rename over an existing file fails, and fsync does nothing. Both break saving settings files.
  - **Speed:** one request at a time.
  - **Integration:** a share isn't an Android storage volume, so pickers and Nano's drive code don't treat it like a card. Launching some emulators from a share opens the wrong file.
- **The plan hardens and extends sharefs, not a new client.**
  - It registers each share with vold as a removable drive, so every app and emulator sees it exactly like a USB drive.
  - It adds a streaming block cache so big games start quickly.
- **No kernel SMB.** The Cube's kernel (5.4.233, a Unisoc build) has no SMB or NFS. A home-built `cifs.ko` isn't viable: only 428 of 1,496 kernel symbols the vendor modules use match Google's android12-5.4 kernel, so a module built from public source would be rejected or crash. Shelved unless Unisoc's exact source appears.

Tags in this document: **[V]** checked in code, binaries or docs; **[I]** inferred, to be proven on a Cube.

## 1. What exists today

### 1.1 gammaos-sharefs [V]
| Part | Where |
|---|---|
| Daemon (about 3,400 lines) | `frameworks/base/cmds/gammaos-sharefs/` (`main.cpp`, `fuse_mount.cpp`, `smb_backend.cpp`, `nfs_backend.cpp`, `curl_backend.cpp`, `share_config.*`) |
| Start/stop | init, from `persist.gammaos.share.<n>.enabled` (`gammaos-sharefs.rc`), at most 4 shares (`kMaxShares`) |
| Shipped | `build/make/target/product/base_system.mk:42` |
| SELinux | `system/sepolicy/private/gammaos_sharefs.te`; share properties are `gammaos_private_prop` |
| Mount | `/mnt/shares/<name>`, bound at `/mnt/user/0/<name>` (apps' `/storage/<name>`), `root:everybody 0770` |
| UIs | Nano Settings > Network Shares; Java `GammaShareConfig` for Settings; GammaShares DocumentsProvider |
| Nano awareness | ROM scan avoids per-frame stats on shares (`NanoMenuXmb.cpp:3255`); ES-DE store path prefixes (`NanoEsdeStore.cpp:645`); the scraper skips share paths (`NanoMenuScraper.cpp:434`) |

### 1.2 Problems found [V]
1. **The vendored libsmb2 is a pre-6.3 snapshot** (6.1.0 plus master, pinned `6723c56`).
   - libsmb2 6.3–6.3.2 (Oct–Nov 2026) fix memory bugs a hostile server reply can trigger: an out-of-bounds read in `smb2_decode_file_all_info`, a double free in QUERY_DIRECTORY, DCE/RPC integer overflows.
   - The vendored `lib/smb2-data-file-info.c:313-318` still has the bad length check.
   - sharefs runs as root.
2. **Rename over an existing file fails:** libsmb2 always sends `replace_if_exist = 0` (`libsmb2.c:2438-2442`). Config writers, ES-DE and Nano's ES-DE store all save by writing a temp file and renaming it over the old one.
3. **fsync does nothing** for SMB (`fuse_mount.cpp:279-282`). There are no lock operations, so locks are device-local.
4. **Everything is serialised:** one mutex, synchronous 128 KiB reads, and one cached file handle (`smb_backend.cpp:10-13, 111-128, 311-327`).
   - Two files read at once (a game plus its BIOS, or a scan) reopen each other.
   - A Nano library scan stalls the running game's reads.
5. **Slow crypto:** reference C AES with key expansion per block (`smb2-signing.c:126-166`), no AES-GMAC signing and no AES-GCM. Windows 11 24H2 Pro/Enterprise/Education require signing on every message, so those shares are CPU-bound, probably around 10 MB/s or less [I].
6. **Passwords:** kept in `persist.gammaos.share.<n>.pass`, hex of the password XORed with a keystream.
   - The key comes from "gammaos-sharefs" plus the first 127 bytes of `/system/build.prop`, which is identical on every device with the same build, so the key can be derived from the public image.
   - The code comment says the key "differs per device". It doesn't.
   - Passwords are capped at 45 characters. The device runs permissive (`enforcing=0` on the kernel command line), so SELinux isn't protecting the property.
7. **Not a storage volume:** missing from `StorageManager.getStorageVolumes()` and `getExternalFilesDirs()`, with no ExternalStorageProvider root.
   - Emulator storage pickers don't list it.
   - Nano's `/storage/<id>` ↔ `/mnt/media_rw/<id>` convention (`NanoLaunch.cpp:83,105,206`) doesn't hold.
8. **Launch links for picker-only emulators are wrong:** `safContentUri` (`NanoLaunch.cpp:87-151`) builds an `externalstorage` link naming the share, which that provider doesn't have. A `/mnt/shares/...` path falls through to `primary:ROMs/...`, a different local file. Affected: Dolphin, Azahar, ARMSX2, PPSSPP.
9. **Smaller issues:**
   - `statfs` reports 0 bytes free when the server query fails.
   - Inode numbers are libfuse's, not the server's file IDs, which defeats the storage denylist's 8.3-alias check (`MediaProvider.java:10870-10893`).
   - The attribute cache is cleared wholesale at 4,096 entries, so a big artwork folder evicts itself.
   - Share names are user labels, so renaming a share breaks every stored path.

### 1.3 Platform facts [V]
- **Kernel** 5.4.233 (Unisoc build, clang r416183b). The config has `FUSE_FS=y`, `MODVERSIONS=y`, CFI+LTO, no module signing, no CIFS and no NFS. `CRYPTO_MD4` and `DNS_RESOLVER` are absent too.
- **FUSE passthrough:** Android's backport is in the kernel; fuse-bpf isn't. Whether it's switched on (`persist.sys.fuse.passthrough.enable`) is unknown.
- **vold stub volumes:** `IVold.createStubVolume` exists (`VoldNativeService.cpp:446`), and the framework accepts the result as a removable public-style volume (`StorageManagerService.java:1708-1715`, `VolumeInfo` `TYPE_STUB`, ExternalStorageProvider root, `StorageSessionController.isSupportedVolume`).
  - Nothing calls it today.
  - `VolumeManager::reset()` drops stub disks without recreating them (`VolumeManager.cpp:1011-1024`).
  - A stub falls into `PrepareAppDirFromRoot` in `setupAppDir`, which likely fails on FUSE.
- **Emulators' storage access** goes through MediaProvider's FUSE: every non-exempt app gets `MOUNT_EXTERNAL_DEFAULT` under Thimsy.
- **Doze:** a root daemon's sockets bypass the Doze firewall (`netd.c:401-419`).
- **Libraries in the tree:** libsmb2 (LGPL-2.1, system only, no krb5); libnfs 16.2 (built without multithreading); libfuse 3.10 (METADATA says 3.8, stale; statically linked into sharefs, which METADATA flags `RESTRICTED_IF_STATICALLY_LINKED`, a licence item to check).

## 2. Design

### 2.1 Shares become drives (vold stub volumes)
Each enabled share is registered with vold, so Android, every app and Nano treat it as a removable drive.

1. sharefs mounts its FUSE filesystem at `/mnt/media_rw/<uuid>`, Nano's existing convention, instead of `/mnt/shares/<name>`.
2. sharefs bind-mounts it at `/mnt/user/0/<uuid>`, which apps see as `/storage/<uuid>`. Propagation into app namespaces already works (`main.cpp:75-112`).
3. sharefs calls `createStubVolume("/mnt/media_rw/<uuid>", "/storage/<uuid>", "smb", "<uuid>", "<share label>", kStubVisible)`.
4. **A stable UUID per share,** made when the share is added and never changed (renaming the share changes only its label).
   - Its format can't collide with GammaOS's synthetic USB UUIDs (`00000000-0000-0000-0000-%012d`).
   - Nano's volume-id checks (`emusync::isVolumeId`, `NanoUriGrant.java:56`) learn the format.
5. **vold patches:**
   - `setupAppDir` treats `kStub` like `kPublic` (`VolumeManager.cpp:1175`).
   - `reset()` recreates stub disks that carry a Thimsy "persistent" flag.
   - `vdc volume createStub|destroyStub` for bring-up from adb.
   - Relax the SELinux neverallow on `vold_service` lookup (`vold.te:64-72`) for `gammaos_sharefs`.
6. **The volume is destroyed only when the user disables the share,** never on a network drop. On a drop the mount stays and requests wait for the reconnect (section 2.4). Before disabling, Nano warns if a game is running from the share.
7. **No MediaProvider FUSE layer on top (design B).** Double FUSE would force uncached mode and `direct_io`; on 5.4, `direct_io` refuses shared mmap, which some emulators use.
   - Consequence: Thimsy's storage denylist isn't enforced by MediaProvider on shares.
   - sharefs enforces the same rules itself for uid ≥ 10000: other apps' `Android/data|obb|media`, Nano Backup, and the owned-files folders. It reuses `ThimsyStorageDenylist`'s rules and reports server file IDs as inode numbers so the 8.3-alias check works.
8. **MediaProvider never scans a network volume.** Patch `MediaService.onScanVolume` and the mounted broadcast to skip stub volumes with fsType `smb`/`nfs`. A first full scan of a big share over Wi-Fi would take a long time and fill the media database.
9. **GammaShares** stays for picker-only apps until the ExternalStorageProvider root is proven, then becomes optional.

**In Nano** (the drive model in `usb_storage_plan.md`): a share is a drive of kind *network*, labelled "Network: <label>". It appears in the folder picker, the ES-DE Drive and Emulator Settings Drive rows, the ROM scan and Move to Drive. Launch links for shares use ExternalStorageProvider's `<uuid>:` form, which the stub root makes valid, so fix 1.2.8 falls out of the stub registration.

### 2.2 What can live on a share
| Kind | On a share? | Why |
|---|---|---|
| ROMs, disc images | Yes | Read-mostly; the streaming cache (2.5) makes big ones start fast |
| Artwork, videos, manuals (ES-DE media) | Yes, with Nano's local thumbnail/index cache (2.6) | Thousands of small files are slow over SMB (ES-DE's own guide calls large SMB libraries "very painful") |
| ES-DE settings, gamelists, collections | Yes, after fixes 2.3.2–2.3.3, with the internal mirror from `usb_storage_plan.md` option A | Written with temp+rename; the mirror keeps the menus working while the share is offline |
| Emulator settings files | Yes, same conditions | Small and written rarely |
| Installed content (Vita3K `ux0/app`, `ux0/addcont`; Eden games, updates, DLC, `load/`, `sdmc/`; Dolphin `Load/`, `ResourcePacks/`; ARMSX2 `textures/`; PSP `GAME/`, `TEXTURES/`) | Yes, by "Move Installed Content to Network" (2.7) | Large and read-mostly |
| Saves, memory cards, save states, NAND, shader/pipeline caches, firmware, keys, BIOS | **No, stay local** | Written during play; a drop mid-write corrupts them; firmware and keys are needed at every boot |

Saves to the share are offered as a backup action instead (2.8), not as live storage.

*(Superseded in part, OA-49, OA-52, OA-53: there is no internal mirror of gamelists or collections, `usb_storage_plan.md` §2's note; on a card, saves, states, NAND and BIOS are shared storage with no mirror, `per_device_config_plan.md` §2.7. Whether a share may hold them is for this plan's next revision.)*

### 2.3 The SMB client: native, hardened
Phase 0 work, worth doing even before the rest:
1. **Update libsmb2 to 6.3.2** (a drop-in security release). Then evaluate 7.0.0 (19 Nov 2026), which breaks the API: per-thread contexts, optional OpenSSL 3 crypto, CSPRNG nonces, a signature-verification fix, and share enumeration as a flat API. sharefs doesn't use share enumeration yet, so the break is small.
2. **Rename with replace:** set `replace_if_exist = 1` for rename. Upstream libsmb2 needs a parameter; carry a patch until it lands.
3. **fsync:** send SMB2 FLUSH on fsync and on close of a written file.
4. **`statfs` fallback:** report the last known free space, or a large value, when the server query fails, never 0.
5. **Passwords** (see 2.9).
6. **Hygiene:** `prctl(PR_SET_DUMPABLE, 0)`; wipe decrypted secrets after use; keep logs free of passwords (they are today).

Phase 3, performance:
1. **Concurrency:** replace the one mutex and sync calls with per-open handles (`fi->fh`) and a small pool of SMB contexts driven by `smb2_pread_async` (about 4 requests in flight). RetroArch's own client uses 4 contexts.
   - Metadata and data use separate contexts, so a library scan never blocks the game.
2. **FUSE:**
   - port to libfuse's low-level API (needed for passthrough and per-handle state);
   - `max_read` 1 MiB if the 5.4 kernel negotiates `max_pages` [I];
   - an LRU attribute cache with longer timeouts for read-only subtrees;
   - `negative_timeout`;
   - `keep_cache` for validated files;
   - `FUSE_FILL_DIR_PLUS`;
   - a short-TTL directory listing cache.
3. **Crypto:**
   - AES-CMAC and HMAC-SHA256 through BoringSSL, which uses the ARMv8 crypto extensions;
   - an SMB 3.1.1 signing-capabilities context offering AES-GMAC, which RetroArch documents as several times faster than CMAC;
   - AES-128-GCM sealing through BoringSSL `EVP_AEAD`;
   - CCM stays portable (BoringSSL has no matching AEAD).
4. **NFS:** use libnfs's async API, or build its multithreading, for the same pipelining. NFS lists directories with READDIRPLUS and has no crypto cost, so it's the fast choice for a NAS. It has no user authentication, though (AUTH_SYS trusts the IP).

### 2.4 Staying connected: sleep, Wi-Fi drops, a NAS asleep
- **On wake** (screen-on, and on a netlink link or route change): reconnect up front instead of waiting for the first failed read.
- **While reconnecting,** FUSE requests wait up to 30 s instead of failing after 10 s, so a game loading from the share survives a wake, or a NAS spinning up.
- **Cached data** (2.5) is served without the network at all.
- **Wi-Fi:** while a share is in use by a running game, Nano asks for low-latency Wi-Fi (`cmd wifi force-low-latency-mode`) and drops it afterwards. Long copies and scans hold a wakelock (`/sys/power/wake_lock`, as `NanoMenuOta.cpp` does).
- **Wake-on-LAN:** an optional MAC address per share. Nano sends a magic packet before connecting, for NAS boxes that sleep.
- **UI:** Network Shares shows each share's state: Connected, Reconnecting, Offline (with the server error), or Signed in as guest. The ROM list shows a share's systems greyed with "Network share offline" instead of an empty library.

### 2.5 Streaming cache for large games
Inside sharefs, below both plain paths and GammaShares, so every emulator benefits with no per-app changes.

- **Blocks:** 1 MiB, with one bit per block in a bitmap (2 KiB for a 16 GB file). Runs of missing blocks are fetched as 1–4 MiB requests, capped by `smb2_get_max_read_size`.
- **Queues, by priority:**
  1. **Demand:** a read waits only for the blocks it covers.
  2. **Read-ahead:** on sequential reads, 8 MiB growing to 64 MiB.
  3. **Launch set:** the blocks a game touched in its first ~60 s last time it was launched. With no trace yet: the first 32 MiB and the last 1 MiB.
  4. **Background fill:** a whole-file fill for the game being played, up to 8 GB, or any pinned game, on its own connection.
- **Where it lives:**
  - **Default:** `/data/misc/thimsy_netcache/<share-uuid>/<hash(path)>/`, with `data` (a sparse file on f2fs), `map` (bitmap plus CRC32C per block) and `meta` (size, server mtime, file ID, last launch, pinned flag, launch trace). It has its own SELinux type, writable only by sharefs, and isn't visible to apps.
  - **Optional on the microSD card:** FAT/exFAT has no sparse files, so it uses 64 MiB segment files under `.thimsy-netcache/` with `.nomedia`.
- **Integrity:**
  - every open checks size, server mtime and file ID, and a mismatch drops the entry;
  - block data is written before its bitmap bit, and the bitmap is persisted every 32 MiB and on close;
  - blocks lost in a crash are simply fetched again;
  - CRCs are checked on read from a card.
- **Read-only:** opening a cached file for writing invalidates the entry and writes through to the server. No write-back.
- **Passthrough:** if FUSE passthrough works on this kernel, opening a complete cached file hands the kernel the cache file directly. Reads and mmap then run at local speed. That also fixes the FUSE mmap memory blow-up recorded in `FakeJNI.cpp:281-306`, and Eden's mmap of games.
- **Eviction:**
  - the cap is min(32 GB, 25% of free `/data`), and it always keeps 10 GB free;
  - least recently launched goes first, partial entries before complete ones;
  - open or pinned entries are never evicted.
- **Keep on Device:** a game option that pins and fills the game completely. That game is then playable with the share offline.
- **Nano control:** extended attributes on the share path, accepted only from root/system, with no new socket.
  - `user.thimsy.cache=warm:<MiB>` when a game has been highlighted ~0.5 s (budget 256 MiB); moving the highlight cancels.
  - `=launch` on select, which raises the launch set to demand priority.
  - `=pin` / `=unpin`.
  - `user.thimsy.cache.state` reads back cached/total/complete/pinned, for an "on device" badge and a progress bar.
  - On select, if the launch set is ≥ 90% cached, or there is no trace, the game starts at once. Otherwise "Preparing game… N/M MB" shows for at most ~10 s, then the game starts anyway.
- **Emulator settings that defeat streaming:** ARMSX2's CDVD precache reads the whole ISO into RAM before boot, and PPSSPP's "Cache full ISO in RAM" does too. Nano turns them off for games on a share.

**Expected start-up** [I], assuming about 25 MB/s over Wi-Fi 5 after the performance work:

| Game | Copy first | Streaming, first launch | After a highlight warm or a second launch |
|---|---|---|---|
| PS2 ISO 4.4 GB | ~3 min | local + 1–3 s | near-local |
| GC/Wii RVZ 1–4 GB | 1–3 min | local + 1–3 s | near-local |
| PSP CSO 0.5–1.8 GB | 20–70 s | local + 1–2 s | local |
| Switch XCI 8–16 GB (Eden) | 5–11 min | local + 5–20 s; loading screens may stretch | near-local; pin for smooth play |
| Vita title, 1–4 GB folder | 1–3 min | local + 5–15 s, dominated by metadata | near-local with a folder warm |

### 2.6 Big libraries over Wi-Fi
- **A background library index:** Nano lists share ROM folders on its own SMB context, not the game's. It caches the listing (name, size, mtime) on internal storage and refreshes it in the background. The menus show the cached list at once.
- **Artwork:** Nano keeps downscaled thumbnails of share artwork in a local cache keyed by path and mtime. Full-size images and videos stream on demand.
- **Header cache:** Dolphin, ARMSX2 and Eden read every game's header during their game-list scans. The streaming cache keeps those first blocks permanently (they're tiny), so rescans are fast.

### 2.7 Move installed content to the network
A new action, **Move Installed Content to Network...**, separate from the settings moves. It uses the FileCopyPair engine and the move rules (copy without overwriting, the share wins, the leftovers prompt).

| Emulator | What moves | How it's pointed there |
|---|---|---|
| Vita3K | `ux0/app/<TID>`, `ux0/addcont/<TID>` (and `ux0/patch` if Vita3K uses it [I]) | Symlinks from `ux0/` on internal storage (`Android/data` has no FUSE layer for apps, so links work there). `pref-path` stays local with saves, firmware and cache. Not possible while Vita3K's files are on a FAT/exFAT card, which can't hold symlinks. |
| Eden (user-installed) | Games (XCI/NSP); updates and DLC as **External Content** folders; `load/` (mods); `sdmc/` | Nano writes Eden's `config.ini` `[Data Storage]` keys (`load_directory`, `sdmc_directory`) and adds the share's game and External Content folders. `nand/` (saves, firmware, registered content), `keys/` and `cache/shader` stay local. Eden's own pickers can't pick a share folder until the stub root exists [I]. |
| Dolphin | `Load/` (HD textures, Riivolution), `ResourcePacks/` | `Dolphin.ini [General] LoadPath`, `ResourcePackPath` (plain paths). The Wii NAND stays local. The README's "Dolphin has no setting for these folders" is wrong for Load, NAND and resource packs; correct it. |
| ARMSX2 | `textures/` | `[Folders]` key. Memory cards, states and cache stay local. |
| PPSSPP | `PSP/GAME`, `PSP/TEXTURES` | Symlinks, when the memstick is on internal storage. |
| Azahar | Nothing (its one root mixes installed content with saves and shaders) | Keep `.cci/.zcci` games on the share and launch them directly, instead of installing CIAs. |

- Before launch, Nano checks that the share is connected and the moved folders exist. That mirrors today's "needs the card in to start".
- The settings sync (NanoEmuSync) isn't extended to installed content.

### 2.8 Other features
- **Discovery** (shared with `file_sharing_plan.md`):
  - **Native one-shot mDNS** queries for `_smb._tcp`, `_nfs._tcp`, `_webdav._tcp` and `_device-info._tcp`. Replies are unicast, which sidesteps Android's multicast filtering. This finds macOS and Avahi NAS boxes.
  - **WS-Discovery** finds Windows PCs and wsdd.
  - **NetBIOS name queries** are the fallback.
  - After sign-in, `smb2_share_enum` lists the server's disk shares (hiding `$` shares).
  - This runs in sharefs or Nano without a JVM.
- **Add Share wizard:** pick a found server, sign in (or guest), pick a share and a folder, then a speed test with a suggestion ("Good for games" / "Slow: keep artwork local").
- **Clear errors** for the common failures:
  - Windows 11 24H2 refuses guest access;
  - a Microsoft-account PC with PIN-only sign-in has no usable password;
  - a router that only speaks SMB1, which libsmb2 doesn't (FTP suggested instead);
  - wrong password;
  - a share not found.
- **Save backup to the share:** a manual or scheduled "Back Up Saves to Network" that copies each emulator's saves to `<share>/Thimsy Backups/<device>/<date>/`. Restore is one action. Not live storage.
- **Syncthing** already ships. The plan notes it as the alternative for people who want their library mirrored locally rather than streamed (Batocera's advice for handhelds on Wi-Fi).
- **RetroArch's own SMB client:** upstream RetroArch master has a built-in SMB2/3 client using `smb://` URLs. It isn't confirmed in 1.22.2. Thimsy's volume approach works for every emulator, so RetroArch's client isn't needed. Check later that it doesn't conflict.

### 2.9 Passwords
- **Move secrets out of properties** into `/data/misc/thimsy-shares/` (0600, root, its own SELinux type), readable by sharefs, Nano (bootanim) and Settings (system_app) only.
- **Encrypt with AES-256-GCM** under a key in Keystore2, in a new keystore namespace for sharefs (precedent: `vold_key`, `odsign_key`). Fallback: a random per-device key file made at first boot.
  - Whether the Cube's KeyMint is hardware-backed is unverified.
  - The key isn't auth-bound, so shares can mount before unlock.
- **SMB stores only the NT hash** (`ntlm:<hash>`, which libsmb2 accepts in place of a password), never the plain password, which is often a reused Windows or Microsoft-account one. WebDAV and FTP still need the plain password.
- **Migration:** existing `persist.gammaos.share.<n>.pass` values are decoded once, re-stored, and the properties cleared.
- **Backups:** secrets never live under `/storage`, so Nano Backup never includes them. Any future settings export must exclude the secrets folder.

## 3. Phases

| Phase | Content | Depends on |
|---|---|---|
| 0 | libsmb2 6.3.2; rename-replace; FLUSH on fsync; `statfs` fallback; password storage; dumpable off; README section on Network Shares | — |
| 1 | Prove on a Cube (section 4) | 0 |
| 2 | Stub volume registration, stable UUIDs, vold patches, no MediaProvider scan, denylist rules in sharefs; Nano drive kind "network"; launch links | 1, USB plan |
| 3 | Concurrency, low-level FUSE, caches, BoringSSL crypto + GMAC + GCM; reconnect and wake handling | 0 |
| 4 | Streaming cache, Keep on Device, highlight warm, launch traces | 3 (passthrough is part of 4 if 1 proves it) |
| 5 | Move Installed Content to Network; the library index and thumbnail cache | 2, 4 |
| 6 | Discovery, Add Share wizard, errors, Wake-on-LAN, save backup | 2 |
| Later | libsmb2 7.x; NFS pipelining; SFTP (needs libssh2, not in the tree); kernel CIFS only with Unisoc's exact kernel source | — |

## 4. To prove on a Cube first
1. `getprop persist.sys.fuse.passthrough.enable`, and whether the passthrough ioctl works from sharefs.
2. Register a stub volume by hand (`vdc` patch). Check:
   - it shows in `sm list-volumes`, the file picker and `getStorageVolumes()`;
   - Nano and RetroArch open `/storage/<uuid>/...`;
   - it survives a system_server restart (vold reset).
3. `getExternalFilesDirs()` on the stub, with and without the `setupAppDir` patch.
4. Throughput and latency on the Cube's Wi-Fi, today's sharefs versus a pipelined build:
   - ROM load times per emulator;
   - a 5,000-game ES-DE scan;
   - Windows 11 Pro (signed) versus a Samba NAS (unsigned).
5. Sleep and wake: Wi-Fi association, reconnect time, a game reading from the share across sleep.
6. Emulators that mmap or lock files (Dolphin, Vita3K, ARMSX2, Azahar, PPSSPP, Eden) on a share.
7. Rename-over-existing from RetroArch, PPSSPP, Dolphin and ES-DE settings saves, before and after the libsmb2 fix.
8. Whether the 5.4 kernel accepts a 1 MiB `max_read`.

## 5. Risks
- **Security:** the vendored libsmb2 parses server replies as root. Phase 0 fixes the known bugs, but a hostile server on the network remains an attack surface.
  - A seccomp filter and a non-root uid for sharefs after mounting are worth evaluating.
  - SELinux is permissive on this build, so its rules don't contain a compromise.
- **Data loss:** anything written to a share during play. The policy in 2.2 keeps saves local, and the mirror protects settings.
- **Wi-Fi realities:** a share over Wi-Fi is slower and less reliable than a card. The cache and the offline UI make it tolerable, not equal.
- **Space:** the cache uses internal space the feature is meant to free. The cap and the free-space floor are essential.
- **vold changes:** stub volumes are lightly used in AOSP. Lifecycle bugs could leave a dead `/storage/<uuid>`.
- **Licence:** libfuse statically linked into sharefs (`RESTRICTED_IF_STATICALLY_LINKED`). Link it dynamically, or confirm the NOTICE covers it.

## 6. Questions for the user
1. **Protocols:** SMB first, as asked. Keep NFS, WebDAV and FTP, which already exist, as they are with fixes, or hide them?
2. **Should ES-DE settings and emulator settings be allowed on a share at all**, given the offline risk (with the internal mirror), or should a share be offered only for ROMs, artwork and installed content?
3. **Cache location:** internal storage only, or let the user put the cache on the microSD card?
4. **Phase 0 (the libsmb2 security update and the password fix) is worth doing in the next build regardless of the rest.** OK to do it then?
