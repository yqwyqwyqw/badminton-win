# Windows 媒体验证工具

这些脚本只读取原始视频，所有代理、日志和报告都写入 `validation-output`。

Python 推理脚本基于 [ZSHYC/BadmintonTrackNet](https://github.com/ZSHYC/BadmintonTrackNet)。
请先在虚拟环境中安装该项目及其依赖，再运行本目录脚本；模型权重和素材不纳入本仓库。

## 1. 解码与硬件加速

```powershell
powershell -ExecutionPolicy Bypass -File .\validation\Test-MediaDecode.ps1 `
  -InputPath 'F:\羽毛球视频\20260828\VID20260828212318_1.mp4' `
  -DurationSeconds 30
```

脚本检查视频规格、恒定帧率、CPU 解码以及 NVDEC/CUDA 解码和缩放吞吐。若素材不是准确的 3840x2160@60fps，结果会是 `PASS_WITH_WARNING`，且不会把本素材当成 4K60 验证样本。

## 2. 720p 代理与时间轴

```powershell
powershell -ExecutionPolicy Bypass -File .\validation\Test-ProxyTimeline.ps1 `
  -InputPath 'F:\羽毛球视频\20260828\VID20260828212318_1.mp4'
```

脚本生成 720p 代理并验证帧数、时长、逐帧时间戳与随机 seek。NVENC 不兼容时会自动退回软件编码。

## 3. TrackNet 原始轨迹复核

TrackNet 推理完成后，用以下命令生成叠加视频、长断轨统计和联系表：

```powershell
.\.venv-validation\Scripts\python.exe .\validation\summarize_tracknet.py `
  --video <720p代理路径> `
  --csv <TrackNet输出CSV> `
  --output-dir <结果目录>
```

没有逐帧人工真值时，检测帧比例不能当成召回率。必须查看叠加视频，把遮挡/出画与真实漏检区分开。

## 4. InpaintNet 断轨修复

```powershell
.\.venv-validation\Scripts\python.exe .\validation\run_inpaint.py `
  --input-csv <TrackNet原始CSV> `
  --checkpoint <InpaintNet_best.pt> `
  --output-csv <修复后CSV> `
  --width 1280 --height 720 --fps 50 --device cuda
```

输出同时保留 `RawVisibility/RawX/RawY`、修复后的坐标和 `Inpainted` 标志。插值坐标不能视为模型真实看见了球。

当前素材的完整结论见 `validation/RESULTS.md`。

## 5. 可恢复的长视频推理与逐回合时间索引

```powershell
.\.venv-validation\Scripts\python.exe .\validation\run_chunked_tracknet.py `
  --video '.\validation-output\序列_01-tracknet-hq\proxy-720p-crf10.mp4' `
  --tracknet-file '.\.tools\models\TrackNetV3\ckpts\TrackNet_best.pt' `
  --output-dir '.\validation-output\sequence01-streaming' `
  --chunk-seconds 6 --overlap-seconds 0.5 --batch-size 8
```

每个核心块完成后立即写入 `chunks/chunk-*.csv`、合并后的
`trajectory-partial.csv` 和包含真实帧进度/ETA 的 `progress.json`。已闭合回合会追加到
`rally-index.csv` 与 `rally-feed.json`，并默认逐个编码独立回合短片。

默认使用 `nonoverlap` 快速模式；需要最高轨迹稳定性时可添加 `--eval-mode weight`。
如果只希望播放器按照时间索引播放原代理视频，可添加 `--no-rally-clips`。收到 Ctrl+C 时，
运行器将状态写为 `paused`；使用完全相同的输入、模型和分块参数再次执行即可跳过已完成块并续跑。
