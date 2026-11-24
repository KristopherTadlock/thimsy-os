"""scripts/patch_dtbo.py and scripts/avb_footer.py, on small device trees built here
(tests/fixtures/dtbo.py), and build.sh's dtbo step."""
import contextlib
import hashlib
import io
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


class Fdt(util.TestCase):
    def test_round_trip(self):
        for t in (fx.overlay(), fx.patched_overlay(), ("", [], [])):
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
        self.assertEqual(tree(parsed(blob)), norm(fx.patched_overlay()))
        self.assertEqual(len(changes), 9)
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
        self.assertTrue(root.child("__fixups__").prop("pmic_typec").endswith(b"\0/fragment@7:target:0\0"))
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

    def test_already_patched_is_left_alone(self):
        blob = fx.fdt(fx.patched_overlay())
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
        pd.check_entry(fx.fdt(fx.patched_overlay()))

    def test_unpatched_fails(self):
        with self.assertRaises(SystemExit) as e:
            pd.check_entry(fx.fdt(fx.overlay()))
        self.assertIn("not patched", str(e.exception))

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
            t = norm(fx.patched_overlay())
            for k in t[2]:
                if k[0] == "fragment@4":
                    k[2][0][1].extend(props)
            with self.assertRaises(SystemExit) as e:
                pd.check_entry(fx.fdt(t))
            self.assertIn(message, str(e.exception), props)

    def test_check_command(self):
        image = self.write("dtbo.img", fx.cube_dtbo(fx.fdt(fx.patched_overlay())))
        proc = self.assertOk(util.run_script("patch_dtbo", "check", image))
        self.assertIn("RG Cube entry 2 is patched", proc.stdout)
        stock = self.write("stock.img", fx.cube_dtbo(fx.fdt(fx.overlay())))
        self.assertFails(util.run_script("patch_dtbo", "check", stock), "not patched")
        other = self.write("other.img", fx.dt_table([fx.fdt(fx.patched_overlay())], [0x2100]))
        self.assertFails(util.run_script("patch_dtbo", "check", other), "no entry of a known board")

    def test_merge_check(self):
        """fdtoverlay applies the patched entry to the base DT in vendor_boot, with every edit at its node."""
        util.need(self, "fdtoverlay")
        base = self.write("vendor_boot.img", fx.vendor_boot(fx.base_dt()))
        self.assertEqual(pd.vendor_boot_dtb(util.slurp(base)), fx.base_dt())
        image = self.write("dtbo.img", fx.cube_dtbo(fx.fdt(fx.patched_overlay())))
        proc = self.assertOk(util.run_script("patch_dtbo", "check", image, base))
        for line in ("/soc/aon/spi@400000/pmic@0/typec@380 use_pdhub_c2c = (true)",
                     "/soc/ipa-apb/usb3@25100000 use_pdhub_c2c", "/soc/aon/usb@a00000 use_pdhub_c2c",
                     '/soc/aon/spi@400000/pmic@0/pd@e00/connector data-role = "dual"',
                     "/soc/aon/spi@400000/pmic@0/pd@e00 vbus-supply = <0x"):
            self.assertIn(line, proc.stdout)
        # Without its local fixup, vbus-supply would point at a base node after the merge.
        t = norm(fx.patched_overlay())
        for k in t[2]:
            if k[0] == "__local_fixups__":
                k[2][:] = [c for c in k[2] if c[0] != "fragment@5"]
        broken = fx.cube_dtbo(fx.fdt(t))
        blob = entry(broken)
        with self.assertRaises(SystemExit) as e, contextlib.redirect_stdout(io.StringIO()):
            pd.merge_check(blob, base)
        self.assertIn("vbus-supply", str(e.exception))

    def test_merge_check_skipped_without_fdtoverlay(self):
        base = self.write("vendor_boot.img", fx.vendor_boot(fx.base_dt()))
        image = self.write("dtbo.img", fx.cube_dtbo(fx.fdt(fx.patched_overlay())))
        proc = self.assertOk(util.run_script("patch_dtbo", "check", image, base, env={"PATH": "/nonexistent"}))
        self.assertIn("merge check skipped: no fdtoverlay on PATH", proc.stdout)

    def test_vendor_boot_dtb_errors(self):
        for data, message in ((b"NOTVNDR!" + bytes(4096), "not a vendor boot image"),):
            with self.assertRaises(SystemExit) as e:
                pd.vendor_boot_dtb(data)
            self.assertIn(message, str(e.exception))


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

    def patch(self, tested=False, tested_sha256=""):
        device = pd.Device("RG Cube", 2, 0x2100, "dtbo", PARTITION, tested_sha256)
        sha = hashlib.sha256(util.slurp(self.src)).hexdigest()
        out = io.StringIO()
        with mock.patch.dict(os.environ, self.env), mock.patch.object(pd, "DEVICES", {sha: device}), \
                contextlib.redirect_stdout(out):
            pd.cmd_patch(self.src, self.dst, tested)
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
        with self.assertRaises(SystemExit) as e:
            self.patch(tested=True)
        self.assertIn("is not the image approved for the RG Cube (none yet)", str(e.exception))
        self.assertFalse(os.path.exists(self.dst))
        self.patch()
        sha = hashlib.sha256(util.slurp(self.dst)).hexdigest()
        os.unlink(self.dst)
        self.patch(tested=True, tested_sha256=sha)
        self.assertEqual(hashlib.sha256(util.slurp(self.dst)).hexdigest(), sha)

    def test_already_patched_input(self):
        """An image the patch already made is not a known input (its sha256 is not in the table)."""
        self.patch()
        self.src = self.dst
        self.dst = self.path("again.img")
        with mock.patch.dict(os.environ, self.env), self.assertRaises(SystemExit) as e:
            pd.cmd_patch(self.src, self.dst, False)
        self.assertIn("is not a dtbo this patch knows", str(e.exception))

    def test_tested_command(self):
        """build.sh's early check for DOCK_FIX=1: the RG Cube's image is the one the owner approved for
        test 40 (without a device test); with none, it is refused."""
        self.assertOk(util.run_script("patch_dtbo", "tested"))
        self.assertEqual([(d.name, d.tested_sha256) for d in pd.DEVICES.values()],
                         [("RG Cube", "94c0f3d4bb1fbbf518a0461db39908d02fd67e320e52c45ced540076d09c5536")])
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
