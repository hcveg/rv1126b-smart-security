#!/bin/sh
set -eu

ACTION="${1:-status}"
FACE_DIR="${FACE_DIR:-/userdata/face_ai}"
VIDEO_DEV="${VIDEO_DEV:-/dev/v4l/by-id/usb-icSpring_icspring_camera-video-index0}"
PORT="${PORT:-8090}"
WIDTH="${WIDTH:-640}"
HEIGHT="${HEIGHT:-480}"
FPS="${FPS:-15}"
PID_FILE="/tmp/face_ai_live_face.pid"
X11_PID_FILE="/tmp/face_ai_x11_recognize.pid"
LOG_FILE="/tmp/face_ai_live_face.log"
X11_LOG_FILE="/tmp/face_ai_x11_recognize.log"
SECURITY_CONFIG_FILE="${SECURITY_CONFIG_FILE:-/userdata/llm/ui/security_config.json}"
SECURITY_STATUS_FILE="${SECURITY_STATUS_FILE:-/tmp/face_ai_security_status.json}"

if [ ! -e "$VIDEO_DEV" ]; then
    VIDEO_DEV="/dev/video52"
fi

stop_live() {
    if [ -f "$PID_FILE" ]; then
        kill "$(cat "$PID_FILE")" 2>/dev/null || true
        sleep 1
        kill -9 "$(cat "$PID_FILE")" 2>/dev/null || true
        rm -f "$PID_FILE"
    fi
    for pid in $(fuser -n tcp "$PORT" 2>/dev/null || true); do
        kill "$pid" 2>/dev/null || true
        sleep 1
        kill -9 "$pid" 2>/dev/null || true
    done
}

stop_x11() {
    if [ -f "$X11_PID_FILE" ]; then
        kill "$(cat "$X11_PID_FILE")" 2>/dev/null || true
        sleep 1
        kill -9 "$(cat "$X11_PID_FILE")" 2>/dev/null || true
        rm -f "$X11_PID_FILE"
    fi
}

start_x11() {
    if [ ! -e "$VIDEO_DEV" ]; then
        echo "没有检测到摄像头设备。"
        exit 1
    fi
    if [ ! -x "$FACE_DIR/run_x11_recognize.sh" ]; then
        echo "人脸识别窗口程序不存在：$FACE_DIR/run_x11_recognize.sh"
        exit 1
    fi
    stop_live
    stop_x11
    for pid in $(fuser "$VIDEO_DEV" 2>/dev/null || true); do
        kill "$pid" 2>/dev/null || true
    done
    cd "$FACE_DIR"
    setsid env DISPLAY=:0 WINDOW_X="${WINDOW_X:-0}" WINDOW_Y="${WINDOW_Y:-0}" \
        WINDOW_WIDTH="${WINDOW_WIDTH:-512}" WINDOW_HEIGHT="${WINDOW_HEIGHT:-600}" \
        WIDTH="${WIDTH:-640}" HEIGHT="${HEIGHT:-480}" FPS="${FPS:-15}" CAMERA_FPS="${CAMERA_FPS:-15}" \
        PIXEL_FORMAT="${PIXEL_FORMAT:-MJPG}" CAMERA_BUFFERS="${CAMERA_BUFFERS:-4}" \
        DETECT_INTERVAL="${DETECT_INTERVAL:-2}" FACE_RECOG_INTERVAL="${FACE_RECOG_INTERVAL:-3}" \
        FACE_DB_DIR="${FACE_DB_DIR:-$FACE_DIR/faces}" FACE_FEATURE_MODEL="${FACE_FEATURE_MODEL:-$FACE_DIR/model/ArcFace_112_rv1126b.rknn}" \
        ./run_x11_recognize.sh "$VIDEO_DEV" >"$X11_LOG_FILE" 2>&1 < /dev/null &
    echo $! >"$X11_PID_FILE"
    sleep 2
    if kill -0 "$(cat "$X11_PID_FILE")" 2>/dev/null; then
        echo "已开启左侧人脸识别窗口。"
    else
        echo "人脸识别窗口启动失败：$(tail -5 "$X11_LOG_FILE" 2>/dev/null | tr '\n' ' ')"
        exit 1
    fi
}

