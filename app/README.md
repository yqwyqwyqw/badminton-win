# Windows 客户端

这是羽毛球回合分析器的 Qt6/QML Windows 客户端。当前已接入视频导入和“前几回合切分试验”两项真实功能；完整回合管理与微调仍为界面占位。

## 当前包含

- 选择或拖入本地视频，并校验文件和格式
- 使用 Qt Multimedia 读取文件大小、时长、分辨率、帧率和编码信息
- 显示原始日期及来源，按“文件名明确时间 → 视频内嵌日期 → Windows 文件时间”回退
- 在导入页直接播放原视频，支持暂停、拖动进度和静音切换
- 生成并缓存时间轴一致的 720p 代理视频，硬件缩放失败时自动使用兼容模式
- 从界面启动可恢复的 TrackNet 分块推理，并显示真实进度和预计剩余时间
- 每识别完一个完整回合就立即加入列表，支持短片预览和单独导出
- 按源文件路径、大小和修改时间隔离缓存，切换素材不会串用结果
- 顶部显示真实素材名称和导入状态
- 支持清空当前项目，以及从导入页进入切分试验页
- 左侧五步工作流导航
- 中央视频预览占位和可拖动时间线
- 分析进度、暂停/继续占位交互
- 右侧回合列表和拍数加减占位交互
- 适配常见 Windows 桌面尺寸的三栏布局

视频导入由 C++ `VideoImportController` 负责；代理生成、Python/TrackNet 子进程、进度文件和增量回合列表由 C++ `TrialAnalysisController` 管理。后续页面使用的演示回合数据仍定义在 `qml/Main.qml` 中。

## 构建要求

- CMake 3.21 或更高版本
- Qt 6.5 或更高版本，包含 `Multimedia`、`Quick` 和 `QuickControls2`
- 支持 C++20 的编译器（Windows 推荐 Visual Studio 2022）

使用 Qt 安装目录下的命令行环境运行：

```powershell
cmake -S app -B build/app -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="D:/Qt/6.11.2/msvc2022_64"
cmake --build build/app --config Release
$env:Path = "D:/Qt/6.11.2/msvc2022_64/bin;$env:Path"
./build/app/Release/badminton-analyzer.exe
```

也可以通过命令行直接导入一个视频：

```powershell
./build/app/Release/badminton-analyzer.exe "F:/path/to/video.mp4"
```

Qt 版本和目录需要按本机实际安装位置调整。使用 Qt Creator 时，可以直接打开 `app/CMakeLists.txt` 并选择 Qt 6.5+ Desktop Kit。

## 后续模块接入顺序

1. 回合显示：用真实回合索引替换假数据模型，按时间索引播放原视频。
2. 拍数显示：接入自动拍数，并保存人工修正值与复核状态。
3. 回合微调：拖动起止边界、合并、拆分和排序；操作只修改索引数据。

播放器、推理任务和导出服务应分别通过窄接口接入 QML。正式视频预览优先使用原视频或单一代理视频的时间索引，独立回合文件只在用户明确导出时生成。

## 本机验证状态

已使用 Qt 6.11.2、CMake 4.4.3 和 Visual Studio 2022 x64 工具链完成 Release 配置、编译及 GUI 启动验证。4K50 MP4 的真实导入、原片播放、720p 代理生成、TrackNet 增量回合输出、短片预览、停止恢复数据保留和单回合导出均已跑通。当前可执行文件位于 `build/app/Release/badminton-analyzer.exe`。尚未执行 Qt 运行库部署，因此从普通终端运行时需要按上例将 Qt 的 `bin` 目录加入当前进程的 `PATH`。
