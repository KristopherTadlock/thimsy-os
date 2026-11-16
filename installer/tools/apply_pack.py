#!/usr/bin/env python3
"""Points a device's default download for a profile at a Thimsy package: sets
`packages.<PROFILE>` in devices/<id>.yaml to the package's file name, its
approximate size (for the free-space check) and where it's published.

  apply_pack.py [--tag TAG] BUNDLE_JSON DEVICE_YAML PROFILE

BUNDLE_JSON is what `thimsy-installer dev bundle` printed for the package
(its file name and size, and its parts when it was split). The download
addresses point at the latest release of the GitHub repository, or at the
release TAG.

Nothing here pins a build: the installer accepts any package whose own
pack.json checks out (see universal_installer.md, section 10), so a new build
under the same file name needs no catalog change. Run this only when the
package's name, size or host changes. A new file name also changes the note
shown when it can't be downloaded: run `thimsy-installer dev i18n-strings`
and translate the new note afterwards.

The other profiles' packages and the rest of the file are kept.
"""
import json, re, sys

REPO = "https://github.com/KristopherTadlock/thimsy-os/releases"


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def scalar(v):
    """A YAML scalar: JSON strings and numbers are valid YAML. Non-ASCII text
    is written as is (YAML can't read JSON's surrogate-pair escapes)."""
    return json.dumps(v, ensure_ascii=False)


def yflow(v):
    """YAML flow style for nested dicts and lists."""
    if isinstance(v, dict):
        return "{ " + ", ".join(f"{k}: {yflow(x)}" for k, x in v.items()) + " }"
    if isinstance(v, list):
        return "[" + ", ".join(yflow(x) for x in v) + "]"
    return scalar(v)


def split_flow(body):
    """Splits the inside of a flow mapping into (key, value text) pairs at
    top-level commas (not inside quotes, brackets or braces)."""
    items, depth, quote, start, i = [], 0, None, 0, 0
    while i < len(body):
        c = body[i]
        if quote:
            if c == "\\" and quote == '"':
                i += 1
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
        elif c in "{[":
            depth += 1
        elif c in "}]":
            depth -= 1
        elif c == "," and depth == 0:
            items.append(body[start:i])
            start = i + 1
        i += 1
    items.append(body[start:])
    out = []
    for item in items:
        item = item.strip()
        if item:
            k, _, v = item.partition(":")
            out.append((k.strip().strip("\"'"), v.strip()))
    return out


def insert_before(text, block, before):
    b = re.search(before, text, flags=re.M) if before else None
    if b:
        return text[:b.start()] + block + text[b.start():]
    return text.rstrip("\n") + "\n" + block


def set_entry(text, name, key, value, before=None):
    """Sets `key: value` in the top-level mapping `name:`, written in flow
    style (`name: { a: 1 }`) or block style (`name:` then `  a: 1` lines).
    The other keys are kept. The key, or the whole mapping, is added when
    missing (before the first line matching `before`, else at the end)."""
    m = re.search(rf"^{re.escape(name)}:[ \t]*\{{(.*)\}}[ \t]*$", text, flags=re.M)
    if m:
        items = dict(split_flow(m.group(1)))
        items[key] = value
        line = f"{name}: {{ " + ", ".join(f"{k}: {v}" for k, v in items.items()) + " }"
        return text[:m.start()] + line + text[m.end():]
    m = re.search(rf"^{re.escape(name)}:[ \t]*\n((?:[ \t]+\S.*\n|[ \t]*\n(?=[ \t]+\S))*)",
                  text, flags=re.M)
    if m and m.group(1).strip():
        body = m.group(1)
        new = f"  {key}: {value}\n"
        # The entry's line and any more-indented lines under it.
        entry = re.compile(rf"^  {re.escape(key)}:.*\n(?:   +\S.*\n)*", flags=re.M)
        if entry.search(body):
            body = entry.sub(lambda _: new, body, count=1)
        else:
            body = body.rstrip("\n") + "\n" + new
        return text[:m.start(1)] + body + text[m.end(1):]
    m = re.search(rf"^{re.escape(name)}:.*\n?", text, flags=re.M)
    if m:  # empty, or a placeholder value
        return text[:m.start()] + f"{name}:\n  {key}: {value}\n" + text[m.end():]
    return insert_before(text, f"{name}:\n  {key}: {value}\n", before)


def main(argv):
    args = argv[1:]
    tag = None
    if args[:1] == ["--tag"]:
        if len(args) < 2:
            sys.exit(__doc__)
        tag, args = args[1], args[2:]
    if len(args) != 3:
        sys.exit(__doc__)
    bundle_json, device_yaml, profile = args
    if not re.fullmatch(r"[A-Za-z0-9_-]+", profile):
        sys.exit(f"bad profile name {profile!r}")
    if tag is not None and not re.fullmatch(r"[A-Za-z0-9._-]+", tag):
        sys.exit(f"bad release tag {tag!r}")
    bundle = json.loads(read(bundle_json))
    names = [bundle["file"]] + [p["file"] for p in bundle.get("parts", [])]
    if any(not re.fullmatch(r"[A-Za-z0-9._-]+", n) for n in names):
        sys.exit(f"package file names must be plain names: {names}")
    base = f"{REPO}/download/{tag}" if tag else f"{REPO}/latest/download"
    # Installer releases (installer-v…) or Thimsy's own host the package.
    page = "Thimsy Installer release page" if tag and tag.startswith("installer-") else "Thimsy release page"
    pkg = {
        "file": bundle["file"],
        "approx_size": bundle["size"],
        "note": f"Download {bundle['file']} from the {page}, then choose it with Use a file I already downloaded.",
    }
    if bundle.get("parts"):
        pkg["parts"] = [{"file": p["file"], "urls": [f"{base}/{p['file']}"]} for p in bundle["parts"]]
    else:
        pkg["urls"] = [f"{base}/{bundle['file']}"]
    write(device_yaml, set_entry(read(device_yaml), "packages", profile, yflow(pkg), before=r"^flash:"))
    print(f"packages.{profile}: {bundle['file']} from {base}")


if __name__ == "__main__":
    main(sys.argv)
