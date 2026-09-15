# 生成"免安装单 exe"：把便携目录压成 zip 负载，附加在自解压启动器后面。
#
#   产物: build\release\badminton-analyzer-v<版本>-portable.exe
#   行为: 双击 -> 解压到该 exe 同级的 BadmintonAnalyzer\ -> 启动应用（再次双击直接启动）
#
# 依赖: 已由 release-windows.ps1 生成便携目录；本机 .NET Framework 的 csc（Windows 自带）。

[CmdletBinding()]
param(
    [string]$Version = "0.1.0",
    [string]$PackageDir,
    [switch]$SkipPayloadZip
)

$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$releaseRoot = Join-Path $projectRoot "build\release"
if (-not $PackageDir) {
    $PackageDir = Join-Path $releaseRoot "badminton-analyzer-v$Version-windows"
}
$launcherSource = Join-Path $projectRoot "tools\portable-launcher\launcher.cs"
$stub = Join-Path $releaseRoot "launcher-stub.exe"
$payload = Join-Path $releaseRoot "payload.zip"
$output = Join-Path $releaseRoot "badminton-analyzer-v$Version-portable.exe"

if (-not (Test-Path $PackageDir)) { throw "找不到便携目录：$PackageDir（先跑 release-windows.ps1）" }
if (-not (Test-Path $launcherSource)) { throw "找不到启动器源码：$launcherSource" }

$csc = "C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path $csc)) { throw "找不到 csc：$csc" }

Write-Host "== 1/4 编译自解压启动器 ==" -ForegroundColor Cyan
& $csc /nologo /target:winexe /optimize+ /platform:x64 /out:"$stub" `
    /reference:System.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll `
    /reference:System.IO.Compression.dll /reference:System.IO.Compression.FileSystem.dll `
    "$launcherSource"
if ($LASTEXITCODE -ne 0) { throw "启动器编译失败：$LASTEXITCODE" }
"  stub: {0:N0} bytes" -f (Get-Item $stub).Length

Write-Host "== 2/4 打包 zip 负载 ==" -ForegroundColor Cyan
if (-not $SkipPayloadZip -or -not (Test-Path $payload)) {
    if (Test-Path $payload) { Remove-Item -LiteralPath $payload -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $sw = [Diagnostics.Stopwatch]::StartNew()
    # includeBaseDirectory = false：解压后文件直接落在目标目录（应用在根）
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $PackageDir, $payload, [System.IO.Compression.CompressionLevel]::Optimal, $false)
    "  用时 {0:N0}s" -f $sw.Elapsed.TotalSeconds
}
$payloadSize = (Get-Item $payload).Length
"  payload: {0:N1} MB" -f ($payloadSize / 1MB)

Write-Host "== 3/4 合成单 exe ==" -ForegroundColor Cyan
if (Test-Path $output) { Remove-Item -LiteralPath $output -Force }
$stubSize = (Get-Item $stub).Length
Copy-Item -LiteralPath $stub -Destination $output
$stream = [System.IO.File]::Open($output, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write)
try {
    $copy = [System.IO.File]::OpenRead($payload)
    try { $copy.CopyTo($stream) } finally { $copy.Dispose() }
    # 尾注：int64 负载偏移 + int64 负载长度（启动器读取）
    $trailer = New-Object byte[] 16
    [BitConverter]::GetBytes([int64]$stubSize).CopyTo($trailer, 0)
    [BitConverter]::GetBytes([int64]$payloadSize).CopyTo($trailer, 8)
    $stream.Write($trailer, 0, 16)
} finally {
    $stream.Dispose()
}

Write-Host "== 4/4 结果 ==" -ForegroundColor Cyan
"  {0,8:N1} MB  {1}" -f ((Get-Item $output).Length / 1MB), $output
"  （便携目录 {0:N1} MB -> 单 exe {1:N1} MB，压缩率 {2:P0}）" -f `
    ((Get-ChildItem -LiteralPath $PackageDir -Recurse -File | Measure-Object Length -Sum).Sum / 1MB), `
    ((Get-Item $output).Length / 1MB), `
    (1 - (Get-Item $output).Length / (Get-ChildItem -LiteralPath $PackageDir -Recurse -File | Measure-Object Length -Sum).Sum)
