"""scripts/sprd_cm.py: the charger manager's PDHUB patch (an SDP behind a PD hub charges as UNKNOWN)
and its PD10W switch (an UNKNOWN source with an explicit 5 V USB-PD contract of 3 A charges as DCP)."""
import contextlib
import hashlib
import io
import os
import struct
import sys
from unittest import mock

from tests import a64, util

sys.path.insert(0, os.path.join(util.ROOT, "scripts"))
import sprd_cm  # noqa: E402

dlkm = util.load_script("patch_vendor_dlkm")

EVIDENCE = os.path.join(util.ROOT, "out", "dp-evidence")
MODULE = os.path.join(EVIDENCE, "test42", "charge", "modules", "sprd-charger-manager.ko")
# The module that ran on the RG Cube (charge/cm2/NOTES.md "On-device results").
TESTED = os.path.join(EVIDENCE, "test42", "charge", "cm2", "sprd-charger-manager-CM12.ko")
TEST41_DLKM = os.path.join(EVIDENCE, "vendor_dlkm_a-test41.img")
MODULE_LOAD = os.path.join(util.ROOT, "out", ".dock", "modules.load")
MODULE_LOAD_CHARGER = os.path.join(EVIDENCE, "test42", "charge", "modules", "modules.load.charger")
# A modules.load with what reorder_modules needs (patch_vendor_dlkm.py).
MODULES = ["sprd-drm.ko", "focaltech_ts.ko", "gt9xx_ts.ko"] + dlkm.LAST
PATCHED_SHA256 = "0818274eeb1f1230ff2f7ad0f0262156d0aa45fe182dc58ae0d6437e20b198d2"
PD10W_SHA256 = "fe3b3de41f95d0698883ee6cc80105e3a8caf6e037332a40ff4f62db1e9c20d9"
TCPM = os.path.join(util.ROOT, "out", "dp-evidence", "test42", "charge", "modules", "sprd_tcpm.ko")
TCPM_SHA256 = "b3ec27ea269b397c0811219f1eee88a2672913457bf4c53560e3dac355a6f1a6"
CALL26, JUMP26 = 283, 282       # R_AARCH64_CALL26, R_AARCH64_JUMP26


def word(ko, addr):
    return struct.unpack_from("<I", ko, sprd_cm.TEXT_OFFSET + addr)[0]


