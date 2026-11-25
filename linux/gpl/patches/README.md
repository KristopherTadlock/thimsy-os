# linux/gpl/patches: patches to third-party packages

Buildroot reads this directory through `BR2_GLOBAL_PATCH_DIR`
(`configs/fragments/common.fragment`): a patch for package `<pkg>` lives in
`<pkg>/` or `<pkg>/<version>/`, applied in file-name order after the package's
own patches. A `<pkg>.hash` file here is read like the one beside the recipe.

Each patch carries the licence of the package it patches (decision I9), stated
on an `SPDX-License-Identifier:` line at its top, and says where it came from
when it was not written for Thimsy. Planned: the RetroArch `drm_ctx.c` rotation
patch (GPL-3.0), the SDL3 KMSDRM rotation patch (zlib), and DRM fixes for
PPSSPP and RetroArch.
