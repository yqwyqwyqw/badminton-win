# ONNX 化与轻量化重构方案（v1 草案）

> 目标分支：`deepseekflash`（基于 `main` @ ec0bdb7）
> 目标：把当前"Qt UI + Python/PyTorch/CUDA 子进程 + 完整版 FFmpeg"的方案，改造成**单一运行时、无 Python、无 CUDA 强依赖**的 Windows 本地应用，产出**免安装单 exe**或**<500 MB 安装包**。
> 本文只写方案，不含代码改动。所有体积/参数均为本机实测或从代码中读出，标注了出处。

> **已确认决策（2026-09-15）**
> - **D1 发布方式**：仅作为**个人 GitHub 发布、完全不商用**。→ 权重可随包分发（上游 MIT 明确覆盖 checkpoint，见附录 C-1）；GPL/LGPL 义务与是否商用无关，仍需履行（附许可、给源码指引、**不要把 libav\* 链进 exe**）。
> - **D2 InpaintNet**：**不放进本次重构**（二期可选）。注意："球飞出画面"**不是** InpaintNet 解决的问题——出画由状态机的顶边/3 秒规则处理，且是本次移植的**重点回归项**（见 §1.4、§4 Phase 2）。
> - **D3 首遍模型**：**锁定 V3-8f，只换运行时**（ONNX Runtime + DirectML EP + fp16）。不换轻量主干；精度变化必须由 golden 回归证明为"零变化"。
> - **D4 BounceNet**：**不放进本次重构**；推理层只需保证"多模型 = 多 session"的可扩展性。若想先改善落地判定，可另评零成本的规则版 `tracknet/landing/predictor.py`。

> **执行状态（2026-09-15）**
> - **Phase 0（golden 回归集）：✅ 完成。** 金标准 = app 自己的 2823 帧运行结果（`golden/<clip>/app-run/`）+ 1200 帧 PyTorch 采集（32 个输入窗口、fp32 热图）。
> - **Phase 1（ONNX 导出与对齐）：✅ 通过。** 采纳模型 **`tracknet-8f-concat-288x512-sizes-fp16.onnx`（21.6 MiB）**；全片段 2823 帧上 **可见性差异 0/2823、坐标位级一致 99.5%、P95 = 0 px、回合切分与基线完全一致**；DirectML 吞吐 **42.4 fps（0.85× 实时）**。详见 **`onnx-m1-parity-report.md`**，决策见 **`决策记录.md`**。
> - **Phase 2（C++ 内核）：已开始。** 第一步（C++ + ONNX Runtime DirectML 冒烟测试）✅ 完成：C++ 侧 32 窗口 fp16 max\|Δ\|=5.47e-3（**3774 万像素中 0 个 >0.01**），仅模型吞吐 **84.75 fps**；构建方式与集成细节见 `onnx-m1-parity-report.md` §10。**前处理/后处理/状态机的移植尚未开始。**
> - ⚠️ **性能口径更正**：`validation/RESULTS.md:41` 的 11.62 fps 是 `weight` 模式（步长 1）的数字；app 用 `nonoverlap`（步长 8），**真实 CUDA 基线是 46.5–51.7 fps（0.93–1.03× 实时）**。故 ONNX+DirectML 是"**几乎不掉性能地去掉 CUDA/Python**"，而不是"大幅提速"。

---

## 0. 结论先行

| 议题 | 结论 |
|---|---|
| 体积能否降到目标 | **能，且余量很大。** 现在这台机器要跑起分析功能，需要约 **11 GB** 物料（`.venv-validation` 7.4 GB + `.tools/packages` 3.3 GB + ffmpeg 253 MB + models 261 MB）。其中 **95% 是 PyTorch 的 CUDA 运行时**，与算法本身无关。 |
| 改造后体积（预估） | 便携目录 **≈180–220 MB**；7z 自解压单 exe **≈85–110 MB**；安装包 **≈90–140 MB**。均满足"<500 MB"，单 exe 方案可行。 |
| 模型侧的最大发现 | `TrackNet_best.pt` 129.9 MiB 里 **2/3 是 Adam 优化器状态**（`optimizer.state` 53 组 `exp_avg/exp_avg_sq`）。**模型本体只有 104 个张量、11.34 M 参数**（按通道配置精算，并由 ckpt 权重形状 `(64,27,3,3)`/`(8,64,1,1)` 校验）：ONNX fp32 **≈45.4 MiB**、fp16 **≈22.7 MiB**、int8 **≈11 MiB**。InpaintNet 0.52 M 参数 ≈2.1 MiB。 |
| 推理运行时的替换 | torch 2.8.0+cu128（**7,054 MB**，其中 `torch/lib` 6,958 MB）→ **ONNX Runtime**：CPU 版 13.6 MB / DirectML 版 23.9 MB（PyPI wheel 实测）。**不采用 onnxruntime-gpu（153 MB，且仍需 CUDA 运行时）**。 |
| ONNX 导出难度 | **低。** 两个模型只有 `Conv2d/Conv1d + BatchNorm + ReLU/LeakyReLU + MaxPool + Upsample(scale_factor=2) + cat + Sigmoid`，无自定义算子、无张量依赖的 Python 控制流、**输入尺寸固定 (288×512)**，导出的最大风险只是数值对齐而非算子支持。 |
| 最大的技术风险 | 不是 ONNX，而是**三处"隐性契约"必须逐帧对齐**：① `cv2.resize` 双线性 vs FFmpeg `swscale`；② `cv2.findContours` 最大外轮廓 → `boundingRect` 中心 的热图解码语义；③ 回合同步状态机的 8 个阈值参数。**必须用第 4 节的 golden 回归集逐帧比对，不能只看"看起来对"。** |
| 最大的产品风险 | **性能**。实测口径更正后：PyTorch+CUDA 基线 **46.5–51.7 fps（0.93–1.03× 实时）**，**ONNX+DirectML fp16 42.4 fps（0.85× 实时，已实测通过）**，纯 CPU 3.81 fps（整片 ≈12 min）。对策（**D3 已决策**：不换模型，只换运行时）：**DirectML EP（任何 DX12 显卡含核显即可）+ fp16**，并把"预估时间"如实暴露给 UI；纯 CPU 时给档位选择（这也是原始方案 `badminton-analyzer-plan.md` 第 3.1 节"两遍处理"的既定思路）。 |
| 附带修掉的现有缺陷 | ① 发布包含 UI 不含推理环境，装上跑不了（`RELEASE.md:28` 自述）；② 720p 代理用 `scale=1280:720` 强制变形，非 16:9 素材会把球的运动学拉歪；③ 回合短片用 `-an` 编码，导致"720p 快速版导出"实际上是无声视频；④ 缓存目录写在工程根 `validation-output/`，安装后不可用。 |
| 工作量（单人） | **≈16–23 人日（3–5 周）**，其中 C++ 推理内核与回归验收占一半以上。 |

