# M1 对齐报告：TrackNet PyTorch → ONNX Runtime

> 分支：`deepseekflash`　日期：2026-09-15
> 对应方案：`onnx-refactor-plan.md` Phase 0 / Phase 1
> 结论：**通过。** 全片段 2823 帧上，ONNX 与 PyTorch 的**可见性判定零差异、回合切分完全一致**，坐标 99.5–99.8% 位级相同（其余为 1 像素的外接框抖动，P95 = 0 px）。DirectML 吞吐达到 PyTorch CUDA 的 **91%**，且不需要 CUDA 运行时。

---

## 1. 判定口径

对齐门禁按**决策级**而非"单像素极值"设定。原因：ONNX Runtime 与 PyTorch 的卷积实现/求和顺序不同，热图上必然有个别像素差到 1e-3 量级；但产品关心的是**解码后的坐标与可见性**是否变化。故：

| 指标 | 门禁 | 依据 |
|---|---|---|
| 可见性不一致率 | ≤ 0.5%（理想 0） | 直接决定"球在不在"→ 影响断轨与回合边界 |
| 解码坐标 P95 | ≤ 1.5 px | 代理分辨率 1280×720 上的像素误差 |
| 热图 p99.9 绝对差 | fp32 ≤ 1e-3 / fp16 ≤ 5e-3 | 稳健的数值对齐指标 |
| 热图最大绝对差 | fp32 ≤ 1e-2 / fp16 ≤ 2e-2 | 仅作异常报警，不作硬门禁 |
| 回合切分 | 回合数一致 + 边界差 ≤ 0.2 s（0.2×50fps = 10 帧） | 交付关键指标（`RESULTS.md:67-69`） |

## 2. 环境与素材

| 项 | 值 |
|---|---|
| 素材 | `validation-output/ui-trials/VID20260828212318_1-c1fe12c0ab/proxy-720p.mp4`（app 自己生成的 720p 代理，1280×720@50fps，2823 帧 / 56.46 s） |
| 源素材 | `F:\羽毛球视频\20260828\VID20260828212318_1.mp4`（4K50；`RESULTS.md` 的已知回归案例：人工复核 4 回合 19/4/6/19 拍，自动 3 回合） |
| 模型 | `.tools/models/TrackNetV3/ckpts/TrackNet_best.pt` sha256 `df867641…e83b1a`；`seq_len=8`、`bg_mode=concat` |
| GPU | NVIDIA RTX 5060 8 GB，驱动 576.52（DirectML 走 D3D12，不经 CUDA） |
| CPU | Intel i5-13400F（10 核 / 16 线程） |
| 运行时 | onnxruntime-directml 1.24.4（含 CPU EP）、torch 2.8.0+cu128、Python 3.11.15 |
| 推理配置 | `nonoverlap`、chunk 6 s、overlap 0.5 s、batch 8（与 app 完全一致） |

## 3. 产物

导出工具：`tools/export_onnx.py`（dev-only，不进发布包）。产物位于 `validation-output/golden/models/`（gitignored）：

| 文件 | 大小 | sha256（前 16 位） | 说明 |
|---|---:|---|---|
| `tracknet-8f-concat-288x512-fp32.onnx` | 43.3 MiB | `d681ab3c94deeca4` | 基准 fp32（Resize 用 float scales） |
| `tracknet-8f-concat-288x512-sizes-fp32.onnx` | 43.3 MiB | `e980d5203c6a454d` | Resize 改用 int64 sizes（为 fp16 铺路） |
| **`tracknet-8f-concat-288x512-sizes-fp16.onnx`** | **21.6 MiB** | `5f335ab412385315` | **✅ 采纳**（fp16 计算 / fp32 输入输出） |

模型规格：输入 `(N,27,288,512)` fp32（8 帧 RGB + 1 张背景帧，背景在通道 0），输出 `(N,8,288,512)` sigmoid 热图，动态 batch，opset 17，11,341,000 参数。

