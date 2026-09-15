[CmdletBinding()]
param(
    [string]$Version = "0.0.3",
    [switch]$SkipBuild,
    [switch]$SkipSourceArchive
)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\")).Path
$buildRoot = Join-Path $projectRoot "build\app"
$releaseRoot = Join-Path $projectRoot "build\release"
$packageName = "badminton-analyzer-v$Version-windows"
$packageRoot = Join-Path $releaseRoot $packageName
$zipPath = Join-Path $releaseRoot "$packageName.zip"
$qtBin = "D:\Qt\6.11.2\msvc2022_64\bin"
$cmake = "C:\Program Files\CMake\bin\cmake.exe"
$windeployqt = Join-Path $qtBin "windeployqt.exe"

if (-not (Test-Path $cmake)) {
    throw "未找到 CMake：$cmake"
}
if (-not (Test-Path $qtBin)) {
    throw "未找到 Qt bin 目录：$qtBin"
}

if (-not $SkipBuild) {
    & $cmake --build $buildRoot --config Release -- /m
    if ($LASTEXITCODE -ne 0) {
        throw "Release 编译失败，退出码：$LASTEXITCODE"
    }
}

$exe = Join-Path $buildRoot "Release\badminton-analyzer.exe"
if (-not (Test-Path $exe)) {
    throw "未找到 Release 可执行文件：$exe"
}

if (Test-Path $releaseRoot) {
    Remove-Item -LiteralPath $releaseRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null
Copy-Item -LiteralPath $exe -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot "README.md") -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot "RELEASE.md") -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot "app\README.md") -Destination (Join-Path $packageRoot "app-README.md")

$env:Path = "$qtBin;$env:Path"
& $windeployqt --release --qmldir (Join-Path $projectRoot "app\qml") --no-translations (Join-Path $packageRoot "badminton-analyzer.exe")
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt 失败，退出码：$LASTEXITCODE"
}

Compress-Archive -Path (Join-Path $packageRoot "*") -DestinationPath $zipPath -CompressionLevel Optimal

$sourceZip = $null
if (-not $SkipSourceArchive) {
    $tag = "v$Version"
    if ((git -C $projectRoot tag --list $tag).Trim() -eq $tag) {
        $sourceZip = Join-Path $releaseRoot "badminton-analyzer-$tag-source.zip"
        git -C $projectRoot archive --format=zip --prefix="badminton-analyzer-$tag/" --output=$sourceZip $tag
        if ($LASTEXITCODE -ne 0) {
            throw "源代码归档失败，退出码：$LASTEXITCODE"
        }
    }
}

Write-Host "Windows 包：$zipPath"
if ($sourceZip) {
    Write-Host "源代码包：$sourceZip"
}
