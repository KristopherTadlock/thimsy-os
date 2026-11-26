# SPDX-License-Identifier: MIT
"""The image's ALSA "default" (review finding linux-host-impls-3): /etc/asound.conf in the common
overlay makes "default" plug over a dmix on the card ALSA_CARD names, which thimsy-session puts in
Nano's environment, so Nano's streams and the game's share the card. Parsed with a small reader
of alsa-lib's configuration syntax (no alsa-lib here)."""

import re
import unittest
from pathlib import Path
from typing import Any, Dict, List

LINUX = Path(__file__).resolve().parents[1]
ASOUND = LINUX / "board/common/rootfs-overlay/etc/asound.conf"
NANO = LINUX.parent / "nano"


def tokens(text: str) -> List[str]:
    out = []
    for m in re.finditer(r'#[^\n]*|"((?:[^"\\]|\\.)*)"|([{}\[\]])|([^\s{}\[\]"#,;=]+)', text):
        if m.group(0).startswith("#"):
            continue
        out.append(m.group(1) if m.group(1) is not None else m.group(0))
    return out


def parse(toks: List[str], i: int = 0, end: str = "") -> Any:
    """A compound as a dict (dotted keys split), an array as a list."""
    if end == "]":
        arr = []
        while toks[i] != "]":
            v, i = value(toks, i)
            arr.append(v)
        return arr, i + 1
    d: Dict[str, Any] = {}
    while i < len(toks) and toks[i] != "}":
        key = toks[i]
        v, i = value(toks, i + 1)
        node = d
        parts = key.split(".")
        for p in parts[:-1]:
            node = node.setdefault(p, {})
        node[parts[-1]] = v
    return d, i + 1


def value(toks: List[str], i: int) -> Any:
    if toks[i] == "{":
        return parse(toks, i + 1, "}")
    if toks[i] == "[":
        return parse(toks, i + 1, "]")
    return toks[i], i + 1


class TestAsoundConf(unittest.TestCase):
    def setUp(self) -> None:
        self.conf, _ = parse(tokens(ASOUND.read_text(encoding="utf-8")))

    def test_default_is_plug_over_a_dmix(self) -> None:
        pcm = self.conf["pcm"]
        self.assertEqual(pcm["!default"]["type"], "plug")
        slave = pcm["!default"]["slave"]["pcm"]
        self.assertEqual(slave, "thimsy_dmix")
        dmix = pcm[slave]
        self.assertEqual(dmix["type"], "dmix")
        self.assertEqual(dmix["ipc_perm"], "0666")  # Nano and the game share it
        self.assertEqual(dmix["slave"]["pcm"]["type"], "hw")

    def test_the_card_is_alsa_card(self) -> None:
        for card in (self.conf["pcm"]["thimsy_dmix"]["slave"]["pcm"]["card"],
                     self.conf["ctl"]["!default"]["card"]):
            self.assertEqual(card["@func"], "getenv")
            self.assertEqual(card["vars"], ["ALSA_CARD"])
            self.assertEqual(card["default"], "0")
        self.assertEqual(self.conf["ctl"]["!default"]["type"], "hw")

    def test_only_the_common_overlay_ships_one(self) -> None:
        found = sorted(str(p.relative_to(LINUX)) for p in LINUX.glob("board/**/asound.conf"))
        self.assertEqual(found, [str(ASOUND.relative_to(LINUX))])

    def test_the_session_gives_nano_the_card_and_nano_opens_default(self) -> None:
        session = (LINUX / "src/thimsy-session/src/Session.cpp").read_text(encoding="utf-8")
        self.assertIn('"ALSA_CARD=" + audioCard_', session)
        alsa = (NANO / "platform/linux/AudioAlsa.cpp").read_text(encoding="utf-8")
        self.assertIn('kDevice = "default"', alsa)


if __name__ == "__main__":
    unittest.main()
