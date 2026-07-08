#!/usr/bin/env bash
set -euo pipefail

BOARD="${BOARD:-root@192.168.0.232}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VOICE_DEPS="${VOICE_DEPS:-/home/enovo/qs/voice_deps}"
MODEL_NAME="${MODEL_NAME:-vosk-model-small-cn-0.22}"

if [ ! -d "${VOICE_DEPS}" ]; then
    echo "missing ${VOICE_DEPS}" >&2
    exit 1
fi

ssh "${BOARD}" "mkdir -p /userdata/llm/voice_deps /userdata/llm/voice /userdata/llm/ui"
scp "${ROOT_DIR}/llm_gtk_chat.py" "${BOARD}:/userdata/llm/ui/llm_gtk_chat.py"
scp "${ROOT_DIR}/start_llm_x_session.sh" "${BOARD}:/userdata/llm/ui/start_llm_x_session.sh"
if [ -f "${ROOT_DIR}/sherpa_tts.py" ]; then
    scp "${ROOT_DIR}/sherpa_tts.py" "${BOARD}:/userdata/llm/ui/sherpa_tts.py"
fi
scp "${VOICE_DEPS}"/*.whl "${BOARD}:/userdata/llm/voice_deps/"
if ls "${VOICE_DEPS}"/*.tar.gz >/dev/null 2>&1; then
    scp "${VOICE_DEPS}"/*.tar.gz "${BOARD}:/userdata/llm/voice_deps/"
fi
if [ -d "${VOICE_DEPS}/sherpa_tts" ]; then
    ssh "${BOARD}" "mkdir -p /userdata/llm/voice_deps/sherpa_tts"
    scp "${VOICE_DEPS}/sherpa_tts"/*.whl "${BOARD}:/userdata/llm/voice_deps/sherpa_tts/" 2>/dev/null || true
    if [ -f "${VOICE_DEPS}/sherpa_tts/vits-icefall-zh-aishell3.tar.bz2" ]; then
        scp "${VOICE_DEPS}/sherpa_tts/vits-icefall-zh-aishell3.tar.bz2" "${BOARD}:/userdata/llm/voice/"
        ssh "${BOARD}" "cd /userdata/llm/voice; rm -rf vits-icefall-zh-aishell3; tar xjf vits-icefall-zh-aishell3.tar.bz2"
    fi
    if [ -f "${VOICE_DEPS}/sherpa_tts/matcha-icefall-zh-baker.tar.bz2" ]; then
        scp "${VOICE_DEPS}/sherpa_tts/matcha-icefall-zh-baker.tar.bz2" "${BOARD}:/userdata/llm/voice/"
        ssh "${BOARD}" "cd /userdata/llm/voice; rm -rf matcha-icefall-zh-baker; tar xjf matcha-icefall-zh-baker.tar.bz2; rm -f matcha-icefall-zh-baker.tar.bz2"
    fi
    if [ -f "${VOICE_DEPS}/sherpa_tts/vocos-22khz-univ.onnx" ]; then
        scp "${VOICE_DEPS}/sherpa_tts/vocos-22khz-univ.onnx" "${BOARD}:/userdata/llm/voice/vocos-22khz-univ.onnx"
    fi
fi
if [ -d "${VOICE_DEPS}/debs" ]; then
    ssh "${BOARD}" "mkdir -p /userdata/llm/voice_deps/debs"
    scp "${VOICE_DEPS}/debs"/*.deb "${BOARD}:/userdata/llm/voice_deps/debs/"
fi

if [ -d "${VOICE_DEPS}/${MODEL_NAME}" ]; then
    scp -r "${VOICE_DEPS}/${MODEL_NAME}" "${BOARD}:/userdata/llm/voice/"
elif [ -f "${VOICE_DEPS}/${MODEL_NAME}.zip" ]; then
    scp "${VOICE_DEPS}/${MODEL_NAME}.zip" "${BOARD}:/userdata/llm/voice/"
    ssh "${BOARD}" "cd /userdata/llm/voice; rm -rf '${MODEL_NAME}'; python3 -m zipfile -e '${MODEL_NAME}.zip' ."
else
    echo "missing ${VOICE_DEPS}/${MODEL_NAME} or ${VOICE_DEPS}/${MODEL_NAME}.zip" >&2
    exit 1
fi

ssh "${BOARD}" "python3 -m pip install --no-index --find-links=/userdata/llm/voice_deps --break-system-packages pycparser cffi certifi charset-normalizer idna urllib3 requests tqdm srt websockets vosk"
ssh "${BOARD}" "if ls /userdata/llm/voice_deps/sherpa_tts/*.whl >/dev/null 2>&1; then python3 -m pip install --no-index --find-links=/userdata/llm/voice_deps/sherpa_tts --break-system-packages sherpa-onnx-core sherpa-onnx; fi"
ssh "${BOARD}" "if ls /userdata/llm/voice_deps/debs/*.deb >/dev/null 2>&1; then dpkg -i /userdata/llm/voice_deps/debs/*.deb; fi"
ssh "${BOARD}" "chmod +x /userdata/llm/ui/llm_gtk_chat.py /userdata/llm/ui/start_llm_x_session.sh /userdata/llm/ui/sherpa_tts.py 2>/dev/null || true"
ssh "${BOARD}" "systemctl restart llm-system-ime.service"
ssh "${BOARD}" "sleep 3; pgrep -af 'llm_gtk_chat|Xorg|openbox|matchbox' || true; tail -80 /userdata/llm/ui/llm_x_session.log || true"
