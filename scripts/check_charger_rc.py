#!/usr/bin/env python3
"""Checks that a system image's init.rc starts the off-mode charger only once the RG Cube's battery is there.

usage: check_charger_rc.py SYSTEM_IMG

The RG Cube's battery (/sys/class/power_supply/battery) is registered by kernel modules that the
vendor's loader (insmod-sh) loads after early-init, and the charger looks for it only once, when it
starts. Started first, it shows "battery unknown" for the whole charge. init.rc must therefore wait
for that node, on the RG Cube in charger mode, in an action that runs before the first
`class_start charger` (actions on one trigger run in the order init parsed them, and
/system/etc/init/hw/init.rc is parsed first).

SYSTEM_IMG is an ext4 image with the system as root (its init.rc is /system/etc/init/hw/init.rc).
Needs debugfs on PATH.
"""

import re
import subprocess
import sys

INIT_RC = "/system/etc/init/hw/init.rc"
BATTERY = "/sys/class/power_supply/battery"
# The conditions the wait may carry: none, or the RG Cube's model.
ALLOWED_CONDITIONS = {"property:ro.product.vendor.model=RGCUBE"}


def sections(text):
    """[(header words, [command words])] of the `on` and `service` sections of an init .rc text."""
    out = []
    pending = ""
    for raw in text.splitlines():
        line = pending + raw
        if line.endswith("\\"):
            pending = line[:-1]
            continue
        pending = ""
        words = line.split()
        # Only the first words of a line matter here, so quotes need no parsing; a comment starts
        # at a word that begins with #.
        for i, w in enumerate(words):
            if w.startswith("#"):
                del words[i:]
                break
        if not words:
            continue
        if words[0] in ("on", "service", "import"):
            out.append((words, []))
        elif out:
            out[-1][1].append(words)
    return out


def triggers(header):
    """(events, conditions) of an `on` header: `on charger && property:a=b` gives ({charger}, {property:a=b}).

    None when init's ParseTriggers refuses the header, which drops the whole action.
    """
    events, conditions, names = set(), set(), set()
    for i, w in enumerate(header[1:]):
        if i % 2:
            if w != "&&":
                return None
        elif w.startswith("property:"):
            name = w[len("property:"):].partition("=")[0]
            if "=" not in w or name in names:
                return None
            names.add(name)
            conditions.add(w)
        elif events or not re.fullmatch(r"[A-Za-z0-9_-]+", w):
            return None
        else:
            events.add(w)
    return events, conditions


def problem(text):
    """None when TEXT (an init.rc) waits for the battery before starting the charger, or what is wrong."""
    waited = False
    for header, commands in sections(text):
        if header[0] != "on":
            continue
        trig = triggers(header)
        if trig is None:
            if "charger" in header:
                return "%s: init rejects `%s`" % (INIT_RC, " ".join(header))
            continue
        events, conditions = trig
        if events != {"charger"}:
            continue
        for words in commands:
            if words[:2] == ["class_start", "charger"] or words[:2] == ["start", "charger"]:
                if waited:
                    return None
                return ("%s: `%s` runs before any `wait %s` in an `on charger` action"
                        % (INIT_RC, " ".join(words), BATTERY))
            if words[:2] == ["wait", BATTERY] and conditions <= ALLOWED_CONDITIONS:
                if len(words) > 3:
                    return "%s: init rejects `%s`" % (INIT_RC, " ".join(words))
                # init blocks on the wait with nothing else running, so it must also end soon.
                # Without a timeout, init waits 5 s (kCommandRetryTimeout).
                timeout = words[2] if len(words) == 3 else "5"
                if not re.fullmatch(r"[0-9]+(\.[0-9]+)?", timeout) or not 5 <= float(timeout) <= 30:
                    return "%s: `%s` needs a timeout of 5 to 30 s" % (INIT_RC, " ".join(words))
                waited = True
    return "%s: no `on charger` action starts the charger" % INIT_RC


def read_init_rc(image):
    proc = subprocess.run(["debugfs", "-R", "cat " + INIT_RC, image], capture_output=True)
    if proc.returncode:
        sys.exit("check_charger_rc: debugfs exited %d on %s" % (proc.returncode, image))
    # debugfs exits 0 for a missing file, and for an image it cannot open, and says so on stderr only.
    if b"File not found" in proc.stderr:
        sys.exit("check_charger_rc: %s has no %s" % (image, INIT_RC))
    if not proc.stdout:
        why = [line for line in proc.stderr.decode("utf-8", "replace").splitlines()
               if line.strip() and not re.match(r"debugfs [0-9]", line)]
        sys.exit("check_charger_rc: debugfs could not read %s from %s: %s"
                 % (INIT_RC, image, "; ".join(why) or "no output"))
    return proc.stdout.decode("utf-8", "replace")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    err = problem(read_init_rc(sys.argv[1]))
    if err:
        sys.exit("check_charger_rc: " + err)
    print("init.rc waits for the battery before starting the charger")


if __name__ == "__main__":
    main()
