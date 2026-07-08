#!/bin/sh
set -eu

BOARD="${BOARD:-root@192.168.0.232}"
BOARD_DIR="${BOARD_DIR:-/userdata/face_ai}"
VIDEO_DEV="${1:-/dev/video52}"
WIDTH="${WIDTH:-640}"
HEIGHT="${HEIGHT:-480}"
FPS="${FPS:-30}"
SCREEN_WIDTH="${SCREEN_WIDTH:-1024}"
SCREEN_HEIGHT="${SCREEN_HEIGHT:-600}"
DETECT_INTERVAL="${DETECT_INTERVAL:-2}"

ssh "$BOARD" "
if test -f /tmp/face_ai_live_face.pid; then
  kill \$(cat /tmp/face_ai_live_face.pid) 2>/dev/null || true
  sleep 1
  kill -9 \$(cat /tmp/face_ai_live_face.pid) 2>/dev/null || true
  rm -f /tmp/face_ai_live_face.pid
fi
if test -f /tmp/face_ai_screen.pid; then
  kill \$(cat /tmp/face_ai_screen.pid) 2>/dev/null || true
  sleep 1
  kill -9 \$(cat /tmp/face_ai_screen.pid) 2>/dev/null || true
  rm -f /tmp/face_ai_screen.pid
fi
for pid in \$(fuser '$VIDEO_DEV' 2>/dev/null || true); do
  kill \$pid 2>/dev/null || true
  sleep 1
  kill -9 \$pid 2>/dev/null || true
done
export DISPLAY=:0
export XAUTHORITY=/var/run/lightdm/root/:0
xset s off -dpms 2>/dev/null || true
cd '$BOARD_DIR'
setsid env DISPLAY=:0 XAUTHORITY=/var/run/lightdm/root/:0 WIDTH='$WIDTH' HEIGHT='$HEIGHT' FPS='$FPS' SCREEN_WIDTH='$SCREEN_WIDTH' SCREEN_HEIGHT='$SCREEN_HEIGHT' DETECT_INTERVAL='$DETECT_INTERVAL' ./run_screen.sh '$VIDEO_DEV' > /tmp/face_ai_screen.log 2>&1 < /dev/null &
echo \$! > /tmp/face_ai_screen.pid
sleep 2
cat /tmp/face_ai_screen.log
"
