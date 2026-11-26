################################################################################
#
# libretro-fceumm
#
################################################################################

# The fceumm core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_FCEUMM_VERSION = TODO
LIBRETRO_FCEUMM_SITE = $(call github,libretro,libretro-fceumm,$(LIBRETRO_FCEUMM_VERSION))
LIBRETRO_FCEUMM_LICENSE = GPL-2.0+
LIBRETRO_FCEUMM_LICENSE_FILES = Copying
LIBRETRO_FCEUMM_MAKEFILE = Makefile.libretro

$(eval $(libretro-core-package))
