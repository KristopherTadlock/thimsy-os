"""The RG Cube vendor charger manager (vendor_dlkm lib/modules/sprd-charger-manager.ko) and its patch.

Test 42 DK1 (out/dp-evidence/test42/charge/SYNTHESIS.md section 3b, Change 1 option B1;
charge/charger-manager/NOTES.md; charge/cm1/README.txt).

PDHUB (cm_update_charge_info, .text 0x2178):
  - the charge currents come from the BC1.2 type alone (desc+216, the jump table at
    .rodata+0x1c for types 1-7, anything else the UNKNOWN tables). Behind a USB-C PD
    dock the Cube is USB host to the dock's hub, BC1.2 sees the hub's upstream port as
    SDP and the Cube charged at the SDP 0.5 A (about 2.4 W) whatever the PD contract.
    Now an SDP type is read as 0 (UNKNOWN: the 1.05 A JEITA table, 5 V) while is_pd_hub
    (desc+912) is set. sprd_tcpm's PD rx handler sets it, through cm_sprd_charger_ops
    (cm_check_pd_update_ac_usb_online, 0x7724, which stores it only while external power
    is online), for any Discover Identity ACK whose UFP product type is a hub or an AMA
    (sprd_tcpm_pd_rx_handler 0xc4e0-0xc4ec: (VDO & 0x18000000) == 0x08000000, which
    matches HUB 001 and AMA 101), whatever the contract's 5 V current; it is cleared
    on every unplug (misc_event_handler's not-online path, 0x148c, the same path that
    zeroes the type at 0x1420) and on the AC-online path of cm_update_charger_type_status
    (0x2090). A genuine PC port, and DCP, CDP and UNKNOWN sources, are judged as before.
    The type stored in desc+216 is unchanged (the log still says type 1); only the choice
    of tables is.
  - NOT CONTRACT-SAFE ON ITS OWN. Nothing in the gate looks at the PD contract, and the
    AW32257 has no input current limit. A PD hub or AMA (a video adapter with PD
    pass-through, say) whose 5 V PDO is below 1 A gets the UNKNOWN table: 1.05 A (ICHG
    992 mA, about 1 A from VBUS) at 15-52 C, and 0.7 A instead of 0.5 A at 0-15 C and
    52-60 C. That includes off-mode charging, which loads this same vendor_dlkm copy
    (modules.load.charger, with sprd_tcpm), where Nano cannot vote the current down.
    SYNTHESIS 3b pairs B1 with the Nano contract cap (B2's down-vote: a 500000 MIN vote
    on constant_charge_current unless the 5 V contract is 1 A or more). That down-vote
    does not exist. Owner decision D11 (accepted for test 42, see "Release" below): the
    patch ships without it, and a sub-1 A hub or AMA may be asked for about 1 A in every
    context (with or without Nano) until the cave is extended to require a 5 V contract of
    1 A or more.
  - hook: 0x21a4 ldr w9, [x21, #216] -> bl CAVE. x21 is cm->desc ([x0, #24], 0x2190); the
    prologue saved x30 (0x217c on the shadow stack, 0x2180 on the stack, both restored at
    0x25e4 and 0x25f0) and the bl to mutex_lock at 0x21a0 already overwrote it; w8 is
    reloaded at 0x21a8 and the flags set at 0x21b0, so the cave may use both. Type 0 makes
    0x21ac's w10 0xffffffff, and 0x21b8's b.hi takes the UNKNOWN tables (0x2200).
  - cave: sprd_fchg_change (0x12040, a power-supply notifier of the fast-charge
    framework). Its only reference is its CFI jump-table slot (0x122a8), and that slot is
    formed only in sprd_fchg_extcon_init (0x107bc-0x107c8, base 0x1229c + 0xc), after
    device_property_present("sprd,support-fchg") has returned false and the function has
    returned (0x106f8). The Cube's charger-manager node has no such property and
    patch_dtbo.py forbids adding it, so the notifier is never registered and the code is
    never run. Its first 7 words carry no relocation.

PD10W (CM2, patch_module's pd10w switch, off by default there, on in patch_tested(), the
release build's patch; it needs PDHUB's hook and is applied on top of it;
out/dp-evidence/test42/charge/cm2/NOTES.md):
  - an UNKNOWN (0) type, with is_pd_hub clear, is read as DCP (2: charge-dcp-current-microamp
    and dcp-jeita-temp-table, 2.0 A at 15-52 C, 0.7 A at 0-15 C and 52-60 C, the bands and
    voltages of the UNKNOWN table) when sprd_tcpm holds an explicit USB-PD contract
    (explicit_contract set, usb_type PD, PD_DRP or PD_PPS) at 5000 mV whose current, and the
    current of the request it last sent, are both 3000 mA or more. The Cube's only sink PDO is
    5 V 3000 mA (live.dts sink-pdos 0x2601912c), so the request is never above 3000: CM2 fires
    for 5 V 3 A contracts only, which leaves about 0.9 A of the contract unused at the ~2.1 A
    the 1984 mA ICHG can draw from VBUS at the top of CC. In every other case the type is what
    PDHUB gives. The contract is read from the tcpm port itself: the cave looks up
    'sprd-tcpm-source-psy-sc27xx-pd' and takes its drvdata (the struct sprd_tcpm_port, as
    cm_update_charger_type_status does at 0x200c-0x2050), then reads usb_type (+1312),
    current_limit (+1148, what the psy reports as current_max), req_current_limit (+1140),
    supply_voltage (+1152) and explicit_contract (+897, a bool). The two calls go through the
    bl words at 0x200c and 0x2050, whose offsets the module loader fills in (CALLW reads the
    word and branches to its target), so no patched word needs a relocation. The offsets are
    those of sprd_tcpm.ko sha b3ec27ea... only (check_tcpm). explicit_contract is cleared by
    SNK_STARTUP and set by PS_RDY, so it closes the first-attach window between the PDO
    selection (usb_type := PD) and PS_RDY, and the hard-reset case, in which
    SNK_HARD_RESET_SINK_ON rewrites current_limit (the Rp current) but leaves usb_type PD and
    the old req_current_limit. The minimum of current_limit and req_current_limit closes the
    window of a renegotiation inside an explicit contract (a new, lower request before its
    PS_RDY).
  - NOT RE-EVALUATED ON A CONTRACT CHANGE. cm_update_charge_info runs only on plug, BC1.2 and
    fchg/cp events; nothing calls it when the source re-sends Source_Capabilities inside a
    contract. A 3 A source that later lowers its 5 V offer (a multi-port charger when a second
    device is plugged in) keeps the DCP table, about 2 A from VBUS, until the next unplug or
    hard reset. cm2/NOTES.md made the Nano MIN vote on constant_charge_current driven by the
    tcpm psy's current_max a precondition for the build, together with owner decision D12. That
    vote does not exist. Owner decision D12 (accepted for test 42, see "Release" below): PD10W
    ships without it, so after such a downgrade the Cube keeps drawing about 2 A until the
    next unplug or hard reset in every context, Nano running or not (off-mode charging, which
    loads this vendor_dlkm copy through modules.load.charger, early boot, a Nano crash).
  - no lock is taken: power_supply_get_by_name walks the class list under its spinlock and
    takes a device reference, power_supply_get_drvdata only loads, the port fields are loaded
    without sprd_tcpm's lock (aligned 32-bit loads; a torn read can only lower the minimum).
    The reference get_by_name takes is not put back, as cm_update_charger_type_status does not
    put the one it takes from the same psy (0x200c).
  - the cave no longer returns with ret: x30 is dead in cm_update_charge_info after the
    mutex_lock call (its prologue saved it), so every exit is b 0x21a8, the word after the hook,
    and the two calls may clobber x30. Callers' registers: only x0, x1, x8-x13, x16, x17 and
    x30 are written; x18-x29 and sp are not touched; nothing is stored.
  - cave: the rest of sprd_fchg_change (dead, see below), in the relocation-free runs between
    its relocated words. The cave runs through four relocated words it does not patch: the
    adrp/add pair at 0x1205c/0x12068 (x1 := the psy name, .rodata+0x382c) and the pair at
    0x12080/0x12084 (x1 := .rodata+0x2a9c, unused); both write x1 only. Every other relocated
    word in the function is jumped over. The function's last two words (0x12150, its epilogue,
    unreached by it) hold the result's csinc and the branch back.

Release (test 42, owner decision): CM12, PDHUB with PD10W on top of it, ships exactly as it
was tested on the RG Cube on 2026-11-26 (vendor_dlkm_a-MOD-Y1-CM12, module TESTED_SHA256;
cm2/NOTES.md "On-device results"). A direct 5 V 3 A PD charger gave about 1.72 A into the
battery (about 6.6 W, about 8.8 W estimated at the port; stock about 4.6 W), the powered dock
about 0.76 A (stock about 0.2 A), and a replug kept the limits. The owner accepted D11 and D12
without the Nano MIN vote, and the module bytes are the tested ones. patch_vendor_dlkm.py
applies patch_tested() to the vendor_dlkm copy (build.sh CHARGE_FIX=1, the default;
CHARGE_FIX=0 keeps the stock module) and check_tcpm() to the same image's sprd_tcpm.ko.
Known limitations of what ships:
  - D11: a PD hub or AMA whose 5 V offer is below 1 A gets the 1.05 A table (about 1 A from
    VBUS), over its offer.
  - D12: a 3 A source that lowers its 5 V offer inside the contract keeps the 2.0 A table
    (about 2 A from VBUS) until the next unplug or hard reset.
  - Neither has a Nano vote to bound it; both hold in off-mode charging too.
  - Not checked on the device: a PC port (SDP, should stay 0.5 A), a sub-1 A hub or AMA, a
    mid-contract downgrade, and the 1.86 A the driver reports against the 1.98 A step
    expected for the 2.0 A target.
  - Recovery keeps the stock module: vendor_boot's copy (modules.load.recovery) is not
    patched.
  - The module is unsigned, like every vendor module (no signature is appended, and
    sprd-drm.ko is patched the same way). The patch changes .text words only, so the length,
    .modinfo (vermagic "5.4.233 SMP preempt mod_unload modversions aarch64", depends
    sprd_tcpm,sprd_battery_info) and the modversions CRCs stay as they are. modules.load and
    modules.load.charger already load sprd_tcpm and sprd_battery_info before it, and
    patch_vendor_dlkm.py's reorder keeps that order.

.text is at file offset 0x6000.
"""
import hashlib
import struct
import sys

