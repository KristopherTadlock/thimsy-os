"""Builds small FDT blobs, dt_table images and vendor_boot images laid out like the RG Cube's (see
scripts/patch_dtbo.py), independently of the code under test.

A tree is (name, [(property, value)], [subtrees]); values are bytes (see u32 and strs).
"""
import struct

STOCK_SINK = (0x0401912C, 0xC0DC213C)
STOCK_SOURCE = (0x04019028,)
PATCHED_SINK = (0x2601912C,)
PATCHED_SOURCE = (0x0401905A,)
VBUS_PHANDLE = 0x2A   # not the RG Cube's 0x07: the patch must take it from __symbols__
MISSING = 0xFFFFFFFF
# The battery node's jeita tables as on the RG Cube (entry 2), in its order: rows of <temperature
# recovery current-uA cv-uV>, temperatures in 0.1 C above -100.0 C (1000 is 0 C).
JEITA_KINDS = ("dcp", "sdp", "cdp", "unknown", "fchg", "flash")
JEITA_STOCK_UA = {"dcp": (2000000, 700000), "sdp": (500000, 500000), "cdp": (1500000, 700000),
                  "unknown": (1050000, 700000), "fchg": (3250000, 700000), "flash": (5000000, 1000000)}


def u32(*words):
    return struct.pack(">%dI" % len(words), *words)


def strs(*texts):
    return b"".join(t.encode() + b"\0" for t in texts)


def pad4(data):
    return data + b"\0" * (-len(data) % 4)


def fdt(tree, rsvmap=()):
    """The FDT blob of TREE, as dtc lays it out (strings in first-use order, no shared suffixes)."""
    strings, offsets, body = bytearray(), {}, bytearray()

    def off(name):
        if name not in offsets:
            offsets[name] = len(strings)
            strings.extend(name.encode() + b"\0")
        return offsets[name]

    def node(t):
        name, props, kids = t
        body.extend(struct.pack(">I", 1) + pad4(name.encode() + b"\0"))
        for prop, value in props:
            body.extend(struct.pack(">III", 3, len(value), off(prop)) + pad4(value))
        for k in kids:
            node(k)
        body.extend(struct.pack(">I", 2))

    node(tree)
    body.extend(struct.pack(">I", 9))
    rsv = b"".join(struct.pack(">QQ", a, s) for a, s in rsvmap) + bytes(16)
    off_struct = 40 + len(rsv)
    off_strings = off_struct + len(body)
    total = off_strings + len(strings)
    header = struct.pack(">10I", 0xD00DFEED, total, off_struct, off_strings, 40, 17, 16, 0,
                         len(strings), len(body))
    return header + rsv + bytes(body) + bytes(strings)


def jeita(*rows):
    return u32(*[w for row in rows for w in row])


def stock_jeita(kind):
    full, low = JEITA_STOCK_UA[kind]
    return jeita((1000, 1030, 0, 4340000), (1150, 1180, low, 4340000), (1520, 1490, full, 4340000),
                 (1600, 1570, low, 4100000))


# The thermal2 tables' 15-45 C, 45-49 C and 49-52 C currents, as the owner gave them (each step no more
# than the table's full rate). The RG Cube's approved dtbo has them (test 42 TH2, `patch --tested`).
THERMAL2_UA = {"dcp": (2000000, 1500000, 1000000), "unknown": (1050000, 1050000, 1000000),
               "cdp": (1500000, 1500000, 1000000)}


def thermal2_jeita(kind):
    """The thermal2 table of KIND (dcp, unknown or cdp), written out from the owner's design."""
    full, warm, hot = THERMAL2_UA[kind]
    return jeita((1000, 1030, 0, 4340000), (1150, 1180, 700000, 4340000), (1450, 1420, full, 4340000),
                 (1490, 1460, warm, 4340000), (1520, 1490, hot, 4340000), (1600, 1570, 700000, 4100000))


def jeita_props(**tables):
    """The battery node's jeita table properties: stock, but for TABLES (kind=value, None: absent)."""
    props = []
    for kind in JEITA_KINDS:
        value = tables.get(kind, stock_jeita(kind))
        if value is not None:
            props.append(("%s-jeita-temp-table" % kind, value))
    return props


