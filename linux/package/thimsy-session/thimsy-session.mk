################################################################################
#
# thimsy-session
#
################################################################################

# thimsy-session (IMPLEMENTATION.md F.1): starts and supervises Nano, drives,
# hotplug, power, sleep and the deferred services. inittab respawns it as
# /usr/libexec/thimsy/session, so its CMake install puts it there (G3b).
#
# Its source is in this external tree (linux/src/thimsy-session/, MIT), which a
# public release publishes with the rest of linux/ at the tag, so a local site
# is enough here (IMPLEMENTATION.md E.3): legal-info saves no source for it.
# The version is the commit (build.sh passes THIMSY_GIT_SHA), so every commit
# gets its own build directory and the sources are synced again: Buildroot
# rsyncs a local site only once per build directory (build-integration-3).
THIMSY_SESSION_VERSION = $(or $(THIMSY_GIT_SHA),dev)
THIMSY_SESSION_SITE = $(BR2_EXTERNAL_THIMSY_PATH)/src/thimsy-session
THIMSY_SESSION_SITE_METHOD = local
THIMSY_SESSION_LICENSE = MIT
THIMSY_SESSION_LICENSE_FILES = LICENSE
THIMSY_SESSION_DEPENDENCIES = libthimsy
THIMSY_SESSION_CONF_OPTS = -DTHIMSY_BUILD_TESTS=OFF

$(eval $(cmake-package))
