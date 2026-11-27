"""A disassembler for the few AArch64 instructions sprd_drm.py's patches use.

disassemble(addr, word) gives the text GNU objdump prints for WORD at ADDR, with
its "<symbol>" and "// comment" parts left out, single spaces, and branch targets
written as 0x... (objdump writes b.cs for b.hs and b.cc for b.lo). A word it does
not know gives "?" and its hex, so a test fails rather than passing on a guess.
"""

CONDS = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"]
BARRIERS = {0x1: "oshld", 0x2: "oshst", 0x3: "osh", 0x5: "nshld", 0x6: "nshst", 0x7: "nsh", 0x9: "ishld",
            0xa: "ishst", 0xb: "ish", 0xd: "ld", 0xe: "st", 0xf: "sy"}
SHIFTS = ["lsl", "lsr", "asr", "ror"]


def _signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def _reg(n, wide, sp=False):
    """Register N, x or w; 31 is the zero register, or the stack pointer with SP."""
    if n == 31:
        return ("sp" if wide else "wsp") if sp else ("xzr" if wide else "wzr")
    return ("x" if wide else "w") + str(n)


def _bitmask(n, immr, imms, width):
    """The value of a logical instruction's immediate (the ARM ARM's DecodeBitMasks)."""
    length = ((n << 6) | (~imms & 0x3f)).bit_length() - 1
    if length < 1:
        return None
    size = 1 << length
    s, r = imms & (size - 1), immr & (size - 1)
    if s == size - 1:
        return None
    elem = (1 << (s + 1)) - 1
    elem = ((elem >> r) | (elem << (size - r))) & ((1 << size) - 1)
    value = 0
    for i in range(0, width, size):
        value |= elem << i
    return value


