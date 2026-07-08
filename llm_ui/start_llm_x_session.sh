#!/bin/sh

export DISPLAY=:0
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/runtime-root}"
export GTK_IM_MODULE=ibus
export QT_IM_MODULE=ibus
export XMODIFIERS=@im=ibus
export LC_CTYPE=C.UTF-8
export LANG=C.UTF-8
export LLM_BACKEND="${LLM_BACKEND:-rkllm}"
export LLM_TOKENS="${LLM_TOKENS:-48}"
export RKLLM_CONTEXT="${RKLLM_CONTEXT:-1024}"
export RKLLM_BIN="${RKLLM_BIN:-/userdata/llm/rkllm/bin/rkllm_once}"
export RKLLM_MODEL="${RKLLM_MODEL:-/userdata/llm/rkllm/models/MiniCPM4-0.5B_w4a16_RV1126B.rkllm}"
export LD_LIBRARY_PATH="/userdata/llm/rkllm/lib:${LD_LIBRARY_PATH:-}"
export RKLLM_LOG_LEVEL="${RKLLM_LOG_LEVEL:-0}"
export LOW_POWER_IDLE_SECONDS="${LOW_POWER_IDLE_SECONDS:-60}"
export LOW_POWER_RESTART_UI_ON_WAKE="${LOW_POWER_RESTART_UI_ON_WAKE:-1}"
export STRANGER_ALERT_INTERVAL_SECONDS="${STRANGER_ALERT_INTERVAL_SECONDS:-10}"
export SECURITY_CONFIG_FILE="${SECURITY_CONFIG_FILE:-/userdata/llm/ui/security_config.json}"
export VOICE_INPUT_DEV="${VOICE_INPUT_DEV:-auto}"
export VOICE_INPUT_CANDIDATES="${VOICE_INPUT_CANDIDATES:-plughw:2,0}"
export VOICE_OUTPUT_DEV="${VOICE_OUTPUT_DEV:-pulse}"
export VOICE_OUTPUT_VOLUME="${VOICE_OUTPUT_VOLUME:-130%}"
export VOICE_RECORD_SECONDS="${VOICE_RECORD_SECONDS:-4}"
export VOICE_MIC_CARD="${VOICE_MIC_CARD:-2}"
export VOICE_MIC_VOLUME="${VOICE_MIC_VOLUME:-115}"
export VOICE_MIN_CONF="${VOICE_MIN_CONF:-0.68}"
export VOICE_MIN_RMS="${VOICE_MIN_RMS:-60}"
export VOICE_TTS_RATE="${VOICE_TTS_RATE:-180}"
export VOSK_MODEL_DIR="${VOSK_MODEL_DIR:-/userdata/llm/voice/vosk-model-small-cn-0.22}"
export SHERPA_TTS_SCRIPT="${SHERPA_TTS_SCRIPT:-/userdata/llm/ui/sherpa_tts.py}"
export SHERPA_TTS_TYPE="${SHERPA_TTS_TYPE:-matcha}"
export SHERPA_TTS_MODEL_DIR="${SHERPA_TTS_MODEL_DIR:-/userdata/llm/voice/matcha-icefall-zh-baker}"
export SHERPA_TTS_VOCODER="${SHERPA_TTS_VOCODER:-/userdata/llm/voice/vocos-22khz-univ.onnx}"
export SHERPA_TTS_SID="${SHERPA_TTS_SID:-0}"
export SHERPA_TTS_SPEED="${SHERPA_TTS_SPEED:-1.15}"
export SHERPA_TTS_SILENCE_SCALE="${SHERPA_TTS_SILENCE_SCALE:-0.16}"
export SHERPA_TTS_THREADS="${SHERPA_TTS_THREADS:-4}"
export VOICE_TTS_MAX_CHARS="${VOICE_TTS_MAX_CHARS:-36}"
export VOICE_TTS_PRELOAD="${VOICE_TTS_PRELOAD:-1}"
export VOICE_TTS_STARTUP_PRELOAD="${VOICE_TTS_STARTUP_PRELOAD:-1}"
export VOICE_TTS_WARM_ON_INPUT="${VOICE_TTS_WARM_ON_INPUT:-1}"
export VOICE_TTS_FAST_FALLBACK="${VOICE_TTS_FAST_FALLBACK:-1}"
export VOICE_ASR_PRELOAD="${VOICE_ASR_PRELOAD:-0}"
export FACE_X11_FPS="${FACE_X11_FPS:-8}"
export FACE_X11_CAMERA_FPS="${FACE_X11_CAMERA_FPS:-8}"
export FACE_X11_DETECT_INTERVAL="${FACE_X11_DETECT_INTERVAL:-4}"
export FACE_X11_RECOG_INTERVAL="${FACE_X11_RECOG_INTERVAL:-6}"
export FACE_X11_NICE="${FACE_X11_NICE:-8}"

mkdir -p "${XDG_RUNTIME_DIR}"
chmod 700 "${XDG_RUNTIME_DIR}"
mkdir -p /root/.config/gtk-3.0
mkdir -p /root/.config/openbox
if [ -f /userdata/llm/ui/openbox_rc.xml ]; then
    cp /userdata/llm/ui/openbox_rc.xml /root/.config/openbox/rc.xml
fi
cat >/root/.config/gtk-3.0/settings.ini <<'EOF'
[Settings]
gtk-font-name=SimHei 24
gtk-theme-name=Adwaita
gtk-application-prefer-dark-theme=false
EOF
cat >/root/.config/gtk-3.0/gtk.css <<'EOF'
* {
  font-family: SimHei, sans-serif;
  color: #111111;
}
window, popover, menu, .background {
  background-color: #ffffff;
  color: #111111;
}
label {
  color: #111111;
}
entry, textview, text {
  background-color: #ffffff;
  color: #111111;
}
EOF

