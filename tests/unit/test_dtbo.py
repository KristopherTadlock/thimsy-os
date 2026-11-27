"""scripts/patch_dtbo.py and scripts/avb_footer.py, on small device trees built here
(tests/fixtures/dtbo.py), and build.sh's dtbo step."""
import contextlib
import hashlib
import io
import json
import os
import re
import struct
import sys
from unittest import mock

from tests import util
from tests.fixtures import dtbo as fx

pd = util.load_script("patch_dtbo")
avb = util.load_script("avb_footer")

DTBO_INFO = """Footer version:           1.0
Image size:               8388608 bytes
Original image size:      538536 bytes
VBMeta offset:            540672
VBMeta size:              2240 bytes
--
Minimum libavb version:   1.0
Header Block:             256 bytes
Authentication Block:     576 bytes
Auxiliary Block:          1408 bytes
Public key (sha1):        2597c218aae470a130f61162feaae70afd97f011
Algorithm:                SHA256_RSA4096
Rollback Index:           0
Flags:                    0
Rollback Index Location:  0
Release String:           'avbtool 1.2.0'
Descriptors:
    Hash descriptor:
      Image Size:            538536 bytes
      Hash Algorithm:        sha256
      Partition Name:        dtbo
      Salt:                  d1d4d01e4112d9ad79be8c171af4fb11589cd20997a5c527d11c5142851909e3
      Digest:                9f008b673f11a834b5e46411ded53ce916564d8838b604d6d5ff1a64a9e5494d
      Flags:                 0
    Prop: com.android.build.dtbo.fingerprint -> 'UNISOC/ums9620_2h10_native/ums9620_2h10:13/TP1A:user/release-keys'
"""

# A stand-in for avbtool: add_hash_footer pads the image to the partition size and ends it with its
# parameters (and the key file's text) as JSON; info_image prints them the way avbtool does.
# FAKE_AVB_RELEASE stands for the avbtool release, which the real one writes into the footer too.
FAKE_AVBTOOL = r"""#!/usr/bin/env python3
import json, os, sys
FOOT = 4096
args = sys.argv[1:]
with open(os.environ.get("SHIM_LOG", os.devnull), "a") as log:
    log.write("avbtool " + " ".join(args) + "\n")
opts, props = {}, []
for k, v in zip(args[1::2], args[2::2]):
    if k == "--prop":
        props.append(v.split(":", 1))
    else:
        opts[k[2:]] = v
with open(opts["image"], "rb") as f:
    data = f.read()
if args[0] == "add_hash_footer":
    with open(opts["key"]) as f:
        key = f.read()
    meta = dict(original=len(data), partition=opts["partition_name"], salt=opts["salt"], props=props, key=key,
                release=os.environ.get("FAKE_AVB_RELEASE", "1"),
                **{o: opts[o] for o in ("algorithm", "rollback_index", "rollback_index_location", "flags")})
    size = int(opts["partition_size"])
    with open(opts["image"], "wb") as f:
        f.write(data.ljust(size - FOOT, b"\0") + json.dumps(meta).encode().ljust(FOOT, b"\0"))
elif args[0] == "info_image":
    m = json.loads(data[-FOOT:].rstrip(b"\0"))
    print("Image size:               %d bytes" % len(data))
    print("Original image size:      %d bytes" % m["original"])
    print("Algorithm:                %s" % m["algorithm"])
    print("Rollback Index:           %s" % m["rollback_index"])
    print("Flags:                    %s" % m["flags"])
    print("Rollback Index Location:  %s" % m["rollback_index_location"])
    print("Descriptors:")
    print("    Hash descriptor:")
    print("      Partition Name:        %s" % m["partition"])
    print("      Salt:                  %s" % m["salt"])
    print("      Flags:                 0")
    for k, v in m["props"]:
        print("    Prop: %s -> '%s'" % (k, v))
"""
SALT = "d1d4d01e4112d9ad79be8c171af4fb11589cd20997a5c527d11c5142851909e3"
FINGERPRINT = "com.android.build.dtbo.fingerprint:UNISOC/ums9620_2h10_native/ums9620_2h10:13/TP1A:user/release-keys"
PARTITION = 1 << 20   # avbtool wants room for its own footer
# Test 42's RG Cube images: TH2 (shipped: fix A, 900 mA source PDO, thermal2 jeita tables) and DK1
# (the same with the stock tables).
TH2_SHA256 = "c87ece8c1133c9cf5ce39540333d3e186f73bc3a4d3579d25af878d2164e1e42"
DK1_SHA256 = "1b727faa07a2c0926b3b50c3d574284ee11be99550ff77c8014da35872baa72f"


def norm(t):
    """A fixture tree with tuples throughout."""
    return (t[0], [tuple(p) for p in t[1]], [norm(k) for k in t[2]])


def tree(node):
    """A parsed node as a fixture tree."""
    return (node.name, [(p[0], p[1]) for p in node.props], [tree(c) for c in node.children])


def parsed(blob):
    return pd.parse_fdt(blob).root


def entry(image, idx=2):
    _, entries = pd.parse_table(image)
    return pd.entry_blob(image, entries, idx)


def fixups(t):
    """The __fixups__ properties of fixture tree T (normed), to change in place."""
    return [k for k in t[2] if k[0] == "__fixups__"][0][1]


def refs(t, label):
    return [r for r in dict(fixups(t))[label].decode().split("\0") if r]


def set_refs(t, label, new):
    fixups(t)[:] = [(n, fx.strs(*new) if n == label else v) for n, v in fixups(t)]


def vbus_ref(t):
    """usb3's VBUS extcon reference ("/fragment@N/__overlay__:extcon:0") in fixture tree T."""
    return refs(t, "usb3")[0].replace(":target:0", "/__overlay__:extcon:0")


def fix_a(t):
    """Fixture tree T with dock fix A: usb3's VBUS extcon reference moved from __fixups__/extcon_gpio
    to the end of __fixups__/pmic_typec."""
    t = norm(t)
    ref = vbus_ref(t)
    set_refs(t, "extcon_gpio", [r for r in refs(t, "extcon_gpio") if r != ref])
    set_refs(t, "pmic_typec", refs(t, "pmic_typec") + [ref])
    return t


def patched():
    """The fixture's overlay as the patch leaves it: fx.patched_overlay() is test 40's (the c2c core set
    without fix A)."""
    return fix_a(fx.patched_overlay())


def with_source(t, *pdos):
    """Fixture tree T (normed) with the connector's source-pdos PDOS."""
    t = norm(t)

    def go(node):
        if node[0] == "connector":
            node[1][:] = [(n, fx.u32(*pdos) if n == "source-pdos" else v) for n, v in node[1]]
        for k in node[2]:
            go(k)

    go(t)
    return t


def prop_diff(a, b):
    """(path, property) of each property that differs between the parsed trees A and B, or is in one only."""
    pa, pb = ({(path, p[0]): p[1] for path, node in root.walk() for p in node.props} for root in (a, b))
    return sorted(k for k in set(pa) | set(pb) if pa.get(k) != pb.get(k))


class Fdt(util.TestCase):
    def test_round_trip(self):
        for t in (fx.overlay(), patched(), ("", [], [])):
            blob = fx.fdt(t, rsvmap=[(0x80000000, 0x1000)])
            fdt = pd.parse_fdt(blob)
            self.assertEqual(tree(fdt.root), norm(t))
            self.assertEqual(pd.build_fdt(fdt), blob)

    def test_new_names_go_to_the_strings_block(self):
        blob = fx.fdt(("", [("a", b"")], []))
        fdt = pd.parse_fdt(blob)
        fdt.root.set_prop("bb", fx.u32(1))
        fdt.root.set_prop("a", b"x\0")
        again = pd.parse_fdt(pd.build_fdt(fdt))
        self.assertEqual(again.root.props, [["a", b"x\0", 0], ["bb", fx.u32(1), 2]])
        self.assertEqual(again.strings, b"a\0bb\0")

    def test_refusals(self):
        blob = fx.fdt(("", [("a", b"")], [("n", [], [])]))
        # NOP tokens (dtc writes none) would not survive the rebuild: the property "a" (12 bytes after
        # the root's 8) becomes three.
        off = struct.unpack_from(">I", blob, 8)[0]
        nop = blob[:off + 8] + struct.pack(">3I", 4, 4, 4) + blob[off + 20:]
        cases = [(blob[:-1], "FDT version 17, size"), (b"x" * 64, "not an FDT"), (b"\xd0\x0d", "too short"),
                 (nop, "unexpected FDT token 4")]
        for data, message in cases:
            with self.assertRaises(SystemExit) as e:
                pd.parse_fdt(data)
            self.assertIn(message, str(e.exception))


