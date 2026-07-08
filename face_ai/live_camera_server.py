#!/usr/bin/env python3
import argparse
import json
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class FrameState:
    def __init__(self):
        self.condition = threading.Condition()
        self.frame = None
        self.frame_id = 0
        self.error = ""
        self.started_at = time.time()

    def set_frame(self, frame):
        with self.condition:
            self.frame = frame
            self.frame_id += 1
            self.condition.notify_all()

    def set_error(self, error):
        with self.condition:
            self.error = error
            self.condition.notify_all()


def camera_worker(args, state, stop_event):
    remote = (
        f"v4l2-ctl -d {args.device} "
        f"--set-fmt-video=width={args.width},height={args.height},pixelformat=MJPG "
        f"--set-parm={args.fps} "
        f"--stream-mmap=3 --stream-skip=2 --stream-to=-"
    )
    cmd = ["sh", "-lc", remote] if args.local else ["ssh", args.board, remote]
    process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def read_stderr():
        chunks = []
        while not stop_event.is_set():
            data = process.stderr.readline()
            if not data:
                break
            text = data.decode("utf-8", errors="replace").strip()
            if text:
                chunks.append(text)
                state.set_error("\n".join(chunks[-8:]))

    stderr_thread = threading.Thread(target=read_stderr, daemon=True)
    stderr_thread.start()

    buffer = bytearray()
    try:
        while not stop_event.is_set():
            chunk = process.stdout.read(8192)
            if not chunk:
                if process.poll() is not None:
                    state.set_error(f"camera stream exited with code {process.returncode}")
                    break
                time.sleep(0.02)
                continue

            buffer.extend(chunk)
            while True:
                start = buffer.find(b"\xff\xd8")
                if start < 0:
                    if len(buffer) > 1024 * 1024:
                        del buffer[:-2]
                    break
                end = buffer.find(b"\xff\xd9", start + 2)
                if end < 0:
                    if start > 0:
                        del buffer[:start]
                    break
                frame = bytes(buffer[start : end + 2])
                del buffer[: end + 2]
                state.set_frame(frame)
    finally:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()


def make_handler(state):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            return

        def do_GET(self):
            if self.path in ("/", "/index.html"):
                body = f"""<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>RV1126B Camera</title>
  <style>
    body {{ margin: 0; background: #101214; color: #f4f4f4; font-family: Arial, sans-serif; }}
    header {{ height: 44px; display: flex; align-items: center; padding: 0 14px; background: #1b1f23; }}
    img {{ display: block; width: 100vw; height: calc(100vh - 44px); object-fit: contain; background: #000; }}
    .meta {{ margin-left: auto; color: #b7bec7; font-size: 13px; }}
  </style>
</head>
<body>
  <header>
    <strong>RV1126B USB Camera</strong>
    <span class="meta">{time.strftime('%Y-%m-%d %H:%M:%S')}</span>
  </header>
  <img src="/stream.mjpg" alt="live camera stream">
</body>
</html>
"""
                data = body.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return

            if self.path == "/status":
                with state.condition:
                    payload = {
                        "frames": state.frame_id,
                        "has_frame": state.frame is not None,
                        "error": state.error,
                        "uptime_sec": round(time.time() - state.started_at, 1),
                    }
                data = json.dumps(payload).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return

            if self.path == "/snapshot.jpg":
                with state.condition:
                    frame = state.frame
                if frame is None:
                    self.send_response(503)
                    self.end_headers()
                    return
                self.send_response(200)
                self.send_header("Content-Type", "image/jpeg")
                self.send_header("Content-Length", str(len(frame)))
                self.end_headers()
                self.wfile.write(frame)
                return

            if self.path == "/stream.mjpg":
                self.send_response(200)
                self.send_header("Age", "0")
                self.send_header("Cache-Control", "no-cache, private")
                self.send_header("Pragma", "no-cache")
                self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
                self.end_headers()

                last_id = -1
                while True:
                    with state.condition:
                        state.condition.wait_for(lambda: state.frame_id != last_id or state.error, timeout=2)
                        frame = state.frame
                        last_id = state.frame_id
                    if frame is None:
                        continue
                    try:
                        self.wfile.write(b"--frame\r\n")
                        self.wfile.write(b"Content-Type: image/jpeg\r\n")
                        self.wfile.write(f"Content-Length: {len(frame)}\r\n\r\n".encode("ascii"))
                        self.wfile.write(frame)
                        self.wfile.write(b"\r\n")
                    except BrokenPipeError:
                        return
                return

            self.send_response(404)
            self.end_headers()

    return Handler


def main():
    parser = argparse.ArgumentParser(description="Serve a live MJPEG preview from an RV1126B USB camera over SSH.")
    parser.add_argument("--board", default="root@192.168.0.232")
    parser.add_argument("--local", action="store_true", help="read the camera on this machine instead of over SSH")
    parser.add_argument("--device", default="/dev/video52")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8090)
    args = parser.parse_args()

    state = FrameState()
    stop_event = threading.Event()
    worker = threading.Thread(target=camera_worker, args=(args, state, stop_event), daemon=True)
    worker.start()

    server = ThreadingHTTPServer((args.host, args.port), make_handler(state))

    def stop(_signum, _frame):
        stop_event.set()
        server.shutdown()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    print(f"open http://{args.host}:{args.port}/", flush=True)
    try:
        server.serve_forever()
    finally:
        stop_event.set()
        server.server_close()


if __name__ == "__main__":
    sys.exit(main())
