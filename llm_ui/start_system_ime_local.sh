#!/bin/sh
set -eu

export HOME=/root
export XDG_RUNTIME_DIR=/tmp/runtime-root

mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

mkdir -p /userdata/llm/ui /usr/local/share/fonts/llm
if [ -f /userdata/llm/ui/fonts/simhei.ttf ] && [ ! -f /usr/local/share/fonts/llm/simhei.ttf ]; then
    cp /userdata/llm/ui/fonts/simhei.ttf /usr/local/share/fonts/llm/simhei.ttf
fi
fc-cache -f /usr/local/share/fonts/llm >/dev/null 2>&1 || true

for d in /sys/class/backlight/*; do
    [ -e "$d/brightness" ] || continue
    echo 0 >"$d/bl_power" 2>/dev/null || true
    max="$(cat "$d/max_brightness" 2>/dev/null || echo 255)"
    echo "$max" >"$d/brightness" 2>/dev/null || true
done

rm -f /tmp/.X0-lock /tmp/.X11-unix/X0 2>/dev/null || true

exec dbus-run-session -- /usr/bin/xinit /userdata/llm/ui/start_llm_x_session.sh -- /usr/lib/xorg/Xorg :0 vt1 -nolisten tcp -noreset