class Edits(util.TestCase):
    def patch(self, t):
        blob, changes = pd.patch_entry(fx.fdt(t))
        return blob, changes

    def test_values(self):
        blob, changes = self.patch(fx.overlay())
        self.assertEqual(tree(parsed(blob)), patched())
        self.assertEqual(len(changes), 10)
        root = parsed(blob)
        con = root.find("/fragment@5/__overlay__/connector")
        self.assertEqual(con.prop("data-role"), b"dual\0")
        self.assertEqual(con.prop("sink-pdos"), fx.u32(0x2601912C))
        self.assertEqual(con.prop("source-pdos"), fx.u32(0x0401905A))
        # Untouched: the rest of the connector.
        self.assertEqual(con.prop("op-sink-microwatt"), fx.u32(10000000))
        for path in ("/fragment@2/__overlay__", "/fragment@3/__overlay__", "/fragment@7/__overlay__"):
            self.assertEqual(root.find(path).prop("use_pdhub_c2c"), b"", path)
        self.assertEqual(root.find("/fragment@7").prop("target"), fx.u32(0xFFFFFFFF))
        self.assertTrue(root.child("__fixups__").prop("pmic_typec").endswith(
            b"\0/fragment@7:target:0\0/fragment@3/__overlay__:extcon:0\0"))
        self.assertEqual(pd.fixup_targets(root, "pmic_typec"), ["/fragment@7"])
        # Nothing on the PD controller itself but the supply (use-pdhub-c2c is a later stage).
        self.assertIsNone(root.find("/fragment@5/__overlay__").prop("use-pdhub-c2c"))

    def test_vbus_supply_is_the_entrys_own_regulator(self):
        """vbus-supply is the phandle __symbols__/vddvbus names, with a __local_fixups__ entry (in fragment
        order) and no __fixups__ one: the base DT has no vddvbus."""
        root = parsed(self.patch(fx.overlay())[0])
        self.assertEqual(root.find("/fragment@5/__overlay__").prop("vbus-supply"), fx.u32(fx.VBUS_PHANDLE))
        local = root.child("__local_fixups__")
        self.assertEqual([c.name for c in local.children], ["fragment@2", "fragment@3", "fragment@5", "fragment@6"])
        self.assertEqual(tree(local.find("fragment@5/__overlay__")), ("__overlay__", [("vbus-supply", fx.u32(0))], []))
        self.assertIsNone(root.child("__fixups__").prop("vddvbus"))

    def test_fix_a_moves_only_usb3s_vbus_extcon(self):
        """Dock fix A: usb3's extcon:0 reference goes from extcon_gpio to the end of pmic_typec; usb's
        stays, the overlays' extcon cells stay <MISSING MISSING>, and both changes are reported."""
        blob, changes = self.patch(fx.overlay())
        fix = parsed(blob).child("__fixups__")
        self.assertEqual(fix.prop("extcon_gpio"), fx.strs("/fragment@2/__overlay__:extcon:0"))
        self.assertEqual(fix.prop("pmic_typec"), fx.strs(
            "/fragment@2/__overlay__:extcon:4", "/fragment@3/__overlay__:extcon:4", "/fragment@7:target:0",
            "/fragment@3/__overlay__:extcon:0"))
        for path in ("/fragment@2/__overlay__", "/fragment@3/__overlay__"):
            self.assertEqual(parsed(blob).find(path).prop("extcon"), fx.u32(fx.MISSING, fx.MISSING), path)
        self.assertIn(("/__fixups__", "extcon_gpio", fix.prop("extcon_gpio")), changes)
        self.assertIn(("/__fixups__", "pmic_typec", fix.prop("pmic_typec")), changes)
        # The rest of __fixups__ is untouched.
        stock = parsed(fx.fdt(fx.overlay())).child("__fixups__")
        self.assertEqual([(p[0], p[1]) for p in fix.props if p[0] not in ("extcon_gpio", "pmic_typec")],
                         [(p[0], p[1]) for p in stock.props if p[0] not in ("extcon_gpio", "pmic_typec")])

    def test_fix_a_refusals(self):
        """usb3's VBUS extcon reference must be once in extcon_gpio (stock) or once in pmic_typec
        (patched), and usb3's extcon <MISSING MISSING> with its id cell from pmic_typec."""
        def moved(t, gpio, typec):
            t = norm(t)
            ref = vbus_ref(t)
            set_refs(t, "extcon_gpio", [r for r in refs(t, "extcon_gpio") if r != ref] + [ref] * gpio)
            set_refs(t, "pmic_typec", [r for r in refs(t, "pmic_typec") if r != ref] + [ref] * typec)
            return t

        def usb3_extcon(t, value):
            t = norm(t)
            frag = vbus_ref(t).split("/")[1]
            over = [k for k in t[2] if k[0] == frag][0][2][0]
            over[1][:] = [(n, value if n == "extcon" else v) for n, v in over[1]]
            return t

        no_id = norm(fx.overlay())
        set_refs(no_id, "pmic_typec", [r for r in refs(no_id, "pmic_typec") if r != "/fragment@3/__overlay__:extcon:4"])
        ref = "/fragment@3/__overlay__:extcon:0"
        cases = []
        for base in (fx.overlay(), fx.patched_overlay()):
            cases += [(moved(base, 0, 0), "%s (the usb3 overlay's VBUS extcon) is in __fixups__ nowhere" % ref),
                      (moved(base, 1, 1), "is in __fixups__ extcon_gpio and pmic_typec"),
                      (moved(base, 2, 0), "is in __fixups__ extcon_gpio and extcon_gpio"),
                      (moved(base, 0, 2), "is in __fixups__ pmic_typec and pmic_typec"),
                      (usb3_extcon(base, fx.u32(fx.MISSING)), "the usb3 overlay's extcon is <0xffffffff>"),
                      (usb3_extcon(base, fx.u32(7, fx.MISSING)), "extcon is <0x00000007 0xffffffff>")]
        cases.append((no_id, "__fixups__/pmic_typec has no /fragment@3/__overlay__:extcon:4"))
        for t, message in cases:
            with self.assertRaises(SystemExit) as e:
                self.patch(t)
            self.assertIn(message, str(e.exception))
        # The same on a fully patched entry, through check.
        with self.assertRaises(SystemExit) as e:
            pd.check_entry(fx.fdt(moved(patched(), 1, 1)))
        self.assertIn("is in __fixups__ extcon_gpio and pmic_typec", str(e.exception))

    def test_finds_nodes_by_meaning(self):
        """The same edits land on other fragment numbers when the labels say so."""
        t = fx.overlay(numbers=(14, 3, 1, 9, 2))
        root = parsed(self.patch(t)[0])
        self.assertEqual(root.find("/fragment@2/__overlay__/connector").prop("data-role"), b"dual\0")
        self.assertEqual(root.find("/fragment@3/__overlay__").prop("use_pdhub_c2c"), b"")   # usb
        self.assertEqual(root.find("/fragment@1/__overlay__").prop("use_pdhub_c2c"), b"")   # usb3
        self.assertIsNone(root.find("/fragment@14/__overlay__").prop("use_pdhub_c2c"))
        self.assertEqual(root.find("/fragment@16/__overlay__").prop("use_pdhub_c2c"), b"")   # new
        self.assertEqual([c.name for c in root.children][-4:],
                         ["fragment@16", "__symbols__", "__fixups__", "__local_fixups__"])
        # Fix A moves usb3's (fragment@1) VBUS extcon, not usb's (fragment@3).
        fix = root.child("__fixups__")
        self.assertEqual(fix.prop("extcon_gpio"), fx.strs("/fragment@3/__overlay__:extcon:0"))
        self.assertTrue(fix.prop("pmic_typec").endswith(b"\0/fragment@16:target:0\0/fragment@1/__overlay__:extcon:0\0"))

    def test_source_ma(self):
        """A test image's source current changes only the connector's source-pdos (5 V, the stock flags);
        900 is the default."""
        default, _ = self.patch(fx.overlay())
        self.assertEqual(pd.patch_entry(fx.fdt(fx.overlay()), 900)[0], default)
        for ma, pdo in ((1000, 0x04019064), (1500, 0x04019096), (3000, 0x0401912C)):
            blob, changes = pd.patch_entry(fx.fdt(fx.overlay()), ma)
            a, b = parsed(default), parsed(blob)
            self.assertEqual([p for p, _ in a.walk()], [p for p, _ in b.walk()])
            self.assertEqual(prop_diff(a, b), [("/fragment@5/__overlay__/connector", "source-pdos")], ma)
            self.assertEqual(b.find("/fragment@5/__overlay__/connector").prop("source-pdos"), fx.u32(pdo))
            self.assertEqual(pd.pdo_limits(pdo), (5000, ma))
            self.assertIn(("/fragment@5/__overlay__/connector", "source-pdos", fx.u32(pdo)), changes)
            self.assertEqual(pd.source_ma_of(b), ma)
            # Patched at one current, it is not patched at another.
            self.assertEqual(pd.patch_entry(blob, ma), (blob, []))
            with self.assertRaises(SystemExit) as e:
                pd.patch_entry(blob)
            self.assertIn("source-pdos is <0x%08x>, expected <0x04019028>" % pdo, str(e.exception))

    def test_source_limit(self):
        for ma in (900, 950, 1000):
            pd.source_limit(ma)
        for ma in (1010, 1500, 3000):
            pd.source_limit(ma, allow_over_rating=True)
            with self.assertRaises(SystemExit) as e:
                pd.source_limit(ma)
            self.assertIn("--source-ma %d is above the OTG boost's 1000 mA rating; a test image needs "
                          "--allow-over-rating" % ma, str(e.exception))
        for ma in (400, 890, 905, 3010, 0):
            with self.assertRaises(SystemExit) as e:
                pd.source_limit(ma, allow_over_rating=True)
            self.assertIn("--source-ma %d is not 900-3000 mA in 10 mA steps" % ma, str(e.exception))

    def test_already_patched_is_left_alone(self):
        blob = fx.fdt(patched())
        self.assertEqual(pd.patch_entry(blob), (blob, []))
        once, _ = self.patch(fx.overlay())
        self.assertEqual(pd.patch_entry(once), (once, []))

    def test_wrong_old_value(self):
        cases = [
            # The plan's entry 1 has other sink PDOs.
            (dict(sink=(0x0401912C, 0xC0DC213C, 0x0002D0C8)), "sink-pdos is <0x0401912c 0xc0dc213c 0x0002d0c8>, "
                                                             "expected <0x0401912c 0xc0dc213c>"),
            (dict(source=(0x26019028,)), "source-pdos is <0x26019028>, expected <0x04019028>"),
            (dict(data_role=fx.strs("host")), 'data-role is "host", expected absent'),
        ]
        for kw, message in cases:
            with self.assertRaises(SystemExit) as e:
                self.patch(fx.overlay(**kw))
            self.assertIn(message, str(e.exception), kw)

    def test_partial_core_set(self):
        """Any part of the c2c core set without the rest is refused, whatever the connector says."""
        for kw in (dict(usb3_c2c=True), dict(usb_c2c=True), dict(typec=True), dict(pd_vbus=fx.VBUS_PHANDLE),
                   dict(pd_local=True), dict(usb_c2c=True, usb3_c2c=True, typec=True)):
            for sink, source in ((fx.STOCK_SINK, fx.STOCK_SOURCE), (fx.PATCHED_SINK, fx.PATCHED_SOURCE)):
                with self.assertRaises(SystemExit) as e:
                    self.patch(fx.overlay(sink=sink, source=source, **kw))
                self.assertIn("partial c2c core set", str(e.exception), kw)
        # Fix A alone, and test 40's set without it (its dtbo), are partial too.
        for t, done in ((fix_a(fx.overlay()), "(only usb3 VBUS extcon on pmic_typec)"),
                        (fx.patched_overlay(), "(only pmic_typec, usb3, usb, pmic_pd, __local_fixups__ of pmic_pd)")):
            with self.assertRaises(SystemExit) as e:
                self.patch(t)
            self.assertIn("partial c2c core set " + done, str(e.exception))

    def test_core_set_wrong_value(self):
        with self.assertRaises(SystemExit) as e:
            self.patch(fx.overlay(pd_vbus=0x99))
        self.assertIn("pmic_pd vbus-supply is <0x00000099>, expected <0x0000002a>", str(e.exception))

    def test_refusals(self):
        t = fx.overlay()
        no_vbus = norm(t)
        for k in no_vbus[2]:
            if k[0] == "__symbols__":
                k[1][:] = [p for p in k[1] if p[0] != "vddvbus"]
        two_pd = norm(t)
        fixups = [k for k in two_pd[2] if k[0] == "__fixups__"][0]
        fixups[1][:] = [(n, fx.strs("/fragment@5:target:0", "/fragment@4:target:0") if n == "pmic_pd" else v)
                        for n, v in fixups[1]]
        usb_elsewhere = norm(t)
        for k in usb_elsewhere[2]:
            if k[0] == "fragment@2":
                k[2][0][1][:] = [p for p in k[2][0][1] if p[0] != "vbus-supply"]
        cases = [(no_vbus, "no node with a phandle for vddvbus"), (two_pd, "2 fragments target pmic_pd"),
                 (usb_elsewhere, "the usb overlay's vbus-supply is not vddvbus")]
        for t, message in cases:
            with self.assertRaises(SystemExit) as e:
                self.patch(t)
            self.assertIn(message, str(e.exception))