---

## 1. 现状盘点

### 1.1 运行架构（现状）

```text
QML/C++ (Qt 6.11.2)                     Python 3.11 (uv venv)            外部进程
┌───────────────────────────┐          ┌────────────────────────┐      ┌──────────┐
│ VideoImportController     │          │ run_chunked_tracknet.py│      │ ffmpeg   │
│  ├ Qt Multimedia 元数据    │          │  ├ cv2.VideoCapture    │      │ (9.0.1   │
│  └ Qt Multimedia 播放      │          │  ├ 背景中值图 (.npy)    │      │  full    │
│ TrialAnalysisController   │ QProcess │  ├ torch + TrackNet     │ CLI  │  shared) │
│  ├ 720p 代理 (ffmpeg)     │─────────▶│  ├ chunks/chunk-*.csv  │─────▶│ 代理/切片 │
│  ├ 进度 progress.json     │          │  └ analyze_..._rallies │      │ /导出    │
│  └ 回合 rally-feed.json   │          │      .py (状态机子进程) │      └──────────┘
└───────────────────────────┘          └────────────────────────┘
```

依据：`app/src/TrialAnalysisController.cpp:529-567`（启动 Python）、`:454-482`（依赖路径）、`validation/run_chunked_tracknet.py:27-28`（导入 `tracknet.*`）、`:293-300`（用 `sys.executable` 再起分析器子进程）。

### 1.2 体积构成（本机实测）

| 项 | 体积 | 说明 |
|---|---:|---|
| `.venv-validation/` | **7,375 MB** | 其中 `torch` **7,054 MB**（`torch/lib` 6,958 MB = cu128 运行时）、`cv2` 112 MB、`pandas` 64 MB、`numpy` 53 MB、`sympy` 25 MB、`PIL` 15 MB；解释器由 uv 托管 CPython 3.11 |
| `.tools/packages/` | **3,301 MB** | 单个文件：`torch-2.8.0+cu128-cp311-cp311-win_amd64.whl` |
| `.tools/ffmpeg/` | **253 MB** | `avcodec-63` 93.3、`avfilter-12` 98.4、`avformat-63` 19.3、`ffplay` 15.1（+ 各 DLL 与 3 个 exe） |
| `.tools/models/` | **261 MB** | `TrackNet_best.pt` 129.9 MiB（**模型仅 45.4 MiB**，其余为 Adam 双动量）、`TrackNetV3_ckpts.zip` 125.6（同一权重的压缩包，冗余）、`InpaintNet_best.pt` 6.0 MiB（模型 2.1 + 优化器 3.9） |
| `.tools/downloads` + `ui-bootstrap*` | **144 MB** | FFmpeg 8.0.1 包 + 两个 pip bootstrap venv（仅安装期使用） |
| `validation-output/` | **871 MB** | 验证期产物（代理、轨迹、短片） |
| Qt 运行库（`windeployqt` 实测） | **118.7 MB** | `opengl32sw` 19.7、`avcodec-61` 13.4、`Qt6Core` 9.9、`Qt6Gui` 9.1、`Qt6Quick` 6.3、`Qt6Qml` 5.1、`Qt6Pdf` 4.4、`D3Dcompiler_47` 4.0、QuickControls2 各样式 3.2/3.0/2.6… |
| **合计（能跑分析的物料）** | **≈11.0 GB** | 当前发布 zip 只含 UI（≈119 MB），**不含任何推理环境** |

### 1.3 关键接口契约（改造必须保持兼容的部分）

**调用参数**（`TrialAnalysisController.cpp:547-559`）：

```
--video <proxy-720p.mp4> --tracknet-file <TrackNet_best.pt> --output-dir <.../tracknet>
--chunk-seconds 6 --overlap-seconds 0.5 --batch-size 8 --eval-mode nonoverlap
--ffmpeg <.tools/.../ffmpeg.exe> [--max-rallies N]
PYTHONPATH=<.tools/vendor/BadmintonTrackNet>
```

**stdout 协议**（C++ 解析于 `TrialAnalysisController.cpp:618-641`）：

```
PROGRESS frames=<done>/<target> percent=<p> eta=<sec|None>s
RALLY_READY rally=<n> hits=<n> time=<a>-<b>s clip=<path>
CHUNK_START / SAMPLE_COMPLETE / DONE / PAUSED
```

**产物文件**：

