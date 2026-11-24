#!/usr/bin/env python3
"""What stands between Nano's source and an NDK build (android_launcher_plan.md, Phase 0, item 1).

    ndk_probe.py NANO_DIR NDK_DIR OUT_DIR [--api 29] [--jobs N]

NANO_DIR is frameworks/base/cmds/gammaos-nano as tools/android-launcher/fetch_nano.sh leaves it.
For every source file of the gammaos-nano binary (Android.bp's first srcs list) it records:

  1. the headers it includes, directly or through Nano's own headers, that the NDK doesn't have,
     each put in a class (a library that can be built into an APK, a small shim, or a platform
     piece that needs the platform layer);
  2. whether it compiles as it is (clang -fsyntax-only for arm64, the NDK's sysroot only);
  3. with an empty stand-in for each missing header, the names it then lacks: the functions,
     types and namespaces it takes from outside the NDK;
  4. what it does that an app may not: properties, shell commands, fixed system paths, raw
     devices, SurfaceFlinger and binder.

Writes OUT_DIR/report.json and OUT_DIR/REPORT.md. Nothing is linked and nothing runs.
"""
import argparse
import collections
import concurrent.futures
import glob
import json
import os
import re
import subprocess
import sys

INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)

# Header prefix -> (class, what it is). First match wins.
CLASSES = [
    (("ft2build.h", "freetype/"), "vendor", "FreeType"),
    (("png.h", "pngconf.h"), "vendor", "libpng"),
    (("tinyxml2.h",), "vendor", "tinyxml2"),
    (("curl/",), "vendor", "libcurl"),
    (("openssl/",), "vendor", "BoringSSL / OpenSSL"),
    (("fpdf", "public/fpdf"), "vendor", "pdfium (or PdfRenderer through JNI)"),
    (("ziparchive/",), "vendor", "libziparchive (or minizip)"),
    (("7z", "Lzma", "Xz", "Alloc.h", "Bra.h", "Bcj2.h", "Delta.h", "CpuArch.h"), "vendor", "LZMA SDK"),
    (("zlib.h",), "ndk", "zlib"),
    (("log/", "utils/Log.h", "android/log_main.h"), "shim", "liblog macros (ALOGx) over __android_log_print"),
    (("cutils/properties.h",), "seam", "system properties: the settings store"),
    (("android-base/properties.h",), "seam", "system properties: the settings store"),
    (("android-base/",), "shim", "libbase helpers"),
    (("utils/",), "shim", "libutils (String8, RefBase, clocks, threads)"),
    (("cutils/",), "shim", "libcutils"),
    (("system/thread_defs.h",), "shim", "libcutils"),
    (("gui/", "ui/", "android/gui/", "vndk/", "system/window.h", "nativebase/"), "seam", "SurfaceFlinger client: the display"),
    (("xf86drm", "drm/", "drm.h", "drm_", "libdrm/"), "seam", "DRM/KMS: the display"),
    (("share_config.h",), "gammaos", "another GammaOS module's header"),
    (("NanoScraperDevCreds.gen.h",), "generated", "made by the build (gen_ss_creds.py): the scraper's developer credentials"),
    (("binder/", "android/binder_", "aidl/", "android/hardware/", "hidl/", "hwbinder/"), "seam", "binder, AIDL and HIDL services"),
    (("tinyalsa/",), "drop", "direct ALSA (boot audio)"),
    (("private/", "selinux/", "sys/_system_properties.h"), "seam", "platform-private"),
    (("gammargb", "GammaRgb", "sharecfg", "gammaos"), "gammaos", "another GammaOS module's header"),
]

# What an app may not do. (label, regex) over code with comments removed.
PRIVILEGED = [
    ("property read", r"\bproperty_get(?:_bool|_int32|_int64)?\s*\(|\bGet(?:Bool|Int|Uint)?Property\s*\(|__system_property_(?:get|find|read)"),
    ("property write", r"\bproperty_set\s*\(|\bSetProperty\s*\(|__system_property_set"),
    ("shell command", r"(?<![\w.>:])system\s*\(|\bpopen\s*\(|\bexecv?[lpe]*\s*\(|\bposix_spawnp?\s*\("),
    ("fork", r"(?<![\w.>:])fork\s*\("),
    ("/data/system", r'"/data/system'),
    ("/data/data (other apps)", r'"/data/data/'),
    ("/data/media or /mnt/media_rw", r'"/data/media|"/mnt/media_rw'),
    ("/data (other)", r'"/data/(?!system|data/|media)'),
    ("/sys", r'"/sys/'),
    ("/proc", r'"/proc/'),
    ("/dev", r'"/dev/'),
    ("/system or /vendor file", r'"/(?:system|vendor|product|odm)/'),
    ("evdev", r"\bEVIOC\w+|\binput_event\b"),
    ("DRM ioctl", r"\bDRM_IOCTL_\w+|\bdrmMode\w+|\bDRM_MODE_\w+"),
    ("SurfaceFlinger", r"\bSurfaceComposerClient\b|\bSurfaceControl\b|\bISurfaceComposer\b"),
    ("binder", r"\bdefaultServiceManager\b|\bAServiceManager_\w+|\bIServiceManager\b|\bIBinder\b"),
    ("chown/chmod/mount", r"(?<![\w.>:])(?:chown|lchown|fchown|mount|umount2?|setuid|setgid)\s*\("),
    ("symlink", r"(?<![\w.>:])symlink(?:at)?\s*\("),
]

