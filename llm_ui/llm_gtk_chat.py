#!/usr/bin/env python3
import json
import gc
import math
import os
import queue
import re
import select
import signal
import shutil
import struct
import subprocess
import sys
import threading
import time
import wave

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import GLib, Gdk, Gtk, Pango


MODEL = os.environ.get("LLM_MODEL", "/userdata/llm/models/qwen2.5-0.5b-instruct-q4_k_m.gguf")
LLAMA = os.environ.get("LLAMA_BIN", "/userdata/llm/llama-simple/bin/llama-simple")
BACKEND = os.environ.get("LLM_BACKEND", "llama").lower()
RKLLM_MODEL = os.environ.get("RKLLM_MODEL", "/userdata/llm/rkllm/models/MiniCPM4-0.5B_w4a16_RV1126B.rkllm")
RKLLM_BIN = os.environ.get("RKLLM_BIN", "/userdata/llm/rkllm/bin/rkllm_once")
RKLLM_CONTEXT = int(os.environ.get("RKLLM_CONTEXT", "1024"))
TOKENS = int(os.environ.get("LLM_TOKENS", "64"))
LOG = os.environ.get("LLM_LOG", "/userdata/llm/ui/llm_gtk_chat.log")
FACE_CONTROL = os.environ.get("FACE_CONTROL", "/userdata/llm/ui/face_control.sh")
SECURITY_STATUS = os.environ.get("SECURITY_STATUS_FILE", "/tmp/face_ai_security_status.json")
SECURITY_STATUS_STALE_SECONDS = float(os.environ.get("SECURITY_STATUS_STALE_SECONDS", "5"))
LOW_POWER_IDLE_SECONDS = float(os.environ.get("LOW_POWER_IDLE_SECONDS", "60"))
LOW_POWER_RESTART_UI_ON_WAKE = os.environ.get("LOW_POWER_RESTART_UI_ON_WAKE", "1") != "0"
INPUT_ACTIVITY_DEBOUNCE_SECONDS = float(os.environ.get("INPUT_ACTIVITY_DEBOUNCE_SECONDS", "0.2"))
LLM_WIN_X = int(os.environ.get("LLM_WIN_X", "0"))
LLM_WIN_Y = int(os.environ.get("LLM_WIN_Y", "0"))
LLM_WIN_W = int(os.environ.get("LLM_WIN_W", "1024"))
LLM_WIN_H = int(os.environ.get("LLM_WIN_H", "330"))
ARRANGE_WINDOWS = os.environ.get("ARRANGE_WINDOWS", "/userdata/llm/ui/arrange_x_windows.py")
KEYBOARD_BIN = os.environ.get("KEYBOARD_BIN", "matchbox-keyboard")
VOICE_INPUT_DEV = os.environ.get("VOICE_INPUT_DEV", "auto")
VOICE_INPUT_CANDIDATES = [
    item.strip()
    for item in os.environ.get("VOICE_INPUT_CANDIDATES", "plughw:2,0").split(";")
    if item.strip()
]
VOICE_LONG_PRESS_MAX_SECONDS = float(os.environ.get("VOICE_LONG_PRESS_MAX_SECONDS", "12"))
VOICE_MIN_HOLD_SECONDS = float(os.environ.get("VOICE_MIN_HOLD_SECONDS", "0.5"))
VOICE_OUTPUT_DEV = os.environ.get("VOICE_OUTPUT_DEV", "pulse")
VOICE_RECORD_SECONDS = int(os.environ.get("VOICE_RECORD_SECONDS", "4"))
VOICE_RECORD_TIMEOUT_EXTRA = float(os.environ.get("VOICE_RECORD_TIMEOUT_EXTRA", "1.5"))
VOICE_RATE = int(os.environ.get("VOICE_RATE", "16000"))
VOICE_TMP_WAV = os.environ.get("VOICE_TMP_WAV", "/tmp/llm_voice_input.wav")
VOICE_TTS_WAV = os.environ.get("VOICE_TTS_WAV", "/tmp/llm_tts_reply.wav")
VOSK_MODEL_DIR = os.environ.get("VOSK_MODEL_DIR", "/userdata/llm/voice/vosk-model-small-cn-0.22")
VOICE_MIN_CONF = float(os.environ.get("VOICE_MIN_CONF", "0.68"))
VOICE_MIN_RMS = float(os.environ.get("VOICE_MIN_RMS", "60"))
VOICE_MAX_PEAK = int(os.environ.get("VOICE_MAX_PEAK", "32700"))
VOICE_TTS_RATE = os.environ.get("VOICE_TTS_RATE", "150")
SHERPA_TTS_SCRIPT = os.environ.get("SHERPA_TTS_SCRIPT", "/userdata/llm/ui/sherpa_tts.py")
SHERPA_TTS_TYPE = os.environ.get("SHERPA_TTS_TYPE", "matcha").lower()
SHERPA_TTS_MODEL_DIR = os.environ.get("SHERPA_TTS_MODEL_DIR", "/userdata/llm/voice/matcha-icefall-zh-baker")
SHERPA_TTS_VOCODER = os.environ.get("SHERPA_TTS_VOCODER", "/userdata/llm/voice/vocos-22khz-univ.onnx")
SHERPA_TTS_SID = os.environ.get("SHERPA_TTS_SID", "0")
SHERPA_TTS_SPEED = float(os.environ.get("SHERPA_TTS_SPEED", "1.02"))
SHERPA_TTS_SILENCE_SCALE = float(os.environ.get("SHERPA_TTS_SILENCE_SCALE", "0.16"))
SHERPA_TTS_THREADS = int(os.environ.get("SHERPA_TTS_THREADS", "3"))
VOICE_TTS_MAX_CHARS = int(os.environ.get("VOICE_TTS_MAX_CHARS", "60"))
VOICE_TTS_PRELOAD = os.environ.get("VOICE_TTS_PRELOAD", "1") != "0"
VOICE_TTS_STARTUP_PRELOAD = os.environ.get("VOICE_TTS_STARTUP_PRELOAD", "0") == "1"
VOICE_ASR_PRELOAD = os.environ.get("VOICE_ASR_PRELOAD", "0") == "1"
VOICE_TTS_WARM_ON_INPUT = os.environ.get("VOICE_TTS_WARM_ON_INPUT", "1") != "0"
VOICE_TTS_FAST_FALLBACK = os.environ.get("VOICE_TTS_FAST_FALLBACK", "1") != "0"


def clean_model_answer(text):
    text = text.replace("\r", "\n")
    text = re.sub(r"(?m)^[IWED] rkllm:.*(?:\n|$)", "", text)
    text = re.split(r"\b[IWED] rkllm:", text, maxsplit=1)[0]
    text = re.split(r"(?:我将如何|您是否愿意|文字转文本|如果对我的模型)", text, maxsplit=1)[0]
    text = re.sub(r"(.{2,12})\1{2,}.*", r"\1", text, flags=re.S)
    for marker in (
        "<|im_end|>",
        "<|im_start|>",
        "<｜im_end｜>",
        "<｜im_start｜>",
        "\nuser:",
        "\nassistant:",
        "\nUser:",
        "\nAssistant:",
        "\n用户：",
        "\n助手：",
    ):
        if marker in text:
            text = text.split(marker, 1)[0]
    sentence_parts = re.split(r"(?<=[。！？!?])", text.strip())
    if len(sentence_parts) > 2:
        text = "".join(sentence_parts[:2])
    return text.strip()


def face_control_action(text):
    compact = re.sub(r"\s+", "", text.lower())
    face_words = ("摄像头", "相机", "camera", "人脸", "检测", "识别")
    if not any(word in compact for word in face_words):
        return None
    if any(word in compact for word in ("状态", "情况", "有没有开", "是否开启", "开了吗")):
        return "status"
    if any(word in compact for word in ("关闭", "关掉", "停止", "停掉", "不要")):
        return "live-off"
    if any(word in compact for word in ("打开", "开启", "启动", "开摄像头", "开始")):
        if any(word in compact for word in ("全屏", "独占屏幕")):
            return "screen-on"
        if any(word in compact for word in ("网页", "浏览器", "电脑", "后台")):
            return "live-on"
        return "x11-on"
    return None


def chinese_number_to_int(text):
    digits = {
        "零": 0,
        "一": 1,
        "二": 2,
        "两": 2,
        "三": 3,
        "四": 4,
        "五": 5,
        "六": 6,
        "七": 7,
        "八": 8,
        "九": 9,
    }
    if not text:
        return None
    if text in digits:
        return digits[text]
    if text == "十":
        return 10
    if "十" in text:
        left, right = text.split("十", 1)
        tens = digits.get(left, 1) if left else 1
        ones = digits.get(right, 0) if right else 0
        return tens * 10 + ones
    return None


