################################################################################
#
# libretro-fake08
#
################################################################################

# The fake08 core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.
# Its libraries are git submodules (guess).

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_FAKE08_VERSION = TODO
LIBRETRO_FAKE08_SITE = https://github.com/jtothebell/fake-08.git
LIBRETRO_FAKE08_SITE_METHOD = git
LIBRETRO_FAKE08_GIT_SUBMODULES = YES
LIBRETRO_FAKE08_LICENSE = MIT
LIBRETRO_FAKE08_LICENSE_FILES = LICENSE.MD
LIBRETRO_FAKE08_SUBDIR = platform/libretro

$(eval $(libretro-core-package))
