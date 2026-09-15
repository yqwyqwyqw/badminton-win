# 第三方组件与许可声明（Windows 便携版）

本目录分发的 `badminton-analyzer.exe` 依赖以下第三方组件。各组件版权归其各自作者所有，
按下列许可分发。

| 组件 | 版本 | 许可 | 在本产品中的形态 |
|---|---|---|---|
| Qt（Core / Gui / Qml / Quick / QuickControls2 / Multimedia 等） | 6.11.2 | LGPL-3.0（或商业许可） | **动态链接**：随包分发的 `Qt6*.dll`、`plugins/`（`platforms`、`styles`、`imageformats`、`multimedia`、`tls` 等） |
| ONNX Runtime | 1.24.4 | MIT | 动态链接：`onnxruntime.dll`、`onnxruntime_providers_shared.dll` |
| DirectML | 1.15.4 | MIT | 动态链接：`DirectML.dll` |
| FFmpeg（含 libx264 等） | 9.0.1 | **GPL-3.0**（构建含 `--enable-gpl` 与 libx264） | **独立进程调用**：`resources/ffmpeg/ffmpeg.exe`、`ffprobe.exe`；本产品不链接 libav* |
| TrackNetV3 预训练权重 | 8 帧 / concat 背景 / 288×512（fp16 导出） | MIT（原项目 LICENSE 明确覆盖预训练 checkpoint） | 随包分发：`resources/models/tracknet-8f-concat-288x512-sizes-fp16.onnx` |
| Microsoft D3DCompiler | 47 | Microsoft 可再分发组件 | `D3Dcompiler_47.dll` |

## 许可全文与源码获取

- Qt（LGPL-3.0）：<https://www.gnu.org/licenses/lgpl-3.0.html>；
  Qt 源码：<https://download.qt.io/official_releases/qt/6.11/>。
  按 LGPL-3.0 要求，本产品对 Qt 仅做动态链接，用户可用自行编译/替换的 Qt 动态库重新运行本程序。
- ONNX Runtime（MIT）：<https://github.com/microsoft/onnxruntime/blob/main/LICENSE>。
- DirectML（MIT）：<https://github.com/microsoft/DirectML/blob/master/LICENSE>。
- FFmpeg（GPL-3.0）：<https://ffmpeg.org/legal.html>、<https://www.gnu.org/licenses/gpl-3.0.html>；
  本包内 FFmpeg 为公开的 Windows 构建（gyan.dev "full build shared"），
  对应源码见 <https://ffmpeg.org/download.html> 与 <https://www.gyan.dev/ffmpeg/builds/>。
  FFmpeg 以独立可执行文件形式调用，未与本程序静态或动态链接。
  libx264（GPL-2.0+）：<https://www.videolan.org/developers/x264.html>。
- TrackNetV3（MIT）：<https://github.com/qaz812345/TrackNetV3>。

## 说明

- 若再分发本目录，请保留本文件与上述许可声明。
- 若需要 LGPL 场景下更严格的合规形式（例如附 Qt 源码压缩包或提供书面要约），可按上述链接补充。