MISSING_NAME = [
    re.compile(r"use of undeclared identifier '([^']+)'"),
    re.compile(r"unknown type name '([^']+)'"),
    re.compile(r"no (?:member|type|template) named '([^']+)' in (?:namespace )?'([^']+)'"),
    re.compile(r"use of undeclared label '([^']+)'"),
    re.compile(r"call to undeclared function '([^']+)'"),
    re.compile(r"expected namespace name"),
]


def strip_comments(text):
    """C and C++ comments out, string literals kept (so a path in a string still counts)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            out.append("\n" * text.count("\n", i, n if j < 0 else j))
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def slurp(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def binary_sources(nano):
    """The srcs of the first cc_binary in Android.bp (gammaos-nano itself)."""
    bp = slurp(os.path.join(nano, "Android.bp"))
    start = bp.index('name: "gammaos-nano"')
    block = bp[start:bp.index("\ncc_binary", start)]
    srcs = block[block.index("srcs: ["):]
    srcs = srcs[:srcs.index("]")]
    return [s for s in re.findall(r'"([^"]+\.(?:cpp|c))"', srcs) if os.path.isfile(os.path.join(nano, s))]


class Tree:
    def __init__(self, nano, ndk):
        self.nano = nano
        host = glob.glob(os.path.join(ndk, "toolchains/llvm/prebuilt/*"))[0]
        self.clang = os.path.join(host, "bin", "clang++")
        sysroot = os.path.join(host, "sysroot/usr/include")
        self.ndk_dirs = [sysroot, os.path.join(sysroot, "c++/v1"), os.path.join(sysroot, "aarch64-linux-android")]
        self.ndk_dirs += glob.glob(os.path.join(host, "lib/clang/*/include"))
        self.local_dirs = [nano, os.path.join(nano, "liba52/include"), os.path.join(nano, "nanosvg")]
        self._includes = {}

    def resolve_local(self, header, from_dir):
        for d in [from_dir] + self.local_dirs:
            p = os.path.normpath(os.path.join(d, header))
            if os.path.isfile(p):
                return p
        return None

    def in_ndk(self, header):
        return any(os.path.isfile(os.path.join(d, header)) for d in self.ndk_dirs)

    def includes(self, path):
        """(local headers, outside headers) PATH includes directly."""
        if path not in self._includes:
            local, outside = [], []
            for _, header in INCLUDE.findall(slurp(path)):
                p = self.resolve_local(header, os.path.dirname(path))
                if p:
                    local.append(p)
                elif not self.in_ndk(header):
                    outside.append(header)
            self._includes[path] = (local, outside)
        return self._includes[path]

    def closure(self, path):
        """Outside headers PATH reaches: {header: the Nano file that includes it}."""
        seen, found, todo = {path}, {}, [path]
        while todo:
            cur = todo.pop()
            local, outside = self.includes(cur)
            for h in outside:
                found.setdefault(h, os.path.relpath(cur, self.nano))
            for p in local:
                if p not in seen:
                    seen.add(p)
                    todo.append(p)
        return found


def classify(header):
    for prefixes, kind, what in CLASSES:
        if any(header.startswith(p) for p in prefixes):
            return kind, what
    return "other", "unclassified"


def compile_one(tree, src, api, stubs):
    cmd = [tree.clang, f"--target=aarch64-linux-android{api}", "-std=gnu++20", "-fsyntax-only",
           "-DGL_GLEXT_PROTOTYPES", "-DEGL_EGLEXT_PROTOTYPES", "-Wno-everything",
           "-ferror-limit=0" if stubs else "-ferror-limit=1"]
    for d in tree.local_dirs:
        cmd += ["-I", d]
    if stubs:
        cmd += ["-I", stubs]
    cmd.append(os.path.join(tree.nano, src))
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    errors = [l for l in proc.stderr.splitlines() if " error: " in l]
    return proc.returncode == 0, errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nano")
    ap.add_argument("ndk")
    ap.add_argument("out")
    ap.add_argument("--api", type=int, default=29)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    nano = os.path.abspath(args.nano)
    tree = Tree(nano, args.ndk)
    os.makedirs(args.out, exist_ok=True)
    srcs = binary_sources(nano)

    files = {}
    all_outside = collections.defaultdict(set)
    for src in srcs:
        path = os.path.join(nano, src)
        text = slurp(path)
        code = strip_comments(text)
        direct = self_outside = tree.includes(path)[1]
        reach = tree.closure(path)
        for h in reach:
            all_outside[h].add(src)
        files[src] = {
            "lines": text.count("\n") + 1,
            "outside_direct": sorted(self_outside),
            "outside_via_headers": {h: via for h, via in sorted(reach.items()) if h not in direct},
            "privileged": {label: n for label, rx in PRIVILEGED if (n := len(re.findall(rx, code)))},
        }

    # Empty stand-ins for every outside header, so the second compile reaches the code.
    stubs = os.path.join(os.path.abspath(args.out), "stub-include")
    for h in all_outside:
        p = os.path.join(stubs, h)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "w").close()

    def probe(src):
        ok, first = compile_one(tree, src, args.api, None)
        missing = collections.Counter()
        errors = 0
        if not ok:
            _, errs = compile_one(tree, src, args.api, stubs)
            errors = len(errs)
            for line in errs:
                for rx in MISSING_NAME:
                    m = rx.search(line)
                    if m and m.groups():
                        name = m.group(1) if len(m.groups()) == 1 else f"{m.group(2)}::{m.group(1)}"
                        missing[name] += 1
                        break
        return src, ok, (first[0].split(" error: ", 1)[1] if first else ""), errors, missing

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for src, ok, first, errors, missing in pool.map(probe, srcs):
            files[src].update(compiles_as_is=ok, first_error=first, errors_with_stubs=errors,
                              missing_names=dict(missing.most_common()))

    headers = {h: {"class": classify(h)[0], "what": classify(h)[1], "files": sorted(s)}
               for h, s in sorted(all_outside.items())}
    report = {"api": args.api, "sources": len(srcs), "lines": sum(f["lines"] for f in files.values()),
              "headers": headers, "files": files}
    with open(os.path.join(args.out, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1, sort_keys=True)
    with open(os.path.join(args.out, "REPORT.md"), "w", encoding="utf-8") as f:
        f.write(render(report))
    print(render(report, brief=True))


def render(r, brief=False):
    files, headers = r["files"], r["headers"]
    ok = sorted(s for s, f in files.items() if f["compiles_as_is"])
    out = ["# Nano against the NDK", "",
           f"{r['sources']} source files of the gammaos-nano binary, {r['lines']} lines, "
           f"arm64, API {r['api']}, NDK headers only.", "",
           f"- **Compile as they are:** {len(ok)} files, {sum(files[s]['lines'] for s in ok)} lines.",
           f"- **Headers from outside the NDK:** {len(headers)}.", ""]

    out += ["## Outside headers by class", "", "| Class | What | Headers | Files that reach one |", "|---|---|---:|---:|"]
    groups = collections.defaultdict(lambda: (set(), set()))
    for h, info in headers.items():
        g = groups[(info["class"], info["what"])]
        g[0].add(h)
        g[1].update(info["files"])
    for (kind, what), (hs, fs) in sorted(groups.items(), key=lambda kv: (kv[0][0], -len(kv[1][1]))):
        out.append(f"| {kind} | {what} | {len(hs)} | {len(fs)} |")
    out.append("")

    totals = collections.Counter()
    spread = collections.Counter()
    for f in files.values():
        for label, n in f["privileged"].items():
            totals[label] += n
            spread[label] += 1
    out += ["## What an app may not do", "", "| Kind | Uses | Files |", "|---|---:|---:|"]
    out += [f"| {label} | {totals[label]} | {spread[label]} |" for label, _ in PRIVILEGED if totals[label]]
    out.append("")

    names = collections.Counter()
    for f in files.values():
        names.update(f["missing_names"])
    out += ["## Names missing once every outside header is an empty file (top 60)", "",
            ", ".join(f"`{n}` ({c})" for n, c in names.most_common(60)), ""]
    if brief:
        return "\n".join(out)

    out += ["## Files that compile as they are", "", ", ".join(f"`{s}`" for s in ok), ""]
    out += ["## Every file", "",
            "| File | Lines | Outside headers (direct) | Privileged uses | Errors with stand-ins |", "|---|---:|---|---|---:|"]
    for s, f in sorted(files.items(), key=lambda kv: -sum(kv[1]["privileged"].values())):
        priv = ", ".join(f"{k} {v}" for k, v in sorted(f["privileged"].items(), key=lambda kv: -kv[1]))
        mark = "" if not f["compiles_as_is"] else " (compiles)"
        out.append(f"| `{s}`{mark} | {f['lines']} | {', '.join(f['outside_direct'])} | {priv} | {f['errors_with_stubs']} |")
    out.append("")
    out += ["## Every outside header", "", "| Header | Class | Files |", "|---|---|---:|"]
    out += [f"| `{h}` | {i['class']}: {i['what']} | {len(i['files'])} |" for h, i in headers.items()]
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    sys.exit(main())