class Table(util.TestCase):
    def test_header_stays_48_bytes_short(self):
        """total_size is where the entries end less 48, before and after, as on the RG Cube."""
        image = fx.cube_dtbo(fx.fdt(fx.overlay()))
        header, entries = pd.parse_table(image)
        self.assertEqual(header[1], entries[-1][1] + entries[-1][0] - 48)
        blob, _ = pd.patch_entry(entry(image))
        new = pd.replace_entry(image, 2, blob)
        header2, entries2 = pd.parse_table(new)
        self.assertEqual(len(new), entries2[-1][1] + entries2[-1][0])
        self.assertEqual(header2[1], len(new) - 48)
        self.assertEqual(header2[:1] + header2[2:], header[:1] + header[2:])

    def test_other_entries_unchanged(self):
        image = fx.dt_table([fx.other_entry("a"), fx.other_entry("b"), fx.fdt(fx.overlay()), fx.other_entry("c")],
                            [1, 2, 0x2100, 3], tail=b"TAIL")
        blob, _ = pd.patch_entry(entry(image))
        new = pd.replace_entry(image, 2, blob)
        _, old_entries = pd.parse_table(image)
        _, new_entries = pd.parse_table(new)
        for k in (0, 1, 3):
            self.assertEqual(pd.entry_blob(new, new_entries, k), pd.entry_blob(image, old_entries, k))
            self.assertEqual(new_entries[k][2:], old_entries[k][2:])
        self.assertEqual(pd.entry_blob(new, new_entries, 2), blob)
        self.assertEqual(new_entries[3][1], new_entries[2][1] + len(blob))
        self.assertTrue(new.endswith(b"TAIL"))
        # Back to the original with the original entry.
        self.assertEqual(pd.replace_entry(new, 2, entry(image)), image)

    def test_refuses_other_layouts(self):
        image = bytearray(fx.cube_dtbo(fx.fdt(fx.overlay())))
        struct.pack_into(">I", image, 32 + 32 + 4, struct.unpack_from(">I", image, 32 + 32 + 4)[0] + 4)
        cases = [(bytes(image), "entry 1 is not where the previous one ends"), (b"\0" * 64, "not a dt_table")]
        for data, message in cases:
            with self.assertRaises(SystemExit) as e:
                pd.parse_table(data)
            self.assertIn(message, str(e.exception))


class Check(util.TestCase):
    def test_pdo_limits(self):
        self.assertEqual(pd.pdo_limits(0x0401912C), (5000, 3000))
        self.assertEqual(pd.pdo_limits(0x2601912C), (5000, 3000))
        self.assertEqual(pd.pdo_limits(0x0401905A), (5000, 900))
        self.assertEqual(pd.pdo_limits(0x04019028), (5000, 400))
        self.assertEqual(pd.pdo_limits(0xC0DC213C), (11000, 3000))   # PPS 3.3-11 V 3 A
        self.assertEqual(pd.pdo_limits(0x0002D0C8)[0], 9000)   # fixed 9 V 2 A

    def test_patched_passes(self):
        pd.check_entry(fx.fdt(patched()))

    def test_unpatched_fails(self):
        with self.assertRaises(SystemExit) as e:
            pd.check_entry(fx.fdt(fx.overlay()))
        self.assertIn("not patched", str(e.exception))

    def test_test_40_image_fails(self):
        """Test 40's dtbo (the core set without fix A) is no longer a patched one."""
        with self.assertRaises(SystemExit) as e:
            pd.check_entry(fx.fdt(fx.patched_overlay()))
        self.assertIn("partial c2c core set", str(e.exception))

    def test_limits(self):
        """Anywhere in the entry: no sink PDO above 5 V, no source PDO above 5 V or 1 A, no fast charging."""
        cases = [
            ([("sink-pdos", fx.u32(0x2601912C, 0xC0DC213C))], "sink-pdos has 0xc0dc213c, above 5000 mV"),
            ([("sink-pdos", fx.u32(0x0002D0C8))], "sink-pdos has 0x0002d0c8, above 5000 mV"),
            ([("source-pdos", fx.u32(0x04019064 + 1))], "source-pdos has 0x04019065, above 5000 mV or 1000 mA"),
            ([("source-pdos", fx.u32(0x0002D05A))], "above 5000 mV or 1000 mA"),
        ] + [([(name, b"")], "has " + name) for name in pd.FORBIDDEN]
        for props, message in cases:
            # On the fast charger's node, not the connector: the edits themselves still check out.
            t = patched()
            for k in t[2]:
                if k[0] == "fragment@4":
                    k[2][0][1].extend(props)
            with self.assertRaises(SystemExit) as e:
                pd.check_entry(fx.fdt(t))
            self.assertIn(message, str(e.exception), props)

    def test_source_ma_images(self):
        """A test image's source PDO: up to 1 A it checks out; above, only with --allow-over-rating (up to
        3 A). Other flags, other currents and the stock 400 mA are not the patch's."""
        pd.check_entry(fx.fdt(with_source(patched(), 0x04019064)))
        for pdo, ma in ((0x04019096, 1500), (0x0401912C, 3000)):
            t = with_source(patched(), pdo)
            with self.assertRaises(SystemExit) as e:
                pd.check_entry(fx.fdt(t))
            self.assertIn("source-pdos has 0x%08x, above 5000 mV or 1000 mA" % pdo, str(e.exception))
            self.assertEqual(pd.source_ma_of(pd.check_entry(fx.fdt(t), allow_over_rating=True).root), ma)
        for pdos, message in (((0x0401912D,), "source-pdos is <0x0401912d>, expected <0x04019028>"),
                              ((0x0C019064,), "source-pdos is <0x0c019064>, expected <0x04019028>"),
                              ((0x04019064, 0x0401905A), "source-pdos is <0x04019064 0x0401905a>"),
                              ((0x04019028,), "not patched (/fragment@5/__overlay__/connector source-pdos)")):
            with self.assertRaises(SystemExit) as e:
                pd.check_entry(fx.fdt(with_source(patched(), *pdos)), allow_over_rating=True)
            self.assertIn(message, str(e.exception), pdos)
        # Anywhere else in the entry the limit is 3 A even with the flag.
        t = with_source(patched(), 0x04019096)
        for k in t[2]:
            if k[0] == "fragment@4":
                k[2][0][1].append(("source-pdos", fx.u32(0x0401912D)))
        with self.assertRaises(SystemExit) as e:
            pd.check_entry(fx.fdt(t), allow_over_rating=True)
        self.assertIn("source-pdos has 0x0401912d, above 5000 mV or 3000 mA", str(e.exception))

    def test_check_command(self):
        image = self.write("dtbo.img", fx.cube_dtbo(fx.fdt(patched())))
        proc = self.assertOk(util.run_script("patch_dtbo", "check", image))
        self.assertIn("RG Cube entry 2 is patched", proc.stdout)
        self.assertIn("source PDO 900 mA\n", proc.stdout)
        over = self.write("over.img", fx.cube_dtbo(fx.fdt(with_source(patched(), 0x04019096))))
        self.assertFails(util.run_script("patch_dtbo", "check", over), "above 5000 mV or 1000 mA")
        proc = self.assertOk(util.run_script("patch_dtbo", "check", "--allow-over-rating", over))
        self.assertIn("source PDO 1500 mA (above the boost's rating: a test image)", proc.stdout)
        self.assertFails(util.run_script("patch_dtbo", "check", "--source-ma", "1500", over), "patch_dtbo.py patch")
        stock = self.write("stock.img", fx.cube_dtbo(fx.fdt(fx.overlay())))
        self.assertFails(util.run_script("patch_dtbo", "check", stock), "not patched")
        test40 = self.write("test40.img", fx.cube_dtbo(fx.fdt(fx.patched_overlay())))
        self.assertFails(util.run_script("patch_dtbo", "check", test40), "partial c2c core set")
        other = self.write("other.img", fx.dt_table([fx.fdt(patched())], [0x2100]))
        self.assertFails(util.run_script("patch_dtbo", "check", other), "no entry of a known board")

    def test_merge_check(self):
        """fdtoverlay applies the patched entry to the base DT in vendor_boot, with every edit at its node."""
        util.need(self, "fdtoverlay")
        base = self.write("vendor_boot.img", fx.vendor_boot(fx.base_dt()))
        self.assertEqual(pd.vendor_boot_dtb(util.slurp(base)), fx.base_dt())
        image = self.write("dtbo.img", fx.cube_dtbo(fx.fdt(patched())))
        proc = self.assertOk(util.run_script("patch_dtbo", "check", image, base))
        for line in ("/soc/aon/spi@400000/pmic@0/typec@380 use_pdhub_c2c = (true)",
                     "/soc/ipa-apb/usb3@25100000 use_pdhub_c2c", "/soc/aon/usb@a00000 use_pdhub_c2c",
                     '/soc/aon/spi@400000/pmic@0/pd@e00/connector data-role = "dual"',
                     "/soc/aon/spi@400000/pmic@0/pd@e00 vbus-supply = <0x",
                     # Fix A: both cells are typec@380's phandle (2 in fx.base_dt()).
                     "/soc/ipa-apb/usb3@25100000 extcon = <0x00000002 0x00000002>",
                     # usb's extcon (cells 0 and 4) and usb3's (cells 0 and 4) resolve to their labels.
                     "merge check: 4 phandle references resolve to their labels' nodes"):
            self.assertIn(line, proc.stdout)

        def fails(t, message):
            with self.assertRaises(SystemExit) as e, contextlib.redirect_stdout(io.StringIO()):
                pd.merge_check(entry(fx.cube_dtbo(fx.fdt(t))), base)
            self.assertIn(message, str(e.exception))

        # Without its local fixup, vbus-supply would point at a base node after the merge.
        t = patched()
        for k in t[2]:
            if k[0] == "__local_fixups__":
                k[2][:] = [c for c in k[2] if c[0] != "fragment@5"]
        fails(t, "vbus-supply")
        # Without fix A (test 40's dtbo), usb3's VBUS extcon is extcon-gpio's (7).
        fails(fx.patched_overlay(), "/soc/ipa-apb/usb3@25100000 extcon is <0x00000007 0x00000002>")
        # A later fragment that rewrites usb's extcon: the reference no longer resolves to extcon_gpio.
        t = patched()
        t[2].insert([k[0] for k in t[2]].index("__symbols__"), (
            "fragment@9", [("target-path", fx.strs("/soc/aon/usb@a00000"))],
            [("__overlay__", [("extcon", fx.u32(0x11, 0x22))], [])]))
        fails(t, "/soc/aon/usb@a00000 extcon is <0x00000011 0x00000022>, cell at byte 0 is not extcon_gpio's "
                 "phandle <0x00000007>")

    def test_merge_check_skipped_without_fdtoverlay(self):
        base = self.write("vendor_boot.img", fx.vendor_boot(fx.base_dt()))
        image = self.write("dtbo.img", fx.cube_dtbo(fx.fdt(patched())))
        proc = self.assertOk(util.run_script("patch_dtbo", "check", image, base, env={"PATH": "/nonexistent"}))
        self.assertIn("merge check skipped: no fdtoverlay on PATH", proc.stdout)

    def test_vendor_boot_dtb_errors(self):
        for data, message in ((b"NOTVNDR!" + bytes(4096), "not a vendor boot image"),):
            with self.assertRaises(SystemExit) as e:
                pd.vendor_boot_dtb(data)
            self.assertIn(message, str(e.exception))