## 4. 窗口级对齐（冻结的 32 个输入窗口 / 256 帧）

| 运行 | 精度 | 热图 max\|Δ\| | p99.9 | >0.01 占比 | 可见性不一致 | 坐标 P50/P95/max | CPU fps | 判定 |
|---|---|---:|---:|---:|---|---|---:|---|
| 主检查 | fp32 | 4.41e-3 | 2.46e-4 | 0% | **0 / 256** | 0 / 0 / 0 px | 0.59 | **PASS** |
| 诊断：关闭图优化 | fp32 | 4.41e-3 | 2.46e-4 | 0% | 0 / 256 | 0 / 0 / 0 px | 0.31 | 同值 |
| 诊断：int64 sizes 图 | fp32 | 4.41e-3 | 2.46e-4 | 0% | 0 / 256 | 0 / 0 / 0 px | 0.58 | 同值 |
| 主检查 | fp16 | 4.72e-3 | 2.89e-4 | 0% | **0 / 256** | 0 / 0 / 0 px | 0.58 | **PASS** |

**三条诊断结论**：
1. **差异不是 ORT 图优化造成的**：`ORT_DISABLE_ALL` 与 `ORT_ENABLE_ALL` 的数字**完全相同**，说明 4.4e-3 来自"导出图 vs 原生 PyTorch"的卷积实现差异，与运行时优化无关 —— 因此不必为了数值一致去关优化（关了反而慢一倍）。
2. **fp16 的额外损失可忽略**：max\|Δ\| 从 4.41e-3 只涨到 4.72e-3（+7%），可见性与坐标完全不变。
3. **解码是稳定的**：热图上 4e-3 量级的扰动没有改变任何一帧的连通域外接框 → 坐标 P50/P95/max 全为 0 px。

## 5. 管线级对齐（全片段 2823 帧，DirectML）

复用了项目自身的 runner 原语（读帧、背景中值、窗口化、ensemble、热图解码、分块网格），**只把模型调用换成 ORT 会话**，因此差异只可能来自模型本身。

| 模型 | 吞吐 | 实时倍率 | 可见性不一致 | 坐标位级一致 | P95 / max | 回合切分（参考 vs ONNX） | 判定 |
|---|---:|---:|---|---|---|---|---|
| fp32 | 38.79 fps | 0.776× | **0 / 2823** | 99.80% | 0.0 / 1.25 px | 3 vs 3，边界差 ≤10 帧 | **PASS** |
| **fp16（采纳）** | **42.43 fps** | **0.849×** | **0 / 2823** | 99.50% | 0.0 / 1.77 px | 3 vs 3，边界差 ≤10 帧 | **PASS** |

参考基线 = **app 自己跑出来的**那份结果（`app-run/trajectory-partial.csv`、3 个回合：`[24,1163] [1264,1543] [1566,2822]`，即 `RESULTS.md` 记录的"3 回合"回归案例）。ONNX 复现出**完全相同的 3 个回合与边界**。

> 那 0.2–0.5% 的"非位级一致"帧是外接框边缘抖动 1 像素（max 1.25–1.77 px），P95 仍为 0 —— 对下游状态机无影响（已验证：回合边界不变）。

## 6. 性能（关键决策数据）

| 路径 | 吞吐（720p 代理） | 相对 CUDA | 备注 |
|---|---:|---:|---|
| PyTorch + CUDA（现状，本次实测） | 46.5 fps | 100% | 1200 帧采集时测得 |
| PyTorch + CUDA（app 自记） | 51.7 fps | 111% | `ui-trials/.../progress.json` 的 `inferenceFramesPerSecondThisRun` |
| **ONNX + DirectML（fp16）** | **42.4 fps** | **91%（对本次实测）/ 82%（对 app 自记）** | **无 CUDA 运行时、无 Python** |
| ONNX + DirectML（fp32） | 38.8 fps | 83% / 75% | |
| ONNX + CPU（fp16，16 线程，端到端） | 3.81 fps | 8% | 整片 2823 帧 ≈ **12.3 分钟** |
| ONNX + CPU（fp16，8 线程，窗口级小 batch） | 0.58 fps | 1.2% | 同一 EP 下线程数与 batch 影响极大，勿用该数做估算 |

