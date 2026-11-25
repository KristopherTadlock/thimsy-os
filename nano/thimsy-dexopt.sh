#!/system/bin/sh
# Compiles the apps setup installed (setup.sh installs them without dexopt to keep setup short).
# Emulators and games are compiled ahead of time in full ("speed"), so they start without
# interpreting or verifying their code at launch; every other installed app gets "verify". Runs
# once, at background priority, a minute after a boot that follows a completed setup.

[ "$(getprop persist.thimsy.dexopt_done)" = 1 ] && exit 0
[ -e /data/setupcompleted ] || exit 0
sleep 60

SPEED="com.retroarch.aarch64 org.ppsspp.ppsspp com.flycast.emulator org.dolphinemu.dolphinemu
com.armsx2 org.azahar_emu.azahar org.vita3k.emulator org.mupen64plusae.v3.fzurita
org.scummvm.scummvm com.dsemu.drastic me.magnum.melondualds com.seedlessds.app com.rfandango.haku_x
com.shatteredpixel.shatteredpixeldungeon"

installed=$(pm list packages -3 2>/dev/null | sed 's/^package://')
[ -n "$installed" ] || exit 0
failed=0
for pkg in $installed; do
    filter=verify
    for s in $SPEED; do [ "$pkg" = "$s" ] && filter=speed; done
    out=$(cmd package compile -r bg-dexopt -m "$filter" "$pkg" 2>&1)
    case "$out" in
        *Success*) log -t thimsy-dexopt "$pkg: $filter" ;;
        *) log -t thimsy-dexopt "$pkg: $filter failed: $out"; failed=1 ;;
    esac
done
[ "$failed" = 0 ] && setprop persist.thimsy.dexopt_done 1
exit 0
