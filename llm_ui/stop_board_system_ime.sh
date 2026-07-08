#!/usr/bin/env bash
set -euo pipefail

BOARD="${BOARD:-root@192.168.0.232}"
ssh "${BOARD}" "if [ -f /userdata/llm/ui/llm_x_session.pid ]; then kill \$(cat /userdata/llm/ui/llm_x_session.pid) 2>/dev/null || true; fi; pkill -f llm_gtk_chat.py 2>/dev/null || true; pkill matchbox-keyboard 2>/dev/null || true; pkill ibus-daemon 2>/dev/null || true; pkill openbox 2>/dev/null || true; pkill Xorg 2>/dev/null || true; systemctl start face-ai-drm.service 2>/dev/null || true"
