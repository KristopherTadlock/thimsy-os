"""Quirk files' KEY="value" facts; no script text is ever copied.

ROCKNIX's quirk files (packages/hardware/quirks/{platforms/<FAMILY>,devices/<model>}/NNN-*)
are shell scripts. Only one shape in them is data: a here-document written to profile.d,

    cat <<EOF >/storage/.config/profile.d/020-gpios
    DEVICE_WIFI="0"
    DEVICE_PWM_MOTOR="pwmchip1"
    EOF

Those KEY="value" lines are read; everything else (echo to sysfs, loops, sourcing) is
skipped. A value that would need the shell (a $, a backtick, a command) is rejected and
reported, never kept. Device directories are keyed by device-tree model, as ROCKNIX keys
them. Platform files are read first and device files override them, both in file-name order.
"""

from __future__ import annotations

import re
from collections import OrderedDict

from .rocknix import is_text_link_to_dir

QUIRKS = "packages/hardware/quirks"

_HEREDOC = re.compile(r"""^\s*cat\s+<<-?\s*['"]?([A-Za-z_]+)['"]?\s*>\s*(\S+)\s*$""")
_KV = re.compile(r"""^([A-Z_][A-Z0-9_]*)=(?:"([^"$`\\]*)"|'([^']*)'|([^\s"'$`;&|<>()\\]*))\s*$""")


def read_facts(text: str) -> tuple:
    """(facts, rejected) from one quirk file."""
    facts: "OrderedDict[str, str]" = OrderedDict()
    rejected = []
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        m = _HEREDOC.match(lines[i])
        if not m:
            i += 1
            continue
        end, target = m.group(1), m.group(2)
        i += 1
        body = []
        while i < len(lines) and lines[i].strip() != end:
            body.append(lines[i])
            i += 1
        i += 1
        if "/profile.d/" not in target:
            continue   # other here-documents write emulator configs: not device facts
        for line in body:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            km = _KV.match(s)
            if km:
                val = next(g for g in (km.group(2), km.group(3), km.group(4)) if g is not None)
                facts[km.group(1)] = val
            else:
                rejected.append(s)
    return facts, rejected


class QuirkFacts:
    def __init__(self):
        self.facts: "OrderedDict[str, str]" = OrderedDict()
        self.sources: list = []
        self.rejected: list = []


def _read_dir(tree, d, qf: QuirkFacts):
    if not d.is_dir():
        return False
    for p in sorted(d.iterdir(), key=lambda q: q.name):
        if p.is_dir() or is_text_link_to_dir(p):
            continue
        real = tree.resolve(p)
        if real is None:
            continue
        facts, rejected = read_facts(tree.read_text(p))
        if facts or rejected:
            qf.sources.append(tree.rel(p))
        qf.facts.update(facts)
        qf.rejected.extend("%s: %s" % (tree.rel(p), r) for r in rejected)
    return True


def for_device(tree, family: str, model: str | None) -> QuirkFacts:
    qf = QuirkFacts()
    base = tree.project / QUIRKS
    _read_dir(tree, base / "platforms" / family, qf)
    if model:
        _read_dir(tree, base / "devices" / model, qf)
    return qf


def device_models(tree) -> list:
    d = tree.project / QUIRKS / "devices"
    return sorted(p.name for p in d.iterdir() if p.is_dir()) if d.is_dir() else []


def package_mk(tree):
    p = tree.project / QUIRKS / "package.mk"
    return p if tree.resolve(p) is not None else None


# Facts mapped into device.json fields; the rest stay in quirk_facts as plain strings.
def _bool(v: str) -> bool | None:
    if v.lower() in ("true", "yes", "1"):
        return True
    if v.lower() in ("false", "no", "0"):
        return False
    return None


def apply_to_device(dev: dict, facts: dict) -> None:
    f = facts
    if "DEVICE_HAS_TOUCHSCREEN" in f:
        b = _bool(f["DEVICE_HAS_TOUCHSCREEN"])
        if b is not None:
            dev.setdefault("touch", OrderedDict())["present"] = b
    audio = OrderedDict()
    if "ALSA_PRIMARY_CARD" in f and f["ALSA_PRIMARY_CARD"].isdigit():
        audio["card_index"] = int(f["ALSA_PRIMARY_CARD"])
    pb = OrderedDict()
    for key, field in (("DEVICE_PLAYBACK_PATH", "control"), ("DEVICE_PLAYBACK_PATH_SPK", "speaker"),
                       ("DEVICE_PLAYBACK_PATH_HP", "headphone")):
        if key in f:
            pb[field] = f[key]
    if pb:
        audio["playback"] = pb
    if "DEVICE_FAKE_JACKSENSE" in f:
        b = _bool(f["DEVICE_FAKE_JACKSENSE"])
        if b is not None:
            audio["fake_jack_sense"] = b
    if audio:
        dev.setdefault("audio", OrderedDict()).update(audio)
    if "DEVICE_PWM_MOTOR" in f:
        dev.setdefault("vibrator", OrderedDict()).update(
            [("type", "pwm"), ("pwm_chip", f["DEVICE_PWM_MOTOR"])])
    if "DEVICE_TEMP_SENSOR" in f:
        dev.setdefault("thermal", OrderedDict())["sensor"] = f["DEVICE_TEMP_SENSOR"]
    if "DEVICE_PWM_FAN" in f:
        dev["fans"] = [OrderedDict([("type", "pwm"), ("path", f["DEVICE_PWM_FAN"])])]
    leds = [OrderedDict([("name", k[len("DEVICE_LED_"):].lower()), ("path", v)])
            for k, v in f.items() if k.startswith("DEVICE_LED_") and v.startswith("/sys/")]
    if leds:
        dev["leds"] = leds