def with_jeita(t, **tables):
    """Fixture tree T (normed) with the battery's jeita tables fx.jeita_props(**TABLES)."""
    t = norm(t)
    battery = t[2][0][2][0][2][0]
    battery[1][:] = [p for p in battery[1] if not p[0].endswith("-jeita-temp-table")]
    battery[1][1:1] = fx.jeita_props(**tables)
    return t


THERMAL2 = {kind: fx.thermal2_jeita(kind) for kind in ("dcp", "unknown", "cdp")}
BATTERY = "/fragment/__overlay__/battery"


def thermal2_rows(kind):
    full, warm, hot = fx.THERMAL2_UA[kind]
    return [(0, 30, 0, 4340000), (150, 180, 700000, 4340000), (450, 420, full, 4340000), (490, 460, warm, 4340000),
            (520, 490, hot, 4340000), (600, 570, 700000, 4100000)]


def jeita_sim(rows, temps):
    """The charger manager's jeita steps for ROWS (jeita_rows()'s) from a plug-in, one poll for each
    battery temperature of TEMPS (0.1 C): the current each poll leaves (uA, or "stop"). Written from
    cm_manager_get_jeita_status, cm_jeita_temp_goes_down/up and cm_update_current_jeita_status."""
    n = len(rows)
    st = dict(status=0, temp=-200, changed=True, up=0, down=0)

    def index(cur, col):
        return next((i + 1 for i in range(n - 1, -1, -1) if cur > rows[i][col] or (i and cur == rows[i][col])), 0)

    def status(cur):
        s, t = st["status"], index(cur, 0)
        if t in (0, n):
            return t
        r = index(cur, 1)
        if st["changed"]:
            s = 0
            st.update(status=0, temp=-200)
        if st["temp"] > cur:   # cooling
            return min(s, r) if r == n or rows[r][0] > rows[r][1] else min(s, t)
        return max(s, r) if r < n and rows[r][0] < rows[r][1] else max(s, t)

    out = []
    for cur in temps:
        s = status(cur)
        if st["changed"]:
            st.update(status=s, temp=cur, changed=False)
        elif s != st["status"]:
            way, other = ("up", "down") if s > st["status"] else ("down", "up")
            st[other] = 0
            st[way] += 1
            if st[way] > 1:
                st.update({"status": s, "temp": cur, way: 0})
        else:
            st.update(up=0, down=0)
        out.append("stop" if st["status"] in (0, n) else rows[st["status"]][2])
    return out


def jeita_steps(rows, temps):
    """(temperature, current) where jeita_sim()'s current changes, polling each of TEMPS twice."""
    polled = [t for t in temps for _ in (0, 1)]
    out = jeita_sim(rows, polled)
    return [(t, ua) for i, (t, ua) in enumerate(zip(polled, out)) if not i or ua != out[i - 1]]