start_live() {
    if [ ! -e "$VIDEO_DEV" ]; then
        echo "没有检测到摄像头设备。"
        exit 1
    fi
    if [ ! -x "$FACE_DIR/run_live_face.sh" ]; then
        echo "人脸检测程序不存在：$FACE_DIR/run_live_face.sh"
        exit 1
    fi
    stop_live
    cd "$FACE_DIR"
    setsid env PORT="$PORT" WIDTH="$WIDTH" HEIGHT="$HEIGHT" FPS="$FPS" ./run_live_face.sh "$VIDEO_DEV" >"$LOG_FILE" 2>&1 < /dev/null &
    echo $! >"$PID_FILE"
    sleep 2
    if kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
        echo "已开启摄像头和后台人脸检测。屏幕继续显示大模型；电脑可访问 http://192.168.0.232:$PORT/ 查看画面。"
    else
        echo "摄像头启动失败：$(tail -5 "$LOG_FILE" 2>/dev/null | tr '\n' ' ')"
        exit 1
    fi
}

stop_all() {
    stop_live
    stop_x11
    systemctl stop face-ai-drm.service 2>/dev/null || true
    echo "已关闭摄像头人脸检测。"
}

screen_on() {
    if [ ! -e "$VIDEO_DEV" ]; then
        echo "没有检测到摄像头设备。"
        exit 1
    fi
    stop_live
    nohup sh -c 'sleep 2; systemctl stop llm-system-ime.service; systemctl start face-ai-drm.service' >/tmp/llm_face_screen_switch.log 2>&1 &
    echo "已准备切换到屏幕人脸检测，约 2 秒后大模型界面会退出。"
}

status() {
    camera="未检测到"
    [ -e "$VIDEO_DEV" ] && camera="$VIDEO_DEV"
    live="未运行"
    if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
        live="运行中"
    fi
    x11="未运行"
    if [ -f "$X11_PID_FILE" ] && kill -0 "$(cat "$X11_PID_FILE")" 2>/dev/null; then
        x11="运行中"
    fi
    screen="$(systemctl is-active face-ai-drm.service 2>/dev/null || true)"
    [ -n "$screen" ] || screen="unknown"
    alert_interval="$(sed -n 's/.*"alert_interval_seconds":\([0-9][0-9]*\).*/\1/p' "$SECURITY_STATUS_FILE" 2>/dev/null | tail -1)"
    if [ -z "$alert_interval" ]; then
        alert_interval="$(sed -n 's/.*"alert_interval_seconds":\([0-9][0-9]*\).*/\1/p' "$SECURITY_CONFIG_FILE" 2>/dev/null | tail -1)"
    fi
    [ -n "$alert_interval" ] || alert_interval="10"
    echo "摄像头：$camera；左侧人脸识别：$x11；后台网页检测：$live；全屏人脸检测服务：$screen；报警间隔：${alert_interval}秒。"
}

set_alert_interval() {
    raw="${1:-}"
    case "$raw" in
        ''|*[!0-9]*)
            echo "请提供报警间隔秒数，例如：alert-interval 5。"
            exit 2
            ;;
    esac
    requested="$raw"
    interval="$raw"
    if [ "$interval" -lt 3 ]; then
        interval=3
    fi
    tmp="${SECURITY_CONFIG_FILE}.tmp"
    printf '{"alert_interval_seconds":%s,"min_alert_interval_seconds":3}\n' "$interval" >"$tmp"
    mv "$tmp" "$SECURITY_CONFIG_FILE"
    if [ "$requested" != "$interval" ]; then
        echo "报警间隔最小为3秒，已按3秒设置。"
    else
        echo "报警间隔已设置为${interval}秒。"
    fi
}

case "$ACTION" in
    live-on)
        start_live
        ;;
    x11-on)
        start_x11
        ;;
    live-off)
        stop_all
        ;;
    screen-on)
        screen_on
        ;;
    status)
        status
        ;;
    alert-interval|alarm-interval)
        set_alert_interval "${2:-}"
        ;;
    *)
        echo "未知控制命令：$ACTION"
        exit 2
        ;;
esac
