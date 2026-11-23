#!/usr/bin/env python3
"""Copies a key.properties for tools/hakux/build.sh, making a relative storeFile
absolute: relative to the copy, Gradle would look for it in the hakuX
checkout's android/app, but it is next to the original.

Usage: keyprops.py SRC DST"""
import os
import sys


def rewrite(text, base):
    """TEXT with a relative storeFile made relative to BASE."""
    out = []
    for line in text.splitlines(keepends=True):
        key, sep, value = line.partition("=")
        path = value.strip()
        if sep and key.strip() == "storeFile" and path and not os.path.isabs(path):
            line = "storeFile=" + os.path.join(base, path) + ("\n" if line.endswith("\n") else "")
        out.append(line)
    return "".join(out)


def main():
    src, dst = sys.argv[1:]
    with open(src, encoding="utf-8") as f:
        text = f.read()
    with open(dst, "w", encoding="utf-8") as f:
        f.write(rewrite(text, os.path.dirname(os.path.abspath(src))))


if __name__ == "__main__":
    main()
