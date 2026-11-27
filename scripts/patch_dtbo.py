#!/usr/bin/env python3
"""Patches the RG Cube's dtbo so a powered USB-C dock gives power in, picture and USB at the same time
(usb_c_dock_plan.md, section 4a).

  patch_dtbo.py patch [--tested] [--source-ma N [--allow-over-rating]] [--jeita thermal2] IN OUT
  patch_dtbo.py check [--allow-over-rating] IMAGE [VENDOR_BOOT]
  patch_dtbo.py tested

dtbo is an Android dt_table image of board overlays with an AVB hash footer. Only the device's own
entry (DEVICES: the RG Cube's is entry 2, board "UMS9620 2H10") changes; the others stay byte-identical.
In it:
  - the USB-C connector (fragment targeting pmic_pd) gets data-role "dual", so the Cube accepts a dock's
    DR_Swap; its 5 V sink PDO gets the dual-role power and data bits, and the PPS APDO (3.3-11 V) goes;
    its 5 V source PDO offers 900 mA instead of 400 mA (what a dock's hub needs; the OTG boost is rated
    1 A);
  - the c2c core set: use_pdhub_c2c on the Type-C controller (pmic_typec, a new fragment), the USB3
    glue (usb3) and the USB2 controller (usb), and vbus-supply on the PD controller (pmic_pd) pointing to
    the entry's own OTG boost regulator (vddvbus), so tcpm switches VBUS. The set is all or nothing: a
    part of it lets the boost back-drive a dock or leaves VBUS unpowered, so a partial one is refused;
  - in the same set (test 42 dock fix A, dock_multifunction_plan.md section 2.1): the USB3 glue's
    VBUS extcon (its extcon's first cell) moves from extcon_gpio to pmic_typec, by moving its
    reference from __fixups__/extcon_gpio to the end of __fixups__/pmic_typec. Its extcon then
    resolves to <pmic_typec pmic_typec>, so the DR_Swap's USB=0 reaches dwc3 and the swap's
    USB_HOST=1 can start the host. Nothing else moves (the USB2 controller, the PHYs and the charger
    keep extcon_gpio).
Nodes are found by what they are, through __fixups__ and __symbols__, never by fragment number. Each
property is checked against the value it is expected to have first: a different value is refused, the
new one is left as it is.

`patch` checks IN's sha256 (the device's stock dtbo, or an image this patch made from it that
PATCHED_INPUTS lists), re-signs the unpatched image and requires it byte-identical to IN (so the
same avbtool and key sign the result), then patches and signs it with IN's salt, properties and AVB
options (avb_footer.py). --tested makes the image approved for the device, which build.sh requires:
the 900 mA offer and the device's approved jeita tables (its tested_jeita), and the result must be
its tested_sha256.

--source-ma N makes a test image whose 5 V source PDO offers N mA (900-3000, in 10 mA steps; 900 is
the default and the approved image's), same flags. Above the boost's 1 A rating (MAX_SOURCE_MA) it
needs --allow-over-rating; --tested takes neither (only the approved image is approved). Without
--source-ma an already patched IN keeps its own offer; a different one is refused (patch the stock
image instead).

--jeita thermal2 makes the battery charge slower as it warms (JEITA): the DCP (wall charger),
UNKNOWN (the dock) and CDP tables of every node in the device's entry that has them go from the
stock 4 rows to thermal2's 6, the same edit on each: the stock steps, and below the stock 52 C one
1.5 A from 45 C and 1.0 A from 49 C (never above the table's own full rate). Every other table, and
every other entry, stays as it is. thermal2 is the RG Cube's approved tables (test 42 TH2), so
--tested applies it without --jeita; with --tested, --jeita may only name the approved tables.
Without --tested and --jeita the tables stay stock (test 42 DK1, a test image now).

`check` requires IMAGE (signed or not) to be patched, with no sink PDO above 5 V, no source PDO above
5 V or 1 A (3 A with --allow-over-rating), and none of the fast-charging switches (FORBIDDEN). Its
source PDO may be any the patch can make. Every jeita table in the entry must keep the charger
manager's rules (jeita_problem()), and the DCP, UNKNOWN and CDP ones must all be stock or all be
thermal2; it prints each table's bands, which variant is the approved one, and whether IMAGE is the
approved image. With VENDOR_BOOT, and fdtoverlay on PATH, it also applies the entry to the base
device tree in vendor_boot, as the bootloader does, and
checks the result has every edit at the nodes the plan names: the USB3 glue's extcon is
<pmic_typec pmic_typec> (the base DT's phandle), the jeita tables are the entry's, and every other
phandle the entry's __fixups__ name still resolves to its own label's node. Without fdtoverlay it
says so and skips that part.

`tested` fails unless every device has an approved image, so build.sh can refuse DOCK_FIX=1 before
its long steps.

`patch` needs avbtool on PATH and AVB_KEY naming AOSP's testkey_rsa4096.pem.
"""
import collections
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from avb_footer import add_footer, footer_info  # noqa: E402

DT_TABLE_MAGIC = 0xD7B7AB1E
FDT_MAGIC = 0xD00DFEED
FDT_HEADER = 40
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9

# dtbo_idx: the entry the bootloader applies (androidboot.dtbo_idx); board_id: that entry's id;
# tested_sha256: the patched image approved for the device, empty until then (build.sh needs it);
# tested_jeita: the JEITA variant `patch --tested` applies to it, None for stock tables.
Device = collections.namedtuple("Device", "name dtbo_idx board_id partition partition_size tested_sha256 "
                                "tested_jeita", defaults=(None,))
DEVICES = {
    # The RG Cube's stock dtbo: the GammaOS Next 1.2 PAC's DTBO and DTBO_B, and a stock Cube's dtbo_a.
    "ca5a55ff02e439a4e0ae069874c80d583dd2cdb967e69f64eb2036e273faafcc":
        # tested_sha256: `patch --jeita thermal2` of this image with AOSP's testkey_rsa4096.pem: fix A
        # (the usb3 extcon on pmic_typec), the 900 mA source PDO and the thermal2 jeita tables. Test 42
        # TH2 (out/dp-evidence/test42/thermal): written to dtbo_a with the owner's approval, the live
        # tables read back as thermal2 and charging logged jeita_size:6; the owner chose to ship it.
        # It is test 42 DK1 with only the DCP, UNKNOWN and CDP tables changed. DK1, `patch` of this
        # image (1b727faa07a2c0926b3b50c3d574284ee11be99550ff77c8014da35872baa72f), gave the
        # multi-function dock a picture, USB 3.0 at 5 Gbit/s, working USB devices and charging with
        # the Cube as data host and power sink (dp_dock_findings.md). (Test 40's image, without fix A,
        # was 94c0f3d4bb1fbbf518a0461db39908d02fd67e320e52c45ced540076d09c5536.)
        Device("RG Cube", 2, 0x2100, "dtbo", 8388608,
               "c87ece8c1133c9cf5ce39540333d3e186f73bc3a4d3579d25af878d2164e1e42", "thermal2"),
}

