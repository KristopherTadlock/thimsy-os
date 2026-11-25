"""thimsy-import --rocknix DIR --tag TAG --family RK3566 [--out linux/] [--check]

Reads one ROCKNIX tree at one tag and writes one family's hardware layer:

  <out>/generated/<family>/      rocknix.json, options.json, catalog.json, firmware.json,
                                 defconfig.fragment, linux-firmware.fragment, linux.hash,
                                 guards.json, devices/<id>.json            (Thimsy's data)
  <out>/gpl/generated/<family>/  linux/patches/NN-<dir>/<ROCKNIX name>.patch, linux/series,
                                 linux/config, dts/<vendor>/*, PROVENANCE  (GPL-2.0)

Exit codes: 0 done (or --check found no difference); 1 --check found differences;
2 bad input or an unknown ROCKNIX shape; 3 a hash guard changed; 4 the conformance check
of Thimsy's kernel fragment failed.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import OrderedDict
from pathlib import Path

from . import (configxml, devicejson, dts, emit, firmware, guards, inputplumber, kconfig,
               linuxmk, patches, quirks)
from .options import Options, words
from .rocknix import GuardStop, ImportError_, RocknixTree

TOOL_DIR = Path(__file__).resolve().parents[1]
CRITICAL = TOOL_DIR / "kernel-critical.conf"
BR_EXT = "$(BR2_EXTERNAL_THIMSY_PATH)"

_SOC_TOKEN = re.compile(r"^(rk\d{4}s?|sun\d+i|h\d{3}|meson|g12[ab]|sm\d{4}|sdm\d{3}|qcs\d{4}|cq\d{4}s?)$")


class Result:
    def __init__(self):
        self.files: "OrderedDict[str, bytes]" = OrderedDict()
        self.report: list = []
        self.guard_diffs: list = []
        self.conformance = None
        self.fragment_used = None

    def add(self, rel: str, data):
        if isinstance(data, str):
            data = data.encode("utf-8")
        self.files[rel] = data

    def say(self, msg: str):
        if msg not in self.report:
            self.report.append(msg)


def product_id(dtb: str) -> str:
    toks = dtb.split("-")
    while len(toks) > 1 and _SOC_TOKEN.match(toks[0]):
        toks = toks[1:]
    return "-".join(toks)


def _norm_name(s: str) -> str:
    return re.sub(r"[^a-z0-9]", "", s.lower())


def _jdump(obj) -> str:
    return json.dumps(obj, indent=2, ensure_ascii=False) + "\n"


def _provenance_after_spdx(text: str, line: str) -> str:
    lines = text.split("\n")
    if lines and "SPDX-License-Identifier" in lines[0]:
        return "\n".join([lines[0], line] + lines[1:])
    return line + "\n" + text


def build(args) -> Result:
    res = Result()
    tree = RocknixTree(args.rocknix, args.tag, args.commit)
    family = args.family.upper()
    fam = family.lower()
    arch = args.arch
    gen = "generated/%s" % fam
    gpl = "gpl/generated/%s" % fam

    # -- options ---------------------------------------------------------------------
    opts = Options()
    for p in (tree.project / "options", tree.device_dir(family) / "options"):
        if tree.resolve(p) is not None:
            opts.read(tree.read_text(p), tree.rel(p), arch)
    if not tree.device_dir(family).is_dir():
        raise ImportError_("no device directory for %s" % family)
    linux_var = opts.get("LINUX", "") or ""
    target_cpu = opts.get("TARGET_CPU", "") or ""
    patch_arch = opts.get("TARGET_KERNEL_PATCH_ARCH") or arch
    for line in opts.ignored:
        res.say("options: ignored shell line: %s" % line)

    # -- config.xml --------------------------------------------------------------------
    cx_path = tree.project / "config.xml"
    families = configxml.parse(tree.read_text(cx_path))
    if family not in families:
        raise ImportError_("%s is not in %s" % (family, tree.rel(cx_path)))
    cfam = families[family]

    # -- packages/linux/package.mk -----------------------------------------------------
    mk_path = tree.linux_pkg_dir() / "package.mk"
    mk = linuxmk.LinuxMk(tree.read_text(mk_path), tree.rel(mk_path))
    ver = mk.version(family)
    version = ver["version"]
    pdirs = mk.patch_dirs(family, linux_var)

    # -- base kernel config --------------------------------------------------------------
    cfg = kconfig.config_path(tree, family, version, linux_var, arch)
    cfg_rel = tree.rel(cfg)
    base_text = tree.read_text(cfg)
    base = kconfig.parse(base_text)
    cfg80211 = base.get("CONFIG_CFG80211") == "y"

    # -- guards ----------------------------------------------------------------------------
    blocks = mk.guard_blocks(family, cfg80211)
    unpack_path = tree.root / "scripts" / "unpack"
    unpack_text = tree.read_text(unpack_path) if tree.resolve(unpack_path) is not None else None
    ub = linuxmk.unpack_patch_block(unpack_text, "scripts/unpack")
    if ub is None:
        ub = linuxmk.Block("scripts/unpack:patch-order", "scripts/unpack", None, 0, "unpack")
        res.say("guard: scripts/unpack is not in the tree; its patch order is assumed (recorded as absent)")
    blocks.append(ub)
    qmk = quirks.package_mk(tree)
    if qmk is not None:
        qm = linuxmk.LinuxMk(tree.read_text(qmk), tree.rel(qmk), require_case=False)
        blocks.extend(qm.family_if_blocks(family))
    current = guards.compute(blocks)
    recorded = guards.load(Path(args.out) / gen / "guards.json")
    res.guard_diffs = guards.compare(recorded, current)
    if recorded is None:
        res.say("guards: none recorded yet; this import records them")

    # -- patches ---------------------------------------------------------------------------
    series = patches.flatten(tree, family, version, pdirs, patch_arch, target_cpu)
    series_lines = []
    for p in series:
        rel = "%s/linux/patches/%s/%s" % (gpl, p.dest_dir, p.name)
        res.add(rel, patches.render(tree, p))
        series_lines.append("%s/%s" % (p.dest_dir, p.name))
    res.add("%s/linux/series" % gpl, "".join(l + "\n" for l in series_lines))
    patch_dirs_out = []
    for p in series:
        if p.dest_dir not in patch_dirs_out:
            patch_dirs_out.append(p.dest_dir)
    bare = [p.name for p in series if not p.has_from]
    res.say("patches: %d in %d directories, %d without a From: line" % (len(series), len(patch_dirs_out), len(bare)))

    prov = ["# Files written by thimsy-import from ROCKNIX (https://github.com/ROCKNIX/distribution),",
            "# tag %s, commit %s. Used under ROCKNIX's licence, GPL-2.0 (gpl/COPYING); no author is" % (tree.tag, tree.commit),
            "# recovered for patches without a From: line (open question 22). Each line: file <- ROCKNIX path.",
            ""]
    # -- kernel config ---------------------------------------------------------------------
    cfg_line = "# " + patches.PROVENANCE_PREFIX + tree.provenance(cfg_rel) + "; GPL-2.0"
    res.add("%s/linux/config" % gpl, cfg_line + "\n" + base_text)
    prov.append("linux/config <- %s" % cfg_rel)
    for p in series:
        prov.append("linux/patches/%s/%s <- %s" % (p.dest_dir, p.name, p.rel))

    # -- device trees ------------------------------------------------------------------------
    prefix = cfam.dtb_prefix
    dts_dir = tree.device_dir(family) / "linux" / "dts"
    oot = dts.DtsSource()
    dts_written = []
    if dts_dir.is_dir():
        for p in sorted(dts_dir.rglob("*"), key=lambda q: q.as_posix()):
            if not p.is_file() or not re.search(r"\.(dts|dtsi|h)$", p.name):
                continue
            sub = p.relative_to(dts_dir).as_posix()
            text = tree.read_text(p)
            oot.add(p.name, text, tree.rel(p))
            line = "// " + patches.PROVENANCE_PREFIX + tree.provenance(tree.rel(p)) + "; GPL-2.0"
            res.add("%s/dts/%s" % (gpl, sub), _provenance_after_spdx(text, line))
            prov.append("dts/%s <- %s" % (sub, tree.rel(p)))
            dts_written.append(sub)
    from_patches = dts.DtsSource()
    family_joy = set()
    for p in series:
        for name, text in dts.from_patch(p.data):
            from_patches.add(name, text, p.rel)
        family_joy.update(dts.joypad_names_in_patch(p.data))
    for name in oot.files:
        n = dts.facts(oot.files[name][0]).get("joypad", {}).get("name")
        if n:
            family_joy.add(n)
    ksrc = None
    if args.kernel_src:
        ksrc = Path(args.kernel_src) / "arch" / "arm64" / "boot" / "dts" / prefix
    res.add("%s/PROVENANCE" % gpl, "\n".join(prov) + "\n")

    # -- model resolution ----------------------------------------------------------------------
    composites, maps = inputplumber.read_family(tree, family)
    candidates = set(quirks.device_models(tree))
    for c in composites:
        candidates.update(c.models)
    for src in (oot, from_patches):
        for name in src.files:
            m = dts.facts(src.expanded(name) or "").get("model")
            if m:
                candidates.add(m)
    by_norm: dict = {}
    for c in sorted(candidates):
        by_norm.setdefault(_norm_name(c), []).append(c)

    def resolve_model(dtb: str):
        for src, kind in ((oot, "dts"), (from_patches, "patch")):
            text = src.expanded(dtb + ".dts")
            if text:
                f = dts.facts(text)
                if f.get("model"):
                    return f["model"], kind, f, src.files[dtb + ".dts"][1]
        if ksrc is not None and (ksrc / (dtb + ".dts")).is_file():
            ks = dts.DtsSource()
            for q in sorted(ksrc.glob("*.dts*")):
                ks.add(q.name, q.read_text(encoding="utf-8", errors="replace"), "kernel-src")
            f = dts.facts(ks.expanded(dtb + ".dts") or "")
            if f.get("model"):
                return f["model"], "kernel-src", f, None
        hits = by_norm.get(_norm_name(product_id(dtb)), [])
        if len(hits) == 1:
            return hits[0], "name-match", OrderedDict(), None
        return None, None, OrderedDict(), None

    # -- firmware ---------------------------------------------------------------------------
    dat = tree.device_dir(family) / "config" / "kernel-firmware.dat"
    dat_text = tree.read_text(dat) if tree.resolve(dat) is not None else None
    builtin = []
    efb = mk.extra_firmware_block(family)
    if efb is not None:
        for pkg, src, dest in firmware.parse_builtin_block(efb.lines, efb.name):
            builtin.append((pkg, src, dest, efb.name))
    cfg_block = next((b.name for b in mk.cfg80211_blocks() if ":pre_make_target:" in b.name), None) \
        if cfg80211 else None
    opt_fw = [(pkg, "%s FIRMWARE" % tree.rel(tree.device_dir(family) / "options"))
              for pkg in words(opts.get("FIRMWARE"))]
    whence = None
    if args.whence:
        whence = firmware.parse_whence(Path(args.whence).read_text(encoding="utf-8", errors="replace"))
    fw = firmware.collect(dat_text, tree.rel(dat) if dat_text is not None else None,
                          builtin, cfg_block, opt_fw, whence)
    fw_doc = OrderedDict([("family", fam), ("rocknix_commit", tree.commit),
                          ("policy", "D24: ship known licences only; the rest is fetched from the user's device or left out"),
                          ("whence", "given" if whence is not None else "not given"),
                          ("entries", [e.to_json() for e in fw])])
    res.add("%s/firmware.json" % gen, _jdump(fw_doc))
    for e in fw:
        if e.decision == "exclude":
            res.say("firmware: excluded %s %s%s (%s)" % (e.package, e.path, " [built in]" if e.builtin else "",
                                                         e.reason))
    extra = firmware.extra_firmware_value(fw)
    frag = ["# Generated by thimsy-import from ROCKNIX %s (%s) for %s. Do not edit." % (tree.tag, tree.commit[:12], family),
            "# Firmware built into the kernel that passed the firmware policy (decision 24).",
            'CONFIG_EXTRA_FIRMWARE="%s"' % extra]
    if extra:
        frag.append('CONFIG_EXTRA_FIRMWARE_DIR="external-firmware"')
    res.add("%s/linux-firmware.fragment" % gen, "\n".join(frag) + "\n")

    # -- devices ---------------------------------------------------------------------------
    schema = devicejson.load_schema(Path(args.schema)) if args.schema else devicejson.load_schema()
    products = []
    unresolved = []
    for dtb, images in cfam.dtbs():
        pid = product_id(dtb)
        model, msrc, dfacts, dts_origin = resolve_model(dtb)
        entry = OrderedDict([("id", pid), ("dtb", "%s/%s.dtb" % (prefix, dtb) if prefix else dtb + ".dtb"),
                             ("images", [i for i in images if i]), ("model", model), ("model_source", msrc)])
        products.append(entry)
        if model is None:
            unresolved.append(dtb)
            continue
        doc = OrderedDict()
        doc["schema_version"] = 1
        doc["id"] = pid
        doc["family"] = fam
        if opts.get("HW_CPU"):
            doc["soc"] = opts.get("HW_CPU")
        doc["model"] = model
        doc["model_source"] = msrc
        doc["dtb"] = entry["dtb"]
        doc["images"] = entry["images"]
        notes = OrderedDict()
        if msrc == "name-match":
            notes["/model"] = "guess: matched from the DTB name against ROCKNIX's model strings"
        ip_used = []
        comp = next((c for c in composites if model in c.models), None)
        if comp is not None:
            pad, pnotes, used = inputplumber.pad_facts(comp, maps)
            doc["pad"] = pad
            ip_used = [comp.path] + used
            if pnotes:
                notes["/pad"] = "; ".join(pnotes)
        elif "joypad" in dfacts:
            jp = dfacts["joypad"]
            if not jp.get("name") and len(family_joy) == 1:
                jp["name"] = sorted(family_joy)[0]
                notes["/pad/sources"] = ("guess: the device tree names no joypad-name; %r is the only one "
                                         "the family's device trees and patches set" % jp["name"])
            if not jp.get("name"):
                raise ImportError_("%s: a rocknix joypad with no joypad-name" % dtb)
            sid = re.sub(r"[^a-z0-9]+", "-", (jp.get("name") or "joypad").lower()).strip("-")
            pad = OrderedDict([("producer", "kernel-joypad"),
                               ("sources", [OrderedDict([("id", sid), ("role", "buttons+sticks"),
                                                         ("match", OrderedDict([("name", jp.get("name", ""))])),
                                                         ("grab", True), ("hide", True)])]),
                               ("buttons", []), ("face_buttons", 4)])
            sticks = []
            amux = jp.get("amux_count")
            if amux is not None:
                for n_, (sx, sy, tx, ty) in enumerate((("ABS_X", "ABS_Y", "ABS_X", "ABS_Y"),
                                                       ("ABS_RX", "ABS_RY", "ABS_RX", "ABS_RY"))):
                    if amux >= 2 * (n_ + 1):
                        sticks.append(OrderedDict([("name", "left" if n_ == 0 else "right"), ("source", sid),
                                                   ("x", OrderedDict([("code", sx), ("to", tx)])),
                                                   ("y", OrderedDict([("code", sy), ("to", ty)]))]))
                pad["sticks"] = sticks
            doc["pad"] = pad
            notes["/pad"] = ("kernel joypad (%s): buttons pass through; face_buttons 4 is a guess"
                             % jp.get("driver", "rocknix-joypad"))
            if jp.get("invert"):
                notes["/pad/sticks"] = ("the driver inverts %s itself (invert-abs* in the device tree); "
                                        "padd must not invert again" % ", ".join(jp["invert"]))
        qf = quirks.for_device(tree, family, model)
        quirks.apply_to_device(doc, qf.facts)
        for r in qf.rejected:
            res.say("quirks: rejected (needs the shell): %s" % r)
        doc["status"] = "imported"
        if qf.facts:
            doc["quirk_facts"] = OrderedDict((k, qf.facts[k]) for k in sorted(qf.facts))
        rx = OrderedDict([("tag", tree.tag), ("commit", tree.commit)])
        if dts_origin:
            rx["dts"] = dts_origin
        if qf.sources:
            rx["quirk_sources"] = qf.sources
        if ip_used:
            rx["inputplumber"] = ip_used
        doc["rocknix"] = rx
        if notes:
            doc["notes"] = notes
        errs = devicejson.check(doc, schema)
        if errs:
            raise ImportError_("device.json for %s fails the schema:\n  %s" % (pid, "\n  ".join(errs)))
        entry["device_json"] = "devices/%s.json" % pid
        res.add("%s/devices/%s.json" % (gen, pid), devicejson.dumps(doc))
    for d in unresolved:
        res.say("catalog: no model for %s (no device tree source; pass --kernel-src)" % d)
    cat = OrderedDict([("family", fam), ("rocknix_tag", tree.tag), ("rocknix_commit", tree.commit),
                       ("dtb_prefix", prefix), ("images", [i.to_json() for i in cfam.images]),
                       ("products", products)])
    res.add("%s/catalog.json" % gen, _jdump(cat))

    # -- options, rocknix.json, defconfig fragment, hash --------------------------------------
    res.add("%s/options.json" % gen, _jdump(opts.to_json()))
    handport = mk.handport(family)
    for h in handport:
        res.say("handport: %s: %s" % (h["block"], h["does"]))
    rj = OrderedDict()
    rj["family"] = family
    rj["tag"] = tree.tag
    rj["commit"] = tree.commit
    rj["linux"] = OrderedDict([("version", version), ("sha256", ver.get("sha256", "")), ("url", ver.get("url", ""))])
    if "git_branch" in ver:
        rj["linux"]["git_branch"] = ver["git_branch"]
    rj["linux"]["LINUX"] = linux_var
    rj["linux"]["patch_dirs"] = pdirs
    rj["linux"]["patch_arch"] = patch_arch
    rj["linux"]["config"] = cfg_rel
    rj["linux"]["patches"] = len(series)
    rj["linux"]["patches_without_from"] = len(bare)
    rj["dts"] = dts_written
    rj["handport"] = handport
    res.add("%s/rocknix.json" % gen, _jdump(rj))

    url = ver.get("url", "")
    df = ["# Generated by thimsy-import from ROCKNIX %s (%s) for %s. Do not edit." % (tree.tag, tree.commit[:12], family),
          "BR2_LINUX_KERNEL=y"]
    if re.match(r"^https://www\.kernel\.org/pub/linux/kernel/", url):
        if version.count(".") == 1:
            df.append("# Q34: ROCKNIX pins an unpatched %s; Thimsy takes the series' last stable point release" % version)
        df += ["BR2_LINUX_KERNEL_CUSTOM_VERSION=y", 'BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE="%s"' % version]
    else:
        df += ["BR2_LINUX_KERNEL_CUSTOM_TARBALL=y", 'BR2_LINUX_KERNEL_CUSTOM_TARBALL_LOCATION="%s"' % url]
    df.append('BR2_LINUX_KERNEL_PATCH="%s"' % " ".join(
        "%s/%s/linux/patches/%s" % (BR_EXT, gpl, d) for d in patch_dirs_out))
    df += ["BR2_LINUX_KERNEL_USE_CUSTOM_CONFIG=y",
           'BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE="%s/%s/linux/config"' % (BR_EXT, gpl),
           'BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES="%s/board/%s/linux.fragment %s/%s/linux-firmware.fragment"'
           % (BR_EXT, fam, BR_EXT, gen)]
    if (opts.get("KERNEL_TARGET") or "Image") == "Image":
        df.append("BR2_LINUX_KERNEL_IMAGE=y")
    df.append("BR2_LINUX_KERNEL_DTS_SUPPORT=y")
    df.append('BR2_LINUX_KERNEL_INTREE_DTS_NAME="%s"' % " ".join(
        ("%s/%s" % (prefix, d) if prefix else d) for d, _ in cfam.dtbs()))
    if dts_written:
        df.append('BR2_LINUX_KERNEL_CUSTOM_DTS_DIR="%s/%s/dts"' % (BR_EXT, gpl))
    res.add("%s/defconfig.fragment" % gen, "\n".join(df) + "\n")
    if ver.get("sha256"):
        res.add("%s/linux.hash" % gen, "# From ROCKNIX %s\nsha256  %s  %s\n"
                % (mk.path, ver["sha256"], url.rsplit("/", 1)[-1]))

    # -- conformance -----------------------------------------------------------------------
    critical = kconfig.read_critical(Path(args.critical) if args.critical else CRITICAL, family)
    frag_path = Path(args.fragment) if args.fragment else Path(args.out) / "board" / fam / "linux.fragment"
    fragment = None
    if args.fragment != "none" and frag_path.is_file():
        fragment = kconfig.parse(frag_path.read_text(encoding="utf-8"))
        res.fragment_used = str(frag_path)
    res.conformance = kconfig.conformance(base, fragment, critical)
    for v in res.conformance["violations"]:
        res.say("conformance: %s want %s, base %s%s (%s)" % (
            v["option"], v["want"], v["base"],
            (", fragment " + v["fragment"]) if "fragment" in v else "", v["problem"]))

    # -- guards file last, so it reflects exactly this import --------------------------------
    res.add("%s/guards.json" % gen, _jdump(guards.document(family, tree.tag, tree.commit, current)))
    res.managed = [gen, gpl]
    return res


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="thimsy-import", description=__doc__.split("\n\n")[1].strip())
    ap.add_argument("--rocknix", required=True, help="a ROCKNIX checkout (the directory holding projects/ROCKNIX)")
    ap.add_argument("--tag", required=True, help="the ROCKNIX tag (or branch name) being imported")
    ap.add_argument("--family", required=True, help="ROCKNIX family, e.g. RK3566")
    ap.add_argument("--out", default="linux", help="the linux/ tree to write into (default: linux)")
    ap.add_argument("--check", action="store_true", help="report differences, write nothing")
    ap.add_argument("--commit", help="the tree's commit, when it has no readable .git")
    ap.add_argument("--whence", help="linux-firmware's WHENCE file, for firmware licences")
    ap.add_argument("--kernel-src", help="a kernel tree at the family's version, for in-tree device-tree models")
    ap.add_argument("--fragment", help="Thimsy's kernel fragment (default <out>/board/<family>/linux.fragment; 'none' to skip)")
    ap.add_argument("--critical", help="Thimsy-critical kernel options (default: kernel-critical.conf beside the tool)")
    ap.add_argument("--schema", help="device.schema.json (default: linux/schema/device.schema.json)")
    ap.add_argument("--arch", default="aarch64", help=argparse.SUPPRESS)
    ap.add_argument("--accept-guards", action="store_true",
                    help="record changed hash guards after the hand-port was reviewed")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args(argv)

    def out(msg):
        if not args.quiet:
            print(msg)

    try:
        res = build(args)
    except GuardStop as e:
        print("thimsy-import: %s" % e, file=sys.stderr)
        return 3
    except ImportError_ as e:
        print("thimsy-import: %s" % e, file=sys.stderr)
        return 2
    for line in res.report:
        out(line)
    if res.guard_diffs and not args.accept_guards:
        print("thimsy-import: ROCKNIX's imperative blocks changed; re-check their translation, then "
              "re-run with --accept-guards:", file=sys.stderr)
        for d in res.guard_diffs:
            print("  " + d, file=sys.stderr)
        return 3
    if res.fragment_used and res.conformance["violations"]:
        print("thimsy-import: the kernel fragment %s fails the conformance check" % res.fragment_used,
              file=sys.stderr)
        return 4
    plan = emit.sync(Path(args.out), res.managed, res.files, check=args.check)
    for line in plan.lines():
        out(line)
    if args.check:
        out("check: %s" % ("no differences" if plan.clean else "%d difference(s)" % len(plan.lines())))
        return 0 if plan.clean else 1
    out("wrote %d, removed %d, unchanged %d" % (len(plan.added) + len(plan.changed), len(plan.removed),
                                              len(res.files) - len(plan.added) - len(plan.changed)))
    return 0