class Jeita(util.TestCase):
    """--jeita thermal2: the DCP, UNKNOWN and CDP jeita tables of the battery node."""

    def test_table_encoding(self):
        """The patch's tables are the owner's, written out by hand in the fixture; the stock ones are the
        RG Cube's."""
        for kind, full in (("dcp", 2000000), ("unknown", 1050000), ("cdp", 1500000)):
            self.assertEqual(pd.jeita_table(pd.JEITA["thermal2"](full)), fx.thermal2_jeita(kind))
            self.assertEqual(pd.jeita_table(pd.JEITA["stock"](full)), fx.stock_jeita(kind))
            self.assertEqual(pd.jeita_rows(fx.thermal2_jeita(kind)), thermal2_rows(kind))
            self.assertEqual(pd.jeita_variant(kind, fx.thermal2_jeita(kind)), "thermal2")
            self.assertEqual(pd.jeita_variant(kind, fx.stock_jeita(kind)), "stock")
        # The owner's DCP table, as the dtbo holds it.
        self.assertEqual(fx.thermal2_jeita("dcp"), fx.u32(
            0x3E8, 0x406, 0, 0x423920, 0x47E, 0x49C, 0xAAE60, 0x423920, 0x5AA, 0x58C, 0x1E8480, 0x423920,
            0x5D2, 0x5B4, 0x16E360, 0x423920, 0x5F0, 0x5D2, 0xF4240, 0x423920, 0x640, 0x622, 0xAAE60, 0x3E8FA0))
        self.assertEqual(len(fx.thermal2_jeita("dcp")), 96)
        self.assertIsNone(pd.jeita_rows(fx.u32(1000, 1030, 0)))
        self.assertIsNone(pd.jeita_rows(b""))
        # Each kind keeps its own full rate: the wall charger's table is no variant of the dock's.
        self.assertIsNone(pd.jeita_variant("unknown", fx.thermal2_jeita("dcp")))
        self.assertEqual(pd.JEITA_EDITS, ["thermal2"])

    def test_bands(self):
        self.assertEqual(pd.jeita_bands(thermal2_rows("dcp")),
                         "stop | 0.0 C/3.0 C | 700 mA 4.34 V | 15.0 C/18.0 C | 2000 mA 4.34 V | 42.0 C/45.0 C | "
                         "1500 mA 4.34 V | 46.0 C/49.0 C | 1000 mA 4.34 V | 49.0 C/52.0 C | 700 mA 4.10 V | "
                         "57.0 C/60.0 C | stop")
        self.assertEqual(pd.jeita_bands(pd.jeita_rows(fx.stock_jeita("dcp"))),
                         "stop | 0.0 C/3.0 C | 700 mA 4.34 V | 15.0 C/18.0 C | 2000 mA 4.34 V | 49.0 C/52.0 C | "
                         "700 mA 4.10 V | 57.0 C/60.0 C | stop")

    def test_steps(self):
        """The charger manager steps through thermal2 as the owner asked, warming and cooling, and keeps
        the stock steps from 52 C; from a plug-in it takes the warming band."""
        up, down = list(range(100, 651)), list(range(650, 99, -1))
        stock = pd.jeita_rows(fx.stock_jeita("dcp"))
        self.assertEqual(jeita_steps(stock, up + down),
                         [(100, 700000), (180, 2000000), (520, 700000), (600, "stop"), (569, 700000),
                          (489, 2000000), (149, 700000)])
        for kind in ("dcp", "unknown", "cdp"):
            full, warm, hot = fx.THERMAL2_UA[kind]
            steps = [(100, 700000), (180, full), (450, warm), (490, hot), (520, 700000), (600, "stop"),
                     (569, 700000), (489, hot), (459, warm), (419, full), (149, 700000)]
            # Equal currents are no step.
            self.assertEqual(jeita_steps(thermal2_rows(kind), up + down),
                             [s for i, s in enumerate(steps) if not i or s[1] != steps[i - 1][1]], kind)
        rows = thermal2_rows("dcp")
        for temp, ua in ((-5, "stop"), (20, "stop"), (100, 700000), (300, 2000000), (449, 2000000), (450, 1500000),
                         (470, 1500000), (500, 1000000), (550, 700000), (610, "stop")):
            self.assertEqual(jeita_sim(rows, [temp]), [ua], temp)

    def test_rules(self):
        """Every stock table and every variant keeps the charger manager's rules; tables that would step
        early, or not stop, are refused."""
        for kind in fx.JEITA_KINDS:
            self.assertIsNone(pd.jeita_problem(pd.jeita_rows(fx.stock_jeita(kind))), kind)
        for kind, value in THERMAL2.items():
            self.assertIsNone(pd.jeita_problem(pd.jeita_rows(value)), kind)
        good = thermal2_rows("dcp")

        def row(i, **kw):
            rows = list(good)
            t, r, ua, uv = rows[i]
            rows[i] = (kw.get("t", t), kw.get("r", r), kw.get("ua", ua), uv)
            return rows

        cases = [(None, "not whole 16-byte rows"),
                 (row(3, t=450), "its temperatures do not rise"),
                 (row(3, r=420), "its recovery temperatures do not rise"),
                 # A cold-side row after the hot-side ones: cooling from 0.7 A would test it.
                 (row(4, r=530), "row 4 is cold-side (recovery above its temperature) after a hot-side row"),
                 ([(0, 160, 0, 4340000)] + good[1:], "row 0's recovery is not below row 1's temperature"),
                 # At the next temperature: warming would skip the 0.7 A band.
                 ([(0, 150, 0, 4340000)] + good[1:], "row 0's recovery is not below row 1's temperature"),
                 ([(0, 30, 0, 4340000), (150, 120, 700000, 4340000), (400, 140, 2000000, 4340000)],
                  "row 2's recovery is below row 1's temperature"),
                 (row(3, ua=0), "row 3's 0 uA does not stop charging")]
        for rows, message in cases:
            self.assertIn(message, pd.jeita_problem(rows) or "", rows)
        # A hot-side recovery at the previous temperature is thermal2's own (49 C): cooling from 0.7 A
        # below 49 C gives 1.0 A, which holds down to 46 C.
        self.assertEqual(good[4][1], good[3][0])
        self.assertEqual(jeita_sim(good, [550, 550, 488, 488, 470, 470, 461, 461, 459, 459])[1::2],
                         [700000, 1000000, 1000000, 1000000, 1500000])

    def test_edits_only_the_three_tables(self):
        """From stock: the DCP, UNKNOWN and CDP tables become thermal2's; nothing else in the entry changes,
        the connector edits are the usual ones."""
        default, _ = pd.patch_entry(fx.fdt(fx.overlay()))
        blob, changes = pd.patch_entry(fx.fdt(fx.overlay()), jeita="thermal2")
        self.assertEqual(len(changes), 10 + 3)
        self.assertEqual(changes[10:], [(BATTERY, kind + "-jeita-temp-table", THERMAL2[kind])
                                        for kind in ("dcp", "unknown", "cdp")])
        self.assertEqual(tree(parsed(blob)), with_jeita(patched(), **THERMAL2))
        a, b = parsed(default), parsed(blob)
        self.assertEqual([p for p, _ in a.walk()], [p for p, _ in b.walk()])
        self.assertEqual(prop_diff(a, b), sorted((BATTERY, k + "-jeita-temp-table") for k in THERMAL2))
        for kind in ("sdp", "fchg", "flash"):
            self.assertEqual(b.find(BATTERY).prop(kind + "-jeita-temp-table"), fx.stock_jeita(kind))
        self.assertEqual(len(blob), len(default) + 3 * 32)
        # Already patched (an image the patch made, or thermal2 already): the same blob.
        self.assertEqual(pd.patch_entry(default, jeita="thermal2")[0], blob)
        self.assertEqual(pd.patch_entry(blob, jeita="thermal2"), (blob, []))
        # Without --jeita, thermal2 tables stay as they are.
        self.assertEqual(pd.patch_entry(blob), (blob, []))

    def test_every_copy(self):
        """Every node of the entry that has the tables gets the edit, wherever it is."""
        t = norm(fx.overlay())
        t[2].insert([k[0] for k in t[2]].index("__symbols__"), (
            "fragment@20", [("target-path", fx.strs("/soc"))],
            [("__overlay__", [], [("battery-b", [("compatible", fx.strs("simple-battery"))] + fx.jeita_props(), [])])]))
        blob, changes = pd.patch_entry(fx.fdt(t), jeita="thermal2")
        root = parsed(blob)
        for path in (BATTERY, "/fragment@20/__overlay__/battery-b"):
            for kind in fx.JEITA_KINDS:
                self.assertEqual(root.find(path).prop(kind + "-jeita-temp-table"),
                                 THERMAL2.get(kind, fx.stock_jeita(kind)), (path, kind))
        self.assertEqual(len(changes), 10 + 6)
        self.assertEqual(pd.check_jeita(root), "thermal2")

    def test_patch_refusals(self):
        other = fx.jeita((1000, 1030, 0, 4340000), (1150, 1180, 700000, 4340000), (1520, 1490, 2100000, 4340000),
                         (1600, 1570, 700000, 4100000))
        cases = [(with_jeita(fx.overlay(), dcp=other), BATTERY + " dcp-jeita-temp-table is <0x000003e8"),
                 # The wall charger's thermal2 table in the dock's place.
                 (with_jeita(fx.overlay(), unknown=THERMAL2["dcp"]), "unknown-jeita-temp-table is <0x000003e8"),
                 (with_jeita(fx.overlay(), cdp=None), BATTERY + " cdp-jeita-temp-table is absent, expected the stock"),
                 (fx.overlay(jeita=[]), "dtbo: no node with jeita tables")]
        for t, message in cases:
            with self.assertRaises(SystemExit) as e:
                pd.patch_entry(fx.fdt(t), jeita="thermal2")
            self.assertIn(message, str(e.exception))
            # Without --jeita they are not the patch's business.
            pd.patch_entry(fx.fdt(t))

    def test_check(self):
        """check passes stock and thermal2 tables, and refuses a mix, an unknown table or a broken one."""
        self.assertEqual(pd.check_jeita(pd.check_entry(fx.fdt(patched())).root), "stock")
        fdt = pd.check_entry(fx.fdt(with_jeita(patched(), **THERMAL2)))
        self.assertEqual(pd.check_jeita(fdt.root), "thermal2")
        self.assertIsNone(pd.check_jeita(pd.check_entry(fx.fdt(fix_a(fx.patched_overlay(jeita=[])))).root))
        lines = pd.jeita_report(fdt.root)
        self.assertEqual(len(lines), 6)
        self.assertIn("jeita %s dcp-jeita-temp-table (thermal2, 6 rows): stop | 0.0 C/3.0 C | 700 mA 4.34 V | "
                      "15.0 C/18.0 C | 2000 mA 4.34 V | 42.0 C/45.0 C | 1500 mA 4.34 V | 46.0 C/49.0 C | 1000 mA "
                      "4.34 V | 49.0 C/52.0 C | 700 mA 4.10 V | 57.0 C/60.0 C | stop" % BATTERY, lines)
        self.assertIn("jeita %s sdp-jeita-temp-table (not edited by the patch, 4 rows): stop | 0.0 C/3.0 C | "
                      "500 mA 4.34 V | 15.0 C/18.0 C | 500 mA 4.34 V | 49.0 C/52.0 C | 500 mA 4.10 V | "
                      "57.0 C/60.0 C | stop" % BATTERY, lines)
        broken = fx.jeita((1000, 1030, 0, 4340000), (1150, 1180, 500000, 4340000), (1520, 1490, 0, 4340000),
                          (1600, 1570, 500000, 4100000))
        cases = [(dict(dcp=THERMAL2["dcp"]), "the jeita tables are a mix of stock and thermal2"),
                 (dict(THERMAL2, cdp=None), "cdp-jeita-temp-table is absent, not a table the patch knows"),
                 (dict(dcp=fx.stock_jeita("cdp")), "dcp-jeita-temp-table is <0x000003e8"),
                 (dict(sdp=broken), BATTERY + " sdp-jeita-temp-table: row 2's 0 uA does not stop charging"),
                 (dict(fchg=fx.u32(1000, 1030, 0)), "fchg-jeita-temp-table: not whole 16-byte rows")]
        for tables, message in cases:
            with self.assertRaises(SystemExit) as e:
                pd.check_entry(fx.fdt(with_jeita(patched(), **tables)))
            self.assertIn(message, str(e.exception), tables)

    def test_check_command(self):
        stock = self.write("stock.img", fx.cube_dtbo(fx.fdt(patched())))
        proc = self.assertOk(util.run_script("patch_dtbo", "check", stock))
        # thermal2 is the RG Cube's approved tables (test 42 TH2); stock ones are a test image's (DK1).
        self.assertIn("%s: jeita tables stock (a test image; thermal2 is approved)\n" % stock, proc.stdout)
        image = self.write("dtbo.img", fx.cube_dtbo(fx.fdt(with_jeita(patched(), **THERMAL2))))
        proc = self.assertOk(util.run_script("patch_dtbo", "check", image))
        self.assertIn("RG Cube entry 2 is patched", proc.stdout)
        self.assertIn("%s: jeita tables thermal2 (the approved tables)\n" % image, proc.stdout)
        # Not the approved image itself (its sha256 is not tested_sha256).
        self.assertNotIn("the image approved for", proc.stdout)
        self.assertEqual(proc.stdout.count("\njeita %s " % BATTERY), 6)
        self.assertIn("unknown-jeita-temp-table (thermal2, 6 rows): stop | 0.0 C/3.0 C | 700 mA 4.34 V | "
                      "15.0 C/18.0 C | 1050 mA 4.34 V | 42.0 C/45.0 C | 1050 mA 4.34 V | 46.0 C/49.0 C | 1000 mA",
                      proc.stdout)
        self.assertIn("cdp-jeita-temp-table (thermal2, 6 rows): stop | 0.0 C/3.0 C | 700 mA 4.34 V | "
                      "15.0 C/18.0 C | 1500 mA 4.34 V | 42.0 C/45.0 C | 1500 mA 4.34 V | 46.0 C/49.0 C | 1000 mA",
                      proc.stdout)
        mixed = self.write("mixed.img", fx.cube_dtbo(fx.fdt(with_jeita(patched(), cdp=THERMAL2["cdp"]))))
        self.assertFails(util.run_script("patch_dtbo", "check", mixed), "a mix of stock and thermal2")
        self.assertFails(util.run_script("patch_dtbo", "check", "--jeita", "thermal2", image), "patch_dtbo.py patch")
        none = self.write("none.img", fx.cube_dtbo(fx.fdt(fix_a(fx.patched_overlay(jeita=[])))))
        proc = self.assertOk(util.run_script("patch_dtbo", "check", none))
        self.assertIn("%s: jeita tables none\n" % none, proc.stdout)
        # A device whose approved tables are stock: thermal2 is then a test image's. The approved
        # image itself (here: by its sha256) says so.
        sha = hashlib.sha256(util.slurp(image)).hexdigest()
        devices = {k: d._replace(tested_jeita=None, tested_sha256=sha) for k, d in pd.DEVICES.items()}
        for path, line, approved in ((stock, "jeita tables stock (the approved tables)\n", False),
                                     (image, "jeita tables thermal2 (a test image; stock is approved)\n", True)):
            out = io.StringIO()
            with mock.patch.object(pd, "DEVICES", devices), contextlib.redirect_stdout(out):
                pd.cmd_check(path)
            self.assertIn("%s: %s" % (path, line), out.getvalue())
            self.assertEqual("%s: the image approved for the RG Cube (sha256 %s)\n" % (path, sha) in out.getvalue(),
                             approved, path)

    def test_merge_check(self):
        """The merged tree has the thermal2 tables at /battery."""
        util.need(self, "fdtoverlay")
        base = self.write("vendor_boot.img", fx.vendor_boot(fx.base_dt()))
        image = self.write("dtbo.img", fx.cube_dtbo(fx.fdt(with_jeita(patched(), **THERMAL2))))
        proc = self.assertOk(util.run_script("patch_dtbo", "check", image, base))
        for kind, value in THERMAL2.items():
            self.assertIn("merge check: /battery %s-jeita-temp-table = %s\n" % (kind, pd.show(value)), proc.stdout)
        self.assertIn("merge check: 4 phandle references resolve to their labels' nodes", proc.stdout)


