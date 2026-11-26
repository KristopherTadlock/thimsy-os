################################################################################
#
# libretro-sameboy
#
################################################################################

# The sameboy core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.
# Upstream SameBoy builds its boot ROMs with rgbds; the libretro fork is
# expected to carry them prebuilt (guess, checked at the first build).

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_SAMEBOY_VERSION = TODO
LIBRETRO_SAMEBOY_SITE = $(call github,libretro,SameBoy,$(LIBRETRO_SAMEBOY_VERSION))
LIBRETRO_SAMEBOY_LICENSE = MIT
LIBRETRO_SAMEBOY_LICENSE_FILES = LICENSE
LIBRETRO_SAMEBOY_SUBDIR = libretro

$(eval $(libretro-core-package))