def frag(n, label_props, kids=()):
    """fragment@N with target <MISSING> and an __overlay__ of LABEL_PROPS and KIDS."""
    return ("fragment@%d" % n, [("target", u32(MISSING))], [("__overlay__", list(label_props), list(kids))])


def overlay(sink=STOCK_SINK, source=STOCK_SOURCE, data_role=None, usb_c2c=False, usb3_c2c=False,
            typec=False, pd_vbus=None, pd_local=False, numbers=(1, 2, 3, 4, 5), extra_pd=(), jeita=None):
    """A board overlay like the RG Cube's dtbo entry 2, cut down: the battery with its jeita tables
    (JEITA: jeita_props()'s list, stock by default), the OTG boost (vddvbus), USB2 (usb), USB3 (usb3),
    the fast charger (pmic_fchg) and the PD controller (pmic_pd) with the connector.
    NUMBERS are their fragment numbers; the stock values unless an argument says otherwise."""
    chg, usb, usb3, fchg, pd = numbers
    connector = [("compatible", strs("usb-c-connector")), ("label", strs("USB-C")),
                 ("power-role", strs("dual")), ("try-power-role", strs("sink")),
                 ("source-pdos", u32(*source)), ("sink-pdos", u32(*sink)),
                 ("op-sink-microwatt", u32(10000000)), ("phandle", u32(0x23))]
    if data_role is not None:
        connector.append(("data-role", data_role))   # where the patch adds it
    c2c = [("use_pdhub_c2c", b"")]
    pd_props = [("status", strs("okay"))] + list(extra_pd)
    if pd_vbus is not None:
        pd_props.append(("vbus-supply", u32(pd_vbus)))
    frags = {
        chg: frag(chg, [("status", strs("okay"))], [
            ("charger@6a", [("compatible", strs("aw32257")), ("phandle", u32(0x21))], [
                ("otg-vbus", [("regulator-name", strs("vddvbus")), ("phandle", u32(VBUS_PHANDLE))], [])])]),
        usb: frag(usb, [("extcon", u32(MISSING, MISSING)), ("vbus-supply", u32(VBUS_PHANDLE)),
                        ("status", strs("okay"))] + (c2c if usb_c2c else [])),
        usb3: frag(usb3, [("extcon", u32(MISSING, MISSING)), ("vbus-supply", u32(VBUS_PHANDLE)),
                          ("status", strs("okay"))] + (c2c if usb3_c2c else [])),
        fchg: frag(fchg, [("status", strs("okay")), ("sprd,input-voltage-microvolt", u32(5000000))]),
        pd: frag(pd, pd_props, [("connector", connector, [])]),
    }
    kids = [("fragment", [("target-path", strs("/"))], [
        ("__overlay__", [], [("battery", [("compatible", strs("simple-battery"))]
                              + (jeita_props() if jeita is None else list(jeita)) + [("phandle", u32(0x03))],
                              [])])])]
    # A fragment after the PD controller's with a local fixup of its own (the battery's phandle).
    ion = max(numbers) + 1
    frags[ion] = frag(ion, [("memory-region", u32(0x03))])
    kids += [frags[n] for n in sorted(frags)]
    typec_n = ion + 1
    if typec:
        kids.append(frag(typec_n, c2c))
    kids.append(("__symbols__", [("bat", strs("/fragment/__overlay__/battery")),
                                 ("vddvbus", strs("/fragment@%d/__overlay__/charger@6a/otg-vbus" % chg)),
                                 ("usb_con", strs("/fragment@%d/__overlay__/connector" % pd))], []))
    typec_refs = ["/fragment@%d/__overlay__:extcon:4" % usb, "/fragment@%d/__overlay__:extcon:4" % usb3]
    if typec:
        typec_refs.append("/fragment@%d:target:0" % typec_n)
    kids.append(("__fixups__", [
        ("i2c6", strs("/fragment@%d:target:0" % chg)),
        ("extcon_gpio", strs("/fragment@%d/__overlay__:extcon:0" % usb, "/fragment@%d/__overlay__:extcon:0" % usb3)),
        ("usb", strs("/fragment@%d:target:0" % usb)),
        ("usb3", strs("/fragment@%d:target:0" % usb3)),
        ("pmic_typec", strs(*typec_refs)),
        ("pmic_fchg", strs("/fragment@%d:target:0" % fchg)),
        ("pmic_pd", strs("/fragment@%d:target:0" % pd)),
        ("ion", strs("/fragment@%d:target:0" % ion)),
    ], []))
    def fixup(n, prop):
        return ("fragment@%d" % n, [], [("__overlay__", [(prop, u32(0))], [])])

    local = [fixup(usb, "vbus-supply"), fixup(usb3, "vbus-supply"), fixup(ion, "memory-region")]
    if pd_local:
        local.append(fixup(pd, "vbus-supply"))
    local.sort(key=lambda t: int(t[0][len("fragment@"):]))
    kids.append(("__local_fixups__", [], local))
    return ("", [], kids)


