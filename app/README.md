# Windows UI 骨架

这是羽毛球回合分析器的 Qt6/QML 桌面界面骨架。当前版本只用于确认布局、视觉方向和渐进式功能接入顺序，不读取视频，也不连接 TrackNet、FFmpeg 或现有 Python 验证流程。

## 当前包含

- 顶部项目名和任务状态区
- 左侧五步工作流导航
- 中央视频预览占位和可拖动时间线
- 分析进度、暂停/继续占位交互
- 右侧回合列表和拍数加减占位交互
- 适配常见 Windows 桌面尺寸的三栏布局

所有回合与进度数据目前都定义在 `qml/Main.qml` 的 `ListModel` 中。组件通过属性和信号读写这些假数据，后续可以用 C++ `QAbstractListModel` 和任务服务替换，而无需重做页面布局。

## 构建要求

- CMake 3.21 或更高版本
- Qt 6.5 或更高版本，包含 `Quick` 和 `QuickControls2`
- 支持 C++20 的编译器（Windows 推荐 Visual Studio 2022）

使用 Qt 安装目录下的命令行环境运行：

```powershell
cmake -S app -B build/app -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
cmake --build build/app --config Release
./build/app/Release/badminton-analyzer.exe
```

Qt 版本和目录需要按本机实际安装位置调整。使用 Qt Creator 时，可以直接打开 `app/CMakeLists.txt` 并选择 Qt 6.5+ Desktop Kit。

## 后续模块接入顺序

1. 视频导入：文件选择、元数据探测和项目创建。
2. 前几回合切分试验：运行有限时段，逐回合显示预览，并保留导出入口。
3. 回合显示：用真实回合索引替换假数据模型，按时间索引播放原视频。
4. 拍数显示：接入自动拍数，并保存人工修正值与复核状态。
5. 回合微调：拖动起止边界、合并、拆分和排序；操作只修改索引数据。

播放器、推理任务和导出服务应分别通过窄接口接入 QML。正式视频预览优先使用原视频或单一代理视频的时间索引，独立回合文件只在用户明确导出时生成。

## 本机验证状态

创建骨架时，本机命令行未发现 CMake、Qt6、Ninja 或 MSVC 编译器，也未在常见 Qt 安装目录找到 Qt，因此当前无法执行 configure、build 或 GUI smoke run。项目文件已保持标准 Qt6 CMake 结构；安装 Qt 6.5+ Desktop Kit 与 CMake 后，应首先执行上面的 configure/build 命令。
