#!/usr/bin/env python3
"""Writes hakuX-build-manifest.json for tools/hakux/build.sh: every source
revision, patch, tool and output hash, so a build can be checked and redone.

Usage: manifest.py OUT_JSON SRC NC_DEPS HERE OUTDIR REPO COMMIT THIMSY_BUILD DEP...
(DEP: "path url commit name", as build.sh's DEPS.)"""
import hashlib
import json
import os
import re
import subprocess
import sys


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True, check=True).stdout.strip()


def tool_version(cmd):
    try:
        out = subprocess.run(cmd, capture_output=True, text=True).stdout + \
            subprocess.run(cmd, capture_output=True, text=True).stderr
        return out.strip().splitlines()[0]
    except OSError:
        return None


def main():
    out, src, deps_dir, here, outdir, repo, commit, build, *deps = sys.argv[1:]
    gradle = open(os.path.join(src, "android/app/build.gradle.kts"), encoding="utf-8").read()
    wrapper = open(os.path.join(src, "android/gradle/wrapper/gradle-wrapper.properties"), encoding="utf-8").read()
    patches = []
    for line in open(os.path.join(here, "patches/series"), encoding="utf-8"):
        if line.strip() and not line.startswith("#"):
            name, source = line.split(None, 1)
            patches.append({"file": name, "sha256": sha256(os.path.join(here, "patches", name)),
                            "source": source.strip()})
    fex = os.path.join(src, "external/fex")
    manifest = {
        "hakux": {
            "repository": repo,
            "commit": commit,
            "versionName": re.search(r'versionName = "(.*?)"', gradle).group(1),
            "versionCode": int(re.search(r"versionCode = (\d+)", gradle).group(1)),
            "thimsy_build": int(build),
        },
        "patches": patches,
        "fex": {
            "commit": git("-C", fex, "rev-parse", "HEAD"),
            "submodules": {line.split()[1]: line.split()[0].lstrip("+-U")
                           for line in git("-C", fex, "submodule", "status").splitlines()
                           if not line.startswith("-")},
        },
        "native_deps": [
            {"path": p, "repository": u, "commit": c, "revision": n,
             "patched": "tools/native/adrenotools-kgsl-context-priority.patch" if p == "adrenotools" else None}
            for p, u, c, n in (d.split() for d in deps)
        ],
        "toolchain": {
            "compileSdk": int(re.search(r"compileSdk = (\d+)", gradle).group(1)),
            "buildTools": re.search(r'buildToolsVersion = "(.*?)"', gradle).group(1),
            "ndk": re.search(r'ndkVersion = "(.*?)"', gradle).group(1),
            "gradle": re.search(r"gradle-([\d.]+)-", wrapper).group(1),
            "cmake": tool_version(["cmake", "--version"]),
            "java": tool_version(["java", "-version"]),
            "build_image": "tools/hakux/Dockerfile (ubuntu:24.04, linux/amd64)",
        },
        "outputs": {name: sha256(os.path.join(outdir, name))
                    for name in sorted(os.listdir(outdir)) if name.endswith((".apk", ".tar.xz"))},
    }
    with open(out, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    main()