class Footer(util.TestCase):
    def setUp(self):
        super().setUp()
        self.info = self.write("info.txt", DTBO_INFO)
        self.log = self.path("shim.log")
        self.bin = util.shim_dir(self.tmp, "avbtool")
        self.env = {"PATH": self.bin + os.pathsep + os.environ["PATH"], "FAKE_AVBTOOL_INFO": self.info,
                    "SHIM_LOG": self.log, "AVB_KEY": self.write("key.pem", "fake")}

    def test_footer_info(self):
        with mock.patch.dict(os.environ, self.env):
            footer = avb.footer_info("dtbo.img", "dtbo")
        self.assertEqual(footer.salt, SALT)
        self.assertEqual(footer.props, [tuple(FINGERPRINT.split(":", 1))])
        self.assertEqual(footer.opts, {"algorithm": "SHA256_RSA4096", "rollback_index": "0", "flags": "0",
                                       "rollback_index_location": "0"})
        self.assertEqual((footer.partition_name, footer.image_size, footer.original_size), ("dtbo", 8388608, 538536))

    def test_footer_info_needs_every_field(self):
        for line in ("Salt:", "Image size:", "Original image size:", "Partition Name:", "Flags:"):
            text = "\n".join(i for i in DTBO_INFO.splitlines() if not i.strip().startswith(line))
            with open(self.info, "w") as f:
                f.write(text)
            with mock.patch.dict(os.environ, self.env), self.assertRaises(SystemExit) as e:
                avb.footer_info("dtbo.img", "dtbo")
            self.assertIn("dtbo: ", str(e.exception))

    def test_add_footer(self):
        with mock.patch.dict(os.environ, self.env):
            avb.add_footer("out.img", avb.footer_info("dtbo.img"), "dtbo", 8388608)
        calls = [c for c in util.slurp(self.log, text=True).splitlines() if c.startswith("avbtool add_hash_footer")]
        self.assertEqual(len(calls), 1)
        args = calls[0].split(" ")
        pairs = dict(zip(args[2::2], args[3::2]))
        self.assertEqual(pairs, {"--image": "out.img", "--partition_name": "dtbo", "--partition_size": "8388608",
                                 "--key": self.env["AVB_KEY"], "--salt": SALT, "--algorithm": "SHA256_RSA4096",
                                 "--flags": "0", "--rollback_index": "0", "--rollback_index_location": "0",
                                 "--prop": FINGERPRINT})

    def test_add_footer_needs_the_key(self):
        with mock.patch.dict(os.environ, dict(self.env, AVB_KEY=self.path("none.pem"))), \
                self.assertRaises(SystemExit) as e:
            avb.add_footer("out.img", avb.footer_info("dtbo.img"), "dtbo", 8388608)
        self.assertIn("AVB_KEY", str(e.exception))