| 文件 | 内容 | 读取方 |
|---|---|---|
| `progress.json` | `status/framesCompleted/framesTarget/percent/currentChunk/chunkCount/fps/inferenceFramesPerSecondThisRun/etaSeconds/updatedAtUnix` | C++ `:706-737` |
| `rally-feed.json` | 数组，元素含 `rally/startFrame/endFrame/startSeconds/endSeconds/hitCount/manualHitCount/effectiveHitCount/reviewStatus/observedHits/inferredGapHits/confidence/clip` | C++ `:739-772` |
| `rally-index.csv` | 上表的 CSV 形式 | 人工核对 |
| `chunks/chunk-*.csv` | `Frame,Visibility,X,Y`（断点续跑的最小单元） | 续跑逻辑 |
| `trajectory-partial.csv` | 合并轨迹（同上 4 列） | 状态机输入 |
| `background-median.npy` | `(H,W,3)` uint8 背景中值图 | 复跑复用 |
| `run-fingerprint.json` | `chunkSchemaVersion=2` + 视频/模型指纹 + 分块参数 | 防止串用结果 |

列语义（`tracknet/data/contracts.py:70-78`）：`Frame:int64, Visibility:int8(0/1), X:float64, Y:float64`，**坐标是"代理视频"像素坐标**，`Visibility = (x!=0 || y!=0)`（`tracknet/inference/tracknet.py:48`、`pipeline.py:194-200`）。

### 1.4 模型与推理链（ONNX 化的确切规格）

**TrackNet**（`tracknet/models/tracknet.py:44-73`）：VGG-ish 编码器 + U-Net 式跳连解码器，通道 27→64→128→256→512；**18 个 Conv2d**（17 个 `Conv+BN+ReLU` 块 + 1 个 1×1 输出层）、`MaxPool2d(2,2)` ×3、`Upsample(scale_factor=2)` ×3（PyTorch 默认 **nearest**）、`torch.cat` ×3、末端 `Sigmoid`。只 import `torch/torch.nn`——**不用 torchvision、无 VGG16 预训练权重**。`Conv2d(padding='same')` 导出为显式对称 `pad=1`。

**checkpoint 元数据**（本次直接解出 `archive/data.pkl`，未依赖 torch）：

```python
# TrackNet_best.pt
param_dict = {'model_name':'TrackNet', 'seq_len': 8, 'bg_mode': 'concat', 'tolerance': 4, ...}
epoch = 18；optimizer = Adam(state: 53 组 exp_avg/exp_avg_sq, step=119852)；state_dict 条目 = 104
# InpaintNet_best.pt
param_dict = {'model_name':'InpaintNet', 'seq_len': 16, 'mask_ratio': 0.3, ...}
epoch = 281；state_dict 条目 = 18
```

由 `tracknet/data/io.py:68-75` 的装配规则可推出**固定输入输出**：

| 模型 | 输入 | 输出 | ONNX 体积 |
|---|---|---|---|
| TrackNet（`bg_mode='concat'`, `seq_len=8`） | `(N, 27, 288, 512)` fp32 = 背景帧 3ch + 8 帧 × 3ch | `(N, 8, 288, 512)` sigmoid 热图（每输入帧一张单通道热图） | fp32 ≈ **45.4 MiB** / fp16 ≈ 22.7 MiB / int8 ≈ 11 MiB |
| InpaintNet（`seq_len=16`） | `coords (N,16,2)` 归一化坐标 + `mask (N,16,1)` | `(N,16,2)` sigmoid | ≈ **2.1 MiB**（当前 app **未启用**：`run_chunked_tracknet.py:401-405` 传 `inpaintnet_file=""`） |

**前处理**（`tracknet/inference/pipeline.py:177-192`）：
`BGR→RGB`（`frames[..., ::-1]`）→ `cv2.resize(frame,(512,288))`（默认 `INTER_LINEAR`；**直接拉伸，无 letterbox、无裁剪**）→ `HWC→CHW` → `bg_mode=='concat'` 时把背景帧（同样 resize 成 RGB）插到**第 0 位** → 全部 `concat` 后 `.astype(float32)/255`。**归一化只有 `/255`，没有 mean/std**，也**没有任何 fp16/autocast**。

**背景中值图**：`global_median()`（`run_chunked_tracknet.py:130-156`）用 `np.linspace` 抽 41 帧（`--median-samples`）做逐像素中值，**BGR、uint8、(H,W,3)**，缓存 `<output-dir>/background-median.npy`，全片共用一张、命中时校验 shape。⚠️ 训练侧另有一条路径保存的中值**已是 RGB**（`tracknet/data/io.py` 的 `get_rally_median`），C++ 复现时**不要混用颜色顺序**。

**窗口化**（`tracknet/data/video.py:19-50`、`run_chunked_tracknet.py:417-421`）：
`nonoverlap` ⇒ `step=seq_len=8`、`pad=true`（末尾不足 8 帧则**重复最后一帧**并记 `valid_length`）；**块起点必须按 `read_start -= read_start % seq_len` 对齐到全局网格**，否则分块边界会产生假的轨迹事件（原注释明确说明）。

**热图解码**（`pipeline.py:194-200`）：
`heatmap > 0.5` → 二值 → `cv2.findContours(RETR_EXTERNAL)` → 取**面积最大轮廓**的 `boundingRect` → 中心 `(x+w/2, y+h/2)` → 缩放 `×(proxyW/512, proxyH/288)`；无轮廓 ⇒ `(0,0)`（不可见）。**没有 argmax、没有 softmax、没有亚像素**——这既是它鲁棒的原因，也是 C++ 复现时必须逐位对齐的地方。

**集合（ensemble）**：`nonoverlap` 权重恒 1；`weight` 模式权重 `min(i+1, L-i)`，`seq_len=8` 时即 **[1,2,3,4,4,3,2,1]**（`pipeline.py:114-118`）。注意**先按真实帧号加权平均热图、再解码**（不是先解码再平均坐标），且只 flush 严格早于当前窗口起始帧的帧。

**InpaintNet 补充规格**（当前未启用，二期可选）：输入是**归一化坐标** `(x/proxyW, y/proxyH)` 与掩码；掩码规则为「相邻可见帧 `left/right` 满足 `right > left+1` **且** `y[left] > H*0.05` **且** `y[right] > H*0.05`」才补（即**顶部出画不补**）；融合式 `out*mask + in*(1-mask)`，最后把 `< COOR_TH` 的坐标归零，`COOR_TH = 50/sqrt(288²+512²) ≈ 0.08512`（`io.py:18-20`）。