def _move_wide(value, width):
    """Whether a movz or movn could set VALUE (objdump then prints orr, not mov)."""
    mask = (1 << width) - 1
    return any(v & ~(0xffff << (16 * hw)) & mask == 0 for v in (value, ~value & mask)
               for hw in range(width // 16))


def disassemble(addr, w):
    sf = w >> 31
    rd, rn, rm = w & 31, (w >> 5) & 31, (w >> 16) & 31
    if w == 0xd503201f:
        return "nop"
    if w & 0xfffff0ff == 0xd50330bf and (w >> 8) & 15 in BARRIERS:
        return "dmb " + BARRIERS[(w >> 8) & 15]
    if w & 0xfffffc1f == 0xd63f0000:
        return "blr x%d" % rn
    if w & 0xfffffc1f == 0xd61f0000:
        return "br x%d" % rn
    if w == 0xd65f03c0:
        return "ret"
    if w & 0x7c000000 == 0x14000000:                       # b, bl
        return "%s 0x%x" % ("bl" if w >> 31 else "b", addr + 4 * _signed(w & 0x3ffffff, 26))
    if w & 0xff000010 == 0x54000000:                       # b.cond
        return "b.%s 0x%x" % (CONDS[w & 15], addr + 4 * _signed((w >> 5) & 0x7ffff, 19))
    if w & 0x7e000000 == 0x34000000:                       # cbz, cbnz
        return "%s %s, 0x%x" % ("cbnz" if w & (1 << 24) else "cbz", _reg(rd, sf),
                                addr + 4 * _signed((w >> 5) & 0x7ffff, 19))
    if w & 0x7e000000 == 0x36000000:                       # tbz, tbnz
        bit = (sf << 5) | ((w >> 19) & 31)
        return "%s %s, #%d, 0x%x" % ("tbnz" if w & (1 << 24) else "tbz", _reg(rd, bit >= 32), bit,
                                     addr + 4 * _signed((w >> 5) & 0x3fff, 14))
    if w & 0x3f000000 == 0x39000000:                       # ldr/str (unsigned offset)
        size, opc = w >> 30, (w >> 22) & 3
        if opc in (0, 1):
            name = ("str" if opc == 0 else "ldr") + {0: "b", 1: "h"}.get(size, "")
            off = ((w >> 10) & 0xfff) << size
            mem = "[%s, #%d]" % (_reg(rn, True, sp=True), off) if off else "[%s]" % _reg(rn, True, sp=True)
            return "%s %s, %s" % (name, _reg(rd, size == 3), mem)
    if w & 0xbfa00c00 == 0xb8000400:                       # str/ldr (post-index), 32/64-bit
        wide = bool(w & (1 << 30))
        name = "ldr" if w & (1 << 22) else "str"
        return "%s %s, [%s], #%d" % (name, _reg(rd, wide), _reg(rn, True, sp=True), _signed((w >> 12) & 0x1ff, 9))
    if w & 0x1f800000 == 0x12800000:                       # movn, movz, movk
        opc, hw, imm = (w >> 29) & 3, (w >> 21) & 3, (w >> 5) & 0xffff
        if opc == 2:
            return "mov %s, #0x%x" % (_reg(rd, sf), imm << (16 * hw))
        if opc == 3:
            return "movk %s, #0x%x" % (_reg(rd, sf), imm) + (", lsl #%d" % (16 * hw) if hw else "")
    if w & 0x1f800000 == 0x12000000:                       # and, orr, eor, ands (immediate)
        opc = (w >> 29) & 3
        value = _bitmask((w >> 22) & 1, (w >> 16) & 63, (w >> 10) & 63, 64 if sf else 32)
        if value is not None and opc == 1 and rn == 31 and not _move_wide(value, 64 if sf else 32):
            return "mov %s, #0x%x" % (_reg(rd, sf, sp=True), value)
        if value is not None and opc in (0, 1, 2):
            return "%s %s, %s, #0x%x" % (["and", "orr", "eor"][opc], _reg(rd, sf, sp=True), _reg(rn, sf), value)
    if w & 0x1f000000 == 0x0a000000:                       # and, orr, eor, ands (shifted register)
        opc, n, shift, amount = (w >> 29) & 3, (w >> 21) & 1, (w >> 22) & 3, (w >> 10) & 63
        if opc == 1 and n == 0 and rn == 31 and amount == 0:
            return "mov %s, %s" % (_reg(rd, sf), _reg(rm, sf))
        if n == 0 and opc in (0, 1, 2):
            text = "%s %s, %s, %s" % (["and", "orr", "eor"][opc], _reg(rd, sf), _reg(rn, sf), _reg(rm, sf))
            return text + (", %s #%d" % (SHIFTS[shift], amount) if amount else "")
    if w & 0x1f800000 == 0x11000000:                       # add, adds, sub, subs (immediate)
        op, s, shift, imm = (w >> 30) & 1, (w >> 29) & 1, (w >> 22) & 1, (w >> 10) & 0xfff
        tail = ", lsl #12" if shift else ""
        if s == 1 and rd == 31:
            return "%s %s, #0x%x%s" % ("cmp" if op else "cmn", _reg(rn, sf, sp=True), imm, tail)
        if op == 0 and s == 0 and imm == 0 and not shift and 31 in (rd, rn):
            return "mov %s, %s" % (_reg(rd, sf, sp=True), _reg(rn, sf, sp=True))
        name = ["add", "adds", "sub", "subs"][op * 2 + s]
        return "%s %s, %s, #0x%x%s" % (name, _reg(rd, sf, sp=not s), _reg(rn, sf, sp=True), imm, tail)
    if w & 0x1f200000 == 0x0b000000:                       # add, adds, sub, subs (shifted register)
        op, s, shift, amount = (w >> 30) & 1, (w >> 29) & 1, (w >> 22) & 3, (w >> 10) & 63
        tail = ", %s #%d" % (SHIFTS[shift], amount) if amount else ""
        if s == 1 and rd == 31:
            return "%s %s, %s%s" % ("cmp" if op else "cmn", _reg(rn, sf), _reg(rm, sf), tail)
        if op == 1 and rn == 31:
            return "%s %s, %s%s" % ("negs" if s else "neg", _reg(rd, sf), _reg(rm, sf), tail)
        name = ["add", "adds", "sub", "subs"][op * 2 + s]
        return "%s %s, %s, %s%s" % (name, _reg(rd, sf), _reg(rn, sf), _reg(rm, sf), tail)
    if w & 0x7fe08000 == 0x1b000000:                       # madd
        ra = (w >> 10) & 31
        if ra == 31:
            return "mul %s, %s, %s" % (_reg(rd, sf), _reg(rn, sf), _reg(rm, sf))
        return "madd %s, %s, %s, %s" % (_reg(rd, sf), _reg(rn, sf), _reg(rm, sf), _reg(ra, sf))
    if w & 0x7fe0fc00 == 0x1ac00800:                       # udiv
        return "udiv %s, %s, %s" % (_reg(rd, sf), _reg(rn, sf), _reg(rm, sf))
    if w & 0x9f000000 == 0x10000000:                       # adr
        return "adr %s, 0x%x" % (_reg(rd, True), addr + _signed(((w >> 5) & 0x7ffff) << 2 | (w >> 29) & 3, 21))
    if w & 0x3f800000 == 0x29000000 and (w >> 30) in (0, 2):   # ldp, stp (signed offset), 32/64-bit
        wide = bool(w >> 31)
        off = _signed((w >> 15) & 0x7f, 7) * (8 if wide else 4)
        base = _reg(rn, True, sp=True)
        mem = "[%s, #%d]" % (base, off) if off else "[%s]" % base
        return "%s %s, %s, %s" % ("ldp" if w & (1 << 22) else "stp", _reg(rd, wide),
                                  _reg((w >> 10) & 31, wide), mem)
    if w & 0x3fe00410 == 0x3a400000:                       # ccmp, ccmn (register, immediate)
        second = "#0x%x" % rm if w & (1 << 11) else _reg(rm, sf)
        return "%s %s, %s, #0x%x, %s" % ("ccmp" if w & (1 << 30) else "ccmn", _reg(rn, sf), second, w & 15,
                                         CONDS[(w >> 12) & 15])
    if w & 0x3fe00800 == 0x1a800000:                       # csel, csinc, csinv, csneg
        op, op2, cond = (w >> 30) & 1, (w >> 10) & 1, (w >> 12) & 15
        if op == 0 and op2 == 1 and rn == 31 and rm == 31 and cond < 14:
            return "cset %s, %s" % (_reg(rd, sf), CONDS[cond ^ 1])
        name = [["csel", "csinc"], ["csinv", "csneg"]][op][op2]
        return "%s %s, %s, %s, %s" % (name, _reg(rd, sf), _reg(rn, sf), _reg(rm, sf), CONDS[cond])
    if w & 0x1f800000 == 0x13000000 and (w >> 29) & 3 in (0, 2) and (w >> 22) & 1 == sf:   # sbfm, ubfm
        unsigned, width = (w >> 29) & 3 == 2, 64 if sf else 32
        immr, imms = (w >> 16) & 63, (w >> 10) & 63
        d, n = _reg(rd, sf), _reg(rn, sf)
        if unsigned and imms != width - 1 and imms + 1 == immr:
            return "lsl %s, %s, #%d" % (d, n, width - 1 - imms)
        if imms == width - 1:
            return "%s %s, %s, #%d" % ("lsr" if unsigned else "asr", d, n, immr)
        if imms < immr:
            return "%s %s, %s, #%d, #%d" % ("ubfiz" if unsigned else "sbfiz", d, n, width - immr, imms + 1)
        # The ARM ARM's BFXPreferred: else an extend (uxtb, uxth; sxtb, sxth, sxtw).
        if immr == 0 and ((not sf and imms in (7, 15)) or (sf and not unsigned and imms in (7, 15, 31))):
            return "%sxt%s %s, %s" % ("u" if unsigned else "s", {7: "b", 15: "h", 31: "w"}[imms], d, _reg(rn, False))
        return "%s %s, %s, #%d, #%d" % ("ubfx" if unsigned else "sbfx", d, n, immr, imms - immr + 1)
    return "? %08x" % w
