#!/usr/bin/env python3
"""Patches the RG Cube's dtbo so a powered USB-C dock gives power in, picture and USB at the same time
(usb_c_dock_plan.md, section 4a).

  patch_dtbo.py patch [--tested] IN OUT
  patch_dtbo.py check IMAGE [VENDOR_BOOT]
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
    part of it lets the boost back-drive a dock or leaves VBUS unpowered, so a partial one is refused.
Nodes are found by what they are, through __fixups__ and __symbols__, never by fragment number. Each
property is checked against the value it is expected to have first: a different value is refused, the
new one is left as it is.

`patch` checks IN's sha256, re-signs the unpatched image and requires it byte-identical to IN (so the
same avbtool and key sign the result), then patches and signs it with IN's salt, properties and AVB
options (avb_footer.py). With --tested the result must also be the image approved for the device
(the device's tested_sha256), which build.sh requires.

`check` requires IMAGE (signed or not) to be patched, with no sink PDO above 5 V, no source PDO above
5 V or 1 A, and none of the fast-charging switches (FORBIDDEN). With VENDOR_BOOT, and fdtoverlay on
PATH, it also applies the entry to the base device tree in vendor_boot, as the bootloader does, and
checks the result has every edit at the nodes the plan names; without fdtoverlay it says so and skips
that part.

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
# tested_sha256: the patched image approved for the device, empty until then (build.sh needs it).
Device = collections.namedtuple("Device", "name dtbo_idx board_id partition partition_size tested_sha256")
DEVICES = {
    # The RG Cube's stock dtbo: the GammaOS Next 1.2 PAC's DTBO and DTBO_B, and a stock Cube's dtbo_a.
    "ca5a55ff02e439a4e0ae069874c80d583dd2cdb967e69f64eb2036e273faafcc":
        # tested_sha256: `patch` of this image with AOSP's testkey_rsa4096.pem. The owner approved it
        # for test 40 WITHOUT a device test (no phase-1 hand test); a bad one may need a Full install.
        Device("RG Cube", 2, 0x2100, "dtbo", 8388608,
               "94c0f3d4bb1fbbf518a0461db39908d02fd67e320e52c45ced540076d09c5536"),
}

# The connector (property, original value, new value); None: no such property.
CONNECTOR_EDITS = [
    ("data-role", None, b"dual\0"),
    # 5 V 3 A fixed + PPS APDO 3.3-11 V 3 A -> 5 V 3 A fixed with DRP|DRD.
    ("sink-pdos", struct.pack(">2I", 0x0401912C, 0xC0DC213C), struct.pack(">I", 0x2601912C)),
    # 5 V 400 mA fixed -> 5 V 900 mA fixed, same flags.
    ("source-pdos", struct.pack(">I", 0x04019028), struct.pack(">I", 0x0401905A)),
]
C2C = "use_pdhub_c2c"
# The overlays of these labels' nodes get C2C (pmic_typec has no fragment of its own in the stock entry).
C2C_TARGETS = ["pmic_typec", "usb3", "usb"]
PD_TARGET = "pmic_pd"
VBUS_SYMBOL = "vddvbus"
FORBIDDEN = ["sprd,support-fchg", "sprd,support-pd-pps", "sprd,support-sfcp"]
MAX_SINK_MV = 5000
MAX_SOURCE_MV, MAX_SOURCE_MA = 5000, 1000


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


def fixup_targets(root, label):
    """The fragments whose target is LABEL (__fixups__ entries "/fragment@N:target:0")."""
    fixups = root.child("__fixups__")
    value = fixups.prop(label) if fixups else None
    found = []
    for ref in (value or b"").split(b"\0"):
        path, _, rest = ref.decode().rpartition(":")
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


def patch_overlay(root):
    """Applies the edits to the overlay tree ROOT in place. Returns the changes made, as (path,
    property, new value); an already patched tree gives none."""
    changes = []
    pd_frag, pd = overlay_of(root, PD_TARGET)
    connector = connector_of(pd)
    if connector.prop("power-role") != b"dual\0":
        sys.exit("dtbo: the USB-C connector is not dual-role")
    con_path = "/%s/__overlay__/%s" % (pd_frag.name, connector.name)
    for name, old, new in CONNECTOR_EDITS:
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
    if done and len(done) != len(core):
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
    fixups = root.child("__fixups__")
    refs = fixups.prop("pmic_typec") or b""
    fixups.set_prop("pmic_typec", refs + ("/%s:target:0" % typec_frag.name).encode() + b"\0")
    changes.append(("/%s/__overlay__" % typec_frag.name, C2C, b""))
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


def patch_entry(blob):
    """(BLOB patched, changes); BLOB itself when it is already patched."""
    fdt = parse_fdt(blob, "dtbo entry")
    if build_fdt(fdt) != blob:
        sys.exit("dtbo entry: the FDT does not rebuild byte for byte")
    changes = patch_overlay(fdt.root)
    return (build_fdt(fdt) if changes else blob), changes


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


def check_entry(blob):
    """Exits unless the overlay BLOB is fully patched and stays within the plan's limits."""
    fdt = parse_fdt(blob, "dtbo entry")
    left = patch_overlay(fdt.root)
    if left:
        sys.exit("dtbo entry: not patched (%s %s)" % left[0][:2])
    for path, node in fdt.root.walk():
        for name in FORBIDDEN:
            if node.prop(name) is not None:
                sys.exit("dtbo entry: %s has %s" % (path, name))
        for name, max_mv, max_ma in (("sink-pdos", MAX_SINK_MV, None),
                                     ("source-pdos", MAX_SOURCE_MV, MAX_SOURCE_MA)):
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
        if node is None or new is None or node.prop(name) != new:
            sys.exit("merge check: %s %s is %s" % (where, name, show(node.prop(name) if node else None)))
        print("merge check: %s %s = %s" % (where, name, show(new) if new else "(true)"))
    return True