def extract_duration_seconds(text):
    compact = re.sub(r"\s+", "", text.lower())
    match = re.search(r"(\d+(?:\.\d+)?)(秒|s|sec|second|seconds)", compact)
    if match:
        return int(math.ceil(float(match.group(1))))
    match = re.search(r"(\d+(?:\.\d+)?)(分钟|分|min|minute|minutes)", compact)
    if match:
        return int(math.ceil(float(match.group(1)) * 60))
    match = re.search(r"([零一二两三四五六七八九十]{1,3})(秒|分钟|分)", compact)
    if match:
        value = chinese_number_to_int(match.group(1))
        if value is None:
            return None
        if match.group(2) in ("分钟", "分"):
            value *= 60
        return value
    return None


def alarm_interval_control(text):
    compact = re.sub(r"\s+", "", text.lower())
    if not any(word in compact for word in ("报警", "警报")):
        return None
    if not any(word in compact for word in ("间隔", "频率", "每隔", "多久")):
        return None
    seconds = extract_duration_seconds(compact)
    if seconds is None:
        if any(word in compact for word in ("多少", "多久", "当前", "现在", "查询")):
            return ("status", None)
        return ("missing", None)
    return ("set", seconds)


def load_security_status():
    try:
        with open(SECURITY_STATUS, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def fmt_seconds(seconds):
    seconds = int(max(0, seconds))
    minutes, sec = divmod(seconds, 60)
    hours, minutes = divmod(minutes, 60)
    if hours:
        return f"{hours}小时{minutes}分{sec}秒"
    if minutes:
        return f"{minutes}分{sec}秒"
    return f"{sec}秒"


def security_answer(text):
    compact = re.sub(r"\s+", "", text.lower())
    security_words = ("陌生人", "人脸", "门禁", "安防", "报警", "访客", "几个人", "多少人", "出现")
    if not any(word in compact for word in security_words):
        return None
    status = load_security_status()
    if not status:
        return "还没有读到人脸识别状态。请确认左侧人脸识别窗口正在运行。"

    uptime = fmt_seconds(status.get("uptime_sec", 0))
    current_faces = int(status.get("current_faces", 0))
    current_unknown = int(status.get("current_unknown", 0))
    current_known = int(status.get("current_known", 0))
    stranger_entries = int(status.get("stranger_entries", 0))
    stranger_alerts = int(status.get("stranger_alerts", 0))
    longest_unknown = float(status.get("longest_unknown_sec", 0.0))
    alert_interval = int(status.get("alert_interval_seconds", 10))
    min_alert_interval = int(status.get("min_alert_interval_seconds", 3))
    known_names = status.get("known_names") or []

    if any(word in compact for word in ("间隔", "频率", "每隔")) and any(word in compact for word in ("报警", "警报")):
        return f"当前报警间隔是 {alert_interval} 秒，最小可设置为 {min_alert_interval} 秒。"
    if any(word in compact for word in ("开机", "启动", "一共", "总共", "累计")) and "陌生人" in compact:
        return f"从本次安防程序启动到现在，累计出现过 {stranger_entries} 次陌生人，触发过 {stranger_alerts} 次停留报警。运行时长 {uptime}。"
    if any(word in compact for word in ("报警", "警报")):
        last_alert = status.get("last_alert") or "暂无新的报警内容"
        return f"累计报警 {stranger_alerts} 次。最近一次：{last_alert}。当前陌生人 {current_unknown} 个。"
    if any(word in compact for word in ("现在", "当前", "屏幕", "画面", "几个人", "多少人")):
        names = "、".join(str(name) for name in known_names) if known_names else "无"
        return f"当前画面内共 {current_faces} 张人脸，已识别 {current_known} 张，陌生人 {current_unknown} 张；已识别人员：{names}。"
    if "停留" in compact or "超过" in compact:
        return f"当前陌生人最长连续停留 {longest_unknown:.1f} 秒，报警阈值是 {status.get('alert_seconds', 10)} 秒。"
    return f"当前安防状态：人脸 {current_faces} 张，陌生人 {current_unknown} 张；累计陌生人 {stranger_entries} 次，报警 {stranger_alerts} 次。"


class SherpaTtsWorker:
    def __init__(self, log_path):
        self.log_path = log_path
        self.q = queue.Queue(maxsize=2)
        self.ready = False
        self.failed = False
        self.suspend_until = 0.0
        self.speaking = False
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def log(self, message):
        try:
            with open(self.log_path, "a", encoding="utf-8") as log:
                log.write(message)
        except Exception:
            pass

    def build_config(self, sherpa_onnx, model_dir):
        model_dir = model_dir.rstrip("/")
        if SHERPA_TTS_TYPE == "matcha":
            return sherpa_onnx.OfflineTtsConfig(
                model=sherpa_onnx.OfflineTtsModelConfig(
                    matcha=sherpa_onnx.OfflineTtsMatchaModelConfig(
                        acoustic_model=f"{model_dir}/model-steps-3.onnx",
                        vocoder=SHERPA_TTS_VOCODER,
                        lexicon=f"{model_dir}/lexicon.txt",
                        tokens=f"{model_dir}/tokens.txt",
                    ),
                    provider="cpu",
                    debug=0,
                    num_threads=SHERPA_TTS_THREADS,
                ),
                rule_fsts=f"{model_dir}/phone.fst,{model_dir}/date.fst,{model_dir}/number.fst",
                max_num_sentences=1,
            )
        return sherpa_onnx.OfflineTtsConfig(
            model=sherpa_onnx.OfflineTtsModelConfig(
                vits=sherpa_onnx.OfflineTtsVitsModelConfig(
                    model=f"{model_dir}/model.onnx",
                    lexicon=f"{model_dir}/lexicon.txt",
                    tokens=f"{model_dir}/tokens.txt",
                ),
                provider="cpu",
                debug=0,
                num_threads=SHERPA_TTS_THREADS,
            ),
            rule_fsts=f"{model_dir}/phone.fst,{model_dir}/date.fst,{model_dir}/number.fst",
            max_num_sentences=1,
        )

    def enqueue(self, text):
        if self.failed:
            return False
        if self.is_suspended():
            return True
        try:
            self.q.put_nowait(text)
            return True
        except queue.Full:
            try:
                self.q.get_nowait()
            except queue.Empty:
                pass
            try:
                self.q.put_nowait(text)
                return True
            except queue.Full:
                return False

    def clear(self):
        while True:
            try:
                self.q.get_nowait()
            except queue.Empty:
                break

    def shutdown(self):
        self.failed = True
        self.clear()
        try:
            self.q.put_nowait(None)
        except queue.Full:
            pass

    def suspend(self, seconds):
        with self.lock:
            self.suspend_until = max(self.suspend_until, time.monotonic() + seconds)
        self.clear()

    def is_suspended(self):
        with self.lock:
            return time.monotonic() < self.suspend_until

    def is_active(self):
        with self.lock:
            speaking = self.speaking
        return speaking or not self.q.empty()

    def run(self):
        if not os.path.isdir(SHERPA_TTS_MODEL_DIR):
            self.failed = True
            return
        try:
            import sherpa_onnx
            from sherpa_tts import normalize_text, write_wav

            model_dir = SHERPA_TTS_MODEL_DIR.rstrip("/")
            config = self.build_config(sherpa_onnx, model_dir)
            if not config.validate():
                raise RuntimeError("invalid sherpa-onnx TTS config")
            start = time.monotonic()
            tts = sherpa_onnx.OfflineTts(config)
            self.ready = True
            self.log(f"\n===== sherpa tts preload =====\nloaded in {time.monotonic() - start:.2f}s\n")
            gen_config = sherpa_onnx.GenerationConfig()
            gen_config.sid = int(SHERPA_TTS_SID)
            gen_config.speed = SHERPA_TTS_SPEED
            gen_config.silence_scale = SHERPA_TTS_SILENCE_SCALE

            while True:
                text = self.q.get()
                if text is None:
                    break
                text = normalize_text(clean_model_answer(text)[:VOICE_TTS_MAX_CHARS])
                if not text:
                    continue
                if self.is_suspended():
                    self.log("\n===== sherpa tts skipped =====\nvoice input active\n")
                    continue
                try:
                    with self.lock:
                        self.speaking = True
                    start = time.monotonic()
                    audio = tts.generate(text, gen_config)
                    if len(audio.samples) == 0:
                        raise RuntimeError("empty generated audio")
                    write_wav(VOICE_TTS_WAV, audio.samples, audio.sample_rate)
                    gen_ms = int((time.monotonic() - start) * 1000)
                    if self.is_suspended():
                        self.log("\n===== sherpa tts playback skipped =====\nvoice input active\n")
                        continue
                    subprocess.run(
                        ["aplay", "-q", "-D", VOICE_OUTPUT_DEV, VOICE_TTS_WAV],
                        text=True,
                        capture_output=True,
                        timeout=45,
                    )
                    self.log(f"\n===== sherpa tts fast =====\ngenerate_ms={gen_ms} chars={len(text)}\n")
                except Exception as exc:
                    self.log(f"\n===== sherpa tts fast error =====\n{exc}\n")
                finally:
                    with self.lock:
                        self.speaking = False
        except Exception as exc:
            self.failed = True
            self.log(f"\n===== sherpa tts preload failed =====\n{exc}\n")


class ChatWindow(Gtk.Window):
    def __init__(self):
        super().__init__(title="RV1126B 本地大模型")
        self.set_decorated(False)
        self.set_default_size(LLM_WIN_W, LLM_WIN_H)
        self.move(LLM_WIN_X, LLM_WIN_Y)
        self.set_keep_above(False)
        self.set_border_width(0)
        activity_mask = Gdk.EventMask.BUTTON_PRESS_MASK | Gdk.EventMask.KEY_PRESS_MASK
        if hasattr(Gdk.EventMask, "TOUCH_MASK"):
            activity_mask |= Gdk.EventMask.TOUCH_MASK
        self.add_events(activity_mask)
        self.messages = []
        self.busy = False
        self.voice_busy = False
        self.result_q = queue.Queue()
        self.voice_q = queue.Queue()
        self.vosk_model = None
        self.vosk_lock = threading.Lock()
        self.tts_worker = SherpaTtsWorker(LOG) if VOICE_TTS_PRELOAD and VOICE_TTS_STARTUP_PRELOAD else None
        self.model_enabled = os.environ.get("LLM_ENABLED", "1") != "0"
        self.model_proc = None
        self.model_proc_lock = threading.Lock()
        self.keyboard_proc = None
        self.keyboard_retry_until = 0.0
        self.audio_activity_count = 0
        self.last_audio_activity = 0.0
        self.audio_activity_lock = threading.Lock()
        self.voice_recording = False
        self.voice_record_procs = []
        self.voice_record_started = 0.0
        self.voice_session_id = 0
        self.voice_record_lock = threading.Lock()
        self.launcher = self.build_launcher_window()
        self.last_security_alerts = None
        self.last_security_ts = 0
        self.activity_lock = threading.Lock()
        self.last_screen_activity = time.monotonic()
        self.last_input_event_ts = 0.0
        self.low_power_mode = False

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=3)
        root.set_margin_top(5)
        root.set_margin_bottom(5)
        root.set_margin_start(7)
        root.set_margin_end(7)
        self.add(root)

        header = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        root.pack_start(header, False, False, 0)

        title = Gtk.Label(label="RV1126B 本地大模型")
        title.set_xalign(0)
        title.get_style_context().add_class("title")
        header.pack_start(title, True, True, 0)

        backend_label = "RKLLM NPU" if BACKEND == "rkllm" else "CPU llama.cpp"
        self.status = Gtk.Label(label=f"{backend_label} / IBus Libpinyin")
        self.status.set_xalign(1)
        self.status.get_style_context().add_class("status")
        header.pack_start(self.status, False, False, 0)

        input_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        root.pack_start(input_row, False, False, 0)

        self.input = Gtk.Entry()
        self.input.set_placeholder_text("点这里，用系统拼音输入法输入中文")
        self.input.set_hexpand(True)
        self.input.set_width_chars(10)
        self.input.set_input_purpose(Gtk.InputPurpose.FREE_FORM)
        self.input.modify_font(Pango.FontDescription("SimHei 20"))
        self.input.connect("activate", self.on_send)
        input_row.pack_start(self.input, True, True, 0)

        self.send_btn = Gtk.Button(label="发送")
        self.send_btn.get_style_context().add_class("primary")
        self.send_btn.connect("clicked", self.on_send)
        input_row.pack_start(self.send_btn, False, False, 0)

        control_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        root.pack_start(control_row, False, False, 0)

        self.model_btn = Gtk.Button()
        self.model_btn.get_style_context().add_class("compact")
        self.model_btn.connect("clicked", self.on_toggle_model)
        control_row.pack_start(self.model_btn, True, True, 0)
        self.update_model_button()

        self.voice_btn = Gtk.Button(label="按住说话")
        self.voice_btn.get_style_context().add_class("compact")
        self.voice_btn.add_events(Gdk.EventMask.BUTTON_PRESS_MASK | Gdk.EventMask.BUTTON_RELEASE_MASK)
        self.voice_btn.connect("button-press-event", self.on_voice_pressed)
        self.voice_btn.connect("button-release-event", self.on_voice_released)
        control_row.pack_start(self.voice_btn, True, True, 0)

        self.clear_btn = Gtk.Button(label="清屏")
        self.clear_btn.get_style_context().add_class("compact")
        self.clear_btn.connect("clicked", self.on_clear)
        control_row.pack_start(self.clear_btn, True, True, 0)

        self.scroll = Gtk.ScrolledWindow()
        self.scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self.scroll.set_vexpand(True)
        root.pack_start(self.scroll, True, True, 0)

        self.text = Gtk.TextView()
        self.text.set_editable(False)
        self.text.set_cursor_visible(False)
        self.text.set_wrap_mode(Gtk.WrapMode.WORD_CHAR)
        self.text.set_left_margin(10)
        self.text.set_right_margin(10)
        self.text.set_top_margin(8)
        self.text.set_bottom_margin(8)
        self.text.modify_font(Pango.FontDescription("SimHei 19"))
        self.buffer = self.text.get_buffer()
        self.buffer.create_tag("user", foreground="#0645ad", weight=Pango.Weight.BOLD)
        self.buffer.create_tag("assistant", foreground="#166534", weight=Pango.Weight.BOLD)
        self.buffer.create_tag("body", foreground="#111111")
        self.scroll.add(self.text)

        self.append_message("assistant", f"系统输入法已接入。当前推理后端：{backend_label}。")
        self.input.grab_focus()
        if VOICE_ASR_PRELOAD:
            threading.Thread(target=self.preload_vosk_model, daemon=True).start()
        threading.Thread(target=self.watch_input_events, daemon=True).start()
        GLib.timeout_add(200, self.poll_result)
        GLib.timeout_add(200, self.poll_voice_result)
        GLib.timeout_add(1000, self.poll_low_power_state)
        GLib.timeout_add(1000, self.poll_security_status)
        GLib.timeout_add(900, self.ensure_active_layout)
        self.connect("button-press-event", self.on_screen_activity_event)
        self.connect("key-press-event", self.on_screen_activity_event)
        if hasattr(Gdk.EventMask, "TOUCH_MASK"):
            self.connect("touch-event", self.on_screen_activity_event)
        self.connect("destroy", Gtk.main_quit)

    def build_launcher_window(self):
        win = Gtk.Window(title="打开大模型")
        win.set_decorated(False)
        win.set_keep_above(True)
        win.set_default_size(LLM_WIN_W, 68)
        win.move(LLM_WIN_X, LLM_WIN_Y)
        win.set_border_width(6)
        button = Gtk.Button(label="打开大模型")
        button.get_style_context().add_class("launcher")
        button.connect("clicked", self.open_model_interface)
        win.add(button)
        return win

    def on_screen_activity_event(self, *_):
        self.mark_screen_activity("gtk")
        return False

    def mark_screen_activity(self, source="input"):
        now = time.monotonic()
        should_wake = False
        with self.activity_lock:
            if now - self.last_input_event_ts < INPUT_ACTIVITY_DEBOUNCE_SECONDS:
                return
            self.last_input_event_ts = now
            self.last_screen_activity = now
            should_wake = self.low_power_mode
        if should_wake:
            GLib.idle_add(self.exit_low_power_from_screen_activity)

    def exit_low_power_from_screen_activity(self):
        self.exit_low_power_mode("screen clicked")
        return False

    def open_input_event_devices(self):
        fds = {}
        input_dir = "/dev/input"
        try:
            names = sorted(os.listdir(input_dir))
        except Exception as exc:
            self.log_runtime("input_watch", f"cannot list {input_dir}: {exc}")
            return fds
        for name in names:
            if not name.startswith("event"):
                continue
            path = os.path.join(input_dir, name)
            try:
                fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
                fds[fd] = path
            except Exception:
                continue
        if fds:
            self.log_runtime("input_watch", "watching " + ", ".join(fds.values()))
        else:
            self.log_runtime("input_watch", "no readable /dev/input/event* devices")
        return fds

    def is_screen_activity_input(self, event_type, code, value):
        ev_key = 0x01
        ev_rel = 0x02
        ev_abs = 0x03
        if event_type == ev_key:
            return value == 1
        if event_type == ev_rel:
            return value != 0
        if event_type == ev_abs:
            touch_abs_codes = {0, 1, 24, 47, 48, 49, 50, 53, 54, 57}
            return code in touch_abs_codes
        return False

    def watch_input_events(self):
        event_size = struct.calcsize("llHHI")
        fds = self.open_input_event_devices()
        while True:
            if not fds:
                time.sleep(2)
                fds = self.open_input_event_devices()
                continue
            try:
                readable, _, _ = select.select(list(fds.keys()), [], [], 1.0)
            except Exception as exc:
                self.log_runtime("input_watch", f"select failed: {exc}")
                for fd in list(fds.keys()):
                    try:
                        os.close(fd)
                    except Exception:
                        pass
                fds = {}
                continue
            for fd in readable:
                try:
                    data = os.read(fd, event_size * 32)
                except BlockingIOError:
                    continue
                except OSError:
                    try:
                        os.close(fd)
                    except Exception:
                        pass
                    fds.pop(fd, None)
                    continue
                if not data:
                    try:
                        os.close(fd)
                    except Exception:
                        pass
                    fds.pop(fd, None)
                    continue
                for offset in range(0, len(data) - event_size + 1, event_size):
                    _sec, _usec, event_type, code, value = struct.unpack_from("llHHI", data, offset)
                    if self.is_screen_activity_input(event_type, code, value):
                        self.mark_screen_activity("input")
                        break

    def backend_label(self):
        return "RKLLM NPU" if BACKEND == "rkllm" else "CPU llama.cpp"

    def update_model_button(self):
        ctx = self.model_btn.get_style_context()
        ctx.remove_class("model-on")
        ctx.remove_class("model-off")
        if self.model_enabled:
            self.model_btn.set_label("关闭")
            ctx.add_class("model-off")
        else:
            self.model_btn.set_label("打开模型")
            ctx.add_class("model-on")

    def append_message(self, role, text):
        label = "我" if role == "user" else "RV1126B"
        end = self.buffer.get_end_iter()
        self.buffer.insert_with_tags_by_name(end, f"{label}\n", role)
        end = self.buffer.get_end_iter()
        self.buffer.insert_with_tags_by_name(end, f"{text.strip()}\n\n", "body")
        adj = self.scroll.get_vadjustment()
        GLib.idle_add(lambda: adj.set_value(adj.get_upper() - adj.get_page_size()) or False)

    def force_redraw(self):
        self.queue_draw()
        window = self.get_window()
        if window is not None:
            window.invalidate_rect(None, True)
        return True

    def build_prompt(self):
        lines = ["<|im_start|>system", "你是运行在RV1126B开发板上的本地离线助手。回答要简短、直接。", "<|im_end|>"]
        for role, text in self.messages[-8:]:
            if role == "user":
                lines += ["<|im_start|>user", text, "<|im_end|>"]
            else:
                lines += ["<|im_start|>assistant", text, "<|im_end|>"]
        lines += ["<|im_start|>assistant", ""]
        return "\n".join(lines)

    def on_clear(self, *_):
        self.mark_screen_activity("ui")
        self.messages.clear()
        self.buffer.set_text("")

    def on_toggle_model(self, *_):
        self.mark_screen_activity("ui")
        if self.model_enabled:
            self.close_model_interface()
        else:
            self.open_model_interface()

    def close_model_interface(self, *_):
        if not self.model_enabled:
            return
        self.model_enabled = False
        self.update_model_button()
        self.stop_model_process()
        self.cancel_voice_recording()
        self.stop_audio_output()
        self.voice_busy = False
        self.busy = False
        self.send_btn.set_sensitive(True)
        self.voice_btn.set_sensitive(True)
        self.voice_btn.set_label("按住说话")
        self.clear_btn.set_sensitive(True)
        self.model_btn.set_sensitive(True)
        self.hide_main_window()
        self.stop_keyboard()
        self.launcher.show_all()
        self.launcher.present()

    def open_model_interface(self, *_):
        self.mark_screen_activity("ui")
        self.low_power_mode = False
        if self.model_enabled and self.get_visible():
            return
        self.model_enabled = True
        self.update_model_button()
        self.launcher.hide()
        self.show_main_window()
        self.status.set_text(f"{self.backend_label()} / IBus Libpinyin")
        self.restart_keyboard()
        self.keyboard_retry_until = time.monotonic() + 3.0
        GLib.timeout_add(400, self.ensure_keyboard_started)
        if VOICE_TTS_WARM_ON_INPUT:
            self.ensure_tts_worker()
        self.input.grab_focus()
        GLib.timeout_add(900, self.arrange_windows)

    def enter_low_power_mode(self, idle_seconds=None):
        if self.low_power_mode:
            return
        self.low_power_mode = True
        self.model_enabled = False
        self.update_model_button()
        self.stop_model_process()
        self.cancel_voice_recording()
        self.stop_audio_output()
        self.release_voice_models()
        self.voice_busy = False
        self.busy = False
        self.send_btn.set_sensitive(True)
        self.voice_btn.set_sensitive(True)
        self.voice_btn.set_label("按住说话")
        self.clear_btn.set_sensitive(True)
        self.model_btn.set_sensitive(True)
        self.hide_main_window()
        self.launcher.hide()
        self.stop_keyboard()
        idle = LOW_POWER_IDLE_SECONDS if idle_seconds is None else idle_seconds
        self.log_runtime("low_power", f"entered after {idle:.1f}s without screen activity")

    def exit_low_power_mode(self, reason="screen clicked"):
        if not self.low_power_mode:
            return
        self.low_power_mode = False
        with self.activity_lock:
            self.last_screen_activity = time.monotonic()
        self.log_runtime("low_power", f"{reason}, exiting")
        if LOW_POWER_RESTART_UI_ON_WAKE:
            self.restart_ui_after_low_power()
        else:
            self.open_model_interface()

    def restart_ui_after_low_power(self):
        self.log_runtime("low_power", "restarting llm ui process")
        self.stop_model_process()
        self.cancel_voice_recording()
        self.stop_audio_output()
        self.release_voice_models()
        self.stop_keyboard()
        os.execv(sys.executable, [sys.executable, os.path.abspath(__file__)])

    def release_voice_models(self):
        if self.tts_worker:
            try:
                self.tts_worker.shutdown()
            except Exception:
                pass
            self.tts_worker = None
        with self.vosk_lock:
            self.vosk_model = None
        gc.collect()

    def ensure_tts_worker(self):
        if not VOICE_TTS_PRELOAD or self.low_power_mode:
            return
        if self.tts_worker is None:
            self.tts_worker = SherpaTtsWorker(LOG)

    def begin_audio_activity(self):
        with self.audio_activity_lock:
            self.audio_activity_count += 1

    def end_audio_activity(self):
        with self.audio_activity_lock:
            self.audio_activity_count = max(0, self.audio_activity_count - 1)
            self.last_audio_activity = time.monotonic()

    def is_audio_active(self):
        worker_active = bool(self.tts_worker and self.tts_worker.is_active())
        with self.audio_activity_lock:
            local_active = self.audio_activity_count > 0
            recently_active = time.monotonic() - self.last_audio_activity < 1.0
        return worker_active or local_active or recently_active

    def is_model_process_running(self):
        with self.model_proc_lock:
            proc = self.model_proc
        return proc is not None and proc.poll() is None

    def current_face_count(self, status):
        if not status:
            return 0
        try:
            if time.time() - os.path.getmtime(SECURITY_STATUS) > SECURITY_STATUS_STALE_SECONDS:
                return 0
        except Exception:
            pass
        try:
            return int(status.get("current_faces", 0))
        except Exception:
            return 0

    def low_power_block_reason(self, status):
        if self.current_face_count(status) > 0:
            return "face detected"
        if self.busy or self.is_model_process_running():
            return "model running"
        if self.voice_busy or self.voice_recording:
            return "voice input active"
        if self.is_audio_active():
            return "voice playback active"
        return None

    def log_runtime(self, section, message):
        try:
            with open(LOG, "a", encoding="utf-8") as log:
                log.write(f"\n===== {section} =====\n{message}\n")
        except Exception:
            pass

    def hide_main_window(self):
        self.move(-2000, -2000)
        self.resize(1, 1)
        self.iconify()
        self.hide()
        window = self.get_window()
        if window is not None:
            window.hide()

    def show_main_window(self):
        self.set_default_size(LLM_WIN_W, LLM_WIN_H)
        self.resize(LLM_WIN_W, LLM_WIN_H)
        self.deiconify()
        self.show_all()
        self.move(LLM_WIN_X, LLM_WIN_Y)
        self.resize(LLM_WIN_W, LLM_WIN_H)
        self.present()
        window = self.get_window()
        if window is not None:
            window.show()
            window.move_resize(LLM_WIN_X, LLM_WIN_Y, LLM_WIN_W, LLM_WIN_H)
            window.invalidate_rect(None, True)
        self.queue_resize()
        self.queue_draw()

    def ensure_active_layout(self):
        if self.low_power_mode or not self.model_enabled:
            return False
        self.show_main_window()
        self.start_keyboard()
        self.arrange_windows()
        self.force_redraw()
        return False

    def stop_keyboard(self):
        proc = self.keyboard_proc
        for pid in self.find_keyboard_pids():
            try:
                os.kill(pid, signal.SIGTERM)
            except Exception:
                pass
        self.wait_for_keyboard_exit(0.6)
        for pid in self.find_keyboard_pids():
            try:
                os.kill(pid, signal.SIGKILL)
            except Exception:
                pass
        self.wait_for_keyboard_exit(0.3)
        if proc is not None:
            try:
                proc.wait(timeout=0.2)
            except subprocess.TimeoutExpired:
                try:
                    proc.kill()
                    proc.wait(timeout=0.2)
                except Exception:
                    pass
            except Exception:
                pass
            self.keyboard_proc = None

    def restart_keyboard(self):
        self.stop_keyboard()
        self.start_keyboard()

    def start_keyboard(self):
        try:
            if self.keyboard_proc is not None and self.keyboard_proc.poll() is not None:
                self.keyboard_proc = None
            if self.find_keyboard_pids():
                return
            self.keyboard_proc = subprocess.Popen(
                [KEYBOARD_BIN],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except Exception as exc:
            with open(LOG, "a", encoding="utf-8") as log:
                log.write(f"\n===== keyboard start failed =====\n{exc}\n")

    def wait_for_keyboard_exit(self, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if not self.find_keyboard_pids():
                return True
            time.sleep(0.05)
        return not self.find_keyboard_pids()

    def ensure_keyboard_started(self):
        if self.low_power_mode or not self.model_enabled:
            return False
        self.start_keyboard()
        self.arrange_windows()
        return time.monotonic() < self.keyboard_retry_until

    def proc_state(self, pid):
        try:
            with open(os.path.join("/proc", str(pid), "stat"), "r", encoding="utf-8", errors="ignore") as f:
                stat = f.read()
            end = stat.rfind(")")
            if end >= 0 and len(stat) > end + 2:
                return stat[end + 2]
        except Exception:
            pass
        return ""

    def find_keyboard_pids(self):
        pids = []
        for name in os.listdir("/proc"):
            if not name.isdigit():
                continue
            if self.proc_state(name) == "Z":
                continue
            comm_path = os.path.join("/proc", name, "comm")
            cmdline_path = os.path.join("/proc", name, "cmdline")
            try:
                with open(comm_path, "r", encoding="utf-8", errors="ignore") as f:
                    comm = f.read().strip()
                cmdline = ""
                try:
                    with open(cmdline_path, "rb") as f:
                        cmdline = f.read().replace(b"\0", b" ").decode("utf-8", errors="ignore")
                except Exception:
                    pass
                if comm == "matchbox-keyboa" or "matchbox-keyboard" in cmdline:
                    pids.append(int(name))
            except Exception:
                continue
        return pids

    def arrange_windows(self):
        if os.path.exists(ARRANGE_WINDOWS):
            try:
                subprocess.Popen(
                    ["python3", ARRANGE_WINDOWS],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
            except Exception as exc:
                with open(LOG, "a", encoding="utf-8") as log:
                    log.write(f"\n===== arrange failed =====\n{exc}\n")
        return False

    def stop_model_process(self):
        with self.model_proc_lock:
            proc = self.model_proc
        if proc is None or proc.poll() is not None:
            return
        try:
            proc.terminate()
        except Exception:
            return
        threading.Thread(target=self.kill_model_process_later, args=(proc,), daemon=True).start()

    def kill_model_process_later(self, proc):
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            try:
                proc.kill()
            except Exception:
                pass

    def on_send(self, *_):
        self.mark_screen_activity("ui")
        text = self.input.get_text().strip()
        self.input.set_text("")
        self.submit_text(text)

    def submit_text(self, text):
        if self.low_power_mode:
            return
        if self.busy:
            return
        if not text:
            return
        self.messages.append(("user", text))
        self.append_message("user", text)
        self.busy = True
        if VOICE_TTS_WARM_ON_INPUT:
            self.ensure_tts_worker()
        action = face_control_action(text)
        alarm_control = None if action else alarm_interval_control(text)
        local_security_answer = None if action or alarm_control else security_answer(text)
        if action:
            self.status.set_text("执行控制...")
        elif alarm_control:
            self.status.set_text("设置报警间隔...")
        elif local_security_answer:
            self.status.set_text("查询安防状态...")
        elif not self.model_enabled:
            self.status.set_text("大模型已关闭")
        else:
            self.status.set_text("生成中...")
        self.send_btn.set_sensitive(False)
        self.voice_btn.set_sensitive(False)
        if action:
            threading.Thread(target=self.run_control, args=(action,), daemon=True).start()
        elif alarm_control:
            threading.Thread(target=self.run_alarm_interval_control, args=(alarm_control,), daemon=True).start()
        elif local_security_answer:
            self.result_q.put(local_security_answer)
        elif not self.model_enabled:
            self.result_q.put("大模型界面已关闭，未启动推理。需要问答时请先点击“打开大模型”。")
        else:
            model_input = text if BACKEND == "rkllm" else self.build_prompt()
            threading.Thread(target=self.run_model, args=(model_input,), daemon=True).start()

    def run_alarm_interval_control(self, alarm_control):
        kind, seconds = alarm_control
        if kind == "status":
            status = load_security_status() or {}
            interval = int(status.get("alert_interval_seconds", 10))
            minimum = int(status.get("min_alert_interval_seconds", 3))
            self.result_q.put(f"当前报警间隔是 {interval} 秒，最小可设置为 {minimum} 秒。")
            return
        if kind == "missing" or seconds is None:
            self.result_q.put("请说明报警间隔秒数，例如：把报警间隔设置为5秒。")
            return
        try:
            proc = subprocess.run(
                [FACE_CONTROL, "alert-interval", str(int(seconds))],
                text=True,
                capture_output=True,
                timeout=8,
            )
            answer = (proc.stdout if proc.returncode == 0 else proc.stdout + proc.stderr).strip()
            if not answer:
                answer = "报警间隔已更新。" if proc.returncode == 0 else "报警间隔设置失败。"
        except Exception as exc:
            answer = f"报警间隔设置异常：{exc}"
        self.result_q.put(answer)

    def on_voice_pressed(self, _widget, event=None):
        self.mark_screen_activity("ui")
        if event is not None and getattr(event, "button", 1) != 1:
            return False
        if self.busy or self.voice_busy or self.voice_recording:
            return True
        self.stop_audio_output()
        if VOICE_TTS_WARM_ON_INPUT:
            self.ensure_tts_worker()
        try:
            procs = self.start_voice_recording_processes()
        except Exception as exc:
            self.status.set_text("录音启动失败")
            self.append_message("assistant", f"录音启动失败：{exc}")
            return True

        with self.voice_record_lock:
            self.voice_session_id += 1
            session_id = self.voice_session_id
            self.voice_recording = True
            self.voice_busy = True
            self.voice_record_procs = procs
            self.voice_record_started = time.monotonic()

        self.voice_btn.set_label("松手识别")
        self.status.set_text("正在录音，松手识别...")
        self.send_btn.set_sensitive(False)
        self.model_btn.set_sensitive(False)
        self.clear_btn.set_sensitive(False)
        GLib.timeout_add(int(VOICE_LONG_PRESS_MAX_SECONDS * 1000), self.auto_stop_voice_recording, session_id)
        return True

    def on_voice_released(self, _widget, event=None):
        self.mark_screen_activity("ui")
        if event is not None and getattr(event, "button", 1) != 1:
            return False
        self.stop_voice_hold_recording("录音完成，正在识别...")
        return True

    def auto_stop_voice_recording(self, session_id):
        with self.voice_record_lock:
            should_stop = self.voice_recording and self.voice_session_id == session_id
        if should_stop:
            self.stop_voice_hold_recording("录音达到最长时间，正在识别...")
        return False

    def stop_voice_hold_recording(self, status_text):
        with self.voice_record_lock:
            if not self.voice_recording:
                return
            procs = self.voice_record_procs
            started = self.voice_record_started
            self.voice_recording = False
            self.voice_record_procs = []
            self.voice_record_started = 0.0

        self.voice_btn.set_label("识别中")
        self.voice_btn.set_sensitive(False)
        self.status.set_text(status_text)
        threading.Thread(target=self.finish_voice_hold_recording, args=(procs, started), daemon=True).start()

    def cancel_voice_recording(self):
        with self.voice_record_lock:
            procs = self.voice_record_procs
            self.voice_recording = False
            self.voice_record_procs = []
            self.voice_record_started = 0.0
        for _device, _path, proc in procs:
            if proc.poll() is None:
                try:
                    proc.send_signal(signal.SIGINT)
                    proc.wait(timeout=0.8)
                except Exception:
                    try:
                        proc.kill()
                    except Exception:
                        pass

    def stop_audio_output(self):
        if self.tts_worker:
            self.tts_worker.suspend(max(VOICE_RECORD_SECONDS, VOICE_LONG_PRESS_MAX_SECONDS) + 2)
        try:
            subprocess.run(
                ["pkill", "-TERM", "aplay"],
                text=True,
                capture_output=True,
                timeout=2,
            )
        except Exception:
            pass

    def preload_vosk_model(self):
        if not os.path.isdir(VOSK_MODEL_DIR):
            return
        try:
            from vosk import Model, SetLogLevel

            SetLogLevel(-1)
            start = time.monotonic()
            model = Model(VOSK_MODEL_DIR)
            with self.vosk_lock:
                if self.vosk_model is None:
                    self.vosk_model = model
            with open(LOG, "a", encoding="utf-8") as log:
                log.write(f"\n===== voice model preload =====\nloaded in {time.monotonic() - start:.2f}s\n")
        except Exception as exc:
            with open(LOG, "a", encoding="utf-8") as log:
                log.write(f"\n===== voice model preload failed =====\n{exc}\n")

    def start_voice_recording_processes(self):
        devices = VOICE_INPUT_CANDIDATES if VOICE_INPUT_DEV.lower() == "auto" else [VOICE_INPUT_DEV]
        procs = []
        for idx, device in enumerate(devices):
            path = f"/tmp/llm_voice_hold_{idx}.raw"
            proc = subprocess.Popen(
                [
                    "arecord",
                    "-q",
                    "-D",
                    device,
                    "-f",
                    "S16_LE",
                    "-r",
                    str(VOICE_RATE),
                    "-c",
                    "1",
                    "-t",
                    "raw",
                    path,
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            procs.append((device, path, proc))
        if not procs:
            raise RuntimeError("没有配置录音设备。")
        return procs

    def raw_pcm_to_wav(self, raw_path, wav_path):
        with open(raw_path, "rb") as f:
            pcm = f.read()
        if len(pcm) % 2:
            pcm = pcm[:-1]
        if len(pcm) < 320:
            raise RuntimeError("录音时间太短。")
        with wave.open(wav_path, "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(VOICE_RATE)
            wav.writeframes(pcm)

    def collect_voice_recordings(self, procs):
        for _device, _path, proc in procs:
            if proc.poll() is None:
                try:
                    proc.send_signal(signal.SIGINT)
                except Exception:
                    try:
                        proc.terminate()
                    except Exception:
                        pass

        records = []
        for device, path, proc in procs:
            try:
                stdout, stderr = proc.communicate(timeout=1.5)
            except subprocess.TimeoutExpired:
                proc.kill()
                stdout, stderr = proc.communicate()

            if os.path.exists(path) and os.path.getsize(path) > 0:
                try:
                    wav_path = path.rsplit(".", 1)[0] + ".wav"
                    self.raw_pcm_to_wav(path, wav_path)
                    peak, rms = self.audio_level(wav_path)
                    records.append((rms, peak, device, wav_path))
                    continue
                except Exception as exc:
                    stderr = f"{stderr or ''}\n{exc}"

            with open(LOG, "a", encoding="utf-8") as log:
                log.write(f"\n===== voice hold record failed =====\ndevice={device}\n{stdout or ''}{stderr or ''}\n")

        if not records:
            raise RuntimeError("没有录到可用声音，请按住按钮说话后再松手。")
        return records

    def finish_voice_hold_recording(self, procs, started):
        try:
            duration = time.monotonic() - started
            records = self.collect_voice_recordings(procs)
            if duration < VOICE_MIN_HOLD_SECONDS:
                raise RuntimeError("按住时间太短，请按住按钮说完再松手。")
            if VOICE_INPUT_DEV.lower() == "auto":
                text = self.recognize_best(records)
            else:
                records.sort(reverse=True)
                text = self.recognize_wav(records[0][3], records[0][2], strict=True)["text"]
            with open(LOG, "a", encoding="utf-8") as log:
                log.write(f"\n===== voice hold total =====\nduration={duration:.2f}s\n")
            if text:
                self.voice_q.put(("text", text))
            else:
                self.voice_q.put(("error", "没有识别到语音，请靠近麦克风再试一次。"))
        except RuntimeError as exc:
            self.voice_q.put(("error", str(exc)))
        except Exception as exc:
            self.voice_q.put(("error", f"语音识别异常：{exc}"))

    def record_and_recognize(self):
        try:
            start = time.monotonic()
            if VOICE_INPUT_DEV.lower() == "auto":
                records = self.record_auto_wavs()
                self.voice_q.put(("status", "录音完成，正在识别..."))
                text = self.recognize_best(records)
            else:
                wav_path = self.record_wav(VOICE_INPUT_DEV, VOICE_TMP_WAV)
                self.voice_q.put(("status", "录音完成，正在识别..."))
                text = self.recognize_wav(wav_path, VOICE_INPUT_DEV, strict=True)["text"]
            with open(LOG, "a", encoding="utf-8") as log:
                log.write(f"\n===== voice total =====\nelapsed={time.monotonic() - start:.2f}s\n")
            if text:
                self.voice_q.put(("text", text))
            else:
                self.voice_q.put(("error", "没有识别到语音，请靠近麦克风再试一次。"))
        except subprocess.TimeoutExpired:
            self.voice_q.put(("error", "录音超时。"))
        except RuntimeError as exc:
            self.voice_q.put(("error", str(exc)))
        except Exception as exc:
            self.voice_q.put(("error", f"语音识别异常：{exc}"))

    def record_wav(self, device, path):
        proc = subprocess.run(
            [
                "arecord",
                "-q",
                "-D",
                device,
                "-f",
                "S16_LE",
                "-r",
                str(VOICE_RATE),
                "-c",
                "1",
                "-d",
                str(VOICE_RECORD_SECONDS),
                path,
            ],
            text=True,
            capture_output=True,
            timeout=VOICE_RECORD_SECONDS + VOICE_RECORD_TIMEOUT_EXTRA,
        )
        if proc.returncode != 0:
            msg = (proc.stderr or proc.stdout or f"录音失败：{device}").strip()
            raise RuntimeError(msg)
        return path

    def record_auto_wavs(self):
        started = time.monotonic()
        procs = []
        for idx, device in enumerate(VOICE_INPUT_CANDIDATES):
            path = f"/tmp/llm_voice_input_{idx}.wav"
            proc = subprocess.Popen(
                [
                    "arecord",
                    "-q",
                    "-D",
                    device,
                    "-f",
                    "S16_LE",
                    "-r",
                    str(VOICE_RATE),
                    "-c",
                    "1",
                    "-d",
                    str(VOICE_RECORD_SECONDS),
                    path,
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            procs.append((device, path, proc))

        records = []
        deadline = started + VOICE_RECORD_SECONDS + VOICE_RECORD_TIMEOUT_EXTRA
        for device, path, proc in procs:
            try:
                remaining = max(0.1, deadline - time.monotonic())
                stdout, stderr = proc.communicate(timeout=remaining)
            except subprocess.TimeoutExpired:
                proc.kill()
                stdout, stderr = proc.communicate()
            if proc.returncode == 0 and os.path.exists(path):
                peak, rms = self.audio_level(path)
                records.append((rms, peak, device, path))
            else:
                with open(LOG, "a", encoding="utf-8") as log:
                    log.write(f"\n===== voice record failed =====\ndevice={device}\n{stdout or ''}{stderr or ''}\n")

        if not records:
            raise RuntimeError("没有可用的录音设备。")
        with open(LOG, "a", encoding="utf-8") as log:
            log.write("\n===== voice record auto =====\n")
            log.write(f"elapsed={time.monotonic() - started:.2f}s\n")
            for rms, peak, device, path in sorted(records, reverse=True):
                log.write(f"candidate={device} peak={peak} rms={rms:.1f} path={path}\n")
        return records

    def record_auto_wav(self):
        records = self.record_auto_wavs()
        usable = [item for item in records if item[1] < VOICE_MAX_PEAK and item[0] >= VOICE_MIN_RMS]
        if usable:
            usable.sort(reverse=True)
            return usable[0][3]
        records.sort(key=lambda item: (item[1] >= VOICE_MAX_PEAK, -item[0], item[1]))
        return records[0][3]

    def recognize_best(self, records):
        results = []
        for rms, peak, device, path in sorted(records, reverse=True):
            try:
                results.append(self.recognize_wav(path, device, strict=False))
            except Exception as exc:
                with open(LOG, "a", encoding="utf-8") as log:
                    log.write(f"\n===== voice asr candidate failed =====\ndevice={device}\n{exc}\n")

        if not results:
            raise RuntimeError("语音识别失败，没有可用录音。")

        valid = [
            item for item in results
            if item["text"] and item["rms"] >= VOICE_MIN_RMS and (item["avg_conf"] == 0.0 or item["avg_conf"] >= VOICE_MIN_CONF)
        ]
        if valid:
            valid.sort(key=lambda item: (item["avg_conf"], item["rms"]), reverse=True)
            chosen = valid[0]
            with open(LOG, "a", encoding="utf-8") as log:
                log.write(f"\n===== voice asr selected =====\ndevice={chosen['device']} text={chosen['text']} conf={chosen['avg_conf']:.3f}\n")
            return chosen["text"]

        text_results = [item for item in results if item["text"]]
        if text_results:
            text_results.sort(key=lambda item: (item["avg_conf"], item["rms"]), reverse=True)
            best = text_results[0]
            raise RuntimeError(f"语音识别不够清楚，听到的是：{best['text']}。请靠近麦克风重说一次。")

        loudest = max(results, key=lambda item: item["rms"])
        if loudest["rms"] < VOICE_MIN_RMS:
            raise RuntimeError("录音声音太小，请靠近麦克风再说一次。")
        if loudest["peak"] >= VOICE_MAX_PEAK:
            raise RuntimeError("录音声音过大，可能录到了播报声；请等播报停止后再说。")
        return ""

    def recognize_wav(self, path, device="", strict=True):
        if not os.path.isdir(VOSK_MODEL_DIR):
            raise RuntimeError(f"缺少中文语音识别模型：{VOSK_MODEL_DIR}")
        try:
            from vosk import KaldiRecognizer, Model, SetLogLevel
        except Exception as exc:
            raise RuntimeError("缺少 Vosk 语音识别库") from exc

        with self.vosk_lock:
            if self.vosk_model is None:
                SetLogLevel(-1)
                self.vosk_model = Model(VOSK_MODEL_DIR)
            model = self.vosk_model

        peak, rms = self.audio_level(path)
        if strict and rms < VOICE_MIN_RMS:
            raise RuntimeError("录音声音太小，请靠近麦克风再说一次。")

        with wave.open(path, "rb") as wav:
            if wav.getnchannels() != 1 or wav.getsampwidth() != 2:
                raise RuntimeError("录音格式不是 16bit 单声道 PCM")
            recognizer = KaldiRecognizer(model, wav.getframerate())
            recognizer.SetWords(True)
            while True:
                data = wav.readframes(4000)
                if not data:
                    break
                recognizer.AcceptWaveform(data)
            result = json.loads(recognizer.FinalResult())
        text = (result.get("text") or "").replace(" ", "").strip()
        words = result.get("result") or []
        avg_conf = sum(float(item.get("conf", 0.0)) for item in words) / len(words) if words else 0.0
        with open(LOG, "a", encoding="utf-8") as log:
            log.write(f"\n===== voice asr =====\ndevice={device}\ntext={text}\npeak={peak} rms={rms:.1f} avg_conf={avg_conf:.3f}\n")
        if strict and peak >= VOICE_MAX_PEAK and not text:
            raise RuntimeError("录音声音过大，可能录到了播报声；请等播报停止后再说。")
        if strict and text and avg_conf and avg_conf < VOICE_MIN_CONF:
            raise RuntimeError("语音识别不够清楚，请靠近麦克风重说一次。")
        return {
            "text": text,
            "avg_conf": avg_conf,
            "peak": peak,
            "rms": rms,
            "device": device,
            "path": path,
        }

    def audio_level(self, path):
        with wave.open(path, "rb") as wav:
            if wav.getsampwidth() != 2:
                return 0, 0.0
            frames = wav.readframes(wav.getnframes())
        if not frames:
            return 0, 0.0
        samples = struct.unpack("<" + "h" * (len(frames) // 2), frames)
        if not samples:
            return 0, 0.0
        peak = max(abs(item) for item in samples)
        rms = math.sqrt(sum(item * item for item in samples) / len(samples))
        return peak, rms

    def poll_voice_result(self):
        try:
            kind, payload = self.voice_q.get_nowait()
        except queue.Empty:
            return True
        if kind == "status":
            if self.low_power_mode:
                return True
            self.status.set_text(payload)
            return True
        if self.low_power_mode:
            self.voice_busy = False
            self.voice_recording = False
            self.voice_btn.set_label("按住说话")
            self.send_btn.set_sensitive(True)
            self.voice_btn.set_sensitive(True)
            self.model_btn.set_sensitive(True)
            self.clear_btn.set_sensitive(True)
            return True
        self.voice_busy = False
        self.voice_recording = False
        self.voice_btn.set_label("按住说话")
        self.send_btn.set_sensitive(not self.busy)
        self.voice_btn.set_sensitive(not self.busy)
        self.model_btn.set_sensitive(True)
        self.clear_btn.set_sensitive(True)
        if kind == "text":
            self.status.set_text("语音识别完成")
            self.submit_text(payload)
        else:
            self.append_message("assistant", payload)
            backend_label = "RKLLM NPU" if BACKEND == "rkllm" else "CPU llama.cpp"
            self.status.set_text(f"{backend_label} / IBus Libpinyin")
            self.input.grab_focus()
        return True

    def run_control(self, action):
        try:
            proc = subprocess.run(
                [FACE_CONTROL, action],
                text=True,
                capture_output=True,
                timeout=20,
            )
            with open(LOG, "a", encoding="utf-8") as log:
                log.write("\n===== control =====\n")
                log.write(f"action={action} returncode={proc.returncode}\n")
                log.write(proc.stdout)
                log.write(proc.stderr)
            answer = (proc.stdout if proc.returncode == 0 else proc.stdout + proc.stderr).strip()
            if not answer:
                answer = "控制命令已执行。" if proc.returncode == 0 else "控制命令执行失败。"
        except subprocess.TimeoutExpired:
            answer = "控制命令超时。"
        except Exception as exc:
            answer = f"控制命令异常：{exc}"
        self.result_q.put(answer)

    def run_model(self, model_input):
        if not self.model_enabled:
            self.result_q.put("大模型已关闭，未启动推理。")
            return
        proc = None
        try:
            if BACKEND == "rkllm":
                env = os.environ.copy()
                lib_path = "/userdata/llm/rkllm/lib"
                env["LD_LIBRARY_PATH"] = lib_path + ":" + env.get("LD_LIBRARY_PATH", "")
                env.setdefault("RKLLM_LOG_LEVEL", "0")
                proc = subprocess.Popen(
                    [RKLLM_BIN, RKLLM_MODEL, str(TOKENS), str(RKLLM_CONTEXT)],
                    cwd=os.path.dirname(RKLLM_BIN),
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    env=env,
                )
                with self.model_proc_lock:
                    self.model_proc = proc
                stdout, stderr = proc.communicate(input=model_input, timeout=240)
            else:
                proc = subprocess.Popen(
                    [LLAMA, "-m", MODEL, "-ngl", "0", "-n", str(TOKENS), model_input],
                    cwd=os.path.dirname(LLAMA),
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                with self.model_proc_lock:
                    self.model_proc = proc
                stdout, stderr = proc.communicate(timeout=240)
            with open(LOG, "a", encoding="utf-8") as log:
                log.write("\n===== run =====\n")
                log.write(stderr)
            if proc.returncode != 0:
                if not self.model_enabled:
                    answer = "大模型已关闭，当前生成已停止。"
                else:
                    answer = "模型运行失败，查看 /userdata/llm/ui/llm_gtk_chat.log"
            elif BACKEND == "rkllm":
                answer = clean_model_answer(stdout) or "没有生成内容。"
            else:
                out = stdout
                answer = out[len(model_input):] if out.startswith(model_input) else out
                answer = clean_model_answer(answer) or "没有生成内容。"
        except subprocess.TimeoutExpired:
            if proc is not None:
                try:
                    proc.kill()
                    proc.communicate(timeout=2)
                except Exception:
                    pass
            answer = "生成超时。"
        except Exception as exc:
            answer = f"运行异常：{exc}"
        finally:
            with self.model_proc_lock:
                if self.model_proc is proc:
                    self.model_proc = None
        self.result_q.put(answer)

    def poll_result(self):
        try:
            answer = self.result_q.get_nowait()
        except queue.Empty:
            return True
        if self.low_power_mode:
            self.busy = False
            self.send_btn.set_sensitive(True)
            self.voice_btn.set_sensitive(True)
            return True
        self.messages.append(("assistant", answer))
        self.append_message("assistant", answer)
        self.speak_async(answer)
        self.busy = False
        self.status.set_text(f"{self.backend_label()} / IBus Libpinyin")
        self.send_btn.set_sensitive(True)
        self.voice_btn.set_sensitive(True)
        self.input.grab_focus()
        return True

    def speak_async(self, text):
        if self.low_power_mode:
            return
        self.ensure_tts_worker()
        if VOICE_TTS_FAST_FALLBACK and self.tts_worker and not self.tts_worker.ready:
            threading.Thread(target=self.speak_espeak, args=(text, "fast fallback"), daemon=True).start()
            return
        if self.tts_worker and self.tts_worker.enqueue(text):
            return
        threading.Thread(target=self.speak_text, args=(text,), daemon=True).start()

    def speak_text(self, text):
        text = clean_model_answer(text)
        if not text:
            return
        short_text = text[:VOICE_TTS_MAX_CHARS]
        self.begin_audio_activity()
        try:
            if os.path.exists(SHERPA_TTS_SCRIPT) and os.path.isdir(SHERPA_TTS_MODEL_DIR):
                try:
                    proc = subprocess.run(
                        [
                            "nice",
                            "-n",
                            "10",
                            "python3",
                            SHERPA_TTS_SCRIPT,
                            "--type",
                            SHERPA_TTS_TYPE,
                            "--model-dir",
                            SHERPA_TTS_MODEL_DIR,
                            "--vocoder",
                            SHERPA_TTS_VOCODER,
                            "--output",
                            VOICE_TTS_WAV,
                            "--sid",
                            SHERPA_TTS_SID,
                            "--speed",
                            str(SHERPA_TTS_SPEED),
                            "--silence-scale",
                            str(SHERPA_TTS_SILENCE_SCALE),
                            "--num-threads",
                            str(SHERPA_TTS_THREADS),
                            short_text,
                        ],
                        text=True,
                        capture_output=True,
                        timeout=90,
                    )
                    if proc.returncode == 0:
                        subprocess.run(
                            ["aplay", "-q", "-D", VOICE_OUTPUT_DEV, VOICE_TTS_WAV],
                            text=True,
                            capture_output=True,
                            timeout=45,
                        )
                        return
                    with open(LOG, "a", encoding="utf-8") as log:
                        log.write("\n===== sherpa tts failed =====\n")
                        log.write(proc.stdout)
                        log.write(proc.stderr)
                except Exception as exc:
                    with open(LOG, "a", encoding="utf-8") as log:
                        log.write(f"\n===== sherpa tts error =====\n{exc}\n")

            self.speak_espeak(short_text, "fallback")
        finally:
            self.end_audio_activity()

    def speak_espeak(self, text, reason="fallback"):
        text = clean_model_answer(text)
        if not text:
            return
        short_text = text[:VOICE_TTS_MAX_CHARS]
        tts_bin = shutil.which("espeak-ng") or shutil.which("espeak")
        if not tts_bin:
            with open(LOG, "a", encoding="utf-8") as log:
                log.write("\n===== tts skipped =====\nespeak-ng/espeak not installed\n")
            return
        self.begin_audio_activity()
        try:
            subprocess.run(
                [tts_bin, "-v", "cmn", "-s", VOICE_TTS_RATE, "-w", VOICE_TTS_WAV, short_text],
                text=True,
                capture_output=True,
                timeout=25,
            )
            subprocess.run(
                ["aplay", "-q", "-D", VOICE_OUTPUT_DEV, VOICE_TTS_WAV],
                text=True,
                capture_output=True,
                timeout=30,
            )
            with open(LOG, "a", encoding="utf-8") as log:
                log.write(f"\n===== espeak tts {reason} =====\nchars={len(short_text)}\n")
        except Exception as exc:
            with open(LOG, "a", encoding="utf-8") as log:
                log.write(f"\n===== tts error =====\n{exc}\n")
        finally:
            self.end_audio_activity()

    def poll_security_status(self):
        status = load_security_status()
        if not status:
            return True
        self.update_low_power_state(status)
        if self.low_power_mode:
            return True
        alerts = int(status.get("stranger_alerts", 0))
        ts_ms = int(status.get("ts_ms", 0))
        if self.last_security_alerts is None:
            self.last_security_alerts = alerts
            self.last_security_ts = ts_ms
            return True
        if alerts > self.last_security_alerts and ts_ms != self.last_security_ts:
            text = status.get("last_alert") or "陌生人停留时间超过阈值"
            current_unknown = int(status.get("current_unknown", 0))
            message = f"安防警报：{text}。当前画面内陌生人 {current_unknown} 个。"
            self.messages.append(("assistant", message))
            self.append_message("assistant", message)
            self.speak_async(message)
            self.last_security_alerts = alerts
            self.last_security_ts = ts_ms
        return True

    def poll_low_power_state(self):
        self.update_low_power_state(load_security_status())
        return True

    def update_low_power_state(self, status=None):
        now = time.monotonic()
        current_faces = self.current_face_count(status)
        if self.low_power_mode:
            if current_faces > 0:
                self.exit_low_power_mode("face detected")
            return

        block_reason = self.low_power_block_reason(status)
        if block_reason:
            with self.activity_lock:
                self.last_screen_activity = now
            return

        with self.activity_lock:
            idle_seconds = now - self.last_screen_activity
        if idle_seconds >= LOW_POWER_IDLE_SECONDS:
            self.enter_low_power_mode(idle_seconds)


def main():
    css = b"""
    window { background: #f2f2f2; color: #111111; }
    * { font-family: SimHei, sans-serif; color: #111111; }
    .title { font: 26px SimHei; color: #111111; }
    .status { font: 16px SimHei; color: #333333; }
    textview, textview text, entry {
        background: #ffffff;
        color: #111111;
        border-radius: 4px;
    }
    textview selection, entry selection {
        background: #cfe3ff;
        color: #111111;
    }
    button {
        font: 20px SimHei;
        padding: 7px 12px;
        color: #111111;
        background: #eeeeee;
    }
    button.primary {
        min-width: 70px;
        min-height: 38px;
    }
    button.compact {
        font: 22px SimHei;
        padding: 8px 10px;
        min-width: 86px;
        min-height: 44px;
    }
    button.model-on {
        background-image: none;
        background: #d8f3dc;
        color: #111111;
    }
    button.model-off {
        background-image: none;
        background: #ffd6d6;
        color: #111111;
    }
    button.launcher {
        font: 24px SimHei;
        padding: 10px 18px;
        min-height: 52px;
        background-image: none;
        background: #d8f3dc;
        color: #111111;
    }
    """
    provider = Gtk.CssProvider()
    provider.load_from_data(css)
    Gtk.StyleContext.add_provider_for_screen(Gdk.Screen.get_default(), provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
    win = ChatWindow()
    win.show_all()
    Gtk.main()


if __name__ == "__main__":
    main()