**回合状态机**（`validation/analyze_tracknet_rallies.py`，待移植到 C++）：

```
--split-gap-seconds 0.35   --top-edge-fraction 0.22   --min-rally-hits 2
--min-rally-seconds 0.8    --smooth-seconds 0.22      --turn-window-seconds 0.24
--merge-seconds 0.32       --min-turn-diagonal-fraction 0.02
```
另有：`restart_gap = 0.10s`、`very_long_gap = 3.0s`、盒式平滑（奇数窗口）、x/y 轴极值（prominence）作为击球候选。

### 1.5 现有缺陷（顺手在本次改造中修掉）

| # | 问题 | 证据 | 建议 |
|---|---|---|---|
| D-1 | 发布包无推理环境，"安装即不可用" | `RELEASE.md:28` | 本次改造直接消除 |
| D-2 | 720p 代理 `scale=1280:720` 强制变形（16:9 源无影响；竖屏/4:3 源会被拉伸） | `TrialAnalysisController.cpp:511-512` | **决策：不修**。切片与导出都按**时间点**进行、与坐标/尺寸无关（用户确认"能识别就行"）；只要代理生成逻辑保持现状，golden 集持续有效（**将来若改代理必须重跑 M0 对齐**）。非 16:9 素材的识别质量略降属已接受风险 |
| D-3 | 回合短片 `-an` 编码后被当作"720p 快速版"直接复制导出 ⇒ **导出无声** | `run_chunked_tracknet.py:245`、`TrialAnalysisController.cpp:232-240` | 短片带音轨，或导出时从原片按时间索引重切 |
| D-4 | 缓存目录写在工程根 `validation-output/ui-trials/...`，且靠 `discoverProjectRoot()` 向上找工作区 | `TrialAnalysisController.cpp:467-468, 793-807` | 改为 `%LOCALAPPDATA%\<App>\cache\<指纹>` |
| D-5 | 全分辨率背景中值图造成 GB 级内存峰值 | `validation/RESULTS.md:75` | 在**模型分辨率**上算中值（512×288） |
| D-6 | 硬编码 `D:\Qt\...`/本机路径 | `scripts/release-windows.ps1:15-17`、`TrialAnalysisController.cpp:472-481` | 资源根可配置 + 首次运行自检 |
| D-7 | NVENC 初始化失败（FFmpeg 9.0.1 要求 NVENC API 13.1 vs 驱动 13.0） | `validation/RESULTS.md:29` | 精简 FFmpeg 时锁 API，或仅用 x264/QSV/AMF |

---

## 2. 目标与硬性指标

| 指标 | 目标值 | 说明 |
|---|---|---|
| 免安装单 exe | **≤120 MB**（目标 ≈85–110 MB） | 7z 自解压，首启解压到 `%LOCALAPPDATA%` |
| 安装包 | **≤500 MB**（目标 ≈90–140 MB） | Inno Setup/WiX，payload 与单 exe 相同 |
| 便携目录（解压后） | ≤220 MB | 可拷贝即用 |
| 运行依赖 | **无 Python、无 CUDA、无 Visual Studio** | 干净 Win10 1809+/Win11 可运行 |
| 唯一运行时 | ONNX Runtime + **DirectML（必需）** | CPU EP 仅用于开发期对齐，不作为发布路径（决策） |
| 质量 | 见第 6 节验收标准（逐帧对齐 + 回合边界不劣化） | **切分召回是交付关键，拍数只是草稿元数据**（`RESULTS.md:67-69`） |
| 架构 | 单进程 C++ 内核 + Qt UI（不再有 Python 子进程） | 为将来的 Android 端共享内核留出空间 |

---

## 3. 体积预算

| 组成 | 现状 | 改造后（目标） | 手段 |
|---|---:|---:|---|
| PyTorch + CUDA 运行时 | 7,054 MB | **0** | ONNX Runtime |
| Python 解释器与站点包（cv2/numpy/pandas/sympy/PIL/gdown…） | ≈620 MB | **0** | C++ 原生实现，解码交给 FFmpeg/Qt |
| torch wheel 缓存 | 3,301 MB | **0** | 不随包 |
| 模型 | 261 MB | **≈21.6 MB**（TrackNet fp16，已实测采纳） | 只导出 `state_dict`，剥离优化器状态 |
| ONNX Runtime | — | **≈34 MB 落盘**（`onnxruntime.dll` 16.5 + `DirectML.dll` 17.7 + providers_shared 0.02；`onnxruntime-directml` 单包，含 DML + CPU 两个 EP） | 已实测：C++ 仅模型 DML fp16 **84.75 fps**；Python 端到端 DML fp16 42.4 fps；CPU 3.81 fps |
| FFmpeg CLI | 253 MB | **≈25–40 MB** | `--disable-everything` 精简构建（见 §4.5） |
| Qt 运行库 | 118.7 MB | **≈85–100 MB** | 只保留用到的 QML 模块与插件；`opengl32sw`(19.7 MB) 视需要取舍 |
| 应用 exe + QML + 资源 | ≈12 MB | ≈15 MB | 静态 CRT（/MT）避免 VC redist |
| **便携目录合计** | **≈11 GB** | **≈180–220 MB** | |
| **7z 自解压单 exe** | — | **≈85–110 MB** | DLL 压缩率高、fp16 权重压缩率低（约 10–20%） |
| **安装包** | — | **≈90–140 MB** | 同上；如需内置 VC redist +25 MB |

> 说明：模型是**唯一不可压缩**的部分（浮点权重近似随机）。因此**优先 fp16 导出**（20 MB），int8 仅在精度回归通过后再选；`TrackNetV3_ckpts.zip`（125.6 MB 冗余副本）与 `.tools/packages`、`ui-bootstrap*` 都应从仓库与发布流程中移除。