MODULE_SHA256 = "ed5b042e799d94a249274af21588399e24e5fe36988ab4a876f005e3234dcf8f"
TEXT_OFFSET = 0x6000             # .text in the module file
TEXT_SIZE = 0x122ac

HOOK = 0x21a4
CAVE = 0x12040                   # sprd_fchg_change
# (address in .text, original words, patched words)
PATCHES = [
    # cm_update_charge_info: ldr w9, [x21, #216] -> bl 0x12040
    (HOOK, [0xb940daa9], [0x94003fa7]),
    # sprd_fchg_change's prologue and first two body words ->
    #   ldr  w9, [x21, #216]      charger type
    #   cmp  w9, #0x1             SDP?
    #   b.ne 0x12058              no: the real type
    #   ldr  w8, [x21, #912]      is_pd_hub
    #   cbz  w8, 0x12058          no hub: the real type
    #   mov  w9, wzr              SDP behind a PD hub: type 0 (UNKNOWN)
    #   ret                       to 0x21a8
    (CAVE, [0xf800865e, 0xa9bd7bfd, 0xa90157f6, 0xa9024ff4, 0x910003fd, 0xf9400048, 0xaa0103f4],
           [0xb940daa9, 0x7100053f, 0x54000081, 0xb94392a8, 0x34000048, 0x2a1f03e9, 0xd65f03c0]),
]

