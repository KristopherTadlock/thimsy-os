################################################################################
#
# thimsy-padd
#
################################################################################

# thimsy-padd (IMPLEMENTATION.md F.2): one virtual gamepad, "Thimsy Gamepad",
# through uinput (decision I13). S02padd starts /usr/bin/thimsy-padd (G3a).
#
# Its source is in this external tree (linux/src/thimsy-padd/, MIT), which a
# public release publishes with the rest of linux/ at the tag, so a local site
# is enough here (IMPLEMENTATION.md E.3): legal-info saves no source for it.
THIMSY_PADD_VERSION = 1.0
THIMSY_PADD_SITE = $(BR2_EXTERNAL_THIMSY_PATH)/src/thimsy-padd
THIMSY_PADD_SITE_METHOD = local
THIMSY_PADD_LICENSE = MIT
THIMSY_PADD_LICENSE_FILES = LICENSE
THIMSY_PADD_DEPENDENCIES = libthimsy
THIMSY_PADD_CONF_OPTS = -DTHIMSY_BUILD_TESTS=OFF

$(eval $(cmake-package))
