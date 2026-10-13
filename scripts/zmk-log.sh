#!/usr/bin/env bash
# Capture the USB logs of every connected Halcyon part (dongle, right half)
# into ~/zmk-logs/<serial>-<interface>.log, one file per USB serial port,
# each line prefixed with the host clock time.
#
# Needs CONFIG_ZMK_USB_LOGGING=y on the part (see config/*.conf). The dongle
# exposes two serial ports (ZMK Studio and the log console); both are
# captured, the one that stays empty is Studio.
#
# Usage: scripts/zmk-log.sh [directory]   (Ctrl-C to stop)
set -euo pipefail

dir=${1:-$HOME/zmk-logs}
mkdir -p "$dir"

pids=()
cleanup() {
    for p in "${pids[@]:-}"; do
        [ -n "$p" ] && kill "$p" 2>/dev/null || true
    done
}
trap cleanup EXIT INT TERM

shopt -s nullglob
ports=(/dev/serial/by-id/usb-ZMK_Project_*)
if [ ${#ports[@]} -eq 0 ]; then
    echo "no ZMK USB serial ports found (is logging enabled and the part plugged in?)" >&2
    exit 1
fi

for dev in "${ports[@]}"; do
    name=$(basename "$dev")
    name=${name#usb-ZMK_Project_}
    tty=$(readlink -f "$dev")
    out="$dir/$name.log"
    if [ ! -r "$tty" ]; then
        echo "cannot read $tty (add yourself to the 'dialout' group?), skipping" >&2
        continue
    fi
    stty -F "$tty" 115200 raw -echo 2>/dev/null || true
    echo "capturing $tty -> $out"
    (
        exec 3<"$tty" || { echo "cannot open $tty (in use by another program?)" >&2; exit 1; }
        while IFS= read -r line <&3; do
            printf '%(%T)T %s\n' -1 "$line"
        done >>"$out"
        echo "$tty closed (unplugged or reflashed), capture of $name stopped" >&2
    ) &
    pids+=($!)
done

if [ ${#pids[@]} -eq 0 ]; then
    echo "nothing to capture" >&2
    exit 1
fi

echo
echo "Logging. Use the keyboard as usual; when the touchpad goes crazy, note the"
echo "clock time (e.g. run: date +%T) and keep going for ~20 s, then press Ctrl-C."
wait
