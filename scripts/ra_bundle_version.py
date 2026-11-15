#!/usr/bin/env python3
"""Marks a retroarch.cfg as already set up for an APK's bundled assets.

  ra_bundle_version.py APK CFG

RetroArch's Java launcher (UserPreferences.updateConfigFile) compares the APK's
versionCode with bundle_assets_extract_last_version on every start from its icon,
and on a mismatch the core extracts the APK's assets over its app folder, the
pinned core info files included. The payload ships those folders already, so
both keys are set to the APK's versionCode, read from its binary manifest.
"""

import re
import struct
import sys
import zipfile

VERSION_CODE_ATTR = 0x0101021B  # android:versionCode
RES_XML_START_ELEMENT = 0x0102
RES_STRING_POOL = 0x0001
RES_XML_RESOURCE_MAP = 0x0180
TYPE_INT_DEC = 0x10
TYPE_INT_HEX = 0x11
KEYS = ("bundle_assets_extract_last_version", "bundle_assets_extract_version_current")


def strings(data, off):
    count, _, flags, start, _ = struct.unpack_from("<IIIII", data, off + 8)
    offsets = struct.unpack_from(f"<{count}I", data, off + 28)
    base = off + start
    utf8 = flags & 0x100
    out = []
    for o in offsets:
        p = base + o
        if utf8:
            p += 2 if data[p] & 0x80 else 1  # length in UTF-16 units
            n = data[p]
            p += 1
            if n & 0x80:
                n = ((n & 0x7F) << 8) | data[p]
                p += 1
            out.append(data[p:p + n].decode("utf-8", "replace"))
        else:
            n = struct.unpack_from("<H", data, p)[0]
            p += 2
            if n & 0x8000:
                n = ((n & 0x7FFF) << 16) | struct.unpack_from("<H", data, p)[0]
                p += 2
            out.append(data[p:p + 2 * n].decode("utf-16-le", "replace"))
    return out


def version_code(manifest):
    """The manifest element's android:versionCode, from AAPT's binary XML."""
    if len(manifest) < 8 or struct.unpack_from("<HH", manifest, 0)[0] != 0x0003:
        raise ValueError("not a binary AndroidManifest.xml")
    pool, resmap = [], []
    off = struct.unpack_from("<H", manifest, 2)[0]
    while off + 8 <= len(manifest):
        ctype, _, size = struct.unpack_from("<HHI", manifest, off)
        if size < 8 or off + size > len(manifest):
            break
        if ctype == RES_STRING_POOL:
            pool = strings(manifest, off)
        elif ctype == RES_XML_RESOURCE_MAP:
            resmap = list(struct.unpack_from(f"<{(size - 8) // 4}I", manifest, off + 8))
        elif ctype == RES_XML_START_ELEMENT:
            name = struct.unpack_from("<I", manifest, off + 20)[0]
            attr_start, attr_size, attr_count = struct.unpack_from("<HHH", manifest, off + 24)
            if name < len(pool) and pool[name] == "manifest":
                for i in range(attr_count):
                    a = off + 16 + attr_start + i * attr_size
                    _, aname, _, _, _, dtype, value = struct.unpack_from("<IIIHBBI", manifest, a)
                    named = aname < len(pool) and pool[aname] == "versionCode"
                    mapped = aname < len(resmap) and resmap[aname] == VERSION_CODE_ATTR
                    if (named or mapped) and dtype in (TYPE_INT_DEC, TYPE_INT_HEX):
                        return value
                raise ValueError("the manifest has no versionCode")
        off += size
    raise ValueError("no manifest element")


def set_keys(text, code):
    for key in KEYS:
        line = f'{key} = "{code}"'
        text, n = re.subn(rf"^{key} = .*$", line, text, flags=re.M)
        if n == 0:
            text += ("" if text.endswith("\n") or not text else "\n") + line + "\n"
    return text


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    apk, cfg = sys.argv[1:]
    with zipfile.ZipFile(apk) as z:
        code = version_code(z.read("AndroidManifest.xml"))
    with open(cfg, encoding="utf-8") as f:
        text = f.read()
    new = set_keys(text, code)
    if new != text:
        with open(cfg, "w", encoding="utf-8") as f:
            f.write(new)
    print(f"{cfg}: bundled assets marked as extracted for versionCode {code}")


if __name__ == "__main__":
    main()