**结论**：DirectML 路径在 4K50 素材的 720p 代理上达到 **0.85× 实时**，与 CUDA 基线同一量级 —— "只换运行时"（决策 D3）在性能上成立。纯 CPU 回退约 **12 分钟/整片**（本机 i5-13400F），可用但慢：UI 必须如实显示预估时间并允许"只分析前 N 个回合"。

## 7. 本次踩到并解决的三个实现坑（都已固化进工具）

1. **fp16 转换会被 `Resize` 卡死**（`nn.Upsample(scale_factor=2)` → ONNX `Resize` 的 `scales` 必须是 fp32，转换器却把它变成 fp16，模型直接非法；`op_block_list=["Resize"]` 也无效）。解决办法：导出时把三次上采样表达为 `F.interpolate(size=<skip 形状>)`，图上变成 **int64 `sizes`** 输入，fp16 转换即可通过。该包装**仅在导出时使用**，且已断言与原生模型**位级一致（max\|Δ\| = 0.000e+00）**。
2. **CPU EP 在大 batch 下会申请几十 GB 的 im2col 缓冲**（batch 32 时申请 32.6 GB 直接 OOM）。→ 对齐与产品路径都按 **batch ≤ 8** 运行。
3. **onnxruntime-directml 与 onnxruntime（CPU 版）互斥**（同一包名）。已统一采用 `onnxruntime-directml`（23.9 MB，内含 `DmlExecutionProvider` + `CPUExecutionProvider`），发布包只需带这一个运行时。另注意：CPU EP 的吞吐对**线程数**极敏感（8 线程 0.58 fps → 16 线程 3.81 fps），产品侧应默认用满物理核。

## 8. 与验收标准的对照

| 验收项（方案 §6） | 目标 | 实测 | 状态 |
|---|---|---|---|
| 可见性一致率 | ≥ 99.5% | **100%（0/2823）** | ✅ |
| 坐标差 P95 | ≤ 1 px | **0.0 px** | ✅ |
| 热图 max\|Δ\|（fp16） | ≤ 5e-3 | 4.72e-3 | ✅ |
| 回合数一致 | 一致 | 3 vs 3 | ✅ |
| 回合边界差 | ≤ 0.2 s | ≤ 10 帧（=0.2 s） | ✅ |
| 已知回归案例不变差 | 不劣化 | 与 app 基线逐帧一致 | ✅ |
| 模型体积 | fp16 ≈ 23 MiB | 21.6 MiB | ✅ |
| 运行时体积 | CPU 15 / DML 25 MB | 23.9 MB（DML 包，含双 EP） | ✅ |

## 9. 未做 / 下一步

- **M2（C++ 推理内核）尚未开始**：前处理（`cv2.resize` 等价实现）、后处理（连通域取最大外框中心）、窗口化/ensemble、状态机移植、FFmpeg 解码替代 cv2、分块断点续跑。
- **InpaintNet 未导出**（决策 D2 本次不做）；如需二期，`tracknet/models/tracknet.py:100` 的 Conv1d 网络同样可导出（输入坐标+掩码）。
- **DirectML 兼容性只在本机 RTX 5060 验证过**：需补测 AMD/Intel 核显与老驱动，并确保 EP 探测失败时静默回退 CPU。
- **长片段内存峰值未测**：本次 2823 帧正常，需补 10 分钟以上素材的内存曲线（方案验收要求 ≤1.5 GB）。
- **golden 数据未入库**（`validation-output/` 被 gitignore）：约 205 MB，重跑命令见 `tools/capture_golden.py` 的 `--help`，`meta.json` 记录了全部哈希与参数。