---

## 4. 技术方案

### Phase 0 — 先固化"金标准"回归集（**必须最先做**）

改造前，用现有 Python 环境在 3 段素材（1080p / 4K / 竖屏各一）上跑一次完整链路，把中间产物**冻结**成回归基准：

```
golden/
  <clip>/trajectory-pytorch.csv      # 逐帧 Frame,Visibility,X,Y（现状输出）
  <clip>/heatmap-sample.npz          # 抽 200 帧的原始热图（fp32），用于数值对齐
  <clip>/rally-feed-pytorch.json     # 回合切分结果（现状输出）
  <clip>/input-tensor-sample.npz     # 抽 200 个窗口的预处理结果 (N,27,288,512)
  <clip>/meta.json                   # 视频指纹、分块参数、模型 sha256
```

理由：ONNX 化最大的风险是**静默劣化**——切分少一个回合、边界偏 0.3 s，肉眼看不出但用户会骂。没有 golden 集，"改完对不对"无法回答。该目录**只用于开发/CI**，不进发布包。

### Phase 1 — ONNX 导出与数值对齐

1. **导出脚本** `tools/export_onnx.py`（开发期工具，需要 torch，不进发布包）：
   - 复刻 `TrackNet`/`InpaintNet` 定义（直接 import 现有 `tracknet.models.tracknet`，**不改模型代码**）；
   - 用 `param_dict` 装配（TrackNet：`in_dim=(seq_len+1)*3=27, out_dim=8`；`bg_mode='concat'`）；
   - `torch.onnx.export(..., opset=17, input=(1,27,288,512), dynamic_axes={'input':{0:'N'}, 'output':{0:'N'}}, do_constant_folding=True)`；`model.eval()`；固定 288×512 无需动态 H/W；
   - `torch.load(pt, map_location="cpu", weights_only=False)`——⚠️ torch 2.8 默认 `weights_only=True`，本 ckpt 含 `param_dict`/`optimizer`，必须显式关掉（这些 ckpt 的张量是按 CUDA 序列化的，`map_location` 也不能省）；
   - **只取 `ckpt['model']`**：state_dict **无任何前缀**（不是 `model.`、也不是 `module.`），对应裸 `nn.Module`，`load_state_dict` strict 即可成功，**不需要剥壳**；
   - 导出后 `onnx.checker` + `onnxruntime` 自比对：随机 8 组输入，`max|Δ| ≤ 1e-4`；
   - fp16 转换（`onnxconverter-common.float16`，注意 BN 与 Sigmoid 前的层保留 fp32），int8 量化（`quantize_dynamic`/QDQ）作为可选；
   - 产出 `resources/models/tracknet-8f-concat-288x512-fp16.onnx` 与 `inpaintnet-16f.onnx` + `models.json`（含 sha256、导出参数、精度等级）。

2. **对齐门禁**（脚本化，纳入 CI）：
   - `max|heatmap_onnx − heatmap_torch|`：fp32 ≤ 1e-3、fp16 ≤ 5e-3；
   - 解码成坐标后：**可见性一致率 ≥ 99.5%**、坐标差 **P95 ≤ 1 px**（相对 golden `trajectory-pytorch.csv`）；
   - 回合切分：回合数一致、每条边界差 ≤ 0.2 s。

### Phase 2 — C++ 推理内核（本次改造的主体）

新增 `app/src/inference/`，把 Python 链路逐层搬过来：

| 模块 | 复刻对象 | 关键点（易错） |
|---|---|---|
| `onnx_session.{h,cpp}` | ORT C++ API | EP 选择：CPU 默认；`DmlExecutionProvider` 探测成功则启用（失败静默回退 CPU）；`intra_op_num_threads = min(物理核, 8)` |
| `video_decoder.{h,cpp}` | `cv2.VideoCapture` + `read_frames` | libavcodec 顺序解码；分块定位用**帧号精确 seek**（`av_seek_frame` 到关键帧后丢弃到目标帧）；可选 NVDEC/D3D11VA 硬解 + 软件回退（保留现有 `startProxy` 的双路径思想，`TrialAnalysisController.cpp:484-527`）。⚠️ **许可约束（D1）**：现有 FFmpeg 是 **GPLv3（含 libx264）**，因此**不要把 libav\* 静态/动态链进 exe**（那会让整个程序受 GPL 覆盖）。二选一：① 继续**子进程调用 `ffmpeg.exe`**（当前做法，属聚合分发，自己的代码不受 GPL 传染）；② 改用**纯 LGPL 构建**（`--disable-gpl`、去掉 x264，编码走硬件 QSV/AMF/NVENC 或 openh264）后再链接。 |
| `preprocess.{h,cpp}` | `pipeline._prepare_frames` | ① BGR→RGB；② **缩放到 512×288 必须与 `cv2.resize(INTER_LINEAR)` 数值一致**（OpenCV 用 1/32 定点 + 半像素中心 `(dst+0.5)*scale−0.5`，直接调 `swscale` 会有 ±1 灰度差 ⇒ 建议自写等价的定点双线性，并用 golden `input-tensor-sample.npz` 逐元素比对，容差 0）；③ 背景帧插到最前；④ `/255` float32 |
| `postprocess.{h,cpp}` | `_decode_heatmap` | `>0.5` 二值 → **8 邻接连通域**取面积最大（等价 `findContours(RETR_EXTERNAL)`）→ `boundingRect` 中心；注意 OpenCV 轮廓宽高是 `max−min+1` ⇒ 中心 `(minx+(maxx−minx+1)/2, …)`；无命中 ⇒ `(0,0)` 并置不可见 |
| `ensemble.{h,cpp}` | `_accumulate_batch` + 权重表 | `nonoverlap` 权重 1；`weight` 模式 `min(i+1,L−i)`；按真实帧号累加、除总权重、`flush(before=window.frame_ids[0])` 的**提前解码**语义要一致 |
| `windowing.{h,cpp}` | `iter_windows` | 末尾 pad = **重复最后一帧**并携带 `valid_length`；块起点按 `read_start % seq_len` 对齐 |
| `rally_state_machine.{h,cpp}` | `analyze_tracknet_rallies.py` 全文 | §1.4 的 8 个参数 + `restart_gap` / `very_long_gap` + 盒式平滑 + 轴极值。🔴 **"球飞出画面"完全由这一层负责（D2，本次移植的重点回归项）**：`--top-edge-fraction 0.22`（两端点 y 是否落在画面上部 22%）+ `very_long_gap = 3.0 s`（长中断判终局）+ `restart_gap = 0.10 s`，据此区分"瞬时出画（回合继续）"与"终局出画（回合结束）"（`analyze_tracknet_rallies.py:94,116-141`）。**绝不能靠 InpaintNet 补出画段**——它的掩码守卫只看两个端点是否在顶部 5%，球从顶部飞出再飞回时两端点可能都低于 5%，于是它会**把画面外的飞行编造成平滑轨迹**（`pipeline.py:240-247`）。 |
| `job_runner.{h,cpp}` | `run_chunked_tracknet.py` 主循环 | 分块落盘 + 断点续跑 + 指纹校验 + 进度/ETA；**保持 `progress.json`、`rally-feed.json`、`chunks/*.csv` 契约不变**（UI 零改动即可切换），或改为 Qt 信号 + 同构 JSON |
| （可选）`inpaint.{h,cpp}` | `_inpaint` | 当前 app 未启用；建议二期再加，用于短事件窗口 |