# Images `patch` made from a DEVICES image that it may patch again (--jeita on the image being
# flashed): sha256 -> (the DEVICES key it was made from, what it is). Each has the stock jeita tables:
# patching one again leaves its connector and core set as they are, and the result is what patching
# the stock image with the same options gives. An image with thermal2 tables (the approved one) is not
# listed: without --jeita it would keep them, unlike the stock image's patch.
PATCHED_INPUTS = {
    # Test 42 DK1: `patch` (900 mA), approved before TH2 (tested_sha256 above).
    "1b727faa07a2c0926b3b50c3d574284ee11be99550ff77c8014da35872baa72f":
        ("ca5a55ff02e439a4e0ae069874c80d583dd2cdb967e69f64eb2036e273faafcc", "test 42 DK1 (900 mA)"),
    # Test 42 DK3: `patch --source-ma 1000` (out/dp-evidence/test42/dk3/NOTES.md).
    "737e9f8081fd3c84645b86f699b9c600ef9fa90eab0d5b7ab51b418cac7ff404":
        ("ca5a55ff02e439a4e0ae069874c80d583dd2cdb967e69f64eb2036e273faafcc", "test 42 DK3-1000 (1000 mA)"),
}

# The 5 V source PDO: fixed, 5 V, USB communications capable (the stock flags), and its current in
# 10 mA units in bits 9:0. The stock one offers 400 mA; the patch's DEFAULT_SOURCE_MA, or a test
# image's --source-ma between DEFAULT_SOURCE_MA and OVER_RATING_MAX_MA.
SOURCE_PDO_5V = 0x04019000
STOCK_SOURCE_MA, DEFAULT_SOURCE_MA, OVER_RATING_MAX_MA = 400, 900, 3000


def source_pdo(ma):
    return SOURCE_PDO_5V | ma // 10


def connector_edits(source_ma=DEFAULT_SOURCE_MA):
    """The connector's edits (property, original value, new value); None: no such property."""
    return [
        ("data-role", None, b"dual\0"),
        # 5 V 3 A fixed + PPS APDO 3.3-11 V 3 A -> 5 V 3 A fixed with DRP|DRD.
        ("sink-pdos", struct.pack(">2I", 0x0401912C, 0xC0DC213C), struct.pack(">I", 0x2601912C)),
        # 5 V 400 mA fixed -> 5 V 900 mA (or --source-ma) fixed, same flags.
        ("source-pdos", struct.pack(">I", source_pdo(STOCK_SOURCE_MA)), struct.pack(">I", source_pdo(source_ma))),
    ]


C2C = "use_pdhub_c2c"
# The overlays of these labels' nodes get C2C (pmic_typec has no fragment of its own in the stock entry).
C2C_TARGETS = ["pmic_typec", "usb3", "usb"]
PD_TARGET = "pmic_pd"
VBUS_SYMBOL = "vddvbus"
# Dock fix A: the label whose overlay's extcon (VBUS cell 0, id cell 4) moves, and from/to which extcon.
EXTCON_TARGET = "usb3"
EXTCON_FROM, EXTCON_TO = "extcon_gpio", "pmic_typec"
FORBIDDEN = ["sprd,support-fchg", "sprd,support-pd-pps", "sprd,support-sfcp"]
MAX_SINK_MV = 5000
# MAX_SOURCE_MA: the AW32257 OTG boost's rated output (1 A at 5.05 V).
MAX_SOURCE_MV, MAX_SOURCE_MA = 5000, 1000

# The battery node's JEITA tables (sprd_battery_info, read by sprd-charger-manager): rows of
# <temperature recovery-temperature current-uA cv-uV>, temperatures in 0.1 C above -100.0 C. The charger
# manager reads the battery NTC every 15 s; with N rows (temperatures T, recoveries R):
#   - at or below T[0] and at or above T[N-1] it stops charging (cold, overheat);
#   - between T[k-1] and T[k] it charges at most row k's current, up to row k's CV (row 0's are unused);
#   - a row with R above T (cold side) moves the step up to R when warming; one with R below T (hot
#     side) moves the step down to R when cooling. A change needs two polls in a row.
# A row's 0 uA does not stop charging: the AW32257's lowest step (496 mA) charges on. The AW32257 takes
# the highest of its ICHG steps (496, 620, then 868-2480 mA in 124 mA steps) not above the request.
JEITA_OFFSET, JEITA_ROW = 1000, 16
JEITA_SUFFIX = "-jeita-temp-table"
# The tables --jeita edits, and their full rate (the stock middle row's current, kept by every variant).
JEITA_TYPES = ("dcp", "unknown", "cdp")
JEITA_FULL_UA = {"dcp": 2000000, "unknown": 1050000, "cdp": 1500000}
CV_UV, CV_HOT_UV = 4340000, 4100000


def jeita_stock(full_ua):
    """The RG Cube's stock rows (0.1 C, 0.1 C, uA, uV): 0.7 A from 0 C, full rate from 18 C, 0.7 A and
    4.1 V from 52 C, stop at 60 C."""
    return [(0, 30, 0, CV_UV), (150, 180, 700000, CV_UV), (520, 490, full_ua, CV_UV),
            (600, 570, 700000, CV_HOT_UV)]


def jeita_thermal2(full_ua):
    """thermal2: stock, with two more steps below the stock 52 C one while warming: 1.5 A from 45 C and
    1.0 A from 49 C, each no more than FULL_UA; each step back below 3 C less (42, 46 C), as the stock
    ones (49, 57 C). The AW32257 programs 1.5 A as 1488 mA and 1.0 A (and 1.05 A) as 992 mA."""
    return [(0, 30, 0, CV_UV), (150, 180, 700000, CV_UV), (450, 420, full_ua, CV_UV),
            (490, 460, min(full_ua, 1500000), CV_UV), (520, 490, min(full_ua, 1000000), CV_UV),
            (600, 570, 700000, CV_HOT_UV)]


JEITA = {"stock": jeita_stock, "thermal2": jeita_thermal2}
JEITA_EDITS = [name for name in JEITA if name != "stock"]


def jeita_table(rows):
    """The property value of ROWS ((temperature, recovery) in 0.1 C, current in uA, CV in uV)."""
    return b"".join(struct.pack(">4I", t + JEITA_OFFSET, r + JEITA_OFFSET, ua, uv) for t, r, ua, uv in rows)


