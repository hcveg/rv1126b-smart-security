#!/usr/bin/env bash
set -euo pipefail

BOARD="${BOARD:-root@192.168.0.232}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ssh "${BOARD}" "mkdir -p /userdata/llm/ui/fonts /userdata/llm/llama-simple/bin /userdata/llm/models /usr/local/share/fonts/llm"
scp "${ROOT_DIR}/llm_gtk_chat.py" "${BOARD}:/userdata/llm/ui/llm_gtk_chat.py"
scp "${ROOT_DIR}/arrange_x_windows.py" "${BOARD}:/userdata/llm/ui/arrange_x_windows.py"
scp "${ROOT_DIR}/openbox_rc.xml" "${BOARD}:/userdata/llm/ui/openbox_rc.xml"
scp "${ROOT_DIR}/start_llm_x_session.sh" "${BOARD}:/userdata/llm/ui/start_llm_x_session.sh"
scp "${ROOT_DIR}/start_system_ime_local.sh" "${BOARD}:/userdata/llm/ui/start_system_ime_local.sh"
scp "${ROOT_DIR}/face_control.sh" "${BOARD}:/userdata/llm/ui/face_control.sh"
scp "${ROOT_DIR}/fonts/simhei.ttf" "${BOARD}:/userdata/llm/ui/fonts/simhei.ttf"
scp "${ROOT_DIR}/fonts/simhei.ttf" "${BOARD}:/usr/local/share/fonts/llm/simhei.ttf"
if [ -f "${ROOT_DIR}/llama-simple" ]; then
    scp "${ROOT_DIR}/llama-simple" "${BOARD}:/userdata/llm/llama-simple/bin/llama-simple"
fi

ssh "${BOARD}" "chmod +x /userdata/llm/ui/llm_gtk_chat.py /userdata/llm/ui/arrange_x_windows.py /userdata/llm/ui/start_llm_x_session.sh /userdata/llm/ui/start_system_ime_local.sh /userdata/llm/ui/face_control.sh /userdata/llm/llama-simple/bin/llama-simple; fc-cache -f /usr/local/share/fonts/llm >/dev/null 2>&1 || true"
ssh "${BOARD}" "systemctl stop face-ai-drm.service 2>/dev/null || true; if [ -f /userdata/llm/ui/llm_touch_chat.pid ]; then kill \$(cat /userdata/llm/ui/llm_touch_chat.pid) 2>/dev/null || true; fi; if [ -f /userdata/llm/ui/llm_x_session.pid ]; then kill \$(cat /userdata/llm/ui/llm_x_session.pid) 2>/dev/null || true; fi; pkill Xorg 2>/dev/null || true; sleep 1; : > /userdata/llm/ui/llm_x_session.log; nohup dbus-run-session -- /usr/bin/xinit /userdata/llm/ui/start_llm_x_session.sh -- /usr/lib/xorg/Xorg :0 vt1 -nolisten tcp -noreset >>/userdata/llm/ui/llm_x_session.log 2>&1 & echo \$! >/userdata/llm/ui/llm_x_session.pid"
sleep 4
ssh "${BOARD}" "cat /userdata/llm/ui/llm_x_session.pid; pgrep -af 'Xorg|llm_gtk_chat|ibus|matchbox|openbox' || true; tail -80 /userdata/llm/ui/llm_x_session.log"