## 10. M2 第一步：C++ + ONNX Runtime(DirectML) 冒烟测试（已完成）

**目的**：在写 C++ 移植之前，先证明"C++ 能加载模型、能在 DirectML 上跑、数值与前缀一致"，把最大的技术未知项前置掉。

**构建方式**（`tools/ort_cpp_smoke/`，dev-only）：
- C++ SDK 取自 NuGet（不用源码编译）：`Microsoft.ML.OnnxRuntime.DirectML 1.24.4`（**与 Python 侧 wheel 同版本**）+ `Microsoft.AI.DirectML 1.15.4`
- CMake + VS 2022 生成器链接 `onnxruntime.lib`，POST_BUILD 自动把 DLL 拷到 exe 旁
- 数据取自 PyTorch 金标准（`tools/dump_golden_bin.py` 把 `.npz` 转成裸 float32 二进制，C++ 无需引入 npy 解析）

**结果**（32 个窗口 = 3774 万个热图像素，batch 8，已排除首次 kernel 编译的预热）：

| 配置 | max\|Δ\| | >0.01 个素 | >0.05 | 仅模型吞吐 | 实时倍率 |
|---|---:|---:|---:|---:|---:|
| **fp16 + DirectML（采纳）** | 5.47e-3 | **0** | 0 | **84.75 fps** | **1.69×** |
| fp32 + DirectML | 4.40e-3 | 0 | 0 | 51.67 fps | 1.03× |
| fp16 + CPU（默认 EP） | 4.72e-3 | 0 | 0 | 0.59 fps | 0.012× |

三条结论：
1. **C++ 侧数值与 Python 侧同一量级**（5.47e-3 vs 4.72e-3，均为 fp16 转换的固有差异），且 3774 万个像素里**没有一个**超过 0.01 → 图是完全一致的。
2. **C++ 仅模型 84.75 fps ≈ Python 端到端 42.4 fps 的 2 倍** → Python 链路约有一半时间在 cv2 解码 / numpy 前处理 / ensemble 记账 / 分块 I/O 上。**这是 M2 移植可以拿回来的性能余量，也说明 C++ 前处理必须做快，否则会把这部分余量还回去。**
3. ⚠️ **不要把 1.69× 当作产品承诺**：它是"仅模型"的上限；端到端（含解码+前处理+后处理+状态机）应以 Python 端到端的 0.85× 为基准，M2 完成后再实测。

**运行时落盘体积（修正预算表）**：`onnxruntime.dll` 16.5 MB + `DirectML.dll` 17.7 MB + `onnxruntime_providers_shared.dll` 0.02 MB = **≈34.2 MB**（Python wheel 的 23.9 MB 是压缩后体积）。方案体积预算已据此更新，仍在便携目录 220 MB 目标内。

**顺带确认的集成细节**：
- DirectML 版 ORT **不接受**按名注册 `"CPU"`（该构建只注册 DML 等 EP，CPU 是隐式兜底）→ C++ 侧 `provider == "cpu"` 时不调用 `AppendExecutionProvider`。
- DML 首次运行有 kernel 编译开销（8 窗口首跑 0.537 s → 稳态 0.09 s）→ 产品侧应在开始分析前做一次**预热推理**，避免第一块进度条忽慢。
- ORT 会警告"部分节点未分配到首选 EP（shape 相关算子留在 CPU）"，属正常行为，不影响结果。

## 11. 复现命令（M2 第一步）