def jeita_rows(value):
    """The rows of a jeita table property, as jeita_table() takes them; None unless whole rows."""
    if not value or len(value) % JEITA_ROW:
        return None
    words = struct.unpack(">%dI" % (len(value) // 4), value)
    return [(words[i] - JEITA_OFFSET, words[i + 1] - JEITA_OFFSET, words[i + 2], words[i + 3])
            for i in range(0, len(words), 4)]


def jeita_problem(rows):
    """Why the charger manager would not step through ROWS as their bands say, or None: temperatures
    and recoveries must rise, cold-side rows come before hot-side ones (the cooling case looks at the
    next row), each recovery stays inside the bands beside it, and only a stop can stop charging."""
    if not rows:
        return "not whole 16-byte rows"
    temps, recs = [r[0] for r in rows], [r[1] for r in rows]
    for what, column in (("temperatures", temps), ("recovery temperatures", recs)):
        if any(a >= b for a, b in zip(column, column[1:])):
            return "its %s do not rise" % what
    hot = False
    for i, (t, r, ua, _) in enumerate(rows):
        if r > t and hot:
            return "row %d is cold-side (recovery above its temperature) after a hot-side row" % i
        hot = hot or r < t
        # A cold-side recovery at the next temperature would skip the next band when warming. A hot-side
        # one may be the previous temperature (thermal2's 49 C below its 49 C step): cooling then
        # lands in this row's band, which holds down to the previous row's recovery.
        if r > t and i + 1 < len(rows) and r >= temps[i + 1]:
            return "row %d's recovery is not below row %d's temperature" % (i, i + 1)
        if r < t and i and r < temps[i - 1]:
            return "row %d's recovery is below row %d's temperature" % (i, i - 1)
        if 0 < i and not ua:
            return "row %d's 0 uA does not stop charging (the charger's lowest step charges on)" % i
    return None


def celsius(t):
    return "%.1f C" % (t / 10)


def jeita_bands(rows):
    """ROWS as the bands the charger manager steps through: each boundary is "cooling/warming", the
    temperature it steps down below and the one it steps up from."""
    parts = ["stop"]
    for i, (t, r, _, _) in enumerate(rows):
        parts.append("%s/%s" % (celsius(min(t, r)), celsius(max(t, r))))
        if i + 1 < len(rows):
            ua, uv = rows[i + 1][2:]
            parts.append("%d mA %.2f V" % (ua // 1000, uv / 1e6))
    return " | ".join(parts + ["stop"])


def jeita_nodes(root):
    """(path, node) of every node of the overlay ROOT that has a jeita table."""
    return [(path, node) for path, node in root.walk() if any(p[0].endswith(JEITA_SUFFIX) for p in node.props)]


def jeita_variant(kind, value):
    """The JEITA variant whose KIND table VALUE is, or None."""
    return next((name for name, rows in JEITA.items() if value == jeita_table(rows(JEITA_FULL_UA[kind]))), None)


def patch_jeita(root, variant):
    """Makes the DCP, UNKNOWN and CDP tables of every jeita node of ROOT the VARIANT ones, from stock.
    Returns the changes, as (path, property, new value); none when they are VARIANT's already."""
    nodes = jeita_nodes(root)
    if not nodes:
        sys.exit("dtbo: no node with jeita tables")
    changes = []
    for path, node in nodes:
        for kind in JEITA_TYPES:
            name, value = kind + JEITA_SUFFIX, node.prop(kind + JEITA_SUFFIX)
            new = jeita_table(JEITA[variant](JEITA_FULL_UA[kind]))
            if value == new:
                continue
            if jeita_variant(kind, value) != "stock":
                sys.exit("dtbo: %s %s is %s, expected the stock table" % (path, name, show(value)))
            node.set_prop(name, new)
            changes.append((path, name, new))
    return changes


def check_jeita(root):
    """Exits unless every jeita table of ROOT keeps jeita_problem()'s rules and the DCP, UNKNOWN and CDP
    ones are all one JEITA variant. Returns that variant, or None when ROOT has no jeita table."""
    found = set()
    for path, node in jeita_nodes(root):
        for name, value, _ in node.props:
            if name.endswith(JEITA_SUFFIX):
                problem = jeita_problem(jeita_rows(value))
                if problem:
                    sys.exit("dtbo entry: %s %s: %s" % (path, name, problem))
        for kind in JEITA_TYPES:
            variant = jeita_variant(kind, node.prop(kind + JEITA_SUFFIX))
            if variant is None:
                sys.exit("dtbo entry: %s %s%s is %s, not a table the patch knows (%s)" % (
                    path, kind, JEITA_SUFFIX, show(node.prop(kind + JEITA_SUFFIX)), ", ".join(JEITA)))
            found.add(variant)
    if len(found) > 1:
        sys.exit("dtbo entry: the jeita tables are a mix of %s" % " and ".join(sorted(found)))
    return found.pop() if found else None


def jeita_report(root):
    """A line for each jeita table of ROOT: where, which variant (the edited kinds), and its bands."""
    lines = []
    for path, node in jeita_nodes(root):
        for name, value, _ in node.props:
            if name.endswith(JEITA_SUFFIX):
                kind = name[:-len(JEITA_SUFFIX)]
                variant = jeita_variant(kind, value) if kind in JEITA_TYPES else "not edited by the patch"
                rows = jeita_rows(value)
                lines.append("jeita %s %s (%s, %d rows): %s" % (path, name, variant, len(rows), jeita_bands(rows)))
    return lines


def source_limit(source_ma, allow_over_rating=False):
    """Exits unless SOURCE_MA is a source current the patch offers (with ALLOW_OVER_RATING: above the
    boost's rating too)."""
    if source_ma % 10 or not DEFAULT_SOURCE_MA <= source_ma <= OVER_RATING_MAX_MA:
        sys.exit("dtbo: --source-ma %d is not %d-%d mA in 10 mA steps"
                 % (source_ma, DEFAULT_SOURCE_MA, OVER_RATING_MAX_MA))
    if source_ma > MAX_SOURCE_MA and not allow_over_rating:
        sys.exit("dtbo: --source-ma %d is above the OTG boost's %d mA rating; a test image needs "
                 "--allow-over-rating" % (source_ma, MAX_SOURCE_MA))


class Node:
    """A device tree node: its name, properties ([name, value, string offset or None]) and subnodes."""

    def __init__(self, name, props=None, children=None):
        self.name = name
        self.props = props or []
        self.children = children or []

    def prop(self, name):
        for p in self.props:
            if p[0] == name:
                return p[1]
        return None

    def set_prop(self, name, value):
        for p in self.props:
            if p[0] == name:
                p[1] = value
                return
        self.props.append([name, value, None])

    def child(self, name):
        for c in self.children:
            if c.name == name:
                return c
        return None

    def find(self, path):
        node = self
        for part in path.strip("/").split("/") if path.strip("/") else []:
            node = node.child(part)
            if node is None:
                return None
        return node

    def walk(self, path=""):
        """(path, node) for this node and every node under it."""
        yield path or "/", self
        for c in self.children:
            yield from c.walk(path + "/" + c.name)


FDT = collections.namedtuple("FDT", "version last_comp boot_cpu rsvmap strings root")


def pad4(n):
    return (n + 3) & ~3


def cstr(data, off, what):
    end = data.find(b"\0", off)
    if end < 0:
        sys.exit("%s: unterminated string" % what)
    return data[off:end].decode()


def parse_fdt(blob, what="FDT"):
    if len(blob) < FDT_HEADER:
        sys.exit("%s: too short for an FDT" % what)
    (magic, total, off_struct, off_strings, off_rsv, version, last_comp, boot_cpu, size_strings,
     size_struct) = struct.unpack_from(">10I", blob, 0)
    if magic != FDT_MAGIC:
        sys.exit("%s: not an FDT" % what)
    if version < 17 or total != len(blob):
        sys.exit("%s: FDT version %d, size %d of %d" % (what, version, total, len(blob)))
    rsv_end = off_rsv
    while True:
        if rsv_end + 16 > len(blob):
            sys.exit("%s: unterminated reserved memory map" % what)
        addr, size = struct.unpack_from(">QQ", blob, rsv_end)
        rsv_end += 16
        if not addr and not size:
            break
    strings = blob[off_strings:off_strings + size_strings]
    root, stack, pos, end = None, [], off_struct, off_struct + size_struct
    while True:
        if pos + 4 > end:
            sys.exit("%s: structure block has no end" % what)
        tag, = struct.unpack_from(">I", blob, pos)
        pos += 4
        if tag == FDT_BEGIN_NODE:
            name = cstr(blob, pos, what)
            pos = pad4(pos + len(name.encode()) + 1)
            node = Node(name)
            if stack:
                stack[-1].children.append(node)
            elif root is None:
                root = node
            else:
                sys.exit("%s: two root nodes" % what)
            stack.append(node)
        elif tag == FDT_END_NODE:
            if not stack:
                sys.exit("%s: unbalanced end of node" % what)
            stack.pop()
        elif tag == FDT_PROP:
            length, nameoff = struct.unpack_from(">II", blob, pos)
            pos += 8
            if not stack or stack[-1].children:
                sys.exit("%s: property outside a node's property list" % what)
            stack[-1].props.append([cstr(strings, nameoff, what), blob[pos:pos + length], nameoff])
            pos = pad4(pos + length)
        elif tag == FDT_END:
            break
        else:
            # NOP included: the rebuilt blob would not match the original.
            sys.exit("%s: unexpected FDT token %d" % (what, tag))
    if stack or root is None or pos != end:
        sys.exit("%s: malformed structure block" % what)
    return FDT(version, last_comp, boot_cpu, blob[off_rsv:rsv_end], strings, root)


def build_fdt(fdt):
    """The blob of FDT, laid out as dtc does: header, reserved memory map, structure, strings. New
    property names are appended to the strings block."""
    strings = bytearray(fdt.strings)

    def nameoff(name):
        key = name.encode() + b"\0"
        off = strings.find(key)
        if off < 0:
            off = len(strings)
            strings.extend(key)
        return off

    out = bytearray()

    def emit(node):
        name = node.name.encode() + b"\0"
        out.extend(struct.pack(">I", FDT_BEGIN_NODE) + name.ljust(pad4(len(name)), b"\0"))
        for name, value, off in node.props:
            out.extend(struct.pack(">III", FDT_PROP, len(value), nameoff(name) if off is None else off))
            out.extend(value.ljust(pad4(len(value)), b"\0"))
        for c in node.children:
            emit(c)
        out.extend(struct.pack(">I", FDT_END_NODE))

    emit(fdt.root)
    out.extend(struct.pack(">I", FDT_END))
    off_struct = FDT_HEADER + len(fdt.rsvmap)
    off_strings = off_struct + len(out)
    total = off_strings + len(strings)
    head = struct.pack(">10I", FDT_MAGIC, total, off_struct, off_strings, FDT_HEADER, fdt.version,
                       fdt.last_comp, fdt.boot_cpu, len(strings), len(out))
    return head + fdt.rsvmap + bytes(out) + bytes(strings)


def parse_table(raw, what="dtbo"):
    """(header words, [entry words]) of a dt_table image; entries must lie back to back, in order,
    right after the entry table (mkdtimg's layout)."""
    if len(raw) < 32 or struct.unpack_from(">I", raw, 0)[0] != DT_TABLE_MAGIC:
        sys.exit("%s: not a dt_table image" % what)
    header = list(struct.unpack_from(">8I", raw, 0))
    _, _, header_size, entry_size, count, entries_off, _, _ = header
    if header_size != 32 or entry_size != 32 or entries_off != 32 or not count:
        sys.exit("%s: unexpected dt_table header" % what)
    entries = [list(struct.unpack_from(">8I", raw, entries_off + k * 32)) for k in range(count)]
    pos = entries_off + count * 32
    for k, e in enumerate(entries):
        if e[1] != pos or pos + e[0] > len(raw):
            sys.exit("%s: entry %d is not where the previous one ends" % (what, k))
        pos += e[0]
    return header, entries


def entry_blob(raw, entries, idx):
    size, off = entries[idx][:2]
    return raw[off:off + size]


def replace_entry(raw, idx, blob):
    """RAW (a dt_table image without its footer) with entry IDX's blob replaced by BLOB. Every other
    entry moves unchanged; anything after the last entry is kept. The header's total size stays as far
    short of the end of the entries as the original: the RG Cube's says 538488 where its entries end
    at 538536 (48 bytes short)."""
    header, entries = parse_table(raw)
    end = entries[-1][1] + entries[-1][0]
    short = end - header[1]
    blobs = [entry_blob(raw, entries, k) for k in range(len(entries))]
    blobs[idx] = blob
    out = bytearray(raw[:32 + 32 * len(entries)])
    pos = len(out)
    for k, (e, b) in enumerate(zip(entries, blobs)):
        e[0], e[1] = len(b), pos
        struct.pack_into(">8I", out, 32 + 32 * k, *e)
        pos += len(b)
    struct.pack_into(">I", out, 4, pos - short)
    return bytes(out) + b"".join(blobs) + raw[end:]


def cells(*words):
    return struct.pack(">%dI" % len(words), *words)


def show(value):
    """VALUE as dtc would print it, roughly: strings, cells or bytes."""
    if value is None:
        return "absent"
    words = value[:-1].split(b"\0")
    if value.endswith(b"\0") and all(w and all(32 <= b < 127 for b in w) for w in words):
        return ", ".join('"%s"' % w.decode() for w in words)
    if len(value) % 4 == 0 and value:
        return "<%s>" % " ".join("0x%08x" % w for w in struct.unpack(">%dI" % (len(value) // 4), value))
    return repr(value)


def fixup_refs(root, label):
    """LABEL's references in __fixups__ ("path:property:offset"), in order."""
    fixups = root.child("__fixups__")
    value = fixups.prop(label) if fixups else None
    return [ref.decode() for ref in (value or b"").split(b"\0") if ref]


def refs_value(refs):
    """A __fixups__ property of the references REFS."""
    return b"".join(ref.encode() + b"\0" for ref in refs)


def fixup_targets(root, label):
    """The fragments whose target is LABEL (__fixups__ entries "/fragment@N:target:0")."""
    found = []
    for ref in fixup_refs(root, label):
        path, _, rest = ref.rpartition(":")
        path, _, prop = path.rpartition(":")
        if prop == "target" and rest == "0":
            found.append(path)
    return found


def overlay_of(root, label, required=True):
    """(fragment, its __overlay__) of the one fragment targeting LABEL, or (None, None)."""
    paths = fixup_targets(root, label)
    if len(paths) > 1:
        sys.exit("dtbo: %d fragments target %s" % (len(paths), label))
    if not paths:
        if required:
            sys.exit("dtbo: no fragment targets %s" % label)
        return None, None
    frag = root.find(paths[0])
    overlay = frag.child("__overlay__") if frag else None
    if overlay is None or frag.prop("target") != cells(0xFFFFFFFF):
        sys.exit("dtbo: %s is not an overlay fragment" % paths[0])
    return frag, overlay


def local_phandle(root, symbol):
    """The phandle of the entry's own node labelled SYMBOL (__symbols__)."""
    symbols = root.child("__symbols__")
    path = symbols.prop(symbol) if symbols else None
    node = root.find(path.rstrip(b"\0").decode()) if path else None
    value = node.prop("phandle") if node else None
    if value is None or len(value) != 4:
        sys.exit("dtbo: no node with a phandle for %s" % symbol)
    return value


def connector_of(overlay):
    found = [c for c in overlay.children
             if b"usb-c-connector" in (c.prop("compatible") or b"").split(b"\0")]
    if len(found) != 1:
        sys.exit("dtbo: %d usb-c-connector nodes under the PD controller's overlay" % len(found))
    return found[0]


def vbus_extcon_ref(frag):
    """The __fixups__ reference of the VBUS cell of FRAG's overlay's extcon."""
    return "/%s/__overlay__:extcon:0" % frag.name


def vbus_extcon_moved(root, frag, overlay):
    """True when FRAG's overlay (EXTCON_TARGET's) takes its VBUS extcon from EXTCON_TO (dock fix A
    made), False when from EXTCON_FROM (stock). Its extcon must be <VBUS id>, both from __fixups__, the
    id from EXTCON_TO; the VBUS reference must be in exactly one of the two, once."""
    if overlay.prop("extcon") != cells(0xFFFFFFFF, 0xFFFFFFFF):
        sys.exit("dtbo: the %s overlay's extcon is %s, expected <0xffffffff 0xffffffff>"
                 % (EXTCON_TARGET, show(overlay.prop("extcon"))))
    id_ref = "/%s/__overlay__:extcon:4" % frag.name
    if id_ref not in fixup_refs(root, EXTCON_TO):
        sys.exit("dtbo: __fixups__/%s has no %s (the %s overlay's id extcon)" % (EXTCON_TO, id_ref, EXTCON_TARGET))
    ref = vbus_extcon_ref(frag)
    where = [label for label in (EXTCON_FROM, EXTCON_TO) for r in fixup_refs(root, label) if r == ref]
    if where == [EXTCON_FROM]:
        return False
    if where == [EXTCON_TO]:
        return True
    sys.exit("dtbo: %s (the %s overlay's VBUS extcon) is in __fixups__ %s; expected once, in %s or %s"
             % (ref, EXTCON_TARGET, " and ".join(where) or "nowhere", EXTCON_FROM, EXTCON_TO))


def new_fragment_name(root):
    numbers = [int(c.name[len("fragment@"):]) for c in root.children
               if c.name.startswith("fragment@") and c.name[len("fragment@"):].isdigit()]
    return "fragment@%d" % (max(numbers, default=-1) + 1)


def insert_in_order(parent, node, order):
    """Inserts NODE among PARENT's children before the first one that comes after it in ORDER (a list
    of names), so the tree reads as dtc would have written it."""
    rank = {name: i for i, name in enumerate(order)}
    at = next((i for i, c in enumerate(parent.children) if rank.get(c.name, -1) > rank[node.name]),
              len(parent.children))
    parent.children.insert(at, node)


def patch_overlay(root, source_ma=DEFAULT_SOURCE_MA):
    """Applies the edits to the overlay tree ROOT in place, the source PDO offering SOURCE_MA. Returns
    the changes made, as (path, property, new value); an already patched tree gives none."""
    changes = []
    pd_frag, pd = overlay_of(root, PD_TARGET)
    connector = connector_of(pd)
    if connector.prop("power-role") != b"dual\0":
        sys.exit("dtbo: the USB-C connector is not dual-role")
    con_path = "/%s/__overlay__/%s" % (pd_frag.name, connector.name)
    for name, old, new in connector_edits(source_ma):
        value = connector.prop(name)
        if value == new:
            continue
        if value != old:
            sys.exit("dtbo: %s %s is %s, expected %s" % (con_path, name, show(value), show(old)))
        connector.set_prop(name, new)
        changes.append((con_path, name, new))

    vbus = local_phandle(root, VBUS_SYMBOL)
    # The Type-C controller has no fragment until the first patch adds one.
    overlays = {label: overlay_of(root, label, required=label != "pmic_typec") for label in C2C_TARGETS}
    for label in ("usb", "usb3"):
        # What makes these the controllers whose VBUS the boost feeds today.
        if overlays[label][1].prop("vbus-supply") != vbus:
            sys.exit("dtbo: the %s overlay's vbus-supply is not %s" % (label, VBUS_SYMBOL))
    local = root.find("/__local_fixups__/%s/__overlay__" % pd_frag.name)
    # The core set: (what, overlay node or None, property, new value).
    core = [(label, overlays[label][1], C2C, b"") for label in C2C_TARGETS]
    core.append((PD_TARGET, pd, "vbus-supply", vbus))
    core.append(("__local_fixups__ of " + PD_TARGET, local, "vbus-supply", cells(0)))
    done = []
    for what, node, name, new in core:
        value = node.prop(name) if node else None
        if value is not None and value != new:
            sys.exit("dtbo: %s %s is %s, expected %s" % (what, name, show(value), show(new)))
        if value is not None:
            done.append(what)
    # Dock fix A, part of the same set.
    extcon_frag, extcon_overlay = overlays[EXTCON_TARGET]
    if vbus_extcon_moved(root, extcon_frag, extcon_overlay):
        done.append("%s VBUS extcon on %s" % (EXTCON_TARGET, EXTCON_TO))
    if done and len(done) != len(core) + 1:
        sys.exit("dtbo: partial c2c core set (only %s); refusing" % ", ".join(done))
    if done:
        return changes
    if overlays["pmic_typec"][0] is not None:
        sys.exit("dtbo: a fragment already targets pmic_typec")

    typec_frag = Node(new_fragment_name(root), [["target", cells(0xFFFFFFFF), None]],
                      [Node("__overlay__", [[C2C, b"", None]])])
    # After the last fragment, before __symbols__ and the fixups.
    last = max((i for i, c in enumerate(root.children)
                if c.name == "fragment" or c.name.startswith("fragment@")), default=-1)
    root.children.insert(last + 1, typec_frag)
    changes.append(("/%s/__overlay__" % typec_frag.name, C2C, b""))
    # The new fragment's target, then dock fix A: usb3's VBUS extcon reference goes from extcon_gpio to
    # the end of pmic_typec (EXTCON_TO is pmic_typec too).
    fixups = root.child("__fixups__")
    vbus_ref = vbus_extcon_ref(extcon_frag)
    fixups.set_prop(EXTCON_FROM, refs_value([r for r in fixup_refs(root, EXTCON_FROM) if r != vbus_ref]))
    fixups.set_prop("pmic_typec", refs_value(fixup_refs(root, "pmic_typec") + ["/%s:target:0" % typec_frag.name]))
    fixups.set_prop(EXTCON_TO, refs_value(fixup_refs(root, EXTCON_TO) + [vbus_ref]))
    changes.append(("/__fixups__", EXTCON_FROM, fixups.prop(EXTCON_FROM)))
    changes.append(("/__fixups__", "pmic_typec", fixups.prop("pmic_typec")))
    for label in ("usb3", "usb"):
        frag, overlay = overlays[label]
        overlay.set_prop(C2C, b"")
        changes.append(("/%s/__overlay__" % frag.name, C2C, b""))
    pd.set_prop("vbus-supply", vbus)
    changes.append(("/%s/__overlay__" % pd_frag.name, "vbus-supply", vbus))
    # The phandle is the entry's own (vddvbus is defined in it): __local_fixups__ tells the loader to
    # renumber it with the entry's nodes. A __fixups__ entry would not resolve: the base DT has no
    # vddvbus.
    lf = root.child("__local_fixups__")
    if lf is None:
        lf = Node("__local_fixups__")
        root.children.append(lf)
    frag_fix = lf.child(pd_frag.name)
    if frag_fix is None:
        frag_fix = Node(pd_frag.name)
        insert_in_order(lf, frag_fix, [c.name for c in root.children])
    over_fix = frag_fix.child("__overlay__")
    if over_fix is None:
        over_fix = Node("__overlay__")
        frag_fix.children.insert(0, over_fix)
    over_fix.set_prop("vbus-supply", cells(0))
    changes.append(("/__local_fixups__/%s/__overlay__" % pd_frag.name, "vbus-supply", cells(0)))
    return changes


def patch_entry(blob, source_ma=DEFAULT_SOURCE_MA, jeita=None):
    """(BLOB patched, changes), with JEITA's tables when given; BLOB itself when it is already patched."""
    fdt = parse_fdt(blob, "dtbo entry")
    if build_fdt(fdt) != blob:
        sys.exit("dtbo entry: the FDT does not rebuild byte for byte")
    changes = patch_overlay(fdt.root, source_ma)
    if jeita is not None:
        changes += patch_jeita(fdt.root, jeita)
    return (build_fdt(fdt) if changes else blob), changes


def source_ma_of(root):
    """The current the overlay ROOT's connector offers when its source PDO is one the patch makes
    (any --source-ma), else DEFAULT_SOURCE_MA (and patch_overlay() says what it found)."""
    _, pd = overlay_of(root, PD_TARGET)
    value = connector_of(pd).prop("source-pdos")
    if value is not None and len(value) == 4:
        pdo, = struct.unpack(">I", value)
        ma = (pdo & 0x3FF) * 10
        if pdo == source_pdo(ma) and DEFAULT_SOURCE_MA <= ma <= OVER_RATING_MAX_MA:
            return ma
    return DEFAULT_SOURCE_MA


def pdo_limits(pdo):
    """(maximum voltage in mV, maximum current in mA or None) of a USB PD power data object."""
    kind = pdo >> 30
    if kind == 0:   # fixed supply
        return ((pdo >> 10) & 0x3FF) * 50, (pdo & 0x3FF) * 10
    if kind in (1, 2):   # battery, variable
        return ((pdo >> 20) & 0x3FF) * 50, None
    if (pdo >> 28) & 3 == 0:   # programmable power supply (PPS) APDO
        return ((pdo >> 17) & 0xFF) * 100, (pdo & 0x7F) * 50
    return None, None


def check_entry(blob, allow_over_rating=False):
    """Exits unless the overlay BLOB is fully patched and stays within the plan's limits (source PDOs
    up to OVER_RATING_MAX_MA with ALLOW_OVER_RATING, a test image's; jeita tables check_jeita()'s)."""
    fdt = parse_fdt(blob, "dtbo entry")
    left = patch_overlay(fdt.root, source_ma_of(fdt.root))
    if left:
        sys.exit("dtbo entry: not patched (%s %s)" % left[0][:2])
    check_jeita(fdt.root)
    max_source_ma = OVER_RATING_MAX_MA if allow_over_rating else MAX_SOURCE_MA
    for path, node in fdt.root.walk():
        for name in FORBIDDEN:
            if node.prop(name) is not None:
                sys.exit("dtbo entry: %s has %s" % (path, name))
        for name, max_mv, max_ma in (("sink-pdos", MAX_SINK_MV, None),
                                     ("source-pdos", MAX_SOURCE_MV, max_source_ma)):
            value = node.prop(name)
            if value is None:
                continue
            if not value or len(value) % 4:
                sys.exit("dtbo entry: %s %s is not a list of PDOs" % (path, name))
            for pdo in struct.unpack(">%dI" % (len(value) // 4), value):
                mv, ma = pdo_limits(pdo)
                if mv is None or mv > max_mv or (max_ma and (ma is None or ma > max_ma)):
                    sys.exit("dtbo entry: %s %s has 0x%08x, above %d mV%s" % (
                        path, name, pdo, max_mv, " or %d mA" % max_ma if max_ma else ""))
    return fdt


def device_entry(raw):
    """(device, entry index) of a dt_table image of a device in DEVICES, by its board entry's id."""
    _, entries = parse_table(raw)
    for device in DEVICES.values():
        if device.dtbo_idx < len(entries) and entries[device.dtbo_idx][2] == device.board_id:
            return device, device.dtbo_idx
    sys.exit("dtbo: no entry of a known board")


def vendor_boot_dtb(image):
    """The base device tree in a vendor_boot (v3/v4) image's dtb section, itself a dt_table of one
    entry on Unisoc devices."""
    if image[:8] != b"VNDRBOOT":
        sys.exit("vendor_boot: not a vendor boot image")
    version, page = struct.unpack_from("<II", image, 8)
    ramdisk_size, = struct.unpack_from("<I", image, 24)
    header_size, dtb_size = struct.unpack_from("<II", image, 2096)
    if version not in (3, 4) or not page:
        sys.exit("vendor_boot: header version %d" % version)
    off = -(-header_size // page) * page + -(-ramdisk_size // page) * page
    dtb = image[off:off + dtb_size]
    if dtb[:4] == struct.pack(">I", FDT_MAGIC):
        return dtb
    _, entries = parse_table(dtb, "vendor_boot dtb")
    if len(entries) != 1:
        sys.exit("vendor_boot: %d device trees" % len(entries))
    # The RG Cube's entry size is 16 bytes short of its FDT, which ends where the dtb section does:
    # the FDT header's size is the one to take.
    off = entries[0][1]
    if dtb[off:off + 4] != struct.pack(">I", FDT_MAGIC):
        sys.exit("vendor_boot: the dtb entry is not an FDT")
    size, = struct.unpack_from(">I", dtb, off + 4)
    if off + size > len(dtb):
        sys.exit("vendor_boot: the base FDT runs past the dtb section")
    return dtb[off:off + size]


def live_path(base, overlay, path):
    """The path in the merged tree of the overlay node at PATH ("/fragment@N/__overlay__/...")."""
    parts = path.strip("/").split("/")
    frag = overlay.child(parts[0])
    rest = "/".join(parts[2:])
    target = frag.prop("target-path")
    if target is not None:
        top = target.rstrip(b"\0").decode()
    else:
        labels = [label for label, _, _ in overlay.child("__fixups__").props
                  if "/%s" % frag.name in fixup_targets(overlay, label)]
        symbols = base.child("__symbols__")
        if len(labels) != 1 or symbols is None or symbols.prop(labels[0]) is None:
            sys.exit("merge check: no base node for %s" % frag.name)
        top = symbols.prop(labels[0]).rstrip(b"\0").decode()
    return (top.rstrip("/") + "/" + rest) if rest else top


def merge_check(blob, vendor_boot):
    """Applies the patched overlay BLOB to VENDOR_BOOT's base DT with fdtoverlay and checks every edit
    is at its node in the result. False when fdtoverlay is not on PATH."""
    tool = shutil.which("fdtoverlay")
    if not tool:
        return False
    with open(vendor_boot, "rb") as f:
        base_blob = vendor_boot_dtb(f.read())
    base = parse_fdt(base_blob, "base DT")
    overlay = parse_fdt(blob, "dtbo entry")
    expected = expected_edits(overlay.root)
    with tempfile.TemporaryDirectory() as tmp:
        paths = [os.path.join(tmp, n) for n in ("base.dtb", "overlay.dtb", "merged.dtb")]
        for p, data in zip(paths, (base_blob, blob)):
            with open(p, "wb") as f:
                f.write(data)
        proc = subprocess.run([tool, "-i", paths[0], "-o", paths[2], paths[1]], capture_output=True, text=True)
        if proc.returncode:
            sys.exit("merge check: fdtoverlay failed: %s" % proc.stderr.strip())
        with open(paths[2], "rb") as f:
            merged = parse_fdt(f.read(), "merged DT").root
    vbus_path = overlay.root.child("__symbols__").prop(VBUS_SYMBOL).rstrip(b"\0").decode()
    for path, name, new in expected:
        if not path.startswith("/fragment"):
            continue   # __fixups__ and __local_fixups__ are not in the merged tree
        where = live_path(base.root, overlay.root, path)
        node = merged.find(where)
        if name == "vbus-supply":
            supply = merged.find(live_path(base.root, overlay.root, vbus_path))
            new = supply.prop("phandle") if supply else None
        if name == "extcon":
            typec = label_phandle(base.root, merged, EXTCON_TO)
            new = typec + typec
        if node is None or new is None or node.prop(name) != new:
            sys.exit("merge check: %s %s is %s" % (where, name, show(node.prop(name) if node else None)))
        print("merge check: %s %s = %s" % (where, name, show(new) if new else "(true)"))
    # Nothing else rewired: every __fixups__ reference resolves to its own label's node.
    count = 0
    for label, _, _ in overlay.root.child("__fixups__").props:
        phandle = label_phandle(base.root, merged, label)
        for ref in fixup_refs(overlay.root, label):
            path, name, off = ref.rsplit(":", 2)
            if name == "target":
                continue   # a fragment's target is not in the merged tree
            where = live_path(base.root, overlay.root, path)
            node = merged.find(where)
            value = node.prop(name) if node else None
            if value is None or value[int(off):int(off) + 4] != phandle:
                sys.exit("merge check: %s %s is %s, cell at byte %s is not %s's phandle %s"
                         % (where, name, show(value), off, label, show(phandle)))
            count += 1
    print("merge check: %d phandle references resolve to their labels' nodes" % count)
    return True


def label_phandle(base, merged, label):
    """The phandle, in the MERGED tree, of the base DT's node labelled LABEL."""
    symbols = base.child("__symbols__")
    path = symbols.prop(label) if symbols else None
    node = merged.find(path.rstrip(b"\0").decode()) if path else None
    value = node.prop("phandle") if node else None
    if value is None or len(value) != 4:
        sys.exit("merge check: no base node with a phandle for %s" % label)
    return value


def expected_edits(root):
    """The edits a patched overlay ROOT carries: the connector's, and the core set's at its nodes."""
    edits = []
    pd_frag, pd = overlay_of(root, PD_TARGET)
    con = connector_of(pd)
    for name, _, new in connector_edits(source_ma_of(root)):
        edits.append(("/%s/__overlay__/%s" % (pd_frag.name, con.name), name, new))
    for label in C2C_TARGETS:
        frag, _ = overlay_of(root, label)
        edits.append(("/%s/__overlay__" % frag.name, C2C, b""))
    edits.append(("/%s/__overlay__" % pd_frag.name, "vbus-supply", local_phandle(root, VBUS_SYMBOL)))
    # Dock fix A: both cells resolve to EXTCON_TO's node; the merge check knows its phandle.
    frag, _ = overlay_of(root, EXTCON_TARGET)
    edits.append(("/%s/__overlay__" % frag.name, "extcon", None))
    # The jeita tables --jeita edits, whichever variant they are.
    for path, node in jeita_nodes(root):
        for kind in JEITA_TYPES:
            if node.prop(kind + JEITA_SUFFIX) is not None:
                edits.append((path, kind + JEITA_SUFFIX, node.prop(kind + JEITA_SUFFIX)))
    return edits


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def cmd_patch(src, dst, tested, source_ma=None, allow_over_rating=False, jeita=None):
    """Patches SRC into DST. SOURCE_MA None: DEFAULT_SOURCE_MA, or what an already patched SRC offers.
    TESTED: the device's approved image, with its approved jeita tables (JEITA None or those)."""
    if tested and (source_ma not in (None, DEFAULT_SOURCE_MA) or allow_over_rating):
        sys.exit("dtbo: --tested is the approved %d mA image only; a --source-ma or --allow-over-rating "
                 "image is a test image" % DEFAULT_SOURCE_MA)
    if jeita is not None and jeita not in JEITA_EDITS:
        sys.exit("dtbo: --jeita %s is not one of %s" % (jeita, ", ".join(JEITA_EDITS)))
    if source_ma is not None:
        source_limit(source_ma, allow_over_rating)
    with open(src, "rb") as f:
        image = f.read()
    stock_sha, made = PATCHED_INPUTS.get(sha256(image), (sha256(image), None))
    device = DEVICES.get(stock_sha)
    if device is None:
        sys.exit("%s is not a dtbo this patch knows (sha256 %s)" % (src, sha256(image)))
    if tested:
        if jeita not in (None, device.tested_jeita):
            sys.exit("dtbo: --tested takes the %s's approved jeita tables (%s) only; a --jeita %s image is "
                     "a test image" % (device.name, device.tested_jeita or "stock", jeita))
        jeita = device.tested_jeita
    footer = footer_info(src, "dtbo")
    if (footer.partition_name, footer.image_size) != (device.partition, device.partition_size) \
            or len(image) != device.partition_size:
        sys.exit("dtbo: the footer is for %s, %d bytes" % (footer.partition_name, footer.image_size))
    raw = image[:footer.original_size]
    _, entries = parse_table(raw)
    if entries[device.dtbo_idx][2] != device.board_id:
        sys.exit("dtbo: entry %d is not the %s's board" % (device.dtbo_idx, device.name))
    old = entry_blob(raw, entries, device.dtbo_idx)
    if made is not None:
        # An image this patch made: its offer stays (a different one is the stock image's patch).
        own_ma = source_ma_of(parse_fdt(old, "dtbo entry").root)
        if source_ma not in (None, own_ma):
            sys.exit("dtbo: %s is %s, which offers %d mA; for --source-ma %d patch the stock image"
                     % (src, made, own_ma, source_ma))
        source_ma = own_ma
        source_limit(source_ma, allow_over_rating)
        print("%s is %s, patched from the %s's stock dtbo" % (src, made, device.name))
    elif source_ma is None:
        source_ma = DEFAULT_SOURCE_MA
    blob, changes = patch_entry(old, source_ma, jeita)
    new_raw = replace_entry(raw, device.dtbo_idx, blob)
    check_entry(blob, allow_over_rating)
    with tempfile.TemporaryDirectory() as tmp:
        # Re-signing the original must give it back, or the result would be signed differently too.
        again = os.path.join(tmp, "original.img")
        with open(again, "wb") as f:
            f.write(raw)
        add_footer(again, footer, device.partition, device.partition_size)
        with open(again, "rb") as f:
            if f.read() != image:
                sys.exit("dtbo: re-signing the original does not give it back (avbtool or AVB_KEY differ)")
        out = os.path.join(tmp, "dtbo.img")
        with open(out, "wb") as f:
            f.write(new_raw)
        add_footer(out, footer, device.partition, device.partition_size)
        with open(out, "rb") as f:
            result = f.read()
        if len(result) != device.partition_size or result[:len(new_raw)] != new_raw:
            sys.exit("dtbo: the signed image is not the patched one")
        if tested and sha256(result) != device.tested_sha256:
            sys.exit("dtbo: the result (sha256 %s) is not the image approved for the %s (%s)"
                     % (sha256(result), device.name, device.tested_sha256 or "none yet"))
        shutil.copyfile(out, dst)
    for path, name, value in changes:
        print("%s %s = %s" % (path, name, show(value) if value else "(true)"))
    print("patched %s entry %d -> %s (sha256 %s)" % (device.name, device.dtbo_idx, dst, sha256(result)))


def cmd_check(image, vendor_boot=None, allow_over_rating=False):
    with open(image, "rb") as f:
        raw = f.read()
    device, idx = device_entry(raw)
    _, entries = parse_table(raw)
    blob = entry_blob(raw, entries, idx)
    fdt = check_entry(blob, allow_over_rating)
    source_ma = source_ma_of(fdt.root)
    print("%s: %s entry %d is patched; no PDO above its limit, no fast-charging switch; source PDO %d mA%s"
          % (image, device.name, idx, source_ma,
             " (above the boost's rating: a test image)" if source_ma > MAX_SOURCE_MA else ""))
    variant = check_jeita(fdt.root)
    approved = device.tested_jeita or "stock"
    print("%s: jeita tables %s" % (image, "none" if variant is None else variant + (
        " (the approved tables)" if variant == approved else " (a test image; %s is approved)" % approved)))
    for line in jeita_report(fdt.root):
        print(line)
    if sha256(raw) == device.tested_sha256:
        print("%s: the image approved for the %s (sha256 %s)" % (image, device.name, device.tested_sha256))
    if vendor_boot and not merge_check(blob, vendor_boot):
        print("merge check skipped: no fdtoverlay on PATH")


def main():
    args = sys.argv[1:]
    flags = {"--tested", "--allow-over-rating"}
    tested, allow = ("--tested" in args), ("--allow-over-rating" in args)
    source_ma = jeita = None
    if "--source-ma" in args:
        at = args.index("--source-ma")
        if at + 1 >= len(args) or not args[at + 1].isdigit():
            sys.exit(__doc__)
        source_ma = int(args[at + 1])
        args = args[:at] + args[at + 2:]
    if "--jeita" in args:
        at = args.index("--jeita")
        if at + 1 >= len(args) or args[at + 1] not in JEITA_EDITS:
            sys.exit(__doc__)
        jeita = args[at + 1]
        args = args[:at] + args[at + 2:]
    args = [a for a in args if a not in flags]
    if len(args) == 3 and args[0] == "patch":
        cmd_patch(args[1], args[2], tested, source_ma, allow, jeita)
    elif len(args) in (2, 3) and args[0] == "check" and not tested and source_ma is None and jeita is None:
        cmd_check(args[1], args[2] if len(args) == 3 else None, allow)
    elif args == ["tested"] and not tested and not allow and source_ma is None and jeita is None:
        untested = sorted(d.name for d in DEVICES.values() if not d.tested_sha256)
        if untested:
            sys.exit("no patched dtbo has been approved for the %s yet (DOCK_FIX=0 keeps the stock one)"
                     % ", ".join(untested))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