# CM12 (PDHUB + PD10W) of MODULE_SHA256: the module that ran on the RG Cube in test 42
# (out/dp-evidence/test42/charge/cm2/sprd-charger-manager-CM12.ko, in vendor_dlkm_a-MOD-Y1-CM12).
TESTED_SHA256 = "fe3b3de41f95d0698883ee6cc80105e3a8caf6e037332a40ff4f62db1e9c20d9"

# The struct sprd_tcpm_port offsets PD10W reads are those of this sprd_tcpm.ko.
TCPM_SHA256 = "b3ec27ea269b397c0811219f1eee88a2672913457bf4c53560e3dac355a6f1a6"
RETURN = 0x21a8                  # the word after the hook
CALLW = 0x12140                  # x16 = a bl word: branch to its target (x30 is the caller's)
GET_BY_NAME_BL = 0x200c          # cm_update_charger_type_status: bl power_supply_get_by_name
GET_DRVDATA_BL = 0x2050          # cm_update_charger_type_status: bl power_supply_get_drvdata
# Applied after PATCHES (the first entry's old word is PDHUB's ret). In w9 the type PDHUB left;
# out, the type the tables are picked by (2 = DCP); x21 = desc.
PD10W_PATCHES = [
    (0x12058, [0xd65f03c0],
              [0x35f80a89]),    # cbnz w9, 0x21a8            not UNKNOWN: PDHUB's type
    #   0x1205c (relocated, not patched) adrp x1, .rodata+0x382c
    (0x12060, [0xd1004016, 0xf9400115],
              [0xb94392a8,      # ldr  w8, [x21, #912]       is_pd_hub
               0x35f80a28]),    # cbnz w8, 0x21a8            hub or AMA: UNKNOWN (or PDHUB's 0)
    #   0x12068 (relocated, not patched) add x1, x1, :lo12:.rodata+0x382c
    #           x1 = "sprd-tcpm-source-psy-sc27xx-pd"
    (0x1206c, [0xaa0203f3, 0xaa1503e0],
              [0xaa0103e0,      # mov  x0, x1
               0x14000002]),    # b    0x12078               (over bl strcmp)
    (0x12078, [0xb5000054, 0x34000300],
              [0x10f7fcb0,      # adr  x16, 0x200c           its bl power_supply_get_by_name
               0x94000031]),    # bl   0x12140               x0 = the tcpm psy or NULL
    #   0x12080, 0x12084 (relocated, not patched) adrp x1 / add x1, x1: x1 only
    (0x12088, [0xaa1503e0],
              [0x14000002]),    # b    0x12090               (over bl strcmp)
    (0x12090, [0xb5000054, 0x34000360, 0xf940e2c1, 0xb4000521, 0xaa1503e0],
              [0xb4000520,      # cbz  x0, 0x12134           no tcpm psy: UNKNOWN
               0x10f7fdf0,      # adr  x16, 0x2050           its bl power_supply_get_drvdata
               0x9400002a,      # bl   0x12140               x0 = the sprd_tcpm_port
               0xb40004c0,      # cbz  x0, 0x12134
               0x14000002]),    # b    0x120a8               (over bl strcmp)
    (0x120a8, [0xb50004d4, 0x350004a0, 0xf94002c0],
              [0xb9452008,      # ldr  w8, [x0, #1312]       usb_type
               0xb9447c0a,      # ldr  w10, [x0, #1148]      current_limit (mA)
               0x14000006]),    # b    0x120c8               (over the _dev_info call)
    (0x120c8, [0x52800028, 0x910756c9, 0x39075ec8, 0x7900013f, 0x14000014, 0xf94002c0],
              [0xb944740b,      # ldr  w11, [x0, #1140]      req_current_limit (mA)
               0xb944800c,      # ldr  w12, [x0, #1152]      supply_voltage (mV)
               0x51001908,      # sub  w8, w8, #0x6
               0x6b0b015f,      # cmp  w10, w11
               0x1a8b314a,      # csel w10, w10, w11, cc     the smaller current
               0x14000006]),    # b    0x120f4               (over the _dev_info call)
    (0x120f4, [0x910756c8, 0x52800029, 0x14000009, 0xf94002c0],
              [0x5282710b,      # mov  w11, #0x1388          5000 mV
               0x7100091f,      # cmp  w8, #0x2              PD 6, PD_DRP 7, PD_PPS 8: ls
               0x7a4b9180,      # ccmp w12, w11, #0x0, ls    and 5000 mV: eq
               0x14000006]),    # b    0x12118               (over the _dev_info call)
    (0x12118, [0x910756c8, 0x52802009, 0x79000109, 0x39075edf, 0xf90016d3],
              [0x5281770b,      # mov  w11, #0xbb8           3000 mA (the Cube's only sink PDO, 5 V 3 A)
               0x7a4b0140,      # ccmp w10, w11, #0x0, eq    and both currents 3000 mA or more: cs
               0x394e040d,      # ldrb w13, [x0, #897]       explicit_contract
               0x7a4129a0,      # ccmp w13, #0x1, #0x0, cs   and an explicit contract: eq
               0x1400000a]),    # b    0x12150
    (0x12134, [0x9103a2c2, 0x52800400],
              [0x2a1f03e9,      # mov  w9, wzr               UNKNOWN
               0x17ffc01c]),    # b    0x21a8
    (0x12140, [0xa9424ff4, 0xa94157f6, 0x52800020, 0xa8c37bfd],
              [0xb9400211,      # CALLW: ldr w17, [x16]      the bl word, as the loader wrote it
               0x93406631,      # sbfx x17, x17, #0, #26     its offset
               0x8b110a10,      # add  x16, x16, x17, lsl #2 its target (a function or its PLT entry)
               0xd61f0200]),    # br   x16                   returns to the bl to CALLW
    (0x12150, [0xf85f8e5e, 0xd65f03c0],
              [0x1a8d17e9,      # csinc w9, wzr, w13, ne     eq: w13 + 1 = 2 (DCP); ne: 0 (UNKNOWN)
               0x17ffc015]),    # b    0x21a8
]


