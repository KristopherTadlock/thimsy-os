################################################################################
#
# libretro-mednafen-ngp
#
################################################################################

# The mednafen_ngp core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_MEDNAFEN_NGP_VERSION = TODO
LIBRETRO_MEDNAFEN_NGP_SITE = $(call github,libretro,beetle-ngp-libretro,$(LIBRETRO_MEDNAFEN_NGP_VERSION))
LIBRETRO_MEDNAFEN_NGP_LICENSE = GPL-2.0
LIBRETRO_MEDNAFEN_NGP_LICENSE_FILES = COPYING

$(eval $(libretro-core-package))
