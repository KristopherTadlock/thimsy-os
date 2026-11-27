# SPDX-License-Identifier: MIT
"""SHA-512 crypt(3), "$6$" (Ulrich Drepper's specification), for the SSH tests.

Python's crypt module is Unix-only and gone in 3.13, so the tests carry their
own: test_rcs.py checks that D50ssh's default hash is the hash of "thimsy",
and runs this file as a fake BusyBox "cryptpw -m sha512" (main()).
Standard library only; Python 3.8+.
"""
from __future__ import annotations

import hashlib

_B64 = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"

# The byte order of the final encoding: 21 groups of three, then byte 63.
_ORDER = [(0, 21, 42), (22, 43, 1), (44, 2, 23), (3, 24, 45), (25, 46, 4),
          (47, 5, 26), (6, 27, 48), (28, 49, 7), (50, 8, 29), (9, 30, 51),
          (31, 52, 10), (53, 11, 32), (12, 33, 54), (34, 55, 13), (56, 14, 35),
          (15, 36, 57), (37, 58, 16), (59, 17, 38), (18, 39, 60), (40, 61, 19),
          (62, 20, 41)]


def _b64(b2: int, b1: int, b0: int, n: int) -> str:
    w = (b2 << 16) | (b1 << 8) | b0
    out = ""
    for _ in range(n):
        out += _B64[w & 0x3F]
        w >>= 6
    return out


def _repeat(block: bytes, length: int) -> bytes:
    return (block * (length // len(block) + 1))[:length]


def sha512_crypt(password: str, setting: str) -> str:
    """crypt(password, setting) for a "$6$[rounds=N$]salt" setting."""
    if not setting.startswith("$6$"):
        raise ValueError("not a $6$ setting: %r" % setting)
    rest = setting[3:]
    rounds = 5000
    custom = False
    if rest.startswith("rounds="):
        num, _, rest = rest[len("rounds="):].partition("$")
        rounds = max(1000, min(999999999, int(num)))
        custom = True
    salt = rest.split("$", 1)[0][:16].encode("ascii")
    p = password.encode("utf-8")
    h = hashlib.sha512

    b = h(p + salt + p).digest()
    a = h(p + salt)
    n = len(p)
    while n > 64:
        a.update(b)
        n -= 64
    a.update(b[:n])
    n = len(p)
    while n > 0:
        a.update(b if n & 1 else p)
        n >>= 1
    c = a.digest()

    p_seq = _repeat(h(p * len(p)).digest(), len(p)) if p else b""
    s_seq = _repeat(h(salt * (16 + c[0])).digest(), len(salt)) if salt else b""

    for i in range(rounds):
        r = h()
        r.update(p_seq if i & 1 else c)
        if i % 3:
            r.update(s_seq)
        if i % 7:
            r.update(p_seq)
        r.update(c if i & 1 else p_seq)
        c = r.digest()

    enc = "".join(_b64(c[x], c[y], c[z], 4) for x, y, z in _ORDER) + _b64(0, 0, c[63], 2)
    head = "$6$" + ("rounds=%d$" % rounds if custom else "")
    return head + salt.decode("ascii") + "$" + enc


def main(argv: "list[str]") -> int:
    """A fake BusyBox "cryptpw -m sha512" for test_rcs.py: one line from stdin,
    hashed with the salt in THIMSY_FAKE_SALT (default "FakeSalt")."""
    import os
    import sys
    if argv != ["-m", "sha512"]:
        sys.stderr.write("fake cryptpw: only -m sha512\n")
        return 2
    # bytes both ways: Windows' text streams would add or keep CR.
    raw = sys.stdin.buffer.readline()
    if not raw:
        return 1
    line = raw.decode("utf-8").rstrip("\r\n")
    salt = os.environ.get("THIMSY_FAKE_SALT", "FakeSalt")
    sys.stdout.buffer.write((sha512_crypt(line, "$6$" + salt) + "\n").encode("ascii"))
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main(sys.argv[1:]))
