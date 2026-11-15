#!/usr/bin/env python3
"""Checks that a system image's off-mode charger looks for the battery again until it finds one.

usage: check_charger_binary.py SYSTEM_IMG

The RG Cube's battery is registered by vendor modules that load beside the charger, and AOSP's
charger (BatteryMonitor) looks for it only once, when it starts: started first, it shows "battery
unknown" (a "?") for the whole charge. The Nano patch's BatteryMonitor scans again on each update
until a battery turns up, and logs MARKER when it does; a charger without that string was built
without the fix.

SYSTEM_IMG is an ext4 image with the system as root (the charger is /system/bin/charger).
Needs debugfs on PATH.
"""

import re
import subprocess
import sys

CHARGER = "/system/bin/charger"
MARKER = b"battery appeared after start"


def problem(binary):
    """None when BINARY (the charger's bytes) has the rescan, or what is wrong."""
    if not binary.startswith(b"\x7fELF"):
        return "%s is not an ELF file" % CHARGER
    if MARKER not in binary:
        return ("%s lacks \"%s\": its BatteryMonitor does not look for a battery that registers "
                "after it starts" % (CHARGER, MARKER.decode()))
    return None


def read_charger(image):
    proc = subprocess.run(["debugfs", "-R", "cat " + CHARGER, image], capture_output=True)
    if proc.returncode:
        sys.exit("check_charger_binary: debugfs exited %d on %s" % (proc.returncode, image))
    # debugfs exits 0 for a missing file, and for an image it cannot open, and says so on stderr only.
    if b"File not found" in proc.stderr:
        sys.exit("check_charger_binary: %s has no %s" % (image, CHARGER))
    if not proc.stdout:
        why = [line for line in proc.stderr.decode("utf-8", "replace").splitlines()
               if line.strip() and not re.match(r"debugfs [0-9]", line)]
        sys.exit("check_charger_binary: debugfs could not read %s from %s: %s"
                 % (CHARGER, image, "; ".join(why) or "no output"))
    return proc.stdout


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    err = problem(read_charger(sys.argv[1]))
    if err:
        sys.exit("check_charger_binary: " + err)
    print("the charger looks for the battery until it finds one")


if __name__ == "__main__":
    main()