```powershell
# 1) 金标准转裸二进制（C++ 可读）
& $py "$root\tools\dump_golden_bin.py" `
  --golden-dir "$root\validation-output\golden\VID20260828212318_1" `
  --out-dir "$root\validation-output\golden\bin" --limit 32

# 2) 配置 + 编译（ORT_ROOT / DML_ROOT 指向解压后的 NuGet 包）
cmake -S "$root\tools\ort_cpp_smoke" -B "$root\build\ort_cpp_smoke" -G "Visual Studio 17 2022" -A x64 `
  -DORT_ROOT="$root/.tools/onnxruntime-cpp/ort-dml" `
  -DDML_ROOT="$root/.tools/onnxruntime-cpp/directml"
cmake --build "$root\build\ort_cpp_smoke" --config Release

# 3) 运行
& "$root\build\ort_cpp_smoke\Release\ort_cpp_smoke.exe" `
  "$root\validation-output\golden\models\tracknet-8f-concat-288x512-sizes-fp16.onnx" `
  "$root\validation-output\golden\bin" dml 8
```

## 13. M2 步骤 1/2/3/6：C++ 内核移植与 golden 对齐（已完成）

C++ 内核落在 `app/src/inference/`（产品代码），由 `tools/parity_harness/` 编译并验证。**注意：harness 编译的是产品源码本身**，不是复制品。

| 测试 | 内容 | 结果 |
|---|---|---|
| T1 前处理 | fixture 原始帧 + 中值图 → `BuildModelInput` vs PyTorch 张量 | max **1.0000 LSB**，超 1 LSB 的元素 **0 / 3185 万** → **PASS** |
| T2 后处理 | golden 热图 → `DecodeHeatmap` vs Python 解码坐标 | **0 / 64 不一致**（完全精确）→ **PASS** |
| T3 端到端 | 原始帧 → 前处理 → ONNX(DirectML) → 解码 vs Python 解码坐标 | 可见性 **0 / 64**、坐标 P50/P95 **0.0 px**、max 1.25 px → **PASS** |
| T4 回合判定 | golden 2823 帧轨迹 → C++ 状态机 vs Python `rallies-provisional.csv` | **3 vs 3，边界/拍数/观测/推断逐项一致** → **PASS** |

```
rally 1: [24..1163]    hits=17 (obs=13 inf=4)  == golden
rally 2: [1264..1543]  hits=3  (obs=3  inf=0)  == golden
rally 3: [1566..2822]  hits=23 (obs=20 inf=3)  == golden
```

**T4 的意义**：回合边界是交付关键指标，且**出画判定（顶边 0.22 比例 + 3.0 s 长中断 + 慢速重启）就在这一层**。逐项一致说明移植没有改变产品行为。

### 13.1 一个必须记录的偏差：resize 的 ≤1 LSB 差异

**现象**：C++ 前处理与 PyTorch 张量在 5.5% 的像素上有 ±1 LSB（1/255）差异，最大恰好 1 LSB。

**根因（已定位到确定结论）**：
- 我的实现与 **`cv2.resize(..., INTER_LINEAR)` 的浮点路径 max|d| = 0.0（完全一致）**；
- 差异来自 OpenCV **8 位定点路径自身的量化**：cv2 的 8 位路径与其自家浮点路径也差 34521 个像素（max 0.625），且 `INTER_LINEAR` 与其 `INTER_LINEAR_EXACT` 之间也差 9.36%（max 1）；
- 经典"两趟 11 位定点 + 中间取整"反而更差（31% 不匹配）；我扫了 8 位/10/11/12/14 位、两种权重约定、单/双趟、22 位单表达式共 60+ 种组合，**没有一种能位级命中** cv2 5.0 的 8 位路径。

**决策**：不再追求位级一致，改为以**产品级门禁**验收（可见性差异 ≤0.5%、坐标 P95 ≤1.5 px、回合边界一致），并要求"差异不超过 1 LSB"。依据：位级一致是手段不是目的；用户要的是"功能不变"，而 T3/T4 已证明轨迹与回合切分不变。该偏差已量化为**输入 5.5% 像素 ±1/255**。

**风险与边界**：若将来换素材后 T3/T4 出现可见性差异，第一嫌疑就是这里；届时的补救选项是改用 `INTER_LINEAR_EXACT` 语义或直接在 C++ 内嵌 OpenCV 的 resize。

