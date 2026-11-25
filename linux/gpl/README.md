# linux/gpl: copyleft and third-party material

Everything under `linux/gpl/` is licensed apart from the rest of `linux/`
(which is MIT, `linux/LICENSE`). This directory holds material whose licence
comes from the code it changes or was taken from, so that the licence lines in
the tree stay directory lines (decisions D6 and I9):

| Path | What | Licence |
|---|---|---|
| `COPYING` | the GNU General Public License, version 2 | |
| `generated/<family>/linux/patches/`, `generated/<family>/linux/config`, `generated/<family>/dts/` | ROCKNIX's kernel patches, kernel config and out-of-tree device trees for a family, written by `thimsy-import` (or by hand for RK3566 in Phase 2a), ROCKNIX's file names kept, each with a provenance line | GPL-2.0 (`COPYING`) |
| `board/<family>/linux/patches/` | FlipBase's kernel patches for the family and Thimsy's | GPL-2.0 (`COPYING`) |
| `board/<family>/uboot/patches/` | FlipBase's U-Boot series and Thimsy's U-Boot fixes | GPL-2.0+ (U-Boot's licence) |
| `package/` | Buildroot recipes ported from GPL sources (ROCKNIX's `package.mk` files, `rocknix-joypad` later) | GPL-2.0 (`COPYING`), as Buildroot's own recipes |
| `patches/<package>/` | Thimsy's patches to third-party packages, read through `BR2_GLOBAL_PATCH_DIR` | **each patch under the licence of the package it patches**: GPL-3.0 for RetroArch, zlib for SDL, GPL-2.0+ for PPSSPP, and so on |

So `linux/gpl/` is "copyleft and third-party patches", not "GPL-2.0 only".
Every patch file starts with an SPDX line naming its licence
(`SPDX-License-Identifier: GPL-3.0-or-later` and so on) and, for imported
files, a provenance line (upstream project, path and commit).

When the tree is split into its own repository (`git subtree split
--prefix=linux`), this directory keeps its licences unchanged; a release
publishes it with the rest of `linux/` at the tag, beside Buildroot's
`legal-info` output (decision D16).
