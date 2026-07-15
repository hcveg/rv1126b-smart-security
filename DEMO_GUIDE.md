# RV-Guard 项目操作演示说明

本文件用于课程/比赛现场演示，包括打开工程、代码讲解、简单编辑和下载方式。

## 1. 打开工程

本地工程目录：

```bash
/home/enovo/qs/rv1126b-smart-security
```

可以用 VS Code、文件管理器或终端打开。

终端进入工程：

```bash
cd /home/enovo/qs/rv1126b-smart-security
```

查看文件结构：

```bash
tree -L 2
```

## 2. 工程结构讲解顺序

建议按下面顺序讲：

1. `README.md`
   - 介绍项目功能、亮点、运行路径和硬件组成。

2. `llm_ui/llm_gtk_chat.py`
   - 大模型触摸屏 UI 主程序。
   - 包含文字问答、语音输入、语音播报、安防状态查询、报警间隔控制和低功耗逻辑。

3. `face_ai/src/x11_recognize_main.cpp`
   - 人脸检测识别主流程。
   - 包含摄像头采集、RetinaFace 检测、ArcFace 特征识别、陌生人统计和报警状态输出。

4. `llm_ui/face_control.sh`
   - 大模型控制人脸识别模块的脚本。
   - 支持打开/关闭摄像头、查询状态、设置报警间隔。

5. `llm_ui/start_llm_x_session.sh`
   - 板端屏幕会话启动脚本。
   - 负责启动 X11、输入法、键盘、人脸识别窗口和大模型界面。

6. `MODEL_FILES.md`
   - 说明模型文件为什么没有放进仓库，以及部署时应该放到哪里。

## 3. 可现场演示的简单编辑

### 示例 1：修改低功耗等待时间

文件：

```bash
llm_ui/start_llm_x_session.sh
```

找到：

```bash
export LOW_POWER_IDLE_SECONDS="${LOW_POWER_IDLE_SECONDS:-60}"
```

可以把 `60` 改成 `120`，表示 2 分钟无操作后进入低功耗。

### 示例 2：修改默认报警间隔

文件：

```bash
llm_ui/start_llm_x_session.sh
```

找到：

```bash
export STRANGER_ALERT_INTERVAL_SECONDS="${STRANGER_ALERT_INTERVAL_SECONDS:-10}"
```

可以把 `10` 改成 `15`，表示陌生人持续停留时每 15 秒报警一次。

## 4. 修改后检查

Python 语法检查：

```bash
python3 -m py_compile llm_ui/llm_gtk_chat.py
```

Shell 脚本检查：

```bash
sh -n llm_ui/start_llm_x_session.sh
sh -n llm_ui/face_control.sh
```

查看 Git 改动：

```bash
git status
git diff
```

## 5. 提交修改

```bash
git add .
git commit -m "Update demo parameter"
git push
```

## 6. 下载方式

GitHub 仓库：

```text
https://github.com/hcveg/rv1126b-smart-security
```

下载方式一：网页下载

1. 打开 GitHub 仓库。
2. 点击绿色 `Code` 按钮。
3. 选择 `Download ZIP`。

下载方式二：Git 克隆

```bash
git clone git@github.com:hcveg/rv1126b-smart-security.git
```

如果需要下载视频文件，需要安装 Git LFS 后执行：

```bash
git lfs pull
```

## 7. 演示重点

- 项目不是单独的人脸检测，而是把视觉识别、本地大模型、语音交互和低功耗管理融合在一起。
- 大模型可以用自然语言查询安防状态和控制报警间隔。
- 系统在 RV1126B 上本地运行，符合端侧 AI 应用场景。
- 低功耗逻辑会避开大模型运行、语音播报和人脸检测，防止任务被中断。
