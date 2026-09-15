# 打包 Windows 便携版（免安装目录 + zip）。
#
# 与旧版本的区别（旧脚本产出的包是跑不起来的）：
#   * 补上 ONNX Runtime / DirectML 运行库
#   * 补上 resources/ffmpeg 与 resources/models（应用按 <exe>/resources 解析）
#   * --no-opengl-sw 去掉 19.7 MB 的软件 OpenGL 回退库
#   * 剔除 Qt6Pdf、translations、ffplay.exe 与开发工具
#   * 打包后做隔离环境验证（最小 PATH，冷跑一次真实分析）

[CmdletBinding()]
param(
    [string]$Version = "0.1.0",
    [switch]$SkipBuild,
    [switch]$SkipZip,
    [switch]$SkipVerify,
    [string]$VerifyClip = "F:\羽毛球视频\20260828\VID20260828212318_1.mp4"
)

# 原生程序（windeployqt / ISCC / ffmpeg）往 stderr 写警告时不应中断脚本；真正的失败看 $LASTEXITCODE
$ErrorActionPreference = "Continue"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDir    = Join-Path $projectRoot "build\app"
$releaseRoot = Join-Path $projectRoot "build\release"
$packageName = "badminton-analyzer-v$Version-windows"
$packageRoot = Join-Path $releaseRoot $packageName
$zipPath     = Join-Path $releaseRoot "$packageName.zip"
$qtBin       = "D:\Qt\6.11.2\msvc2022_64\bin"
$ffmpegSrc   = Join-Path $projectRoot ".tools\ffmpeg\ffmpeg-9.0.1-full_build-shared\bin"
$modelSrc    = Join-Path $projectRoot "validation-output\golden\models\tracknet-8f-concat-288x512-sizes-fp16.onnx"
$cmake       = (Get-Command cmake).Source

function Get-FolderSizeMB([string]$path) {
    if (-not (Test-Path $path)) { return 0 }
    $item = Get-Item -LiteralPath $path
    if (-not $item.PSIsContainer) { return [math]::Round($item.Length / 1MB, 1) }
    $sum = (Get-ChildItem -LiteralPath $path -Recurse -File | Measure-Object Length -Sum).Sum
    return [math]::Round($sum / 1MB, 1)
}

Write-Host "== 1/6 编译 Release ==" -ForegroundColor Cyan
if (-not $SkipBuild) {
    & $cmake --build $buildDir --config Release -- /m
    if ($LASTEXITCODE -ne 0) { throw "编译失败：$LASTEXITCODE" }
} else {
    Write-Host "   (跳过)"
}

$exe = Join-Path $buildDir "Release\badminton-analyzer.exe"
if (-not (Test-Path $exe)) { throw "找不到 $exe" }
foreach ($required in @($ffmpegSrc, $modelSrc)) {
    if (-not (Test-Path $required)) { throw "缺少打包素材：$required" }
}