class Patch(util.TestCase):
    """cmd_patch() with a stand-in avbtool, on a dtbo whose sha256 the device table is given."""

    def setUp(self):
        super().setUp()
        self.log = self.path("shim.log")
        shims = util.shim_dir(self.tmp, extra={"avbtool": FAKE_AVBTOOL})
        self.env = {"PATH": shims + os.pathsep + os.environ["PATH"], "SHIM_LOG": self.log,
                    "AVB_KEY": self.write("key.pem", "fake key"), "FAKE_AVB_RELEASE": "1"}
        self.src = self.sign("stock.img", fx.cube_dtbo(fx.fdt(fx.overlay())))
        self.dst = self.path("out.img")

    def sign(self, name, raw, **env):
        path = self.write(name, raw)
        cmd = ["avbtool", "add_hash_footer", "--image", path, "--partition_name", "dtbo", "--partition_size",
               str(PARTITION), "--key", self.env["AVB_KEY"], "--salt", SALT, "--algorithm", "SHA256_RSA4096",
               "--rollback_index", "0", "--rollback_index_location", "0", "--flags", "0", "--prop", FINGERPRINT]
        self.assertOk(util.run(cmd, env=dict(self.env, **env)))
        return path

    def patch(self, tested=False, tested_sha256="", tested_jeita="thermal2", argv=None, **kw):
        """cmd_patch(), or main() with ARGV, with the device table knowing self.src (approved tables
        TESTED_JEITA, thermal2 as the RG Cube's)."""
        device = pd.Device("RG Cube", 2, 0x2100, "dtbo", PARTITION, tested_sha256, tested_jeita)
        sha = hashlib.sha256(util.slurp(self.src)).hexdigest()
        out = io.StringIO()
        with mock.patch.dict(os.environ, self.env), mock.patch.object(pd, "DEVICES", {sha: device}), \
                contextlib.redirect_stdout(out):
            if argv is None:
                pd.cmd_patch(self.src, self.dst, tested, **kw)
            else:
                with mock.patch.object(sys, "argv", ["patch_dtbo.py"] + argv):
                    pd.main()
        return out.getvalue()

    def test_patch_and_sign(self):
        out = self.patch()
        result = util.slurp(self.dst)
        self.assertEqual(len(result), PARTITION)
        # The same parameters as the input: re-signing the patched raw by hand gives the same image.
        raw = fx.cube_dtbo(pd.patch_entry(fx.fdt(fx.overlay()))[0])
        self.assertEqual(result, util.slurp(self.sign("expected.img", raw)))
        self.assertIn("patched RG Cube entry 2 -> %s (sha256 %s)" % (self.dst, hashlib.sha256(result).hexdigest()),
                      out)
        self.assertIn("/fragment@5/__overlay__ vbus-supply = <0x0000002a>", out)
        self.assertIn('/__fixups__ extcon_gpio = "/fragment@2/__overlay__:extcon:0"\n', out)
        # The original, then the result.
        calls = [c for c in util.slurp(self.log, text=True).splitlines() if "add_hash_footer" in c]
        self.assertEqual(len(calls), 2 + 2)   # setUp's and the test's own signing, then the patch's two
        self.assertOk(util.run_script("patch_dtbo", "check", self.dst))

    def test_unknown_image(self):
        with mock.patch.dict(os.environ, self.env), self.assertRaises(SystemExit) as e:
            pd.cmd_patch(self.src, self.dst, False)
        self.assertIn("is not a dtbo this patch knows", str(e.exception))
        proc = util.run_script("patch_dtbo", "patch", self.src, self.dst, env=self.env)
        self.assertFails(proc, "is not a dtbo this patch knows")
        self.assertFalse(os.path.exists(self.dst))

    def test_resign_must_reproduce_the_input(self):
        """A different avbtool (here: release) would sign the result differently from the original too."""
        self.env["FAKE_AVB_RELEASE"] = "2"
        with self.assertRaises(SystemExit) as e:
            self.patch()
        self.assertIn("re-signing the original does not give it back", str(e.exception))
        self.assertFalse(os.path.exists(self.dst))

    def test_tested_gate(self):
        """--tested makes the image with the device's approved tables, and it must be tested_sha256."""
        with self.assertRaises(SystemExit) as e:
            self.patch(tested=True)
        self.assertIn("is not the image approved for the RG Cube (none yet)", str(e.exception))
        self.assertFalse(os.path.exists(self.dst))
        self.patch(jeita="thermal2")
        sha = hashlib.sha256(util.slurp(self.dst)).hexdigest()
        os.unlink(self.dst)
        self.patch(tested=True, tested_sha256=sha)
        self.assertEqual(hashlib.sha256(util.slurp(self.dst)).hexdigest(), sha)
        os.unlink(self.dst)
        # Without --tested and --jeita: stock tables (test 42 DK1), not the approved image.
        self.patch()
        plain = hashlib.sha256(util.slurp(self.dst)).hexdigest()
        self.assertNotEqual(plain, sha)
        os.unlink(self.dst)
        with self.assertRaises(SystemExit) as e:
            self.patch(tested=True, tested_sha256=plain)
        self.assertIn("(sha256 %s) is not the image approved for the RG Cube (%s)" % (sha, plain), str(e.exception))
        self.assertFalse(os.path.exists(self.dst))
        # A device whose approved tables are stock: --tested leaves them.
        self.patch(tested=True, tested_sha256=plain, tested_jeita=None)
        self.assertEqual(hashlib.sha256(util.slurp(self.dst)).hexdigest(), plain)

    def test_source_ma(self):
        """--source-ma 1000: against the default image, only entry 2's connector source-pdos differs; the
        other entries, the table and the footer's parameters are the same. --source-ma 900 is the default."""
        self.patch()
        default = util.slurp(self.dst)
        os.unlink(self.dst)
        self.patch(argv=["patch", "--source-ma", "900", self.src, self.dst])
        self.assertEqual(util.slurp(self.dst), default)
        os.unlink(self.dst)
        out = self.patch(argv=["patch", "--source-ma", "1000", self.src, self.dst])
        image = util.slurp(self.dst)
        self.assertIn("/fragment@5/__overlay__/connector source-pdos = <0x04019064>\n", out)
        header, entries = pd.parse_table(image)
        header0, entries0 = pd.parse_table(default)
        self.assertEqual((header, [e[2:] for e in entries]), (header0, [e[2:] for e in entries0]))
        for k in (0, 1):
            self.assertEqual(pd.entry_blob(image, entries, k), pd.entry_blob(default, entries0, k))
        a, b = parsed(entry(default)), parsed(entry(image))
        self.assertEqual([p for p, _ in a.walk()], [p for p, _ in b.walk()])
        self.assertEqual(prop_diff(a, b), [("/fragment@5/__overlay__/connector", "source-pdos")])
        self.assertEqual(len(entry(image)), len(entry(default)))
        # The stand-in's footer: the same salt, properties, options and key.
        self.assertEqual(image[-4096:], default[-4096:])
        self.assertOk(util.run_script("patch_dtbo", "check", self.dst))

    def test_over_rating_needs_the_flag(self):
        for argv in (["patch", "--source-ma", "1500", self.src, self.dst],
                     ["patch", "--source-ma", "1010", self.src, self.dst]):
            with self.assertRaises(SystemExit) as e:
                self.patch(argv=argv)
            self.assertIn("above the OTG boost's 1000 mA rating; a test image needs --allow-over-rating",
                          str(e.exception))
            self.assertFalse(os.path.exists(self.dst))
        out = self.patch(argv=["patch", "--source-ma", "1500", "--allow-over-rating", self.src, self.dst])
        self.assertIn("/fragment@5/__overlay__/connector source-pdos = <0x04019096>\n", out)
        self.assertFails(util.run_script("patch_dtbo", "check", self.dst), "source-pdos has 0x04019096, above 5000 mV or 1000 mA")
        proc = self.assertOk(util.run_script("patch_dtbo", "check", "--allow-over-rating", self.dst))
        self.assertIn("source PDO 1500 mA (above the boost's rating: a test image)", proc.stdout)
        for argv in (["patch", "--source-ma", self.src, self.dst], ["patch", self.src, self.dst, "--source-ma"],
                     ["patch", "--source-ma", "-1500", self.src, self.dst]):
            with self.assertRaises(SystemExit) as e:
                self.patch(argv=argv)
            self.assertIn("patch_dtbo.py patch [--tested]", str(e.exception))

    def test_tested_takes_only_the_default(self):
        """--tested never makes a test image: no other --source-ma, no --allow-over-rating, even when the
        result would be the approved one."""
        self.patch(jeita="thermal2")
        sha = hashlib.sha256(util.slurp(self.dst)).hexdigest()
        os.unlink(self.dst)
        for kw in (dict(source_ma=1000), dict(source_ma=1500, allow_over_rating=True), dict(allow_over_rating=True),
                   dict(argv=["patch", "--tested", "--source-ma", "1000", self.src, self.dst]),
                   dict(argv=["patch", "--source-ma", "1500", "--allow-over-rating", "--tested", self.src, self.dst])):
            with self.assertRaises(SystemExit) as e:
                self.patch(tested=True, tested_sha256=sha, **kw)
            self.assertIn("--tested is the approved 900 mA image only; a --source-ma or --allow-over-rating "
                          "image is a test image", str(e.exception), kw)
            self.assertFalse(os.path.exists(self.dst))
        self.patch(argv=["patch", "--tested", "--source-ma", "900", self.src, self.dst], tested_sha256=sha)
        self.assertEqual(hashlib.sha256(util.slurp(self.dst)).hexdigest(), sha)
        self.assertFails(util.run_script("patch_dtbo", "tested", "--allow-over-rating"), "patch_dtbo.py patch")

    def test_jeita(self):
        """--jeita thermal2: against the default image, only entry 2's DCP, UNKNOWN and CDP tables differ
        (the entry grows by their 3 new rows); the other entries and the footer's parameters are the same."""
        self.patch()
        default = util.slurp(self.dst)
        os.unlink(self.dst)
        out = self.patch(argv=["patch", "--jeita", "thermal2", self.src, self.dst])
        image = util.slurp(self.dst)
        self.assertIn("%s dcp-jeita-temp-table = %s\n" % (BATTERY, pd.show(THERMAL2["dcp"])), out)
        header, entries = pd.parse_table(image)
        header0, entries0 = pd.parse_table(default)
        self.assertEqual(header[1], header0[1] + 96)
        self.assertEqual([e[2:] for e in entries], [e[2:] for e in entries0])
        for k in (0, 1):
            self.assertEqual(pd.entry_blob(image, entries, k), pd.entry_blob(default, entries0, k))
        a, b = parsed(entry(default)), parsed(entry(image))
        self.assertEqual(prop_diff(a, b), sorted((BATTERY, k + "-jeita-temp-table") for k in THERMAL2))
        self.assertEqual(len(entry(image)), len(entry(default)) + 96)
        # The stand-in's footer: the same salt, properties, options and key; a longer image.
        footer, footer0 = (json.loads(i[-4096:].rstrip(b"\0")) for i in (image, default))
        self.assertEqual(footer.pop("original"), footer0.pop("original") + 96)
        self.assertEqual(footer, footer0)
        proc = self.assertOk(util.run_script("patch_dtbo", "check", self.dst))
        self.assertIn("jeita tables thermal2 (the approved tables)", proc.stdout)
        # With --source-ma too: both edits.
        os.unlink(self.dst)
        self.patch(argv=["patch", "--source-ma", "1000", "--jeita", "thermal2", self.src, self.dst])
        root = parsed(entry(util.slurp(self.dst)))
        self.assertEqual(pd.source_ma_of(root), 1000)
        self.assertEqual(pd.check_jeita(root), "thermal2")

    def test_tested_jeita(self):
        """--tested applies the device's approved tables (thermal2), with or without a --jeita naming
        them; a --jeita naming any other is refused. --jeita needs a variant the patch has."""
        self.patch(jeita="thermal2")
        sha = hashlib.sha256(util.slurp(self.dst)).hexdigest()
        os.unlink(self.dst)
        for kw in (dict(), dict(jeita="thermal2"), dict(argv=["patch", "--tested", self.src, self.dst]),
                   dict(argv=["patch", "--tested", "--jeita", "thermal2", self.src, self.dst])):
            out = self.patch(tested=True, tested_sha256=sha, **kw)
            self.assertEqual(hashlib.sha256(util.slurp(self.dst)).hexdigest(), sha, kw)
            self.assertEqual(sum(1 for line in out.splitlines() if "-jeita-temp-table = " in line), 3, kw)
            os.unlink(self.dst)
        with self.assertRaises(SystemExit) as e:
            self.patch(tested=True, tested_sha256=sha, tested_jeita=None, jeita="thermal2")
        self.assertIn("--tested takes the RG Cube's approved jeita tables (stock) only; a --jeita thermal2 image is "
                      "a test image", str(e.exception))
        self.assertFalse(os.path.exists(self.dst))
        thermal3 = lambda full: pd.jeita_thermal2(min(full, 1800000))
        with mock.patch.dict(pd.JEITA, thermal3=thermal3), mock.patch.object(pd, "JEITA_EDITS", ["thermal2", "thermal3"]):
            for kw in (dict(jeita="thermal3"), dict(argv=["patch", "--tested", "--jeita", "thermal3", self.src, self.dst])):
                with self.assertRaises(SystemExit) as e:
                    self.patch(tested=True, tested_sha256=sha, **kw)
                self.assertIn("--tested takes the RG Cube's approved jeita tables (thermal2) only; a --jeita thermal3 "
                              "image is a test image", str(e.exception), kw)
                self.assertFalse(os.path.exists(self.dst))
        with self.assertRaises(SystemExit) as e:
            self.patch(jeita="stock")
        self.assertIn("--jeita stock is not one of thermal2", str(e.exception))
        for argv in (["patch", "--jeita", "thermal1", self.src, self.dst], ["patch", self.src, self.dst, "--jeita"],
                     ["patch", "--jeita", "stock", self.src, self.dst]):
            with self.assertRaises(SystemExit) as e:
                self.patch(argv=argv)
            self.assertIn("patch_dtbo.py patch [--tested]", str(e.exception), argv)
            self.assertFalse(os.path.exists(self.dst))
        self.assertFails(util.run_script("patch_dtbo", "tested", "--jeita", "thermal2"), "patch_dtbo.py patch")

    def test_patched_input(self):
        """An image the patch made that PATCHED_INPUTS lists: --jeita on it gives what --jeita on the stock
        image with the same offer gives; its offer stays, and another one is refused. --tested on the
        900 mA one (test 42 DK1) gives the approved image; on the 1000 mA one it is refused."""
        stock = self.src
        self.patch(argv=["patch", "--source-ma", "1000", stock, self.dst])
        made = self.dst
        self.patch(argv=["patch", "--source-ma", "1000", "--jeita", "thermal2", stock, self.path("direct.img")])
        self.patch(argv=["patch", stock, self.path("dk1.img")])
        self.patch(argv=["patch", "--jeita", "thermal2", stock, self.path("approved.img")])
        stock_sha = hashlib.sha256(util.slurp(stock)).hexdigest()
        made_sha = hashlib.sha256(util.slurp(made)).hexdigest()
        dk1_sha = hashlib.sha256(util.slurp(self.path("dk1.img"))).hexdigest()
        approved = hashlib.sha256(util.slurp(self.path("approved.img"))).hexdigest()
        device = pd.Device("RG Cube", 2, 0x2100, "dtbo", PARTITION, approved, "thermal2")
        inputs = {made_sha: (stock_sha, "a 1000 mA test image"), dk1_sha: (stock_sha, "a 900 mA test image")}

        def run(*argv):
            out = io.StringIO()
            with mock.patch.dict(os.environ, self.env), mock.patch.object(pd, "DEVICES", {stock_sha: device}), \
                    mock.patch.object(pd, "PATCHED_INPUTS", inputs), \
                    mock.patch.object(sys, "argv", ["patch_dtbo.py", "patch"] + list(argv)), \
                    contextlib.redirect_stdout(out):
                pd.main()
            return out.getvalue()

        out = run("--jeita", "thermal2", made, self.path("again.img"))
        self.assertIn("%s is a 1000 mA test image, patched from the RG Cube's stock dtbo\n" % made, out)
        self.assertEqual(util.slurp(self.path("again.img")), util.slurp(self.path("direct.img")))
        # Only the three tables changed.
        self.assertEqual(sum(1 for line in out.splitlines() if "-jeita-temp-table = " in line), 3)
        self.assertNotIn("source-pdos", out)
        run("--source-ma", "1000", made, self.path("same.img"))
        self.assertEqual(util.slurp(self.path("same.img")), util.slurp(made))
        with self.assertRaises(SystemExit) as e:
            run("--source-ma", "900", "--jeita", "thermal2", made, self.path("x.img"))
        self.assertIn("which offers 1000 mA; for --source-ma 900 patch the stock image", str(e.exception))
        self.assertFalse(os.path.exists(self.path("x.img")))
        out = run("--tested", self.path("dk1.img"), self.path("tested.img"))
        self.assertEqual(sum(1 for line in out.splitlines() if "-jeita-temp-table = " in line), 3)
        self.assertEqual(util.slurp(self.path("tested.img")), util.slurp(self.path("approved.img")))
        with self.assertRaises(SystemExit) as e:
            run("--tested", made, self.path("x.img"))
        self.assertIn("is not the image approved for the RG Cube (%s)" % approved, str(e.exception))
        self.assertFalse(os.path.exists(self.path("x.img")))
        # The approved image is not an input: patched again without --jeita it would keep thermal2.
        with self.assertRaises(SystemExit) as e:
            run(self.path("approved.img"), self.path("x.img"))
        self.assertIn("is not a dtbo this patch knows", str(e.exception))

    def test_already_patched_input(self):
        """An image the patch already made is not a known input unless PATCHED_INPUTS lists it."""
        self.patch()
        self.src = self.dst
        self.dst = self.path("again.img")
        with mock.patch.dict(os.environ, self.env), self.assertRaises(SystemExit) as e:
            pd.cmd_patch(self.src, self.dst, False)
        self.assertIn("is not a dtbo this patch knows", str(e.exception))

    def test_tested_command(self):
        """build.sh's early check for DOCK_FIX=1: the RG Cube's image is the one the owner chose to ship
        in test 42, TH2 (DK1's fix A and 900 mA offer, tested on the device with the dock, and the
        thermal2 tables, read back from the device's live tree); with none, it is refused."""
        self.assertOk(util.run_script("patch_dtbo", "tested"))
        self.assertEqual([(d.name, d.tested_sha256, d.tested_jeita) for d in pd.DEVICES.values()],
                         [("RG Cube", TH2_SHA256, "thermal2")])
        # PATCHED_INPUTS: images made from a DEVICES image, never an approved one.
        approved = {d.tested_sha256 for d in pd.DEVICES.values()}
        self.assertEqual(set(pd.PATCHED_INPUTS) & approved, set())
        self.assertIn(DK1_SHA256, pd.PATCHED_INPUTS)
        for stock, _ in pd.PATCHED_INPUTS.values():
            self.assertIn(stock, pd.DEVICES)
        with mock.patch.object(pd, "DEVICES", {k: d._replace(tested_sha256="") for k, d in pd.DEVICES.items()}), \
                mock.patch.object(sys, "argv", ["patch_dtbo.py", "tested"]), self.assertRaises(SystemExit) as e:
            pd.main()
        self.assertIn("no patched dtbo has been approved for the RG Cube", str(e.exception))