def expected_edits(root):
    """The edits a patched overlay ROOT carries: the connector's, and the core set's at its nodes."""
    edits = []
    pd_frag, pd = overlay_of(root, PD_TARGET)
    con = connector_of(pd)
    for name, _, new in CONNECTOR_EDITS:
        edits.append(("/%s/__overlay__/%s" % (pd_frag.name, con.name), name, new))
    for label in C2C_TARGETS:
        frag, _ = overlay_of(root, label)
        edits.append(("/%s/__overlay__" % frag.name, C2C, b""))
    edits.append(("/%s/__overlay__" % pd_frag.name, "vbus-supply", local_phandle(root, VBUS_SYMBOL)))
    return edits


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def cmd_patch(src, dst, tested):
    with open(src, "rb") as f:
        image = f.read()
    device = DEVICES.get(sha256(image))
    if device is None:
        sys.exit("%s is not a dtbo this patch knows (sha256 %s)" % (src, sha256(image)))
    footer = footer_info(src, "dtbo")
    if (footer.partition_name, footer.image_size) != (device.partition, device.partition_size) \
            or len(image) != device.partition_size:
        sys.exit("dtbo: the footer is for %s, %d bytes" % (footer.partition_name, footer.image_size))
    raw = image[:footer.original_size]
    _, entries = parse_table(raw)
    if entries[device.dtbo_idx][2] != device.board_id:
        sys.exit("dtbo: entry %d is not the %s's board" % (device.dtbo_idx, device.name))
    blob, changes = patch_entry(entry_blob(raw, entries, device.dtbo_idx))
    new_raw = replace_entry(raw, device.dtbo_idx, blob)
    check_entry(blob)
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


def cmd_check(image, vendor_boot=None):
    with open(image, "rb") as f:
        raw = f.read()
    device, idx = device_entry(raw)
    _, entries = parse_table(raw)
    blob = entry_blob(raw, entries, idx)
    check_entry(blob)
    print("%s: %s entry %d is patched; no PDO above its limit, no fast-charging switch"
          % (image, device.name, idx))
    if vendor_boot and not merge_check(blob, vendor_boot):
        print("merge check skipped: no fdtoverlay on PATH")


def main():
    args = sys.argv[1:]
    tested = "--tested" in args
    args = [a for a in args if a != "--tested"]
    if len(args) == 3 and args[0] == "patch":
        cmd_patch(args[1], args[2], tested)
    elif len(args) in (2, 3) and args[0] == "check" and not tested:
        cmd_check(*args[1:])
    elif args == ["tested"] and not tested:
        untested = sorted(d.name for d in DEVICES.values() if not d.tested_sha256)
        if untested:
            sys.exit("no patched dtbo has been approved for the %s yet (DOCK_FIX=0 keeps the stock one)"
                     % ", ".join(untested))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
