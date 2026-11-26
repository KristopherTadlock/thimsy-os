# boot.cmd: the RK3566 family's boot script (U-Boot hush).
# SPDX-License-Identifier: MIT
#
# Finds the partition named "boot" on the SD card, reads the 512-byte
# header tools/fit.py writes (decision G2b-6: the magic "THIMSFIT" as two
# u32 words at +0 and +4, the FIT's length in sectors as a u32 at +8), reads
# exactly that many sectors of FIT from the next sector, takes the static
# bootargs from the FIT's default configuration (fdt get value; bootm itself
# ignores that property, G2b-5), appends root=PARTUUID= of entry 3 of the
# same card (what S00base resolves the boot disk from, E.5) and runs bootm.
# Addresses are fit.py's FAMILIES["rk3566"] (G2b-7): header at 0x0c000000,
# FIT at 0x0a000000; the FIT decompresses the kernel to 0x02000000.
#
# Every line is one whole command (an "if" is closed on its line) and lines
# starting with "#" are comments, so the file also works as one bootcmd:
# drop the comments and join the lines with "; " (tests/test_post_image.py
# checks this). How U-Boot gets it is board/rk3566/uboot/uboot.fragment's
# (Phase 2a, FlipBase's options): CONFIG_BOOTCOMMAND, or a default
# environment file as FlipBase's board/my355/uboot.env does.
# TODO(Phase 2a): embed it there.
#
# Needs CMD_MMC, CMD_PART, PARTITION_UUIDS, CMD_SETEXPR, CMD_FDT, FIT.
# Guesses, to check on the ARC-D and ARC-S:
#   - (guess) the SD slot is mmc 1 and the eMMC mmc 0 on the rgxx3 board;
#     the script never reads mmc 0 (the SD-only principle, plan 1), and on
#     Android's eMMC the magic check would refuse its "boot" anyway;
#   - (guess) "part start" and "setexpr" print hexadecimal without "0x",
#     which "mmc read" and "setexpr" read back as hexadecimal;
#   - (guess) setexpr's one-operand form with "*ADDR" loads a u32 (".l").
# Not checked here: the sector count against the 32 MiB FIT limit (U-Boot's
# "test" compares decimal only); fit.py refuses a larger FIT when it writes
# boot.img, and the 40 MiB partition bounds what can be read.

setenv thimsy_dev 1
setenv thimsy_hdr 0x0c000000
setenv thimsy_fit 0x0a000000
if mmc dev ${thimsy_dev}; then echo "thimsy: mmc ${thimsy_dev}"; else echo "thimsy: no SD card at mmc ${thimsy_dev}"; exit; fi
if part start mmc ${thimsy_dev} boot thimsy_bs; then echo "thimsy: boot at sector ${thimsy_bs}"; else echo "thimsy: no partition named boot"; exit; fi
mmc read ${thimsy_hdr} ${thimsy_bs} 1
setexpr.l thimsy_m0 *${thimsy_hdr}
setexpr thimsy_a ${thimsy_hdr} + 4
setexpr.l thimsy_m1 *${thimsy_a}
if test "${thimsy_m0}" = "4d494854" && test "${thimsy_m1}" = "54494653"; then echo "thimsy: FIT header found"; else echo "thimsy: boot holds no Thimsy FIT header"; exit; fi
setexpr thimsy_a ${thimsy_hdr} + 8
setexpr.l thimsy_n *${thimsy_a}
setexpr thimsy_fs ${thimsy_bs} + 1
if mmc read ${thimsy_fit} ${thimsy_fs} ${thimsy_n}; then echo "thimsy: FIT read"; else echo "thimsy: reading the FIT failed"; exit; fi
fdt addr ${thimsy_fit}
fdt get value thimsy_conf /configurations default
fdt get value thimsy_args /configurations/${thimsy_conf} bootargs
if part uuid mmc ${thimsy_dev}:3 thimsy_root; then echo "thimsy: root ${thimsy_root}"; else echo "thimsy: no entry 3"; exit; fi
setenv bootargs "${thimsy_args} root=PARTUUID=${thimsy_root}"
bootm ${thimsy_fit}