class RealAvbtool(util.TestCase):
    def test_patch_with_avbtool(self):
        """With AOSP's avbtool and test key: the self-check passes and the result verifies."""
        path = util.need(self, "avbtool")
        key = os.environ.get("AVB_KEY")
        if not key or not os.path.isfile(key):
            self.skipTest("AVB_KEY names no key")
        env = {"PATH": os.pathsep.join(path + [os.environ["PATH"]])}
        src = self.write("stock.img", fx.cube_dtbo(fx.fdt(fx.overlay())))
        self.assertOk(util.run(["avbtool", "add_hash_footer", "--image", src, "--partition_name", "dtbo",
                                "--partition_size", str(PARTITION), "--key", key, "--salt", SALT,
                                "--algorithm", "SHA256_RSA4096", "--prop", FINGERPRINT], env=env))
        # verify_image checks the hash against <partition name>.img beside the image.
        dst = self.path("dtbo.img")
        sha = hashlib.sha256(util.slurp(src)).hexdigest()
        device = pd.Device("RG Cube", 2, 0x2100, "dtbo", PARTITION, "")
        with mock.patch.dict(os.environ, env), mock.patch.object(pd, "DEVICES", {sha: device}), \
                contextlib.redirect_stdout(io.StringIO()):
            pd.cmd_patch(src, dst, False)
        self.assertOk(util.run(["avbtool", "verify_image", "--image", dst, "--key", key], env=env))
        info = self.assertOk(util.run(["avbtool", "info_image", "--image", dst], env=env)).stdout
        self.assertIn(SALT, info)
        self.assertIn(FINGERPRINT.replace(":", " -> '", 1), info)

    def test_shipped_image(self):
        """build.sh's DOCK_FIX=1 step on the real stock image: `patch --tested` on the RG Cube's stock
        dtbo (out/.dock/dtbo_a.img) gives tested_sha256, byte for byte the TH2 image that runs on the
        Cube in test 42 (out/dp-evidence/test42/thermal/dtbo_a-DK1-TH2.img), so a patch_dtbo.py change
        that no longer makes it fails here rather than late in a build. `patch` without --tested and
        --jeita still gives DK1 (out/dp-evidence/test42/dock/dtbo_a-DK1.img). Without one of the
        two device images, that image's byte comparison is left out (the sha256 checks still run).
        Skips without the stock image, avbtool (PATH or the AOSP host build) or the key (AVB_KEY,
        else the AOSP tree's test key)."""
        stock = os.path.join(util.ROOT, "out", ".dock", "dtbo_a.img")
        ran = os.path.join(util.ROOT, "out", "dp-evidence", "test42", "thermal", "dtbo_a-DK1-TH2.img")
        dk1 = os.path.join(util.ROOT, "out", "dp-evidence", "test42", "dock", "dtbo_a-DK1.img")
        if not os.path.isfile(stock):
            self.skipTest("no %s" % stock)
        path = util.need(self, "avbtool")
        key = os.environ.get("AVB_KEY") or os.path.join(util.AOSP, "external", "avb", "test", "data",
                                                         "testkey_rsa4096.pem")
        if not os.path.isfile(key):
            self.skipTest("no AVB key (AVB_KEY, or %s)" % key)
        device = pd.DEVICES.get(hashlib.sha256(util.slurp(stock)).hexdigest())
        self.assertIsNotNone(device, "%s is not a stock dtbo patch_dtbo.py knows" % stock)
        dst = self.path("dtbo.img")
        proc = util.run_script("patch_dtbo", "patch", "--tested", stock, dst,
                               env={"AVB_KEY": key}, path_prepend=path, timeout=300)
        if proc.returncode and "re-signing the original does not give it back" in proc.stderr:
            # Another avbtool than the stock image's (the AOSP host build's, say) signs it
            # differently; the build's Docker image has the one that reproduces it.
            self.skipTest("this avbtool and key don't reproduce the stock dtbo's signature")
        self.assertOk(proc)
        self.assertIn("(sha256 %s)" % device.tested_sha256, proc.stdout)
        self.assertEqual(hashlib.sha256(util.slurp(dst)).hexdigest(), device.tested_sha256)
        if os.path.isfile(ran):
            self.assertEqual(util.slurp(dst), util.slurp(ran))
        self.assertIn("jeita tables thermal2 (the approved tables)",
                      self.assertOk(util.run_script("patch_dtbo", "check", dst)).stdout)
        plain = self.path("dk1.img")
        self.assertOk(util.run_script("patch_dtbo", "patch", stock, plain,
                                      env={"AVB_KEY": key}, path_prepend=path, timeout=300))
        self.assertEqual(hashlib.sha256(util.slurp(plain)).hexdigest(), DK1_SHA256)
        if os.path.isfile(dk1):
            self.assertEqual(util.slurp(plain), util.slurp(dk1))


class Build(util.TestCase):
    def test_build_sh(self):
        """DOCK_FIX defaults to 1 (test 40, DOCK_FIX=0 opts out); with 1, build.sh refuses early
        without an approved image or fdtoverlay, and later patches DTBO from the base PAC into both
        slots, after vendor_boot, checking the merge with the patched vendor_boot the build ships."""
        text = util.slurp(os.path.join(util.SCRIPTS, "build.sh"), text=True)
        self.assertIn('DOCK_FIX="${DOCK_FIX:-1}"\n', text)
        self.assertIn("\n  0) ;;\n", text)
        early = text.index('python3 -I "$ROOT/scripts/patch_dtbo.py" tested')
        self.assertLess(early, text.index("\nsudo -v\n"))
        # Without fdtoverlay the merge check would be skipped, not failed.
        fdt = text.index('[[ "$DOCK_FIX" == 1 ]] && ! command -v fdtoverlay')
        self.assertLess(fdt, text.index("\nsudo -v\n"))
        step = text.index('if [[ "$DOCK_FIX" == 1 ]]; then\n')
        self.assertGreater(step, text.index('replace "$pac.partial" "$slot" "$vendor_boot.patched"'))
        self.assertLess(step, text.index('scripts/pactool.py" fixcrc "$pac.partial"'))
        block = text[step:text.index("\nfi\n", step)]
        self.assertIn('pactool.py" extract "$base_pac" DTBO "$dtbo"', block)
        self.assertIn('patch_dtbo.py" patch --tested "$dtbo" "$dtbo.patched"', block)
        self.assertIn('patch_dtbo.py" check "$dtbo.patched" "$vendor_boot.patched"', block)
        # The patched vendor_boot it checks against goes only after the step.
        self.assertGreater(text.index('rm -f "$vendor_boot.patched"'), step + len(block))
        self.assertIn("for slot in DTBO DTBO_B; do", block)

    def test_all_modes_only_with_the_1_4_4_system(self):
        text = util.slurp(os.path.join(util.SCRIPTS, "build.sh"), text=True)
        self.assertRegex(text, r'if \[\[ -n "\$\{SYSTEM_IMG:-\}" \]\]; then\n  export THIMSY_DP_ALL_MODES=1\n'
                               r'else\n  export THIMSY_DP_ALL_MODES=0\nfi\n')
        self.assertEqual(len(re.findall("THIMSY_DP_ALL_MODES=", text)), 2)
