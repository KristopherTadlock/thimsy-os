################################################################################
#
# libretro
#
################################################################################
# The shared recipe of the libretro cores (IMPLEMENTATION.md E.3). Not a package:
# this file defines two package infrastructures that each
# package/libretro-<core>/libretro-<core>.mk expands, and external.mk includes
# it before the cores (decision G2a-16).
#
# A core built by its own Makefile:
#
#   LIBRETRO_<CORE>_VERSION = <commit>       mods/retroarch-cores.src.lock's
#                                            commit for the core (decision I10)
#   LIBRETRO_<CORE>_SITE = ...               the lock's repository
#   LIBRETRO_<CORE>_LICENSE = ...
#   LIBRETRO_<CORE>_LICENSE_FILES = ...
#   $(eval $(libretro-core-package))
#
# Optional, with their defaults:
#   LIBRETRO_<CORE>_CORE       the library name: the package name without
#                              "libretro-", "-" as "_" (libretro-pcsx-rearmed:
#                              pcsx_rearmed); the core is installed as
#                              /usr/lib/libretro/<CORE>_libretro.so
#   LIBRETRO_<CORE>_SUBDIR     the directory of the core's Makefile, relative to
#                              the source (the top)
#   LIBRETRO_<CORE>_MAKEFILE   Makefile
#   LIBRETRO_<CORE>_PLATFORM   the platform= argument: unix
#   LIBRETRO_<CORE>_MAKE_OPTS  more make arguments (GLES3=1 and the like)
#   LIBRETRO_<CORE>_SO         the file the build writes, relative to SUBDIR
#                              (CMake cores: to the build directory):
#                              <CORE>_libretro.so
#
# A core built with CMake: the same variables plus the cmake-package ones
# (_CONF_OPTS), and $(eval $(libretro-cmake-core-package)).
#
# Every enabled core adds its library name to LIBRETRO_CORE_NAMES, from which
# retroarch.mk installs the matching core info files.
#
# The compiler and the flags reach a core's Makefile through the environment
# (TARGET_CONFIGURE_OPTS) so that a Makefile's own "CFLAGS +=" lines still
# apply; CC, CXX and AR are also given on the command line, because some core
# Makefiles assign them for platform=unix.

LIBRETRO_INSTALL_DIR = /usr/lib/libretro

# libretro-core-defaults PKGNAME PKGVAR: the optional variables' defaults.
define libretro-core-defaults
$(2)_CORE ?= $$(subst -,_,$$(patsubst libretro-%,%,$(1)))
$(2)_SUBDIR ?=
$(2)_MAKEFILE ?= Makefile
$(2)_PLATFORM ?= unix
$(2)_MAKE_OPTS ?=
$(2)_SO ?= $$($(2)_CORE)_libretro.so
$(2)_INSTALL_STAGING = NO

ifeq ($$(BR2_PACKAGE_$(2)),y)
LIBRETRO_CORE_NAMES += $$($(2)_CORE)
endif
endef

# libretro-install-core FILE CORE: installs one built core.
define libretro-install-core
	$(INSTALL) -D -m 0644 $(1) $(TARGET_DIR)$(LIBRETRO_INSTALL_DIR)/$(2)_libretro.so
endef

define inner-libretro-core-package
$(call libretro-core-defaults,$(1),$(2))

$(2)_SRC_SUBDIR = $$(@D)$$(if $$($(2)_SUBDIR),/$$($(2)_SUBDIR))

ifndef $(2)_BUILD_CMDS
define $(2)_BUILD_CMDS
	$$(TARGET_MAKE_ENV) $$(TARGET_CONFIGURE_OPTS) \
		$$(MAKE) -C $$($(2)_SRC_SUBDIR) -f $$($(2)_MAKEFILE) \
		CC="$$(TARGET_CC)" CXX="$$(TARGET_CXX)" AR="$$(TARGET_AR)" \
		platform="$$($(2)_PLATFORM)" $$($(2)_MAKE_OPTS)
endef
endif

ifndef $(2)_INSTALL_TARGET_CMDS
define $(2)_INSTALL_TARGET_CMDS
	$$(call libretro-install-core,$$($(2)_SRC_SUBDIR)/$$($(2)_SO),$$($(2)_CORE))
endef
endif

$(call inner-generic-package,$(1),$(2),$(3),$(4))
endef

define inner-libretro-cmake-core-package
$(call libretro-core-defaults,$(1),$(2))

ifndef $(2)_INSTALL_TARGET_CMDS
define $(2)_INSTALL_TARGET_CMDS
	$$(call libretro-install-core,$$($(2)_BUILDDIR)/$$($(2)_SO),$$($(2)_CORE))
endef
endif

$(call inner-cmake-package,$(1),$(2),$(3),$(4))
endef

libretro-core-package = $(call inner-libretro-core-package,$(pkgname),$(call UPPERCASE,$(pkgname)),$(call UPPERCASE,$(pkgname)),target)
libretro-cmake-core-package = $(call inner-libretro-cmake-core-package,$(pkgname),$(call UPPERCASE,$(pkgname)),$(call UPPERCASE,$(pkgname)),target)
