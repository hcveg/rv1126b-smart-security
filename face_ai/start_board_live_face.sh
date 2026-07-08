#!/bin/sh
set -eu

BOARD="${BOARD:-root@192.168.0.232}"
BOARD_DIR="${BOARD_DIR:-/userdata/face_ai}"
VIDEO_DEV="${1:-/dev/video52}"
PORT="${PORT:-8090}"
WIDTH="${WIDTH:-640}"
HEIGHT="${HEIGHT:-480}"
FPS="${FPS:-15}"

ssh "$BOARD" "
if test -f /tmp/live_camera_server.pid; then
  kill \$(cat /tmp/live_camera_server.pid) 2>/dev/null || true
  rm -f /tmp/live_camera_server.pid
fi
rm -f /tmp/live_camera_server.py /tmp/live_camera_server.log
if test -f /tmp/face_ai_live_face.pid; then
  kill \$(cat /tmp/face_ai_live_face.pid) 2>/dev/null || true
  rm -f /tmp/face_ai_live_face.pid
fi
for pid in \$(fuser -n tcp '$PORT' 2>/dev/null || true); do
  kill \$pid 2>/dev/null || true
done
for pid in \$(fuser '$VIDEO_DEV' 2>/dev/null || true); do
  kill \$pid 2>/dev/null || true
done
sleep 1
for pid in \$(fuser -n tcp '$PORT' 2>/dev/null || true); do
  kill -9 \$pid 2>/dev/null || true
done
for pid in \$(fuser '$VIDEO_DEV' 2>/dev/null || true); do
  kill -9 \$pid 2>/dev/null || true
done
sleep 1
cd '$BOARD_DIR'
setsid env PORT='$PORT' WIDTH='$WIDTH' HEIGHT='$HEIGHT' FPS='$FPS' ./run_live_face.sh '$VIDEO_DEV' > /tmp/face_ai_live_face.log 2>&1 < /dev/null &
echo \$! > /tmp/face_ai_live_face.pid
sleep 2
cat /tmp/face_ai_live_face.log
"

echo "open http://192.168.0.232:$PORT/"
