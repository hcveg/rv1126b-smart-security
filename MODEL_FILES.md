# Model Files

为了控制仓库体积并避免权重版权问题，本仓库不提交模型文件。部署时需要把模型放到板端对应位置。

## 人脸检测与识别

需要放入 `/userdata/face_ai/model/`：

- `RetinaFace_mobile320_rv1126b.rknn`
- `ArcFace_112_rv1126b.rknn`

用途：

- RetinaFace：人脸检测。
- ArcFace：人脸特征提取与身份识别。

## 本地大模型

默认 RKLLM 模型路径：

- `/userdata/llm/rkllm/models/MiniCPM4-0.5B_w4a16_RV1126B.rkllm`

可选 llama.cpp GGUF 模型路径：

- `/userdata/llm/models/qwen2.5-0.5b-instruct-q4_k_m.gguf`

## 语音识别与语音合成

默认语音识别模型：

- `/userdata/llm/voice/vosk-model-small-cn-0.22`

默认语音合成模型：

- `/userdata/llm/voice/matcha-icefall-zh-baker`
- `/userdata/llm/voice/vocos-22khz-univ.onnx`

## 人脸库

人脸库不建议上传 GitHub。板端默认路径：

- `/userdata/face_ai/faces`

每个人一个目录，注册后生成对应特征数据。
