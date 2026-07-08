# RV1126B LLM Runtime

This directory keeps the host-side backup for the minimal CPU-only llama.cpp test runtime.

## Files

- `llama-simple`: a board-compiled aarch64 executable copied back from the board.
- `models/qwen2.5-0.5b-instruct-q4_k_m.gguf`: small Qwen2.5 0.5B instruct GGUF model.
- `../rkllm_minicpm4/MiniCPM4-0.5B_w4a16_RV1126B.rkllm`: RKLLM/NPU model for RV1126B.
- `restore_to_board.sh`: restore the binary and model to `/userdata/llm` on the board.
- `run_board_llm.sh`: run one prompt through SSH.
- `llama-simple.Makefile`: manual Makefile used because the board has no CMake and the official prebuilt binary needs newer glibc than Debian 12.

## Run

```bash
/home/enovo/qs/llm_board_runtime/run_board_llm.sh "用一句话解释什么是NPU"
```

Limit generated tokens:

```bash
N=32 /home/enovo/qs/llm_board_runtime/run_board_llm.sh "你好"
```

## Restore After Power Loss

```bash
/home/enovo/qs/llm_board_runtime/restore_to_board.sh
```

The board target directory is `/userdata/llm`, not the small root filesystem.

## Board Paths

- Runtime: `/userdata/llm/llama-simple/bin/llama-simple`
- Model: `/userdata/llm/models/qwen2.5-0.5b-instruct-q4_k_m.gguf`
- Last run log: `/userdata/llm/last_run.log`
- Touch chat UI: `/userdata/llm/ui/llm_touch_chat.py`

## MIPI Touch Screen UI

Start the local full-screen chat UI on the board screen:

```bash
/home/enovo/qs/llm_board_runtime/start_board_llm_screen.sh
```

This stops `face-ai-drm.service` because both programs need the same DRM screen.

Stop the LLM screen UI and restore face detection:

```bash
/home/enovo/qs/llm_board_runtime/stop_board_llm_screen.sh
```

The UI includes an embedded pinyin IME. It reads the FT5x06 touch panel directly from `/dev/input/event2`, so it does not depend on SDL converting touch to mouse events.

Pinyin input flow:

```text
tap letters -> tap a candidate -> send
```

The pinyin dictionary is copied from the SDK LVGL IME source to `/userdata/llm/ui/lv_ime_pinyin.c`, with extra common simplified Chinese candidates and phrases added in `llm_touch_chat.py`.

## System IME UI

The preferred screen UI now runs under X11 with GTK and system IBus:

```bash
/home/enovo/qs/llm_board_runtime/start_board_system_ime.sh
```

It starts:

- Xorg on the MIPI screen
- Openbox
- IBus with `libpinyin`
- `matchbox-keyboard`
- GTK chat window `/userdata/llm/ui/llm_gtk_chat.py`
- RKLLM NPU backend `/userdata/llm/rkllm/bin/rkllm_once`
- Face control helper `/userdata/llm/ui/face_control.sh`

Supported local control phrases include:

- `打开摄像头` / `打开人脸识别`: starts the left-side X11 face recognition window while keeping the LLM screen.
- `打开后台摄像头` / `电脑查看摄像头`: starts background face detection on port 8090.
- `关闭摄像头`: stops left-side, background, and DRM face detection.
- `摄像头状态`: reports camera and face service state.
- `全屏打开人脸识别`: exits the LLM screen and starts `face-ai-drm.service`.

The GTK chat window also reads the left-side face recognition status every second.
It can answer simple local security questions without calling the LLM backend:

- `从开机到现在一共出现了几个陌生人`
- `现在画面里几个人`
- `报警几次` / `最近报警`
- `陌生人停留多久`

The face recognition process writes:

- Status: `/tmp/face_ai_security_status.json`
- Events: `/tmp/face_ai_security_events.jsonl`

Current security behavior:

- Known people are shown by name from `/userdata/face_ai/faces`.
- Unknown faces are counted as stranger entries after they stay unknown for about 1 second, which filters brief recognition jitter.
- If an unknown face stays on screen for 10 seconds, the chat window automatically appends one security alert.
- The alert threshold can be changed with `STRANGER_ALERT_SECONDS` in `/userdata/llm/ui/start_llm_x_session.sh`.
  The entry confirmation time can be changed with `STRANGER_ENTRY_CONFIRM_SECONDS`.
- The integrated screen UI runs the left face-recognition window in a touch-friendly profile by default:
  `FACE_X11_FPS=8`, `FACE_X11_CAMERA_FPS=8`, `FACE_X11_DETECT_INTERVAL=4`,
  `FACE_X11_RECOG_INTERVAL=6`, and `FACE_X11_NICE=8`.
  Increase these only if touch responsiveness is still acceptable.

Stop it and restore face detection:

```bash
/home/enovo/qs/llm_board_runtime/stop_board_system_ime.sh
```

## Voice LLM Interaction

The screen UI supports USB audio interaction through the `语音` button:

- records from the USB speakerphone/microphone module
- recognizes Chinese with Vosk small Chinese model
- sends the recognized text to the same LLM/security-control path as typed input
- automatically broadcasts the assistant reply with offline `sherpa-onnx` Chinese Matcha TTS
- falls back to `espeak-ng` only if the neural TTS runtime/model is missing

Default USB audio devices:

```text
VOICE_INPUT_DEV=plughw:2,0
VOICE_OUTPUT_DEV=pulse
VOICE_RECORD_SECONDS=4
VOSK_MODEL_DIR=/userdata/llm/voice/vosk-model-small-cn-0.22
SHERPA_TTS_TYPE=matcha
SHERPA_TTS_MODEL_DIR=/userdata/llm/voice/matcha-icefall-zh-baker
SHERPA_TTS_VOCODER=/userdata/llm/voice/vocos-22khz-univ.onnx
SHERPA_TTS_SPEED=1.02
```

Host-side offline dependency cache:

```text
/home/enovo/qs/voice_deps
```

Deploy or restore voice support to the board:

```bash
/home/enovo/qs/llm_board_runtime/deploy_voice_runtime.sh
```

If the USB audio card number changes after reconnecting devices, check it on the board:

```bash
cat /proc/asound/cards
arecord -l
aplay -l
```

Then restart with an override, for example:

```bash
VOICE_INPUT_DEV=plughw:2,0 VOICE_OUTPUT_DEV=pulse systemctl restart llm-system-ime.service
```

## Boot Default

The board can boot directly into the X11/IBus LLM screen UI with:

```bash
systemctl disable --now face-ai-drm.service
systemctl enable --now llm-system-ime.service
```

Host-side backup files for that boot service:

- `llm-system-ime.service`
- `start_system_ime_local.sh`
- `start_llm_x_session.sh`
- `llm_gtk_chat.py`
- `arrange_x_windows.py`
- `openbox_rc.xml`
- `face_control.sh`

Restore this integrated LLM + face recognition UI after power loss:

```bash
/home/enovo/qs/llm_board_runtime/restore_to_board.sh
systemctl disable --now face-ai-drm.service
systemctl enable --now llm-system-ime.service
```

## Current Test Result

Test prompt:

```text
用一句话解释什么是NPU
```

Output:

```text
NPU是神经网络专用处理器，专为深度学习和机器学习
```

Measured on the RV1126B CPU with `N=16`:

- model load: about 5.7 s
- prompt eval: about 9.6 tokens/s
- generation: about 2.1 tokens/s for this short run