**线程与取消**：推理放 `QThread`/线程池；`cancel()` 语义沿用"已完成分块保留、可续跑"（现有 `TrialAnalysisController.cpp:160-172` 的行为）。

**资源路径**：新增 `ResourceLocator`——按 `exe 同级 resources/` → `%LOCALAPPDATA%` 顺序找 `models/*.onnx`、`ffmpeg/ffmpeg.exe`；启动时自检并给出可读错误（替代 `discoverProjectRoot()`）。

### Phase 3 — 摘掉 Python 与 OpenCV

- 删除 `PYTHONPATH` / `QProcess` 启动 Python（`TrialAnalysisController.cpp:539-566`）；
- 删除对 `validation/analyze_tracknet_rallies.py` 的子进程调用（`run_chunked_tracknet.py:293-300`）；
- **保留** `validation/` 与 `tools/export_onnx.py` 作为开发期"预言机"（Oracle），用于回归对比，但不进发布包；
- 预览继续用 **Qt Multimedia**（Qt 6.11 自带 LGPL 版 FFmpeg：`avcodec-61` 等 18 MB，已在部署体积内）；**导出与解码用自己的精简 FFmpeg**。

### Phase 4 — 打包

| 方案 | 做法 | 评价 |
|---|---|---|
| **A. 7z 自解压单 exe（推荐）** | 便携目录打成 `7z` + SFX 模块 ⇒ 单个 `BadmintonAnalyzer.exe`；首启解压到 `%LOCALAPPDATA%\BadmintonAnalyzer\<version>\`，之后直接运行 | 体积最优（≈85–110 MB）、实现简单、可增量升级；代价是首启几秒解压 |
| B. Nuitka/PyInstaller onefile | — | **不采用**：我们已去掉 Python，此路无意义 |
| C. 真·单文件（exe 内嵌 DLL，运行时释放） | 自研 loader | 成本高、收益低（与 A 等价但更脆） |
| D. Inno Setup / WiX 安装包 | 同一 payload + 开始菜单/卸载项 + 可选 VC redist | 满足"<500 MB 安装包"要求；与 A 共用打包输入 |

工程要点：静态 CRT（`/MT`）或随包 VC redist（二选一，避免"缺 vcruntime140.dll"投诉）；`windeployqt --no-translations` + 只保留用到的 QML import（当前 118.7 MB 里 `Qt6Pdf`、`opengl32sw`、多余 Controls2 样式可再砍 ≈25 MB）；ffmpeg 精简构建（见下）；模型与代码版本解耦（`resources/models/models.json` 记 sha256）。

**FFmpeg 精简构建**（预计 25–40 MB，需实测）：

```
--disable-everything --disable-doc --disable-programs --enable-ffmpeg --enable-ffprobe
--enable-decoder=h264,hevc,mjpeg,png,mp3,aac --enable-parser=h264,hevc,aac
--enable-encoder=libx264,aac --enable-muxer=mp4,mov --enable-demuxer=mov,matroska
--enable-filter=scale,format,trim,atrim,setpts,asetpts,concat,fps,drawtext,aresample
--enable-hwaccel=h264_d3d11va,h264_dxva2,hevc_d3d11va,hevc_dxva2   # 可选，硬解
--enable-swscale --enable-swresample --enable-libx264
```
（`libx264` ⇒ 整个 FFmpeg 二进制为 **GPLv3**。**D1 已确认仅个人非商用发布**，但 GPL 义务与是否商用无关：仍需随包提供 GPL 全文、版权声明，以及**该二进制的完整对应源码**（或三年有效的书面提供承诺，实践上给出所用版本 + 你的 configure 命令 + 源码链接即可）。**自己的程序不受传染的前提是"不链接 libav\*"**——保持 `ffmpeg.exe` 子进程调用即为聚合分发。Qt 为 LGPLv3（动态链接 + 声明 + 允许替换 DLL + 提供/承诺 Qt 源码）、ORT 为 MIT。M4 必须产出 `THIRD-PARTY-NOTICES.md`。）

---

## 5. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| **性能回退**（失去 CUDA 后首遍变慢，基线本就只有 0.23× 实时） | 体验 | DirectML EP + fp16 + 批 8→16 调优；**首遍换轻量模型**（TrackNetV2/V4 主干，`RESULTS.md:74` 已给出同样建议）；4K 走"720p 全帧粗筛 + ROI 精算"（原方案 §③-补二）；UI 如实显示预估时间与档位选择 |
| **前处理数值不一致**（swscale vs cv2） | 轨迹漂移、切分变化 | 自写与 OpenCV `INTER_LINEAR` 等价的定点实现；用 golden `input-tensor-sample.npz` **逐元素**比对（容差 0） |
| **后处理语义不一致**（连通域/边界像素） | 坐标偏 1–2 px | 用 golden 逐帧比对；容差 >0 的帧必须逐个人工确认 |
| **fp16/int8 精度损失** | 可见帧减少、断轨变多 | fp16 为默认候选（20 MB）；int8 必须过对齐门禁（§Phase 1.2）才允许发布 |
| **DirectML 不可用**（无 DX12 显卡 / 老驱动 / 虚拟机） | 无法分析 | **决策：不做 CPU 慢速回退**（CPU 实测仅 3.81 fps ≈ 整片 12 min，体验不可接受）。启动探测 DML：不可用即**快速失败 + 明确硬件要求提示**；老驱动不覆盖（决策） |
| **长视频内存峰值** | OOM | 背景中值图在 512×288 上算（D-5）；分块解码 + 有界队列；峰值内存纳入验收（≤1.5 GB） |
| **FFmpeg 精简后缺少某编解码器** | 个别素材打不开/导出失败 | 建立"素材兼容性矩阵"测试（H.264/H.265/HEVC 10bit/VFR/竖屏/HDR）；打开失败时给出明确提示与转码建议 |
| **许可证合规** | 法务 | 建立 `THIRD-PARTY-NOTICES.md`：Qt(LGPLv3, 动态链接)、FFmpeg+x264(GPL)、ORT(MIT)、模型权重来源与许可需向 `ZSHYC/BadmintonTrackNet` 确认 |

---

## 6. 里程碑与验收

### 里程碑（单人估算）

| 阶段 | 内容 | 人日 |
|---|---|---:|
| M0 | golden 回归集 + 对齐脚本骨架 | 2 |
| M1 | ONNX 导出 + 数值对齐 + fp16 | 2–3 |
| M2a | C++ 前处理/后处理/窗口化（逐帧对齐 golden） | 3–4 |
| M2b | 状态机移植 + 分块/断点续跑 | 3–4 |
| M2c | 解码（FFmpeg）+ ORT 会话 + 线程/取消 + UI 对接 | 3–4 |
| M3 | 删除 Python 链路、资源定位改造、缺陷 D-2~D-7 | 2–3 |
| M4 | 打包（SFX 单 exe + 安装包）+ 干净机验证 | 2–3 |
| M5 | 回归与性能验收 | 2–3 |
| | **合计** | **19–26** |

### 验收标准（可执行）

1. **体积**：便携目录 ≤220 MB；单 exe ≤120 MB；安装包 ≤500 MB（`Get-ChildItem -Recurse | Measure-Object` 判定）。
2. **环境无关**：干净 Win11（无 Python、无 CUDA、无 VS）双击即用；`%LOCALAPPDATA%` 解压后能完成"导入 → 分析 → 选回合 → 导出"全流程。
3. **数值对齐**：3 段素材上 fp16 ONNX vs PyTorch 基线——可见性一致率 ≥99.5%、坐标差 P95 ≤1 px、热图 `max|Δ|` ≤5e-3。
4. **切分不劣化**：回合数一致、边界差 ≤0.2 s；`validation/RESULTS.md` 的两个已知案例（4 回合 19/4/6/16 拍；"序列 01"前 5 回合边界全对）**不得变差**。
5. **性能与硬件要求**：720p 代理首遍 **DirectML ≥ 0.8× 实时**（已实测 0.85×）；**不提供纯 CPU 回退**（决策）——启动时探测 DML，不可用则**快速失败**并明确提示"需要支持 DirectX 12 的显卡与较新驱动"；导出（10 分钟素材、720p 合集）≤ 60 s。
6. **稳定性**：连续分析 3 段素材无崩溃；峰值内存 ≤1.5 GB；中途取消后可续跑且结果与不取消一致。
7. **合规**：`THIRD-PARTY-NOTICES.md` 覆盖全部随包组件与模型权重来源。

---

## 7. 附录

### A. 现状 → 改造 映射表

| 现状 | 改造后 | 备注 |
|---|---|---|
| `run_chunked_tracknet.py`（Python 主循环） | `app/src/inference/job_runner.cpp` | 保持 JSON/CSV 契约 |
| `tracknet/inference/pipeline.py` | `preprocess.cpp` + `postprocess.cpp` + `ensemble.cpp` | 逐帧对齐 golden |
| `tracknet/models/tracknet.py` | `resources/models/*.onnx` | 模型定义仅用于导出脚本 |
| `analyze_tracknet_rallies.py` | `rally_state_machine.cpp` | 8 个阈值参数全部保留为常量表；**出画（瞬时/终局）判定是重点回归项** |
| `cv2.VideoCapture` | `video_decoder.cpp` | 硬解可选 + 软回退；**许可约束：保持 ffmpeg 子进程调用，或改用纯 LGPL 构建后再链接** |
| `torch.load` / `torch.inference_mode` | `ort_session.cpp`（ORT C++） | CPU/DirectML |
| `PYTHONPATH` + `QProcess` 启 Python | 进程内 `InferenceJob` | UI 从 stdout 解析改为信号/回调 |
| `.tools/ffmpeg/...full_build_shared` | `resources/ffmpeg/ffmpeg.exe`（精简） | 导出与解码 |
| `discoverProjectRoot()` | `ResourceLocator` | 安装后可用 |
| `validation-output/ui-trials/` | `%LOCALAPPDATA%\<App>\cache\` | D-4 |

### B. ONNX 导出脚本骨架（M1 直接用）

```python
# tools/export_onnx.py（开发期工具，依赖 torch/onnx，不进发布包）
import torch, onnx, sys
sys.path.insert(0, ".tools/vendor/BadmintonTrackNet")           # 复用未修改的模型定义
from tracknet.models.tracknet import TrackNet, InpaintNet
from tracknet.data.io import get_model

ckpt = torch.load(TRACKNET_PT, map_location="cpu", weights_only=False)
p = ckpt["param_dict"]                                          # seq_len=8, bg_mode='concat'
model = get_model("TrackNet", p["seq_len"], p.get("bg_mode", ""))  # in_dim=27, out_dim=8
model.load_state_dict(ckpt["model"]); model.eval()               # state_dict 无前缀

dummy = torch.zeros(1, (p["seq_len"] + 1) * 3, 288, 512)         # (1, 27, 288, 512)
torch.onnx.export(
    model, dummy, "tracknet-8f-concat-fp32.onnx",
    input_names=["frames"], output_names=["heatmaps"],
    opset_version=17, do_constant_folding=True,
    dynamic_axes={"frames": {0: "N"}, "heatmaps": {0: "N"}},
)
onnx.checker.check_model("tracknet-8f-concat-fp32.onnx")
# 之后：fp16 转换 + onnxruntime 与 torch 的 max|Δ| 比对（见 Phase 1.2 门禁）
```

### C. 开放问题与已决策项

> 状态：**1 已定论、2/3/4 已决策**（见文首"已确认决策"）。下面保留事实依据与剩余待办。

1. **模型权重再分发许可 → ✅ 已定论：可随包分发，需附 MIT 声明**
   - 上游 `qaz812345/TrackNetV3` 的 LICENSE（本次已取回原文并 base64 解码）是 MIT，且**授权范围显式包含预训练权重**：
     > "Permission is hereby granted … to any person obtaining a copy of this software, **pretrained model checkpoints**, and associated documentation files (the "Software"), to deal in the Software without restriction, including … the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software …"
   - 即：**20–45 MiB 的 ONNX 权重可以打进安装包**（个人非商用 GitHub 发布更是宽松），唯一义务是**保留版权与许可声明**（在仓库/安装包的 `THIRD-PARTY-NOTICES.md` 与"关于"里给出 `MIT License, Copyright (c) 2024 qaz812345`）。
   - 残余提示：权重训练自 **Shuttlecock Trajectory Dataset**（HackMD 条款未逐条核对）。作者已对 checkpoint 授予 MIT，实践上据此分发是站得住的；若将来转商用，建议再核一遍数据集条款（或改走"首启下载/自训"）。
   - ⚠️ **比模型更需注意的仍是 GPL**：随包 FFmpeg 为 GPLv3（见 Phase 4 与该行说明），**别把 libav\* 链进 exe**。
2. **InpaintNet（断线修复）→ ⛔ 本次不做（D2）**
   - 它做的是"**内部断轨**"修复：TrackNet 因遮挡/模糊/漏检输出 (0,0) 的帧，用 16 帧窗口的"归一化坐标 + 掩码"把坐标补出来；融合 `out*mask + in*(1-mask)`。
   - 实测收益（`RESULTS.md:42`）：2823 帧素材可见帧 1984 → 2196（+212 点，0.349 s）。
   - 现状：**主链路未启用**（`run_chunked_tracknet.py:401-405` 写死 `inpaintnet_file=""`）。
   - 🔴 **重要澄清**：**"球飞出画面"不是它解决的问题**。它的掩码守卫只排除"两端点位于顶部 5%"的情形，而高远球"顶部飞出→再飞回"时两端点常低于 5%，于是它**会补出一段画面外的假轨迹**，进而污染方向突变/落地判定。出画必须由状态机的顶边比例 + 长中断规则处理（见 Phase 2 的 `rally_state_machine` 行）；判死球/回合结束也**只能用原始可见性**（`RESULTS.md:43,76`）。
   - 待办（二期）：等 golden 回归暴露"因断轨导致切分错误"的案例后再接入（ONNX 仅 2.1 MiB，成本在语义对齐而非体积）。
3. **首遍模型选型 → ✅ 已决策：V3-8f + ONNX/DirectML/fp16（D3）**
   - 记录备查：V3-8f-concat = 8 帧 + 背景 = **27 通道**、11.34 M 参数；V2/V4 = 3 帧 **9 通道**、无背景图 ⇒ 每帧算力差 **3–9×**（V4 论文口径 V2 3in3out ≈163 fps vs V3 ≈15 fps），但召回从 95.4–99.3% 掉到 85–89%（**会丢拍**），且**仓库内没有 V2/V4 权重**（换模型需另找权重 + 重做验收）。故本次**不换模型**，精度由 golden 回归锁定；"换轻量主干"留作后续优化项。
4. **BounceNet → ⛔ 本次不做（D4）**
   - 作用：把规则生成的候选点三分类为 `landing / hit / none`（`tracknet/events/bouncenet.py`，`EVENT_LABELS`，`num_classes=3`），融合运动学特征（轨迹序列）+ 视觉特征（局部 patch），用于替代/增强现在纯规则的击球与落地判定。**回合终点由 landing 决定**，`RESULTS.md:55` 记录的失败案例（Rallies 3/4 因落地与随后的发球时空接近而被错误合并）正属此类。
   - 现状：代码完整，但 **`.tools/models/` 下无 BounceNet 权重**，App 与 validation 均未引用；另有零成本的规则版 `tracknet/landing/predictor.py`。
   - 要求：本次只需保证推理层"多模型 = 多 session"可扩展。
5. **Android 端**：本次 C++ 内核的接口设计是否要为 Android（NNAPI/ORT Mobile）预留（影响 `onnx_session` 的抽象层厚度）。