ibus-daemon -drx
sleep 1
stop_ibus_gtk_extension() {
    pkill -f '[i]bus-extension-gtk3' 2>/dev/null || true
}
stop_ibus_gtk_extension
gsettings set org.freedesktop.ibus.general preload-engines "['libpinyin', 'xkb:us::eng']" 2>/dev/null
gsettings set org.freedesktop.ibus.general use-system-keyboard-layout false 2>/dev/null
gsettings set org.freedesktop.ibus.panel use-custom-font true 2>/dev/null
gsettings set org.freedesktop.ibus.panel custom-font 'SimHei 18' 2>/dev/null
ibus engine libpinyin
stop_ibus_gtk_extension

openbox >/userdata/llm/ui/openbox.log 2>&1 &
sleep 1
matchbox-keyboard >/userdata/llm/ui/matchbox-keyboard.log 2>&1 &
sleep 1
ibus engine libpinyin
xset s off
xset -dpms
xset dpms force on

if [ -f /userdata/llm/rkllm/fix_freq_rv1126b.sh ]; then
    sh /userdata/llm/rkllm/fix_freq_rv1126b.sh >/userdata/llm/rkllm/fix_freq.log 2>&1 || true
fi

if command -v amixer >/dev/null 2>&1; then
    amixer sset Master 100% unmute >/dev/null 2>&1 || true
    amixer -c "$VOICE_MIC_CARD" sset 'Auto Gain Control' off >/dev/null 2>&1 || true
    amixer -c "$VOICE_MIC_CARD" sset Mic "$VOICE_MIC_VOLUME" >/dev/null 2>&1 || true
    amixer -c 1 sset Mic 2800 >/dev/null 2>&1 || true
fi
if command -v pactl >/dev/null 2>&1; then
    pactl set-sink-volume @DEFAULT_SINK@ "$VOICE_OUTPUT_VOLUME" >/dev/null 2>&1 || true
fi

start_face_recognition_window() {
    FACE_ROOT="${FACE_ROOT:-/userdata/face_ai}"
    VIDEO_DEV="${VIDEO_DEV:-/dev/v4l/by-id/usb-icSpring_icspring_camera-video-index0}"
    i=0
    while [ "$i" -lt 12 ]; do
        [ -e "$VIDEO_DEV" ] && break
        i=$((i + 1))
        sleep 1
    done
    if [ ! -e "$VIDEO_DEV" ]; then
        VIDEO_DEV="/dev/video52"
    fi
    if [ ! -e "$VIDEO_DEV" ] || [ ! -x "$FACE_ROOT/run_x11_recognize.sh" ]; then
        echo "face x11 skipped: camera or runtime missing" >/tmp/face_ai_x11_recognize.log
        return 0
    fi
    systemctl stop face-ai-drm.service 2>/dev/null || true
    if [ -f /tmp/face_ai_live_face.pid ]; then
        kill "$(cat /tmp/face_ai_live_face.pid)" 2>/dev/null || true
        rm -f /tmp/face_ai_live_face.pid
    fi
    if [ -f /tmp/face_ai_x11_recognize.pid ]; then
        kill "$(cat /tmp/face_ai_x11_recognize.pid)" 2>/dev/null || true
        rm -f /tmp/face_ai_x11_recognize.pid
    fi
    for pid in $(fuser "$VIDEO_DEV" 2>/dev/null || true); do
        kill "$pid" 2>/dev/null || true
    done
    setsid nice -n "$FACE_X11_NICE" env DISPLAY=:0 WINDOW_X=0 WINDOW_Y=0 WINDOW_WIDTH=512 WINDOW_HEIGHT=600 \
        WIDTH=640 HEIGHT=480 FPS="$FACE_X11_FPS" CAMERA_FPS="$FACE_X11_CAMERA_FPS" PIXEL_FORMAT=MJPG CAMERA_BUFFERS=3 DETECT_INTERVAL="$FACE_X11_DETECT_INTERVAL" FACE_RECOG_INTERVAL="$FACE_X11_RECOG_INTERVAL" \
        STRANGER_ALERT_SECONDS=10 STRANGER_ALERT_INTERVAL_SECONDS="$STRANGER_ALERT_INTERVAL_SECONDS" STRANGER_ENTRY_CONFIRM_SECONDS=1 SECURITY_STATUS_FILE=/tmp/face_ai_security_status.json SECURITY_EVENTS_FILE=/tmp/face_ai_security_events.jsonl SECURITY_CONFIG_FILE="$SECURITY_CONFIG_FILE" \
        FACE_DB_DIR="$FACE_ROOT/faces" FACE_FEATURE_MODEL="$FACE_ROOT/model/ArcFace_112_rv1126b.rknn" \
        "$FACE_ROOT/run_x11_recognize.sh" "$VIDEO_DEV" >/tmp/face_ai_x11_recognize.log 2>&1 < /dev/null &
    face_pid=$!
    echo "$face_pid" >/tmp/face_ai_x11_recognize.pid
    renice -n "$FACE_X11_NICE" -p "$face_pid" >/dev/null 2>&1 || true
    (
        sleep 3
        renice -n "$FACE_X11_NICE" -p "$face_pid" >/dev/null 2>&1 || true
        stop_ibus_gtk_extension
    ) &
}

start_face_recognition_window

export LLM_WIN_X=512
export LLM_WIN_Y=0
export LLM_WIN_W=512
export LLM_WIN_H=360
python3 /userdata/llm/ui/llm_gtk_chat.py &
sleep 5
python3 /userdata/llm/ui/arrange_x_windows.py >/userdata/llm/ui/arrange_x_windows.log 2>&1 || true
wait