### 13.2 新增的开发者工具

| 工具 | 用途 |
|---|---|
| `tools/dump_parity_fixtures.py` | 把 golden 转成 C++ 可读的裸二进制（原始帧/中值图/预处理张量/热图/Python 解码坐标） |
| `tools/dump_resize_probe.py` | 生成 resize 校准用例（随机/渐变/真实 720p） |
| `tools/resize_probe/` | C++ 多变体对比，用于锁定 OpenCV resize 语义 |
| `tools/resize_sweep.py` / `tools/resize_calibrate.py` | numpy 侧快速扫描候选公式 |
| `tools/parity_harness/` | T1–T4 对齐测试，编译产品源码 |

## 14. M2 步骤 4/5：FFmpeg 解码与任务编排（C++ 全链路已跑通）

新增产品源码：`app/src/inference/video_decoder.{h,cpp}`（ffprobe 元数据 + ffmpeg 子进程解码）、`job_runner.{h,cpp}`（中值图 + 分块推理 + 轨迹组装 + 断点续跑 + 进度回调）。验证工具：`tools/pipeline_e2e/`。

### 14.1 解码保真度：与 cv2 逐字节一致 ✅

| 检查 | 结果 |
|---|---|
| 41 帧采样中值图 vs Python `background-median.npy` | **differing = 0 / 2,764,800，max = 0 → 完全一致** |

说明 `ffmpeg.exe -ss <t> -i proxy -frames:v N -f rawvideo -pix_fmt bgr24 -` 的解码结果与 `cv2.VideoCapture` 的读帧结果**像素级相同**，因此解码路径没有引入任何额外偏差（此前测得的轨迹差异只来自 resize 的 ≤1 LSB）。

### 14.2 全链路（C++ vs Python golden，2823 帧 720p 代理）

```
video: 1280x720 fps=50.000 frames=2823  inference=81.6s  wall=88.5s (31.89 fps)
MEDIAN:     differing=0/2764800 max=0                    -> PASS（与 cv2 一致）
TRAJECTORY: golden=2823 mine=2823 visibilityMismatch=1 (0.035%)
            jointVisible=1994 P50=0.000 P95=0.000 max=5.000 -> PASS
RALLIES:    mine=3 golden=3，边界与拍数逐项一致          -> PASS
```

- **可见性差异 0.035%**（门禁 0.5%）、**坐标 P95 = 0.0 px**（门禁 1.5 px）
- 唯一差异帧的 5.0 px 来自画幅边缘的外接框差 2 像素（resize 的 ≤1 LSB 传播）
- 端到端吞吐 **31.9 fps（0.64× 实时）**，比 Python 端到端 42.4 fps 低，差额是**每分块一次 ffmpeg 子进程解码（同步等待）**与 41 次中值采样进程启动；后续可用"一次流式解码 + 子块处理"消除（已列入步骤 9）

### 14.3 两个必须记录的实现修正

1. **`PredictChunk` 原先按帧累加热图**（照搬 Python 的 `totals[frame_id]` 累加），非重叠模式下每帧只属于一个窗口，325 帧分块会因此多占 **≈1.5 GB**（325 × 4.7 MB），并直接导致进程访问违例崩溃。已改为**直通解码**（预测完立即解码），内存从 ~2.6 GB 降到 ~1.1 GB。输出不变（T3/T4 与 14.2 全部复验通过）。
2. **解码读取改为流式**：原先"先 reserve 再 assign"会同时持有两份帧缓冲（一次分块 898 MB → 峰值 1.8 GB），改为直接写入目标缓冲区。

### 14.4 断点续跑已验证

`chunks/chunk-NNNNN.csv` 存在时直接读取复用（新增 `ReadChunkCsv`）：第二次运行 **7 秒**（复用 10 个分块）得到与首次 89 秒完整运行**完全相同**的轨迹与回合结果。

### 14.5 复现命令