Write-Host "== 2/6 准备目录 ==" -ForegroundColor Cyan
if (Test-Path $releaseRoot) { Remove-Item -LiteralPath $releaseRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null
Copy-Item -LiteralPath $exe -Destination $packageRoot

Write-Host "== 3/6 windeployqt（Qt 运行库与 QML 模块）==" -ForegroundColor Cyan
$env:Path = "$qtBin;$env:Path"
& (Join-Path $qtBin "windeployqt.exe") --release --no-translations --no-opengl-sw `
    --qmldir (Join-Path $projectRoot "app\qml") (Join-Path $packageRoot "badminton-analyzer.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt 失败：$LASTEXITCODE" }

Write-Host "== 4/6 补运行库与资源 ==" -ForegroundColor Cyan
$releaseOut = Join-Path $buildDir "Release"
foreach ($dll in @("onnxruntime.dll", "onnxruntime_providers_shared.dll", "DirectML.dll")) {
    Copy-Item -LiteralPath (Join-Path $releaseOut $dll) -Destination $packageRoot
}
$ffmpegDst = Join-Path $packageRoot "resources\ffmpeg"
$modelDst  = Join-Path $packageRoot "resources\models"
New-Item -ItemType Directory -Force -Path $ffmpegDst, $modelDst | Out-Null
Get-ChildItem -LiteralPath $ffmpegSrc -File |
    Where-Object { $_.Name -notin @("ffplay.exe") } |
    Copy-Item -Destination $ffmpegDst
Copy-Item -LiteralPath $modelSrc -Destination $modelDst
Copy-Item -LiteralPath (Join-Path $projectRoot "README.md") -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot "THIRD-PARTY-NOTICES.md") -Destination $packageRoot

$readmeLines = @(
    "羽毛球回合分析器 · Windows 便携版 v$Version",
    "================================================",
    "",
    "1. 免安装：解压后直接双击 badminton-analyzer.exe 运行。",
    "2. 运行要求：Windows 10/11 64 位，支持 DirectX 12 的显卡（轨迹推理用 DirectML 跑在 GPU 上）。",
    "   首次分析会在显存里建会话，GPU 占用高属正常。",
    "3. 缓存位置：%LOCALAPPDATA%\BadmintonAnalyzer\羽毛球回合分析器\cache\<视频名-hash>\",
    "   删除整个 cache 目录即可强制重新分析；不同视频各自隔离。",
    "4. 全程本地运行，不联网、不上传任何数据。",
    "5. 第三方组件与许可：见 THIRD-PARTY-NOTICES.md（FFmpeg 为 GPLv3，独立进程调用）。"
)
Set-Content -Path (Join-Path $packageRoot "运行说明.txt") -Value $readmeLines -Encoding UTF8

Write-Host "== 5/6 精简 ==" -ForegroundColor Cyan
# 目标环境为 Windows 10/11 x64 + DX12 显卡，且 main.cpp 固定 QQuickStyle::setStyle("Basic")，
# 因此可以安全剔除未使用的 Qt 模块（仅保留 Basic 样式）。
$drop = @(
    (Join-Path $packageRoot "Qt6Pdf.dll"),
    (Join-Path $packageRoot "translations"),
    (Join-Path $packageRoot "opengl32sw.dll"),
    (Join-Path $packageRoot "ffplay.exe"),
    (Join-Path $packageRoot "resources\ffmpeg\ffplay.exe"),
    (Join-Path $packageRoot "qmltooling"),
    (Join-Path $packageRoot "qml\QtQuick\VirtualKeyboard"),
    (Join-Path $packageRoot "Qt6VirtualKeyboard.dll"),
    (Join-Path $packageRoot "qml\QtQuick3D"),
    (Join-Path $packageRoot "Qt6Quick3DUtils.dll"),
    (Join-Path $packageRoot "qml\QtQuick\Pdf"),
    (Join-Path $packageRoot "qml\QtQuick\Effects"),
    (Join-Path $packageRoot "Qt6QuickEffects.dll"),
    (Join-Path $packageRoot "qml\QtQuick\Lottie"),
    (Join-Path $packageRoot "Qt6Lottie.dll"),
    (Join-Path $packageRoot "Qt6LottieVectorImageGenerator.dll"),
    (Join-Path $packageRoot "qml\QtQuick\Scene2D"),
    (Join-Path $packageRoot "qml\QtQuick\Scene3D"),
    (Join-Path $packageRoot "qml\QtQuick\Timeline"),
    (Join-Path $packageRoot "qml\QtQuick\Particles"),
    (Join-Path $packageRoot "qml\QtQuick\LocalStorage"),
    (Join-Path $packageRoot "qml\QtQuick\tooling")
)
foreach ($style in @('Fusion', 'Imagine', 'Material', 'Universal', 'FluentWinUI3', 'Windows', 'NativeStyle')) {
    $drop += Join-Path $packageRoot "qml\QtQuick\Controls\$style"
    $drop += Join-Path $packageRoot "Qt6QuickControls2$style.dll"
    $drop += Join-Path $packageRoot "Qt6QuickControls2${style}StyleImpl.dll"
}
$removedMB = 0.0
foreach ($item in $drop) {
    if (Test-Path $item) {
        $removedMB += Get-FolderSizeMB $item
        Remove-Item -LiteralPath $item -Recurse -Force
    }
}
"  剔除 {0:N1} MB（未使用的 Qt 模块与开发件）" -f $removedMB

Write-Host "== 6/6 体积 ==" -ForegroundColor Cyan
$rows = Get-ChildItem -LiteralPath $packageRoot | ForEach-Object {
    [pscustomobject]@{ Name = $_.Name; MB = Get-FolderSizeMB $_.FullName }
} | Sort-Object MB -Descending
$rows | ForEach-Object { "  {0,8:N1} MB  {1}" -f $_.MB, $_.Name }
$totalMB = Get-FolderSizeMB $packageRoot
"  --------"
"  {0,8:N1} MB  合计（{1} 个顶层项）" -f $totalMB, (Get-ChildItem -LiteralPath $packageRoot).Count

if (-not $SkipZip) {
    Write-Host "== 压缩 zip ==" -ForegroundColor Cyan
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $sw = [Diagnostics.Stopwatch]::StartNew()
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $packageRoot, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $true)
    $sw.Stop()
    "  zip 用时 {0:N0}s" -f $sw.Elapsed.TotalSeconds
    "  {0,8:N1} MB  {1}" -f (Get-FolderSizeMB $zipPath), $zipPath
}

if (-not $SkipVerify) {
    Write-Host "== 隔离环境验证（最小 PATH，冷跑）==" -ForegroundColor Cyan
    $tool = Join-Path $packageRoot "integration_check.exe"
    Copy-Item -LiteralPath (Join-Path $projectRoot "build\integration_check\Release\integration_check.exe") -Destination $tool
    $cache = Join-Path $env:LOCALAPPDATA "BadmintonAnalyzer\羽毛球回合分析器\cache"
    if (Test-Path $cache) { Remove-Item -LiteralPath $cache -Recurse -Force }
    $savedPath = $env:Path
    $savedPreference = $ErrorActionPreference
    $env:Path = "C:\Windows\system32;C:\Windows"
    # 原生程序往 stderr 写日志时 PS 5.1 会产生 NativeCommandError，这里不能让它中断脚本
    $ErrorActionPreference = "Continue"
    try {
        & $tool $VerifyClip 56460 3840 2160 2>&1 | Select-String -Pattern 'preflight|result|final state|rallies in|MISS'
    } finally {
        $ErrorActionPreference = $savedPreference
        $env:Path = $savedPath
        Remove-Item -LiteralPath $tool -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "完成：$packageRoot" -ForegroundColor Green