def patched_overlay(**kw):
    """overlay() as the patch leaves it."""
    return overlay(sink=PATCHED_SINK, source=PATCHED_SOURCE, data_role=strs("dual"), usb_c2c=True,
                   usb3_c2c=True, typec=True, pd_vbus=VBUS_PHANDLE, pd_local=True, **kw)


def other_entry(text):
    return fdt(("", [("compatible", strs(text))], []))


def dt_table(blobs, ids, short=48, tail=b""):
    """A dt_table image of BLOBS (back to back after the entry table), whose header's total size is
    SHORT bytes less than where the entries end, as the RG Cube's is."""
    pos = 32 + 32 * len(blobs)
    entries = b""
    for blob, board in zip(blobs, ids):
        entries += struct.pack(">8I", len(blob), pos, board, 0, 0, 0, 0, 0)
        pos += len(blob)
    header = struct.pack(">8I", 0xD7B7AB1E, pos - short, 32, 32, len(blobs), 32, 2048, 0)
    return header + entries + b"".join(blobs) + tail


def cube_dtbo(entry):
    """A dtbo of three boards with ENTRY as the RG Cube's (index 2, id 0x2100), as patch_dtbo.DEVICES has it."""
    return dt_table([other_entry("board-a"), other_entry("board-b"), entry], [0x2700, 0x1100, 0x2100])


def base_dt():
    """A base device tree with the nodes the overlay's fixups name, and their symbols."""
    node = lambda name, ph, kids=(): (name, [("phandle", u32(ph))], list(kids))
    tree = ("", [("compatible", strs("sprd,ums9620"))], [
        ("soc", [], [
            ("aon", [], [
                node("usb@a00000", 1),
                ("spi@400000", [], [("pmic@0", [], [node("typec@380", 2), node("pd@e00", 3), node("fchg@1", 4)])]),
                node("extcon-gpio", 7)]),
            ("ipa-apb", [], [node("usb3@25100000", 5)]),
            node("i2c@6", 6),
            node("ion", 8)]),
        ("__symbols__", [
            ("usb", strs("/soc/aon/usb@a00000")),
            ("pmic_typec", strs("/soc/aon/spi@400000/pmic@0/typec@380")),
            ("pmic_pd", strs("/soc/aon/spi@400000/pmic@0/pd@e00")),
            ("pmic_fchg", strs("/soc/aon/spi@400000/pmic@0/fchg@1")),
            ("extcon_gpio", strs("/soc/aon/extcon-gpio")),
            ("usb3", strs("/soc/ipa-apb/usb3@25100000")),
            ("i2c6", strs("/soc/i2c@6")),
            ("ion", strs("/soc/ion")),
        ], []),
    ])
    return fdt(tree)


def vendor_boot(dtb, page=4096, ramdisk=b"R" * 5000):
    """A vendor_boot v4 image whose dtb section is the dt_table of DTB alone, with its entry size 16
    bytes short of the FDT, as the RG Cube's is."""
    table = bytearray(dt_table([dtb], [0]))
    struct.pack_into(">I", table, 32, len(dtb) - 16)
    header = bytearray(2128)
    header[:8] = b"VNDRBOOT"
    struct.pack_into("<IIIII", header, 8, 4, page, 0, 0, len(ramdisk))
    struct.pack_into("<II", header, 2096, 2128, len(table))
    align = lambda b: bytes(b) + b"\0" * (-len(b) % page)
    return align(header) + align(ramdisk) + align(table)
