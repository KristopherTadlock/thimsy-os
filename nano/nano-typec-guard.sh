#!/system/bin/sh
# Puts the USB-C port back to dual role if Nano's monitor fix (NanoTypec.cpp) left it one-role, so
# the Cube still charges if Nano dies or stalls during a try. Nano restarts it with each source-only
# or sink-only write; it follows the try (sys.gammaos.nano.typec) and, if one state lasts 25 s,
# writes dual role back itself. An init service: a Nano crash takes down its own process group, not
# this.

port=$(getprop sys.gammaos.nano.typec_port)
case "$port" in /sys/class/typec/port[0-9]) ;; *) exit 0 ;; esac
state=$(getprop sys.gammaos.nano.typec)
while :; do
    case "$state" in flip:*|sink:*) ;; *) exit 0 ;; esac
    sleep 25
    now=$(getprop sys.gammaos.nano.typec)
    if [ "$now" = "$state" ]; then
        for try in 1 2 3 4 5; do
            echo dual > "$port/port_type" && { setprop sys.gammaos.nano.typec tried:0; exit 0; }
            sleep 2
        done
        exit 1
    fi
    state=$now
done
