# Thimsy Base (BR2_EXTERNAL name THIMSY): every package recipe under package/
# and gpl/package/.
#
# The libretro cores' shared macro (package/libretro/libretro.mk) comes first:
# $(sort) puts "libretro-<core>/" before "libretro/" ('-' sorts before '/'),
# and each core's recipe expands the macro as it is read.
include $(wildcard $(BR2_EXTERNAL_THIMSY_PATH)/package/libretro/libretro.mk)
include $(filter-out %/package/libretro/libretro.mk,\
	$(sort $(wildcard $(BR2_EXTERNAL_THIMSY_PATH)/package/*/*.mk)))
include $(sort $(wildcard $(BR2_EXTERNAL_THIMSY_PATH)/gpl/package/*/*.mk))