```powershell
cmake -S "$root\tools\pipeline_e2e" -B "$root\build\pipeline_e2e" -G "Visual Studio 17 2022" -A x64 `
  -DORT_ROOT="$root/.tools/onnxruntime-cpp/ort-dml" -DDML_ROOT="$root/.tools/onnxruntime-cpp/directml"
cmake --build "$root\build\pipeline_e2e" --config Release

& "$root\build\pipeline_e2e\Release\pipeline_e2e.exe" `
  "$root\.tools\ffmpeg\ffmpeg-9.0.1-full_build-shared\bin\ffmpeg.exe" `
  "$root\validation-output\golden\models\tracknet-8f-concat-288x512-sizes-fp16.onnx" `
  "$root\validation-output\ui-trials\VID20260828212318_1-c1fe12c0ab\proxy-720p.mp4" `
  "$root\validation-output\golden\e2e-run" `
  "$root\validation-output\golden\VID20260828212318_1" `
  "$root\validation-output\golden\fixtures"
```


### 13.3 复现命令（T1–T4 对齐工具）

```powershell
& $py "$root\tools\dump_parity_fixtures.py" `
  --golden-dir "$root\validation-output\golden\VID20260828212318_1" `
  --out-dir "$root\validation-output\golden\fixtures" --windows 8

cmake -S "$root\tools\parity_harness" -B "$root\build\parity_harness" -G "Visual Studio 17 2022" -A x64 `
  -DORT_ROOT="$root/.tools/onnxruntime-cpp/ort-dml" -DDML_ROOT="$root/.tools/onnxruntime-cpp/directml"
cmake --build "$root\build\parity_harness" --config Release

& "$root\build\parity_harness\Release\parity_harness.exe" `
  "$root\validation-output\golden\fixtures" `
  "$root\validation-output\golden\models\tracknet-8f-concat-288x512-sizes-fp16.onnx" `
  dml 8 1280 720 8 `
  "$root\validation-output\golden\VID20260828212318_1\app-run\trajectory-partial.csv" `
  "$root\validation-output\golden\VID20260828212318_1\app-run\analysis\rallies-provisional.csv" 50
```


## 15. 复现命令（M0 / M1 主链路）

```powershell
$py = 'D:\Desktop\badminton-win\.venv-validation\Scripts\python.exe'
$root = 'D:\Desktop\badminton-win'

# 1) 冻结 PyTorch 金标准（1200 帧 + 32 个输入窗口 + fp32 热图）
& $py "$root\tools\capture_golden.py" `
  --video "$root\validation-output\ui-trials\VID20260828212318_1-c1fe12c0ab\proxy-720p.mp4" `
  --tracknet-file "$root\.tools\models\TrackNetV3\ckpts\TrackNet_best.pt" `
  --output-dir "$root\validation-output\golden\VID20260828212318_1" `
  --max-frames 1200 --keep-windows 24 --window-stride 6 --heatmap-dtype float32

# 2) 导出 ONNX（fp32 + fp16，含窗口级对齐门禁）
& $py "$root\tools\export_onnx.py" `
  --tracknet-file "$root\.tools\models\TrackNetV3\ckpts\TrackNet_best.pt" `
  --output-dir "$root\validation-output\golden\models" `
  --golden-dir "$root\validation-output\golden\VID20260828212318_1"

# 3) 全片段管线级对齐 + 回合切分对比（DirectML）
& $py "$root\tools\compare_onnx_pipeline.py" `
  --video "$root\validation-output\ui-trials\VID20260828212318_1-c1fe12c0ab\proxy-720p.mp4" `
  --tracknet-file "$root\.tools\models\TrackNetV3\ckpts\TrackNet_best.pt" `
  --onnx "$root\validation-output\golden\models\tracknet-8f-concat-288x512-sizes-fp16.onnx" `
  --output-dir "$root\validation-output\golden\pipeline-full" `
  --provider dml --label full-dml-sizes-fp16
```
