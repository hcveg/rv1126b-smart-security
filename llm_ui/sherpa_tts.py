#!/usr/bin/env python3
import argparse
import re
import wave

import numpy as np
import sherpa_onnx


def normalize_text(text):
    replacements = {
        "RV1126B": "阿尔维一一二六比",
        "RV1126": "阿尔维一一二六",
        "RKLLM": "本地大模型",
        "RKNN": "模型",
        "NPU": "神经网络处理器",
        "CPU": "处理器",
        "USB": "优艾斯比",
        "FPS": "帧率",
        "ASR": "语音识别",
        "TTS": "语音播报",
        "unknown": "未知",
        "Unknown": "未知",
    }
    for key, value in replacements.items():
        text = text.replace(key, value)
    letter_names = {
        "a": "诶",
        "b": "比",
        "c": "西",
        "d": "地",
        "e": "伊",
        "f": "艾弗",
        "g": "吉",
        "h": "艾尺",
        "i": "艾",
        "j": "杰",
        "k": "开",
        "l": "艾勒",
        "m": "艾姆",
        "n": "恩",
        "o": "欧",
        "p": "批",
        "q": "丘",
        "r": "阿尔",
        "s": "艾斯",
        "t": "提",
        "u": "优",
        "v": "维",
        "w": "达不溜",
        "x": "艾克斯",
        "y": "歪",
        "z": "贼德",
    }

    def spell_letters(match):
        return "".join(letter_names.get(ch.lower(), "") for ch in match.group(0))

    text = re.sub(r"[A-Za-z]+", spell_letters, text)
    text = re.sub(r"<\|[^>]+?\|>", "", text)
    text = re.sub(r"https?://\S+", "", text)
    text = re.sub(r"[`*_#{}\\[\\]<>]", "", text)
    text = re.sub(r"\s+", " ", text).strip()
    return text


def write_wav(path, samples, sample_rate):
    samples = np.asarray(samples, dtype=np.float32)
    samples = np.clip(samples, -1.0, 1.0)
    pcm = (samples * 32767.0).astype(np.int16)
    with wave.open(path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(sample_rate)
        f.writeframes(pcm.tobytes())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--type", choices=("vits", "matcha"), default="matcha")
    parser.add_argument("--model-dir", default="/userdata/llm/voice/matcha-icefall-zh-baker")
    parser.add_argument("--vocoder", default="/userdata/llm/voice/vocos-22khz-univ.onnx")
    parser.add_argument("--output", default="/tmp/llm_tts_reply.wav")
    parser.add_argument("--sid", type=int, default=0)
    parser.add_argument("--speed", type=float, default=1.02)
    parser.add_argument("--silence-scale", type=float, default=0.16)
    parser.add_argument("--num-threads", type=int, default=3)
    parser.add_argument("text")
    args = parser.parse_args()

    text = normalize_text(args.text)
    if not text:
        raise SystemExit("empty text")

    model_dir = args.model_dir.rstrip("/")
    if args.type == "matcha":
        config = sherpa_onnx.OfflineTtsConfig(
            model=sherpa_onnx.OfflineTtsModelConfig(
                matcha=sherpa_onnx.OfflineTtsMatchaModelConfig(
                    acoustic_model=f"{model_dir}/model-steps-3.onnx",
                    vocoder=args.vocoder,
                    lexicon=f"{model_dir}/lexicon.txt",
                    tokens=f"{model_dir}/tokens.txt",
                ),
                provider="cpu",
                debug=0,
                num_threads=args.num_threads,
            ),
            rule_fsts=f"{model_dir}/phone.fst,{model_dir}/date.fst,{model_dir}/number.fst",
            max_num_sentences=1,
        )
    else:
        config = sherpa_onnx.OfflineTtsConfig(
            model=sherpa_onnx.OfflineTtsModelConfig(
                vits=sherpa_onnx.OfflineTtsVitsModelConfig(
                    model=f"{model_dir}/model.onnx",
                    lexicon=f"{model_dir}/lexicon.txt",
                    tokens=f"{model_dir}/tokens.txt",
                ),
                provider="cpu",
                debug=0,
                num_threads=args.num_threads,
            ),
            rule_fsts=f"{model_dir}/phone.fst,{model_dir}/date.fst,{model_dir}/number.fst",
            max_num_sentences=1,
        )
    if not config.validate():
        raise RuntimeError("invalid sherpa-onnx TTS config")

    tts = sherpa_onnx.OfflineTts(config)
    gen_config = sherpa_onnx.GenerationConfig()
    gen_config.sid = args.sid
    gen_config.speed = args.speed
    gen_config.silence_scale = args.silence_scale
    audio = tts.generate(text, gen_config)
    if len(audio.samples) == 0:
        raise RuntimeError("empty generated audio")
    write_wav(args.output, audio.samples, audio.sample_rate)


if __name__ == "__main__":
    main()
