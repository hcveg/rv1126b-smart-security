#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOARD="${BOARD:-root@192.168.0.232}"
MODEL_NAME="qwen2.5-0.5b-instruct-q4_k_m.gguf"
PINYIN_SRC="${PINYIN_SRC:-/home/enovo/qs/ELF-RV1126B-linux-source/rtos/components/system/lvgl9/src/others/ime/lv_ime_pinyin.c}"
RKLLM_DIR="${RKLLM_DIR:-${ROOT_DIR}/../rkllm_minicpm4}"

if [ ! -f "${ROOT_DIR}/llama-simple" ]; then
    echo "missing ${ROOT_DIR}/llama-simple"
    echo "先等板子编译完成，或把已编译的 llama-simple 放到这个目录。"
    exit 1
fi

ssh "${BOARD}" "mkdir -p /userdata/llm/llama-simple/bin /userdata/llm/models /userdata/llm/ui/fonts /usr/local/share/fonts/llm"
scp "${ROOT_DIR}/llama-simple" "${BOARD}:/userdata/llm/llama-simple/bin/llama-simple"
scp "${ROOT_DIR}/llm_touch_chat.py" "${BOARD}:/userdata/llm/ui/llm_touch_chat.py"
scp "${ROOT_DIR}/llm_gtk_chat.py" "${BOARD}:/userdata/llm/ui/llm_gtk_chat.py"
scp "${ROOT_DIR}/arrange_x_windows.py" "${BOARD}:/userdata/llm/ui/arrange_x_windows.py"
scp "${ROOT_DIR}/openbox_rc.xml" "${BOARD}:/userdata/llm/ui/openbox_rc.xml"
scp "${ROOT_DIR}/start_llm_x_session.sh" "${BOARD}:/userdata/llm/ui/start_llm_x_session.sh"
scp "${ROOT_DIR}/start_system_ime_local.sh" "${BOARD}:/userdata/llm/ui/start_system_ime_local.sh"
scp "${ROOT_DIR}/face_control.sh" "${BOARD}:/userdata/llm/ui/face_control.sh"
scp "${ROOT_DIR}/fonts/simhei.ttf" "${BOARD}:/userdata/llm/ui/fonts/simhei.ttf"
scp "${ROOT_DIR}/fonts/simhei.ttf" "${BOARD}:/usr/local/share/fonts/llm/simhei.ttf"
scp "${ROOT_DIR}/llm-system-ime.service" "${BOARD}:/etc/systemd/system/llm-system-ime.service"
if [ -f "${PINYIN_SRC}" ]; then
    scp "${PINYIN_SRC}" "${BOARD}:/userdata/llm/ui/lv_ime_pinyin.c"
fi

if [ -f "${ROOT_DIR}/models/${MODEL_NAME}" ]; then
    scp "${ROOT_DIR}/models/${MODEL_NAME}" "${BOARD}:/userdata/llm/models/${MODEL_NAME}"
else
    echo "warning: model not found: ${ROOT_DIR}/models/${MODEL_NAME}"
fi

ssh "${BOARD}" "chmod +x /userdata/llm/llama-simple/bin/llama-simple /userdata/llm/ui/llm_touch_chat.py /userdata/llm/ui/llm_gtk_chat.py /userdata/llm/ui/arrange_x_windows.py /userdata/llm/ui/start_llm_x_session.sh /userdata/llm/ui/start_system_ime_local.sh /userdata/llm/ui/face_control.sh; fc-cache -f /usr/local/share/fonts/llm >/dev/null 2>&1 || true; systemctl daemon-reload"
if [ -x "${RKLLM_DIR}/deploy_runtime_to_board.sh" ]; then
    BOARD="${BOARD}" "${RKLLM_DIR}/deploy_runtime_to_board.sh"
fi
echo "restored to ${BOARD}:/userdata/llm"