def _apply(ko, patches):
    for addr, old, new in sorted(patches):
        n = len(old)
        if not (0 <= addr and addr + 4 * n <= TEXT_SIZE) or len(new) != n:
            sys.exit("bad patch at 0x%x" % addr)
        off = TEXT_OFFSET + addr
        if list(struct.unpack_from("<%dI" % n, ko, off)) != old:
            sys.exit("unexpected code at 0x%x" % addr)
        struct.pack_into("<%dI" % n, ko, off, *new)


def patch_module(ko, pd10w=False):
    """PDHUB; with PD10W, PD10W on top of it."""
    if hashlib.sha256(ko).hexdigest() != MODULE_SHA256:
        sys.exit("sprd-charger-manager.ko is not the module this patch is for")
    ko = bytearray(ko)
    _apply(ko, PATCHES)
    if pd10w:
        _apply(ko, PD10W_PATCHES)
    return bytes(ko)


def patch_tested(ko):
    """The release build's patch, CM12 (PDHUB + PD10W): the stock module in (MODULE_SHA256), exactly
    the module tested on the RG Cube out (TESTED_SHA256); else exit."""
    out = patch_module(ko, pd10w=True)
    if hashlib.sha256(out).hexdigest() != TESTED_SHA256:
        sys.exit("sprd-charger-manager.ko: the patch no longer gives the module tested on the RG Cube")
    return out


def check_tcpm(ko):
    """The sprd_tcpm.ko PD10W was written for (its struct offsets), unchanged; else exit."""
    if hashlib.sha256(ko).hexdigest() != TCPM_SHA256:
        sys.exit("sprd_tcpm.ko is not the module PD10W reads the port of")
    return ko


if __name__ == "__main__":
    args = sys.argv[1:]
    pd10w = args[:1] == ["--pd10w"]
    args = args[1:] if pd10w else args
    if len(args) != 2:
        sys.exit("usage: sprd_cm.py [--pd10w] IN OUT")
    with open(args[0], "rb") as f:
        out = patch_module(f.read(), pd10w)
    with open(args[1], "wb") as f:
        f.write(out)
    print("sprd-charger-manager.ko: PDHUB%s, sha256 %s" % (" + PD10W" if pd10w else "",
                                                         hashlib.sha256(out).hexdigest()))
