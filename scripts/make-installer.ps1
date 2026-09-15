# 用 Inno Setup 6 生成安装包（单文件 setup.exe，LZMA2 压缩）。
#
#   产物: build\release\badminton-analyzer-v<版本>-setup.exe
#   默认: 按用户安装到 %LOCALAPPDATA%\Programs\BadmintonAnalyzer（无需管理员），
#         建开始菜单快捷方式，可选桌面快捷方式，带卸载项。
#
# 依赖: .tools\packers\innosetup\ISCC.exe（脚本会自动提示如何获取）。

[CmdletBinding()]
param(
    [string]$Version = "0.1.0",
    [string]$PackageDir,
    [switch]$SkipVerify
)

# 原生程序（windeployqt / ISCC / ffmpeg）往 stderr 写警告时不应中断脚本；真正的失败看 $LASTEXITCODE
$ErrorActionPreference = "Continue"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$releaseRoot = Join-Path $projectRoot "build\release"
if (-not $PackageDir) {
    $PackageDir = Join-Path $releaseRoot "badminton-analyzer-v$Version-windows"
}
$iss = Join-Path $projectRoot "scripts\installer.iss"
$iscc = Get-ChildItem (Join-Path $projectRoot ".tools\packers") -Recurse -Filter ISCC.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1

if (-not (Test-Path $PackageDir)) { throw "找不到便携目录：$PackageDir（先跑 release-windows.ps1）" }
if (-not $iscc) {
    throw "找不到 ISCC.exe。下载 Inno Setup 6 并静默安装到 .tools\packers：`n" +
          "  Invoke-WebRequest https://jrsoftware.org/download.php/is.exe -OutFile is.exe`n" +
          "  .\is.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /PORTABLE=1 /DIR=<packers>\innosetup"
}

# Inno 官方只带部分语言，简中需要补齐（用与 ISCC 对应的标签，避免消息 ID 不匹配）
$languageFile = Join-Path $iscc.DirectoryName "Languages\ChineseSimplified.isl"
if (-not (Test-Path $languageFile)) {
    Write-Host "补齐简体中文语言文件…" -ForegroundColor Cyan
    New-Item -ItemType Directory -Force -Path (Split-Path $languageFile) | Out-Null
    $ProgressPreference = 'SilentlyContinue'
    $sources = @(
        'https://raw.githubusercontent.com/jrsoftware/issrc/is-6_7_3/Files/Languages/Unofficial/ChineseSimplified.isl',
        'https://raw.githubusercontent.com/jrsoftware/issrc/main/Files/Languages/Unofficial/ChineseSimplified.isl'
    )
    foreach ($source in $sources) {
        try {
            Invoke-WebRequest -TimeoutSec 60 -UseBasicParsing -OutFile $languageFile -Uri $source
            if ((Get-Item $languageFile).Length -gt 1024) { break }
        } catch {
            Write-Host "  下载失败：$source" -ForegroundColor DarkYellow
        }
    }
}
$hasChinese = (Test-Path $languageFile) -and ((Get-Item $languageFile).Length -gt 1024)

function Invoke-Iscc([bool]$withChinese) {
    $arguments = @("/DAppVersion=$Version", "/DBuildDir=$PackageDir")
    if ($withChinese) { $arguments += "/DWithChinese=1" }
    $arguments += $iss
    $text = & $iscc.FullName @arguments 2>&1
    return [pscustomobject]@{ Code = $LASTEXITCODE; Text = $text }
}

Write-Host "== 编译安装包（LZMA2/ultra64 压缩，约 10-20 分钟）==" -ForegroundColor Cyan
$sw = [Diagnostics.Stopwatch]::StartNew()
$result = Invoke-Iscc $hasChinese
if ($result.Code -ne 0 -and $hasChinese) {
    Write-Host "带中文语言文件编译失败，回退到英文界面重试…" -ForegroundColor DarkYellow
    $result = Invoke-Iscc $false
}
if ($result.Code -ne 0) {
    $result.Text | Select-Object -Last 20 | ForEach-Object { "  $_" }
    throw "ISCC 编译失败：$($result.Code)"
}
$sw.Stop()

$setup = Join-Path $releaseRoot "badminton-analyzer-v$Version-setup.exe"
"  用时 {0:N0}s" -f $sw.Elapsed.TotalSeconds
"  {0,8:N1} MB  {1}" -f ((Get-Item $setup).Length / 1MB), $setup

if (-not $SkipVerify) {
    Write-Host "== 隔离验证：静默安装 -> 最小 PATH 启动 -> 卸载 ==" -ForegroundColor Cyan
    $target = Join-Path $env:TEMP "badminton-installer-check"
    if (Test-Path $target) { Remove-Item -LiteralPath $target -Recurse -Force }
    $install = Start-Process -FilePath $setup `
        -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/NOICONS', "/DIR=$target" -Wait -PassThru
    "  安装退出码: $($install.ExitCode)"
    $entry = Join-Path $target "badminton-analyzer.exe"
    "  入口 exe: " + (Test-Path $entry)
    $files = Get-ChildItem -LiteralPath $target -Recurse -File -ErrorAction SilentlyContinue
    "  安装文件 {0} 个, {1:N1} MB" -f $files.Count, (($files | Measure-Object Length -Sum).Sum / 1MB)
    "  关键资源: ffmpeg=" + (Test-Path (Join-Path $target "resources\ffmpeg\ffmpeg.exe")) +
        "  model=" + (Test-Path (Join-Path $target "resources\models\tracknet-8f-concat-288x512-sizes-fp16.onnx"))

    $savedPath = $env:Path
    $savedPreference = $ErrorActionPreference
    $env:Path = "C:\Windows\system32;C:\Windows"
    $ErrorActionPreference = "Continue"
    $log = Join-Path $env:TEMP "installer-gui.log"
    try {
        Start-Process -FilePath $entry -RedirectStandardOutput $log -RedirectStandardError "$log.err"
        Start-Sleep -Seconds 12
        $process = Get-Process -Name "badminton-analyzer" -ErrorAction SilentlyContinue
        if ($process) { "  启动成功 PID={0} 标题='{1}'" -f $process.Id, $process.MainWindowTitle }
        else { "  !! 启动失败" }
        $qmlErrors = (Select-String -Path "$log.err" -Pattern 'qml|TypeError|ReferenceError|is not installed' -ErrorAction SilentlyContinue |
                      Measure-Object).Count
        "  QML/模块报错: $qmlErrors 条"
        Stop-Process -Name "badminton-analyzer" -Force -ErrorAction SilentlyContinue
    } finally {
        $ErrorActionPreference = $savedPreference
        $env:Path = $savedPath
    }

    $uninstaller = Join-Path $target "unins000.exe"
    if (Test-Path $uninstaller) {
        Start-Process -FilePath $uninstaller -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait
        Start-Sleep -Seconds 2
        "  卸载后残留目录: " + (Test-Path $target)
    }
    if (Test-Path $target) { Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction SilentlyContinue }
}