def run_cave(words, base, regs, memory):
    """Runs the cave's words (the five kinds it uses) from BASE until ret; gives the registers."""
    regs, flags, pc = dict(regs), None, base
    for _ in range(len(words) + 1):
        op, _, args = a64.disassemble(pc, words[(pc - base) // 4]).partition(" ")
        args = args.split(", ")
        pc += 4
        if op == "ret":
            return regs
        if op == "ldr":
            regs[args[0]] = memory[(args[1].strip("["), int(args[2].strip("#]")))]
        elif op == "cmp":
            flags = regs[args[0]] - int(args[1].strip("#"), 16)
        elif op == "b.ne":
            pc = int(args[0], 16) if flags != 0 else pc
        elif op == "cbz":
            pc = int(args[1], 16) if regs[args[0]] == 0 else pc
        elif op == "mov" and args[1] == "wzr":
            regs[args[0]] = 0
        else:
            raise AssertionError("cave word %s" % op)
    raise AssertionError("no ret")


COND = {"eq": lambda n, z, c, v: z, "ne": lambda n, z, c, v: not z, "cs": lambda n, z, c, v: c,
        "cc": lambda n, z, c, v: not c, "hi": lambda n, z, c, v: c and not z,
        "ls": lambda n, z, c, v: not (c and not z), "ge": lambda n, z, c, v: n == v,
        "lt": lambda n, z, c, v: n != v}
# The relocated words PD10W runs without patching them, as the loader leaves them: x1 only.
RELOCATED_RUN = {0x1205c: ("adrp", "name"), 0x12068: ("add", "name"),
                 0x12080: ("adrp", "other"), 0x12084: ("add", "other")}
# Where the loader's offsets in the two bl words lead (any address outside .text will do).
FUNCTIONS = {0x200c: 0x100000, 0x2050: 0x100100}     # power_supply_get_by_name, _get_drvdata
SAVED = ["x%d" % i for i in range(18, 30)] + ["sp"]


def run_pd10w(ko, kind, hub, psy, port):
    """Runs patched KO from the hook's bl (0x12040, x30 = 0x21a8) until it is back at 0x21a8, on a
    model of the machine: desc's type KIND and is_pd_hub HUB; PSY whether the tcpm psy exists;
    PORT None (drvdata NULL) or {offset: value} of the sprd_tcpm_port. Gives (w9, pcs run).
    Every call clobbers x1-x17 and the flags; nothing may be stored; x18-x29 and sp must survive."""
    regs = {"x%d" % i: 0xbad00000 + i for i in range(31)}
    regs.update({r: r + "-in" for r in SAVED})
    regs.update({"x21": "desc", "x30": sprd_cm.RETURN})
    saved = {r: regs[r] for r in SAVED}
    memory = {("desc", 216): kind, ("desc", 912): hub}
    memory.update({("port", off): value for off, value in (port or {}).items()})
    relocated = {off for _, off in text_relocations(ko)}
    flags, pc, ran = (False, True, False, True), 0x12040, []

    def get(r):
        if r in ("wzr", "xzr"):
            return 0
        value = regs["x" + r[1:] if r[0] == "w" else r]
        if r[0] == "w":
            assert isinstance(value, int), (r, value)
            return value & 0xffffffff
        return value

    def put(r, value):
        regs["x" + r[1:]] = value & 0xffffffff if r[0] == "w" else value

    def compare(a, b, bits):
        mask = (1 << bits) - 1
        res = (a - b) & mask
        sa, sb, sr = a >> (bits - 1) & 1, b >> (bits - 1) & 1, res >> (bits - 1) & 1
        return (bool(sr), res == 0, a >= b, sa != sb and sr != sa)

    def operand(a):
        return int(a.strip("#"), 0) if a.startswith("#") else get(a)

    for _ in range(300):
        if pc == sprd_cm.RETURN:
            for r in SAVED:
                assert regs[r] == saved[r], r
            return get("w9"), ran
        ran.append(pc)
        if pc in FUNCTIONS.values():                # a called function: x0, then back to x30
            if pc == FUNCTIONS[0x200c]:
                assert regs["x0"] == "name", regs["x0"]
                result = "psy" if psy else 0
            else:
                assert regs["x0"] == "psy", regs["x0"]
                result = 0 if port is None else "port"
            regs.update({"x%d" % i: 0xdead0000 + i for i in range(1, 18)})
            regs["x0"], flags, pc = result, (True, False, True, True), regs["x30"]
            continue
        assert 0x12040 <= pc < 0x12158, hex(pc)
        if pc in RELOCATED_RUN:
            op, what = RELOCATED_RUN[pc]
            if op == "add":
                assert regs["x1"] == "page of " + what, regs["x1"]
            regs["x1"], pc = ("page of " if op == "adrp" else "") + what, pc + 4
            continue
        assert pc not in relocated, hex(pc)
        op, _, args = a64.disassemble(pc, word(ko, pc)).partition(" ")
        args, nxt = args.split(", "), pc + 4
        if op in ("ldr", "ldrb"):
            base, _, off = " ".join(args[1:]).strip("[]").partition(" #")
            base, off = get(base.rstrip(",")), int(off or 0)
            if isinstance(base, str):
                value = memory[(base, off)]
                assert op == "ldr" or 0 <= value <= 0xff, (op, off, value)
            elif base + off in FUNCTIONS:           # a bl word, as the loader filled it in
                value = 0x94000000 | ((FUNCTIONS[base + off] - base - off) // 4 & 0x3ffffff)
            else:
                raise AssertionError("load from 0x%x" % (base + off))
            put(args[0], value)
        elif op in ("cmp", "ccmp"):
            bits = 64 if args[0][0] == "x" else 32
            if op == "cmp" or COND[args[3]](*flags):
                flags = compare(get(args[0]), operand(args[1]), bits)
            else:
                nzcv = int(args[2].strip("#"), 0)
                flags = tuple(bool(nzcv >> s & 1) for s in (3, 2, 1, 0))
        elif op == "sub":
            put(args[0], get(args[1]) - operand(args[2]))
        elif op == "csel":
            put(args[0], get(args[1]) if COND[args[3]](*flags) else get(args[2]))
        elif op == "csinc":
            put(args[0], get(args[1]) if COND[args[3]](*flags) else get(args[2]) + 1)
        elif op == "cset":
            put(args[0], int(COND[args[1]](*flags)))
        elif op == "lsl":
            put(args[0], get(args[1]) << int(args[2].strip("#")))
        elif op == "mov":
            put(args[0], operand(args[1]))
        elif op == "adr":
            put(args[0], int(args[1], 16))
        elif op == "sbfx":
            lsb, width = int(args[2].strip("#")), int(args[3].strip("#"))
            field = get(args[1]) >> lsb & ((1 << width) - 1)
            put(args[0], (field - (1 << width) if field >> (width - 1) else field) & ((1 << 64) - 1))
        elif op == "add" and len(args) == 4 and args[3].startswith("lsl #"):
            put(args[0], (get(args[1]) + (get(args[2]) << int(args[3][5:]))) & ((1 << 64) - 1))
        elif op in ("cbz", "cbnz"):
            if (get(args[0]) == 0) == (op == "cbz"):
                nxt = int(args[1], 16)
        elif op.startswith("b."):
            if COND[op[2:]](*flags):
                nxt = int(args[0], 16)
        elif op == "b":
            nxt = int(args[0], 16)
        elif op == "bl":
            regs["x30"], nxt = pc + 4, int(args[0], 16)
        elif op in ("br", "ret"):
            nxt = get(args[0] if op == "br" else "x30")
        else:
            raise AssertionError("cave word %s at 0x%x" % (op, pc))
        pc = nxt
    raise AssertionError("no return")


def text_relocations(ko):
    secs, relocs, _ = elf(ko)
    text = [i for i, s in enumerate(secs) if s[0] == ".text"][0]
    return [(kind, off) for sec, off, kind, _, _, _ in relocs if sec == text]


def elf(ko):
    """(sections, relocations) of KO: (name, type, offset, size, link, info) and
    (section it applies to, offset, type, symbol name, symbol's section, value + addend)."""
    shoff, = struct.unpack_from("<Q", ko, 0x28)
    shnum, shstrndx = struct.unpack_from("<HH", ko, 0x3c)
    raw = [struct.unpack_from("<IIQQQQIIQQ", ko, shoff + 64 * i) for i in range(shnum)]

    def cstr(off):
        return ko[off:ko.index(b"\0", off)].decode()
    secs = [(cstr(raw[shstrndx][4] + s[0]), s[1], s[4], s[5], s[6], s[7]) for s in raw]
    symtab = [s for s in secs if s[1] == 2][0]
    syms = [struct.unpack_from("<IBBHQQ", ko, symtab[2] + 24 * i) for i in range(symtab[3] // 24)]
    names = secs[symtab[4]][2]
    relocs = []
    for s in secs:
        if s[1] == 4:                                       # SHT_RELA
            for j in range(s[3] // 24):
                off, info, addend = struct.unpack_from("<QQq", ko, s[2] + 24 * j)
                sym = syms[info >> 32]
                relocs.append((s[5], off, info & 0xffffffff, cstr(names + sym[0]), sym[3], sym[4] + addend))
    funcs = {cstr(names + s[0]): (s[4], s[4] + s[5]) for s in syms if s[3] and s[5]}
    return secs, relocs, funcs


class SprdCm(util.TestCase):
    def module(self):
        if not os.path.isfile(MODULE):
            self.skipTest("no %s" % MODULE)
        ko = util.slurp(MODULE)
        self.assertEqual(hashlib.sha256(ko).hexdigest(), sprd_cm.MODULE_SHA256)
        return ko

    def test_patched(self):
        ko = self.module()
        out = sprd_cm.patch_module(ko)
        self.assertEqual(hashlib.sha256(out).hexdigest(), PATCHED_SHA256)
        self.assertEqual(len(out), len(ko))
        changed = [a for a in range(0, len(ko), 4) if ko[a:a + 4] != out[a:a + 4]]
        text = sprd_cm.TEXT_OFFSET
        self.assertEqual(changed, [text + 0x21a4] + [text + 0x12040 + 4 * i for i in range(7)])

    def test_words(self):
        """The new words decode as the comments say; the bl goes to the cave and the cave returns
        to the word after the hook."""
        text = {addr + 4 * i: a64.disassemble(addr + 4 * i, w)
                for addr, _, new in sprd_cm.PATCHES for i, w in enumerate(new)}
        self.assertEqual(text, {
            0x21a4: "bl 0x12040",
            0x12040: "ldr w9, [x21, #216]", 0x12044: "cmp w9, #0x1", 0x12048: "b.ne 0x12058",
            0x1204c: "ldr w8, [x21, #912]", 0x12050: "cbz w8, 0x12058", 0x12054: "mov w9, wzr",
            0x12058: "ret"})

    def test_cave_result(self):
        """w9 after the cave: 0 for an SDP (1) with is_pd_hub set, else the stored type."""
        words = dict((a, n) for a, _, n in sprd_cm.PATCHES)[sprd_cm.CAVE]
        for kind in range(9):
            for hub in (0, 1):
                regs = run_cave(words, sprd_cm.CAVE, {"w8": 0x5a5a, "w9": 0x7777},
                                {("x21", 216): kind, ("x21", 912): hub})
                self.assertEqual(regs["w9"], 0 if (kind, hub) == (1, 1) else kind, (kind, hub))

    def test_hook_site(self):
        """cm_update_charge_info around the hook, as the docstring describes it."""
        ko = self.module()
        dis = {a: a64.disassemble(a, word(ko, a)) for a in range(0x2178, 0x26a4, 4)}
        self.assertEqual([dis[a] for a in (0x2178, 0x217c, 0x2180, 0x2190, 0x2194, 0x21a4, 0x21a8, 0x21ac,
                                           0x21b0, 0x21b4, 0x21b8, 0x25e4, 0x25f8)],
                         ["sub sp, sp, #0x80", "str x30, [x18], #8", "stp x29, x30, [sp, #80]",
                          "ldr x21, [x0, #24]", "mov x19, x0", "ldr w9, [x21, #216]", "ldr w8, [x21, #448]",
                          "sub w10, w9, #0x1", "cmp w10, #0x6", "mov w9, w8", "b.hi 0x2200",
                          "ldp x29, x30, [sp, #80]", "ret"])
        self.assertEqual(word(ko, 0x25f0), 0xf85f8e5e)              # ldr x30, [x18, #-8]!
        # x30 is named only by the prologue and the epilogue, and in no word a64.py lacks.
        self.assertEqual(sorted(a for a, t in dis.items() if "x30" in t), [0x217c, 0x2180, 0x25e4])
        for a, t in dis.items():
            w = word(ko, a)
            if t.startswith("?") and a != 0x25f0:
                self.assertNotIn(30, (w & 31, (w >> 5) & 31, (w >> 10) & 31, (w >> 16) & 31), hex(a))
        self.assertEqual(sum(1 for t in dis.values() if t == "ret"), 1)

    def test_relocations(self):
        """No relocation applies to a patched word. The cave function, sprd_fchg_change, is
        referred to only by its CFI jump-table slot 0x122a8; nothing branches into it; the slot
        (0x1229c + 0xc) is formed only in sprd_fchg_extcon_init, after the support-fchg check."""
        ko = self.module()
        secs, relocs, funcs = elf(ko)
        text = [i for i, s in enumerate(secs) if s[0] == ".text"][0]
        self.assertEqual((secs[text][2], secs[text][3]), (sprd_cm.TEXT_OFFSET, sprd_cm.TEXT_SIZE))
        self.assertNotIn(".altinstructions", [s[0] for s in secs])
        self.assertNotIn("__ex_table", [s[0] for s in secs])
        in_text = {off: (kind, name) for sec, off, kind, name, _, _ in relocs if sec == text}
        patched = [addr + 4 * i for addr, old, _ in sprd_cm.PATCHES for i in range(len(old))]
        for a in patched:
            self.assertNotIn(a, in_text, hex(a))
        self.assertEqual(in_text[0x21a0], (CALL26, "mutex_lock"))
        self.assertEqual(funcs["cm_update_charge_info"], (0x2178, 0x26a4))
        cave = funcs["sprd_fchg_change"]
        self.assertEqual(cave, (0x12040, 0x12158))
        # Every relocation that points into the cave function, from any section.
        into = [(secs[sec][0], off, kind) for sec, off, kind, _, symsec, value in relocs
                if symsec == text and cave[0] <= value < cave[1]]
        self.assertEqual(into, [(".text", 0x122a8, JUMP26)])
        self.assertEqual(word(ko, 0x122a8), 0x14000000)
        # No unrelocated b, b.cond, cbz, tbz, bl or adr outside it goes in (adrp always has one).
        for a in range(0, sprd_cm.TEXT_SIZE, 4):
            if a in in_text or cave[0] <= a < cave[1]:
                continue
            t = a64.disassemble(a, word(ko, a)).split(" ")
            if t[0] in ("b", "bl", "cbz", "cbnz", "tbz", "tbnz", "adr") or t[0].startswith("b."):
                target = int(t[-1], 16)
                self.assertFalse(cave[0] <= target < cave[1], hex(a))
        # The slot group of notifier callbacks at 0x1229c: who forms its addresses.
        users = sorted((secs[sec][0], off) for sec, off, _, _, symsec, value in relocs
                       if symsec == text and value in (0x1229c, 0x122a0, 0x122a4, 0x122a8))
        self.assertEqual(users, [(".text", 0x5ec), (".text", 0x5f0), (".text", 0xfcc4), (".text", 0xfcc8),
                                 (".text", 0xfd50), (".text", 0xfd54), (".text", 0x107bc), (".text", 0x107c0)])
        dis = {a: a64.disassemble(a, word(ko, a)) for a in range(0, sprd_cm.TEXT_SIZE, 4)}
        # __cfi_check (0x5ec) only range-checks against it. sprd_vchg_detect_init (x8 at 0xfcc8,
        # x22 at 0xfd54) adds 0, 4 and 8 only: sprd_vchg_change and the two extcon notifiers.
        self.assertEqual(dis[0x5f4], "sub x8, x1, x8")
        start, end = funcs["sprd_vchg_detect_init"]
        x22 = sorted(dis[a] for a in range(start, end, 4)
                     if dis[a].startswith("add ") and dis[a].split(", ")[1] == "x22")
        self.assertEqual(x22, ["add x22, x22, #0x0", "add x8, x22, #0x4", "add x8, x22, #0x8"])
        self.assertEqual((dis[0xfcc8], word(ko, 0xfcd0)), ("add x8, x8, #0x0", 0xf8010c28))  # str x8, [x1, #16]!
        # sprd_fchg_extcon_init, entered only through its slot 0x1222c (fchg ops):
        # device_property_present("sprd,support-fchg") and, when it is absent, tbz to the return.
        # Before that test the code branches only to the two NULL-argument messages and the
        # return, and those only to the return; so the slot of sprd_fchg_change (+0xc, 0x107c8)
        # and power_supply_reg_notifier (0x107f8) come only after the property was found.
        start, end = funcs["sprd_fchg_extcon_init"]
        self.assertEqual((start, end), (0x106b4, 0x108b8))
        into = [(secs[sec][0], off) for sec, off, _, _, symsec, value in relocs
                if symsec == text and start <= value < end]
        self.assertEqual(into, [(".text", 0x1222c)])
        self.assertEqual(in_text[0x106e8], (CALL26, "device_property_present"))
        rodata = [s for s in secs if s[0] == ".rodata"][0]
        name = [value for sec, off, _, _, symsec, value in relocs if sec == text and off == 0x106e0][0]
        self.assertEqual(ko[rodata[2] + name:rodata[2] + name + 18], b"sprd,support-fchg\0")
        self.assertEqual([dis[a] for a in (0x106f8, 0x10858, 0x1086c, 0x107c0, 0x107c8)],
                         ["tbz w0, #0, 0x10858", "mov w0, w20", "ret", "add x8, x8, #0x0", "add x8, x8, #0xc"])
        self.assertEqual(in_text[0x107f8], (CALL26, "power_supply_reg_notifier"))

        def jumps(lo, hi):
            out = {}
            for a in range(lo, hi, 4):
                t = dis[a].split(" ")
                if t[0] in ("b", "cbz", "cbnz", "tbz", "tbnz") or t[0].startswith("b."):
                    out[a] = int(t[-1], 16)
                self.assertNotIn(t[0], ("ret", "br"), hex(a))
                if t[0] == "?":                     # not in the branch/system encoding group
                    self.assertNotEqual(word(ko, a) & 0x1c000000, 0x14000000, hex(a))
            return out
        self.assertEqual(jumps(start, 0x106fc), {0x106c8: 0x10824, 0x106cc: 0x1083c, 0x106f8: 0x10858})
        self.assertEqual(jumps(0x10824, 0x1086c), {0x10838: 0x10850})

    def test_is_pd_hub(self):
        """desc+912 is written only by cm_check_pd_update_ac_usb_online (the tcpm op) and cleared on
        unplug (with the type) and on the AC-online path; read only at 0x2134 (and by the cave)."""
        ko = self.module()
        found = []
        for a in range(0, sprd_cm.TEXT_SIZE, 4):
            t = a64.disassemble(a, word(ko, a))
            if t.endswith(", #912]"):
                found.append((a, t))
        self.assertEqual(found, [(0x148c, "str wzr, [x8, #912]"), (0x2090, "str wzr, [x8, #912]"),
                                 (0x2134, "ldr w8, [x8, #912]"), (0x7844, "str w20, [x8, #912]"),
                                 (0x7eec, "str x9, [x22, #912]"), (0x7f54, "str x0, [x22, #912]"),
                                 (0x90cc, "ldr x2, [x22, #912]")])
        # The last three are cm+912 (a pointer): x22 = the 0x440-byte charger_manager (0x7bbc),
        # whose +24 holds desc (0x7bc0). desc is 0x398 bytes, zeroed (devm_kzalloc, 0x7978).
        self.assertEqual([a64.disassemble(a, word(ko, a)) for a in (0x7978, 0x797c, 0x7994, 0x7ba8, 0x7bbc, 0x7bc0)],
                         ["mov w1, #0x398", "mov w2, #0xdc0", "mov x21, x0", "mov w1, #0x440", "mov x22, x0",
                          "stp x19, x21, [x0, #16]"])
        # On unplug, the type is zeroed with it.
        self.assertEqual(a64.disassemble(0x1420, word(ko, 0x1420)), "str wzr, [x8, #216]")
        self.assertEqual(a64.disassemble(0x77f4, word(ko, 0x77f4)), "and w20, w20, #0x1")

    def test_tcpm_hub_gate(self):
        """What sets is_pd_hub: sprd_tcpm_pd_rx_handler calls the charger manager's op with w0 = 1
        (0xc52c-0xc530) after a Discover Identity whose UFP product type has bits 28:27 = 01, which is
        HUB (001) and AMA (101). Nothing on that path reads the PD contract, so the patch alone is not
        contract-safe: a hub or AMA with a 5 V PDO below 1 A gets the 1.05 A table too (sprd_cm's
        docstring; its build wiring needs the Nano down-vote first)."""
        if not os.path.isfile(TCPM):
            self.skipTest("no %s" % TCPM)
        ko = util.slurp(TCPM)
        self.assertEqual(hashlib.sha256(ko).hexdigest(), TCPM_SHA256)
        secs, relocs, funcs = elf(ko)
        text = [i for i, s in enumerate(secs) if s[0] == ".text"][0]
        base = secs[text][2]
        start, end = funcs["sprd_tcpm_pd_rx_handler"]
        self.assertTrue(start <= 0xc4d0 and 0xc548 <= end)
        dis = [a64.disassemble(a, struct.unpack_from("<I", ko, base + a)[0]) for a in range(0xc4d0, 0xc548, 4)]
        self.assertEqual(dict((a, dis[(a - 0xc4d0) // 4]) for a in (0xc4d0, 0xc4e0, 0xc4e4, 0xc4e8, 0xc4ec,
                                                                    0xc504, 0xc50c, 0xc52c, 0xc530, 0xc544)),
                         {0xc4d0: "ubfx w2, w25, #27, #3", 0xc4e0: "and w8, w25, #0x18000000",
                          0xc4e4: "mov w9, #0x8000000", 0xc4e8: "cmp w8, w9", 0xc4ec: "b.ne 0xc544",
                          0xc504: "ldr x8, [x8]", 0xc50c: "ldr x24, [x8, #8]", 0xc52c: "mov w0, #0x1",
                          0xc530: "blr x24", 0xc544: "str w25, [x21, #168]"})
        self.assertEqual([p for p in range(8) if (p << 27) & 0x18000000 == 0x08000000], [1, 5])
        # Between the test and the op call: the two log lines, __cfi_slowpath and the op; the only
        # loads are the ops pointer and its op (no PDO or contract field).
        calls = sorted(off for sec, off, kind, _, _, _ in relocs if sec == text and kind == CALL26
                       and 0xc4f0 <= off < 0xc544)
        self.assertEqual(calls, [0xc4fc, 0xc528, 0xc540])
        self.assertEqual([t for t in dis[(0xc4f0 - 0xc4d0) // 4:(0xc544 - 0xc4d0) // 4] if t.startswith("ldr")],
                         ["ldr x8, [x8]", "ldr x24, [x8, #8]"])

    def test_pd10w_patched(self):
        """PD10W is applied on top of PDHUB only, and changes PDHUB's ret and the words of its runs."""
        ko = self.module()
        out = sprd_cm.patch_module(ko, pd10w=True)
        self.assertEqual(hashlib.sha256(out).hexdigest(), PD10W_SHA256)
        self.assertEqual(hashlib.sha256(sprd_cm.patch_module(ko, pd10w=False)).hexdigest(), PATCHED_SHA256)
        changed = [a - sprd_cm.TEXT_OFFSET for a in range(0, len(ko), 4) if ko[a:a + 4] != out[a:a + 4]]
        want = sorted({addr + 4 * i for addr, old, _ in sprd_cm.PATCHES + sprd_cm.PD10W_PATCHES
                       for i in range(len(old))})
        self.assertEqual(changed, want)
        self.assertEqual(len(out), len(ko))
        # Its first word replaces PDHUB's ret, so PD10W cannot be applied to the stock module.
        self.assertEqual(sprd_cm.PD10W_PATCHES[0][:2], (0x12058, [sprd_cm.PATCHES[1][2][6]]))
        stock = bytearray(ko)
        with self.assertRaises(SystemExit):
            sprd_cm._apply(stock, sprd_cm.PD10W_PATCHES)

    def test_pd10w_words(self):
        """The new words decode as the comments say (tests/a64.py), and every one of them, and every
        word that replaced, lies in sprd_fchg_change after PDHUB's 7 words and carries no relocation."""
        text = {addr + 4 * i: a64.disassemble(addr + 4 * i, w)
                for addr, _, new in sprd_cm.PD10W_PATCHES for i, w in enumerate(new)}
        self.assertEqual(text, {
            0x12058: "cbnz w9, 0x21a8",
            0x12060: "ldr w8, [x21, #912]", 0x12064: "cbnz w8, 0x21a8",
            0x1206c: "mov x0, x1", 0x12070: "b 0x12078",
            0x12078: "adr x16, 0x200c", 0x1207c: "bl 0x12140",
            0x12088: "b 0x12090",
            0x12090: "cbz x0, 0x12134", 0x12094: "adr x16, 0x2050", 0x12098: "bl 0x12140",
            0x1209c: "cbz x0, 0x12134", 0x120a0: "b 0x120a8",
            0x120a8: "ldr w8, [x0, #1312]", 0x120ac: "ldr w10, [x0, #1148]", 0x120b0: "b 0x120c8",
            0x120c8: "ldr w11, [x0, #1140]", 0x120cc: "ldr w12, [x0, #1152]", 0x120d0: "sub w8, w8, #0x6",
            0x120d4: "cmp w10, w11", 0x120d8: "csel w10, w10, w11, cc", 0x120dc: "b 0x120f4",
            0x120f4: "mov w11, #0x1388", 0x120f8: "cmp w8, #0x2", 0x120fc: "ccmp w12, w11, #0x0, ls",
            0x12100: "b 0x12118",
            0x12118: "mov w11, #0xbb8", 0x1211c: "ccmp w10, w11, #0x0, eq", 0x12120: "ldrb w13, [x0, #897]",
            0x12124: "ccmp w13, #0x1, #0x0, cs", 0x12128: "b 0x12150",
            0x12134: "mov w9, wzr", 0x12138: "b 0x21a8",
            0x12140: "ldr w17, [x16]", 0x12144: "sbfx x17, x17, #0, #26",
            0x12148: "add x16, x16, x17, lsl #2", 0x1214c: "br x16",
            0x12150: "csinc w9, wzr, w13, ne", 0x12154: "b 0x21a8"})
        ko = self.module()
        relocated = {off for _, off in text_relocations(ko)}
        for addr, old, _ in sprd_cm.PD10W_PATCHES:
            for i in range(len(old)):
                a = addr + 4 * i
                self.assertTrue(0x1205c <= a < 0x12158 or a == 0x12058, hex(a))
                self.assertNotIn(a, relocated, hex(a))
                if a != 0x12058:                    # (PDHUB's ret)
                    self.assertEqual(word(ko, a), old[i], hex(a))

    def test_pd10w_flow(self):
        """Every way through the cave: each branch goes to a patched word, a relocated word the cave
        runs (the two adrp/add pairs, x1 only), CALLW or 0x21a8; each word that falls through falls on
        one of those; bl goes only to CALLW, adr names only the two live bl words, br is CALLW's;
        sprd_fchg_change's last two words are not reached."""
        ko = self.module()
        out = sprd_cm.patch_module(ko, pd10w=True)
        patched = {addr + 4 * i for addr, old, _ in sprd_cm.PATCHES + sprd_cm.PD10W_PATCHES
                   for i in range(len(old)) if addr != sprd_cm.HOOK}
        ok = patched | set(RELOCATED_RUN)
        targets = set()
        for a in sorted(ok):
            if a in RELOCATED_RUN:
                self.assertIn(a + 4, ok, hex(a))
                continue
            t = a64.disassemble(a, word(out, a)).split(" ")
            op = t[0]
            if op in ("b", "cbz", "cbnz", "bl") or op.startswith("b."):
                target = int(t[-1], 16)
                targets.add(target)
                self.assertTrue(target in ok or target == sprd_cm.RETURN, (hex(a), hex(target)))
                if op == "bl":
                    self.assertEqual(target, sprd_cm.CALLW, hex(a))
            if op == "adr":
                self.assertIn(int(t[-1], 16), (sprd_cm.GET_BY_NAME_BL, sprd_cm.GET_DRVDATA_BL), hex(a))
            if op == "br":
                self.assertEqual(a, sprd_cm.CALLW + 12)
            self.assertNotEqual(op, "ret", hex(a))   # PD10W exits by b 0x21a8 (x30 is not kept)
            if op not in ("b", "br", "ret"):
                self.assertIn(a + 4, ok, hex(a))   # falls through (or a bl returns) onto ok words
            self.assertFalse(t[0].startswith("?"), hex(a))
        # sprd_fchg_change's epilogue (0x12150, 0x12154) is now the result's tail, entered only by
        # the b at 0x12128; every word of the function is a patched word or a relocated word.
        self.assertEqual((word(ko, 0x12150), word(ko, 0x12154)), (0xf85f8e5e, 0xd65f03c0))
        entries = [a for a in ok if a not in RELOCATED_RUN and a != 0x12150 - 4
                   and a64.disassemble(a, word(out, a)).split(" ")[-1] == "0x12150"]
        self.assertEqual(entries, [0x12128])
        relocated = {off for _, off in text_relocations(ko)}
        self.assertEqual([a for a in range(0x12040, 0x12158, 4) if a not in patched | relocated], [])
        # The relocated words it runs: adrp x1 and add x1, x1 into .rodata, as the loader fills them.
        secs, relocs, _ = elf(ko)
        text = [i for i, s in enumerate(secs) if s[0] == ".text"][0]
        rodata = [i for i, s in enumerate(secs) if s[0] == ".rodata"][0]
        info = {off: (kind, symsec, value) for sec, off, kind, _, symsec, value in relocs if sec == text}
        self.assertEqual([(info[a], word(ko, a)) for a in sorted(RELOCATED_RUN)],
                         [((275, rodata, 0x382c), 0x90000001), ((277, rodata, 0x382c), 0x91000021),
                          ((275, rodata, 0x2a9c), 0x90000001), ((277, rodata, 0x2a9c), 0x91000021)])
        ro = secs[rodata][2]
        self.assertEqual(ko[ro + 0x382c:ro + 0x384b], b"sprd-tcpm-source-psy-sc27xx-pd\0")
        # (adrp x1 = 0x90000001: rd 1, the page offset the loader's; add x1, x1 = 0x91000021.)
        self.assertEqual([0x90000001 & 31, 0x91000021 & 31, (0x91000021 >> 5) & 31], [1, 1, 1])

    def test_pd10w_bl_words(self):
        """The two bl words CALLW reads are live: cm_update_charger_type_status's calls of
        power_supply_get_by_name (0x200c, on the tcpm psy's name, .rodata+0x382c) and
        power_supply_get_drvdata (0x2050, on its result); misc_event_handler calls that function."""
        ko = self.module()
        secs, relocs, funcs = elf(ko)
        text = [i for i, s in enumerate(secs) if s[0] == ".text"][0]
        info = {off: (kind, name, value) for sec, off, kind, name, _, value in relocs if sec == text}
        self.assertEqual((info[0x200c], info[0x2050]), ((CALL26, "power_supply_get_by_name", 0),
                                                        (CALL26, "power_supply_get_drvdata", 0)))
        self.assertEqual((word(ko, 0x200c), word(ko, 0x2050)), (0x94000000, 0x94000000))
        start, end = funcs["cm_update_charger_type_status"]
        self.assertTrue(start <= 0x200c < 0x2050 < end)
        self.assertEqual(info[0x1fe0][2], 0x382c)
        self.assertEqual(info[0x16c4][:2], (CALL26, ""))
        self.assertEqual([v for sec, off, k, _, ss, v in relocs if sec == text and off == 0x16c4], [0x1ec0])
        # Both within adr's reach of the cave.
        self.assertLess(0x12094 - 0x2050, 1 << 20)

    def test_pd10w_cave(self):
        """w9 after the cave, for every type 0-8, is_pd_hub 0/1 and contract: none (no tcpm psy, a
        Type-C-only source: usb_type C with its Rp 3000 mA, the same with the stale explicit_contract
        and request a PD source's detach leaves), or an explicit 5 V PD (or PD_PPS) contract of
        1000/1500/2000/2500/3000 mA. DCP (2) only for UNKNOWN, no hub and an explicit 3000 mA
        contract; else PDHUB's."""
        ko = self.module()
        out = sprd_cm.patch_module(ko, pd10w=True)
        cm1 = sprd_cm.patch_module(ko)
        contracts = [("no psy", False, None),
                     ("type-c 3 A", True, {1312: 5, 1148: 3000, 1140: 0, 1152: 5000, 897: 0}),
                     ("type-c 3 A, stale", True, {1312: 5, 1148: 3000, 1140: 3000, 1152: 5000, 897: 1})]
        for ma in (1000, 1500, 2000, 2500, 3000):
            for usb_type in (6, 8):
                contracts.append(("PD %d" % ma, True,
                                  {1312: usb_type, 1148: ma, 1140: ma, 1152: 5000, 897: 1}))
        for kind in range(9):
            for hub in (0, 1):
                pdhub = 0 if (kind, hub) == (1, 1) else kind
                for name, psy, port in contracts:
                    w9, _ = run_pd10w(out, kind, hub, psy, port)
                    dcp = kind == 0 and not hub and port is not None and port[1312] in (6, 8) \
                        and port[1148] >= 3000 and port[897] == 1
                    self.assertEqual(w9, 2 if dcp else pdhub, (kind, hub, name, port))
                    # PDHUB alone, on the same machine: its own result.
                    self.assertEqual(run_pd10w(cm1, kind, hub, psy, port)[0], pdhub, (kind, hub, name))

    def test_pd10w_cave_edges(self):
        """The contract gates one at a time, for an UNKNOWN source with no hub."""
        out = sprd_cm.patch_module(self.module(), pd10w=True)
        good = {1312: 6, 1148: 3000, 1140: 3000, 1152: 5000, 897: 1}
        cases = [
            ({}, 2),
            ({1312: 7}, 2),                          # PD_DRP
            ({1312: 5}, 0), ({1312: 9}, 0), ({1312: 4}, 0), ({1312: 0}, 0),
            ({1148: 2999, 1140: 2999}, 0),
            ({1148: 2999, 1140: 3000}, 0), ({1148: 3000, 1140: 2999}, 0),
            ({1148: 2000, 1140: 2000}, 0), ({1148: 2500, 1140: 2500}, 0),
            ({1148: 3000, 1140: 1500}, 0),           # a 1.5 A request before its PS_RDY
            ({1148: 1500, 1140: 3000}, 0),           # a 3 A request not (yet) accepted
            ({1152: 9000}, 0), ({1152: 4999}, 0), ({1152: 0}, 0),
            ({1148: 0xffffffff, 1140: 0xffffffff}, 2),
            # explicit_contract (+897): cleared by SNK_STARTUP, set by PS_RDY (test_pd10w_tcpm_port).
            ({897: 0}, 0),                           # first attach: PDO selected, before PS_RDY
            ({897: 0, 1148: 3000, 1140: 3000}, 0),   # after a hard reset: SINK_ON's Rp 3 A, the old request
            ({897: 2}, 0), ({897: 0xff}, 0),         # a bool: only 1 counts
        ]
        for change, want in cases:
            port = dict(good)
            port.update(change)
            self.assertEqual(run_pd10w(out, 0, 0, True, port)[0], want, change)
        self.assertEqual(run_pd10w(out, 0, 0, True, None)[0], 0)       # drvdata NULL
        self.assertEqual(run_pd10w(out, 0, 0, False, good)[0], 0)      # no tcpm psy
        # The path that decides (DCP) runs both calls and every word of the gate.
        _, ran = run_pd10w(out, 0, 0, True, good)
        self.assertEqual(ran.count(sprd_cm.CALLW), 2)
        self.assertEqual(ran.count(0x100000) + ran.count(0x100100), 2)

    def test_pd10w_tcpm_port(self):
        """The five fields, in sprd_tcpm.ko (sha pinned as sprd_cm.TCPM_SHA256): its psy's get_property
        is upstream tcpm_psy_get_prop on power_supply_get_drvdata(psy), with ONLINE, VOLTAGE_MAX/MIN/NOW,
        CURRENT_MAX/NOW and USB_TYPE (4, 8, 9, 12, 16, 17, 64), so +1148 is current_limit (current_max
        unless PPS is active, +1401), +1152 supply_voltage, +1312 usb_type. The PDO selection stores
        usb_type PD (6) or PD_PPS (8); the request builder stores req_current_limit (+1140) and
        req_supply_voltage (+1144); PS_RDY copies them to current_limit and supply_voltage; the reset
        stores usb_type C (5); the Type-C attach paths store 5000 mV and the Rp current; and the
        hard-reset SINK_ON (0x99a8/0x99ac) rewrites only supply_voltage and current_limit (the Rp
        current), leaving usb_type and req_current_limit as the old contract left them, which is why
        PD10W also needs explicit_contract (+897, check_explicit_contract)."""
        if not os.path.isfile(TCPM):
            self.skipTest("no %s" % TCPM)
        ko = util.slurp(TCPM)
        sprd_cm.check_tcpm(ko)
        self.assertEqual(sprd_cm.TCPM_SHA256, TCPM_SHA256)
        with self.assertRaises(SystemExit):
            sprd_cm.check_tcpm(ko + b"\0")
        secs, relocs, funcs = elf(ko)
        text = [i for i, s in enumerate(secs) if s[0] == ".text"][0]
        base = secs[text][2]
        ro = [s for s in secs if s[0] == ".rodata"][0][2]
        info = {off: (kind, name, value) for sec, off, kind, name, _, value in relocs if sec == text}

        def dis(a):
            return a64.disassemble(a, struct.unpack_from("<I", ko, base + a)[0])
        self.assertEqual(funcs["sprd_tcpm_psy_get_prop"], (0x2e04, 0x2ee8))
        self.assertEqual(info[0x2e1c][:2], (CALL26, "power_supply_get_drvdata"))
        self.assertEqual([dis(a) for a in (0x2e18, 0x2e20, 0x2e24, 0x2e28, 0x2e3c, 0x2e44, 0x2e48)],
                         ["mov w20, w1", "sub w9, w20, #0x4", "mov x8, x0", "cmp w9, #0x3c", "adr x11, 0x2e4c",
                          "add x11, x11, x12, lsl #2", "br x11"])
        self.assertEqual(struct.unpack_from("<I", ko, base + 0x2e40)[0], 0x3869694c)   # ldrb w12, [x10, x9]
        self.assertEqual(info[0x2e38][2], 0xe0)
        table = {}
        for i, b in enumerate(ko[ro + 0xe0:ro + 0xe0 + 61]):
            if 0x2e4c + 4 * b != 0x2eb4:            # 0x2eb4: -EINVAL
                table[i + 4] = 0x2e4c + 4 * b
        self.assertEqual(table, {4: 0x2e4c, 8: 0x2e68, 9: 0x2e78, 12: 0x2e88, 16: 0x2e90, 17: 0x2ea0, 64: 0x2ec4})
        self.assertEqual([dis(a) for a in (0x2e88, 0x2e90, 0x2e94, 0x2e98, 0x2ea0, 0x2ec4, 0x2ea8, 0x2eac)],
                         ["ldr w8, [x8, #1152]", "ldrb w9, [x8, #1401]", "cbz w9, 0x2ea0", "ldr w8, [x8, #1384]",
                          "ldr w8, [x8, #1148]", "ldr w8, [x8, #1312]", "mov w9, #0x3e8", "mul w8, w8, w9"])
        stores = {}
        for a in range(0, secs[text][3], 4):
            t = dis(a)
            for off in ("#1140]", "#1148]", "#1152]", "#1312]"):
                if t.startswith("str") and t.endswith(off):
                    stores.setdefault(int(off[1:-1]), []).append(a)
        self.assertEqual(stores, {1140: [0xa8e8, 0xc2dc], 1148: [0x40f0, 0x98b0, 0x99ac, 0xbca8],
                                  1152: [0x40ec, 0x98ac, 0x99a8, 0xbca4], 1312: [0x1140, 0x1d70, 0x6174, 0x6234]})
        self.assertEqual([dis(a) for a in (0x616c, 0x6174, 0x6230, 0x6234, 0x1134, 0x1138, 0x1140,
                                           0xa8e8, 0xa8ec, 0xbc80, 0xbc84, 0xbca4, 0xbca8,
                                           0x40e0, 0x40ec, 0x40f0, 0x98a0, 0x999c)],
                         ["mov w9, #0x6", "str w9, [x19, #1312]", "mov w9, #0x8", "str w9, [x19, #1312]",
                          "mov x8, #0x5", "movk x8, #0x5, lsl #32", "str x8, [x19, #1312]",
                          "str w22, [x19, #1140]", "str w21, [x19, #1144]",
                          "ldr w22, [x21, #1140]", "ldr w23, [x21, #1144]",
                          "str w23, [x21, #1152]", "str w22, [x21, #1148]",
                          "mov w21, #0x1388", "str w21, [x19, #1152]", "str w20, [x19, #1148]",
                          "mov w21, #0x1388", "mov w21, #0x1388"])
        for a, fn in ((0x1140, "sprd_tcpm_reset_port"), (0x6174, "sprd_run_state_machine"),
                      (0xa8e8, "sprd_run_state_machine"), (0xbca8, "sprd_tcpm_pd_rx_handler"),
                      (0x40f0, "_sprd_tcpm_cc_change"), (0xc2dc, "sprd_tcpm_pd_rx_handler")):
            start, end = funcs[fn]
            self.assertTrue(start <= a < end, (hex(a), fn))
        self.check_explicit_contract(ko, secs, relocs, funcs, info, dis)

    def check_explicit_contract(self, ko, secs, relocs, funcs, info, dis):
        """+897 is explicit_contract (a bool; +896 pd_capable, +900 rx_msgid, upstream's order):
        every access in .text, and the state each store is in, from sprd_run_state_machine's jump
        table. Cleared (0) by SNK_STARTUP (before SNK_DISCOVERY and so before any PDO selection),
        SRC_STARTUP and PR_SWAP_SRC_SNK_TRANSITION_OFF; set (1) by PS_RDY in SNK_TRANSITION_SINK
        (right after current_limit := req_current_limit), by the VBUS-on event in
        SNK_TRANSITION_SINK_VBUS, by the SNK_TRANSITION_SINK(_VBUS) state when port+0x1064 is set
        (the power-role-swap path), and by SRC_TRANSITION_SUPPLY (source role). The hard-reset
        states (SNK_HARD_RESET_SINK_OFF, _WAIT_VBUS, _SINK_ON) leave it alone: SINK_ON goes on to
        SNK_STARTUP, which clears it."""
        text = [i for i, s in enumerate(secs) if s[0] == ".text"][0]
        rodata = [i for i, s in enumerate(secs) if s[0] == ".rodata"][0]
        ro = secs[rodata][2]
        acc = [(a, dis(a)) for a in range(0, secs[text][3], 4) if dis(a).endswith(", #897]")]
        self.assertEqual(acc, [(0x2a90, "strb w8, [x22, #897]"), (0x54f0, "strb w8, [x19, #897]"),
                               (0x5990, "ldrb w9, [x19, #897]"), (0x5a04, "strb w8, [x19, #897]"),
                               (0x5fbc, "strb wzr, [x19, #897]"), (0x7360, "strb wzr, [x19, #897]"),
                               (0x9184, "ldrb w8, [x19, #897]"), (0x9d14, "strb wzr, [x19, #897]"),
                               (0xb7f4, "ldrb w8, [x21, #897]"), (0xbce4, "strb w9, [x21, #897]")])
        # The four stores of w8/w9: 1, with nothing writing w8/w9 in between.
        self.assertEqual([dis(a) for a in range(0x2a7c, 0x2a94, 4)],
                         ["mov w8, #0x1", "mov x0, x22", "mov x1, x24", "add x2, x2, #0x0", "add x3, x3, #0x0",
                          "strb w8, [x22, #897]"])
        self.assertEqual([dis(a) for a in range(0x54dc, 0x54f4, 4)],
                         ["mov w8, #0x1", "add x1, x1, #0x0", "add x3, x3, #0x0", "mov x0, x19",
                          "strb w8, [x19, #232]", "strb w8, [x19, #897]"])
        self.assertEqual([dis(a) for a in range(0x59fc, 0x5a08, 4)],
                         ["mov w8, #0x1", "mov w1, #0x3", "strb w8, [x19, #897]"])
        self.assertEqual([dis(a) for a in (0xbca8, 0xbcdc, 0xbce0, 0xbce4)],
                         ["str w22, [x21, #1148]", "ldr w8, [x21, #264]", "mov w9, #0x1", "strb w9, [x21, #897]"])
        # The state machine: w8 = state - 1 (0x53d0), checked against 0x53 (0x53d4, b.hi), words of
        # .rodata+0x22dc relative to 0x53f0; the state names: the pointers at .rodata+0x26a8.
        self.assertEqual([dis(a) for a in (0x53cc, 0x53d0, 0x53d4, 0x53f0, 0x53f8, 0x5404)],
                         ["ldr w1, [x0, #264]", "sub w8, w1, #0x1", "cmp w8, #0x53", "adr x9, 0x53f0",
                          "add x9, x9, x11", "br x9"])
        self.assertEqual(struct.unpack_from("<I", ko, secs[text][2] + 0x53f4)[0], 0xb8a8794b)  # ldrsw x11, [x10, x8, lsl #2]
        self.assertEqual((info[0x53e0][0], info[0x53e0][2], info[0x53e8][0], info[0x53e8][2]),
                         (275, 0x22dc, 277, 0x22dc))
        handler = {s + 1: 0x53f0 + struct.unpack_from("<i", ko, ro + 0x22dc + 4 * s)[0] for s in range(0x54)}
        names = {}
        for sec, off, _, _, symsec, value in relocs:
            if sec == rodata and symsec == rodata and 0x26a8 <= off < 0x26a8 + 8 * 0x55:
                names[(off - 0x26a8) // 8] = ko[ro + value:ko.index(b"\0", ro + value)].decode()
        state = {names[s]: handler[s] for s in handler}
        self.assertEqual({n: state[n] for n in ("SRC_STARTUP", "SRC_TRANSITION_SUPPLY", "SRC_READY", "SNK_STARTUP",
                                                "SNK_DISCOVERY", "SNK_TRANSITION_SINK", "SNK_TRANSITION_SINK_VBUS",
                                                "SNK_READY", "SNK_HARD_RESET_SINK_OFF", "SNK_HARD_RESET_WAIT_VBUS",
                                                "SNK_HARD_RESET_SINK_ON", "PR_SWAP_SRC_SNK_TRANSITION_OFF",
                                                "PR_SWAP_SRC_SNK_SOURCE_OFF")},
                         {"SRC_STARTUP": 0x5878, "SRC_TRANSITION_SUPPLY": 0x59a4, "SRC_READY": 0x5a64,
                          "SNK_STARTUP": 0x5f6c, "SNK_DISCOVERY": 0x5ffc, "SNK_TRANSITION_SINK": 0x54c0,
                          "SNK_TRANSITION_SINK_VBUS": 0x54c0, "SNK_READY": 0x64b0,
                          "SNK_HARD_RESET_SINK_OFF": 0x6a20, "SNK_HARD_RESET_WAIT_VBUS": 0x6b2c,
                          "SNK_HARD_RESET_SINK_ON": 0x6b84, "PR_SWAP_SRC_SNK_TRANSITION_OFF": 0x72f0,
                          "PR_SWAP_SRC_SNK_SOURCE_OFF": 0x73a8})
        # SNK_STARTUP runs straight to its clear (only the typec_set_pwr_opmode call on the way).
        for a in range(0x5f6c, 0x5fbc, 4):
            t = dis(a).split(" ")
            self.assertFalse(t[0] in ("b", "cbz", "cbnz", "tbz", "tbnz", "ret", "br") or t[0].startswith("b."), hex(a))
            if t[0] == "?":
                self.assertNotEqual(struct.unpack_from("<I", ko, secs[text][2] + a)[0] & 0x1c000000, 0x14000000, hex(a))
        self.assertEqual(info[0x5f9c][:2], (CALL26, "typec_set_pwr_opmode"))
        self.assertEqual(dis(0x5fb8), "str w9, [x19, #900]")       # rx_msgid := -1 (0x5fb4 movn)
        self.assertEqual(struct.unpack_from("<I", ko, secs[text][2] + 0x5fb4)[0], 0x12800009)
        # 0x54c0, both SNK_TRANSITION_SINK states: when port+0x1064 is set, SNK_READY with the contract
        # flags set; else (0x87d4) the hard-reset path.
        self.assertEqual([dis(a) for a in (0x54c0, 0x54c4, 0x54f4)],
                         ["ldrb w8, [x24, #4]", "cbz w8, 0x87d4", "b 0x9e0c"])
        self.assertEqual(dis(0x53ec), "add x24, x0, x9")            # x24 = port + 0x1060 (0x53e4)
        self.assertEqual((info[0x54d4][2], info[0x54d8][2]), (0xd54, 0x17b6))   # "state change %s -> %s", SNK_READY
        self.assertEqual(ko[ro + 0x17b6:ro + 0x17c0], b"SNK_READY\0")
        # The VBUS-on store: pd_event_handler's SNK_TRANSITION_SINK_VBUS -> SNK_READY.
        start, end = funcs["sprd_tcpm_pd_event_handler"]
        self.assertTrue(start <= 0x2a90 < end)
        self.assertEqual((info[0x2a74][2], info[0x2a78][2]), (0x182e, 0x17b6))
        self.assertEqual(ko[ro + 0x182e:ro + 0x1847], b"SNK_TRANSITION_SINK_VBUS\0")
        # The other stores, by state: SRC_TRANSITION_SUPPLY (then pwr_opmode PD, 3), the source
        # STARTUP path (SRC_STARTUP's branches to 0x9ce8), PR_SWAP_SRC_SNK_TRANSITION_OFF; PS_RDY in
        # the rx handler.
        self.assertTrue(state["SRC_TRANSITION_SUPPLY"] <= 0x5a04 < state["SRC_READY"])
        self.assertEqual(info[0x5a0c][:2], (CALL26, "typec_set_pwr_opmode"))
        self.assertEqual([dis(a) for a in (0x5880, 0x58b4)], ["b.lt 0x9ce8", "b 0x9ce8"])
        for a in range(0x9ce8, 0x9d14, 4):
            t = dis(a).split(" ")
            self.assertFalse(t[0] in ("b", "cbz", "cbnz", "tbz", "tbnz", "ret", "br") or t[0].startswith("b."), hex(a))
        self.assertEqual(dis(0x9d10), "str w9, [x19, #900]")
        self.assertTrue(state["PR_SWAP_SRC_SNK_TRANSITION_OFF"] <= 0x7360 < state["PR_SWAP_SRC_SNK_SOURCE_OFF"])
        start, end = funcs["sprd_tcpm_pd_rx_handler"]
        self.assertTrue(start <= 0xbca8 < 0xbce4 < end)

    def test_wrong_module(self):
        with self.assertRaises(SystemExit):
            sprd_cm.patch_module(b"\0" * 4096)
        with self.assertRaises(SystemExit):
            sprd_cm.patch_module(b"\0" * 4096, pd10w=True)

    def test_refuses_its_own_output(self):
        out = sprd_cm.patch_module(self.module())
        with self.assertRaises(SystemExit):
            sprd_cm.patch_module(out)

    def test_tested(self):
        """The release build's patch: CM12 of the stock module is, byte for byte, the module that ran on
        the RG Cube in test 42; any other input, or a patch that no longer gives that module, stops it."""
        ko = self.module()
        self.assertEqual(sprd_cm.TESTED_SHA256, PD10W_SHA256)
        out = sprd_cm.patch_tested(ko)
        self.assertEqual(out, sprd_cm.patch_module(ko, pd10w=True))
        self.assertEqual(hashlib.sha256(out).hexdigest(), sprd_cm.TESTED_SHA256)
        if os.path.isfile(TESTED):
            self.assertEqual(out, util.slurp(TESTED))
        for bad in (out, sprd_cm.patch_module(ko), b"\0" * 4096):
            with self.assertRaises(SystemExit):
                sprd_cm.patch_tested(bad)
        with mock.patch.object(sprd_cm, "TESTED_SHA256", "0" * 64), self.assertRaises(SystemExit) as e:
            sprd_cm.patch_tested(ko)
        self.assertIn("tested on the RG Cube", str(e.exception))

    def test_loads_like_the_stock_module(self):
        """Nothing the module loader checks changes: no signature (the vendor modules carry none), the
        same size and section headers, and only .text bytes differ, so .modinfo (vermagic, depends),
        __versions (the modversions CRCs) and .gnu.linkonce.this_module are the stock ones."""
        ko = self.module()
        out = sprd_cm.patch_tested(ko)
        self.assertEqual(len(out), len(ko))
        for k in (ko, out):
            self.assertNotIn(b"~Module signature appended~", k)
        secs = elf(ko)[0]
        self.assertEqual(elf(out)[0], secs)
        sec = {s[0]: (s[2], s[3]) for s in secs}
        changed = [i for i in range(len(ko)) if ko[i] != out[i]]
        start, size = sec[".text"]
        self.assertTrue(start <= changed[0] and changed[-1] < start + size)
        for name in (".modinfo", "__versions", ".gnu.linkonce.this_module"):
            a, n = sec[name]
            self.assertEqual(out[a:a + n], ko[a:a + n], name)
        a, n = sec[".modinfo"]
        info = out[a:a + n].split(b"\0")
        self.assertIn(b"vermagic=5.4.233 SMP preempt mod_unload modversions aarch64", info)
        self.assertIn(b"depends=sprd_tcpm,sprd_battery_info", info)


class VendorDlkmCharger(util.TestCase):
    """patch_vendor_dlkm.py's charger manager step (build.sh CHARGE_FIX)."""

    def test_changes(self):
        """By default the rebuild also patches the charger manager (patch_tested) and checks sprd_tcpm.ko;
        --stock-charger leaves both out."""
        full = dlkm.changes(False, True)
        self.assertEqual(sorted(full), sorted([dlkm.MODULE, dlkm.MODULE_LIST, dlkm.CHARGER, dlkm.TCPM]))
        self.assertEqual(dlkm.CHARGER, "lib/modules/sprd-charger-manager.ko")
        self.assertEqual(dlkm.TCPM, "lib/modules/sprd_tcpm.ko")
        self.assertIs(full[dlkm.CHARGER], sprd_cm.patch_tested)
        self.assertIs(full[dlkm.TCPM], sprd_cm.check_tcpm)
        self.assertEqual(sorted(dlkm.changes(True, False)), sorted([dlkm.MODULE, dlkm.MODULE_LIST]))

    def test_check_tcpm(self):
        if not os.path.isfile(TCPM):
            self.skipTest("no %s" % TCPM)
        ko = util.slurp(TCPM)
        self.assertEqual(sprd_cm.check_tcpm(ko), ko)
        with self.assertRaises(SystemExit):
            sprd_cm.check_tcpm(ko[:-1] + b"\1")

    def run_main(self, *args):
        seen = {}

        def rebuild(src, label, changes):
            seen.update(src=src, label=label, changes=changes)
            return b"image"
        dst = self.path("out.img")
        with mock.patch.object(dlkm, "rebuild", rebuild), \
                mock.patch.object(sys, "argv", ["patch_vendor_dlkm.py", *args, "in.img", dst]), \
                contextlib.redirect_stdout(io.StringIO()) as out:
            dlkm.main()
        self.assertEqual(util.slurp(dst), b"image")
        self.assertEqual((seen["src"], seen["label"]), ("in.img", "vendor_dlkm"))
        return seen["changes"], out.getvalue()

    def test_main(self):
        changes, out = self.run_main()
        self.assertIn(dlkm.CHARGER, changes)
        self.assertIn(dlkm.TCPM, changes)
        self.assertIn(dlkm.CHARGER, out)
        changes, out = self.run_main("--stock-charger", "--load-leds")
        self.assertEqual(sorted(changes), sorted([dlkm.MODULE, dlkm.MODULE_LIST]))
        self.assertIn("stock charger manager", out)
        self.assertTrue(changes[dlkm.MODULE_LIST]("\n".join(MODULES).encode()).endswith(b"cust-leds.ko\n"))

    def test_load_order(self):
        """Boot (modules.load, after the reorder) and off-mode charging (modules.load.charger, not changed)
        load the charger manager after the two modules it depends on."""
        for path, reorder in ((MODULE_LOAD, True), (MODULE_LOAD_CHARGER, False)):
            if not os.path.isfile(path):
                self.skipTest("no %s" % path)
            data = util.slurp(path)
            mods = (dlkm.reorder_modules(data, False) if reorder else data).decode().split()
            i = mods.index("sprd-charger-manager.ko")
            self.assertLess(mods.index("sprd_tcpm.ko"), i, path)
            self.assertLess(mods.index("sprd_battery_info.ko"), i, path)

    def test_rebuild(self):
        """The real rebuild (erofs_image.py) of test 41's vendor_dlkm with the charger step: the image's
        charger manager becomes the tested module, and every other file is as it was."""
        tools = util.need(self, "fsck.erofs", "mkfs.erofs")
        if not os.path.isfile(TEST41_DLKM):
            self.skipTest("no %s" % TEST41_DLKM)
        erofs_image = util.load_script("erofs_image")
        env = {"PATH": os.pathsep.join(tools + [os.environ.get("PATH", "")])}
        keep = lambda d: d  # noqa: E731  (test 41's sprd-drm.ko and modules.load are already patched)
        todo = dict(dlkm.changes(False, True), **{dlkm.MODULE: keep, dlkm.MODULE_LIST: keep})
        with mock.patch.dict(os.environ, env):
            new = erofs_image.rebuild(TEST41_DLKM, "vendor_dlkm", todo)
        self.assertEqual(len(new), os.path.getsize(TEST41_DLKM))
        img = self.write("new.img", new)
        trees = {}
        for name, src in (("old", TEST41_DLKM), ("new", img)):
            trees[name] = self.path(name)
            self.assertOk(util.run(["fsck.erofs", "--extract=" + trees[name], "--no-preserve", src], env=env))
        old, new = util.manifest(trees["old"]), util.manifest(trees["new"])
        self.assertEqual(sorted(old), sorted(new))
        self.assertEqual([p for p in old if old[p] != new[p]], [dlkm.CHARGER])
        self.assertEqual(hashlib.sha256(util.slurp(os.path.join(trees["old"], dlkm.CHARGER))).hexdigest(),
                         sprd_cm.MODULE_SHA256)
        self.assertEqual(hashlib.sha256(util.slurp(os.path.join(trees["new"], dlkm.CHARGER))).hexdigest(),
                         sprd_cm.TESTED_SHA256)


class BuildSwitch(util.TestCase):
    def test_build_sh(self):
        """CHARGE_FIX defaults to 1 with SYSTEM_IMG and to 0 without it (the 1.2 base, where CM12 never
        ran), is checked before the long steps, and CHARGE_FIX=0 alone passes
        --stock-charger to patch_vendor_dlkm.py."""
        text = util.slurp(os.path.join(util.SCRIPTS, "build.sh"), text=True)
        self.assertIn('FIX_DEFAULT=0\n[[ -z "${SYSTEM_IMG:-}" ]] || FIX_DEFAULT=1\n', text)
        self.assertIn('CHARGE_FIX="${CHARGE_FIX:-$FIX_DEFAULT}"\n', text)
        self.assertLess(text.index("FIX_DEFAULT=0"), text.index('CHARGE_FIX="${CHARGE_FIX:-$FIX_DEFAULT}"'))
        self.assertIn('  *) echo "CHARGE_FIX must be 0 or 1" >&2; exit 1 ;;\n', text)
        self.assertLess(text.index('CHARGE_FIX="${CHARGE_FIX:-$FIX_DEFAULT}"'), text.index("\nsudo -v\n"))
        self.assertIn('charger=()\n[[ "$CHARGE_FIX" == 1 ]] || charger=(--stock-charger)\n', text)
        self.assertIn('patch_vendor_dlkm.py" "${leds[@]}" "${charger[@]}" "$dlkm" "$dlkm.patched"', text)
        self.assertEqual(text.count("--stock-charger"), 1)

    def test_build_144_passes_it_through(self):
        text = util.slurp(os.path.join(util.SCRIPTS, "build_144.sh"), text=True)
        self.assertRegex(text, r"\n  for v in [A-Z_ ]*\bCHARGE_FIX\b[A-Z_ ]*; do\n")
