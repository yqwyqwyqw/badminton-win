[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,

    [ValidateRange(1, 20)]
    [int]$VideoBitrateMbps = 6,

    [string]$OutputRoot
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [Console]::OutputEncoding

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'validation-output'
}

function Convert-RationalToDouble {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) { return 0.0 }
    $parts = $Value.Split('/')
    if ($parts.Count -eq 2) {
        $denominator = [double]$parts[1]
        if ($denominator -eq 0) { return 0.0 }
        return [double]$parts[0] / $denominator
    }
    return [double]$Value
}

function Get-FirstVideoStream {
    param([string]$Path, [string]$ProbeOutputPath)

    $text = (& $script:ffprobePath -v error -show_format -show_streams -of json -- $Path) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "ffprobe failed for $Path" }
    $text | Set-Content -LiteralPath $ProbeOutputPath -Encoding UTF8
    $data = $text | ConvertFrom-Json
    $stream = $data.streams | Where-Object codec_type -eq 'video' | Select-Object -First 1
    if (-not $stream) { throw "No video stream in $Path" }
    return [pscustomobject]@{ data = $data; stream = $stream }
}

function Get-FrameTimestamps {
    param([string]$Path)

    $lines = & $script:ffprobePath -v error -select_streams v:0 `
        -show_entries frame=best_effort_timestamp_time -of csv=p=0 -- $Path
    if ($LASTEXITCODE -ne 0) { throw "Frame timestamp extraction failed for $Path" }
    $values = New-Object System.Collections.Generic.List[double]
    foreach ($line in $lines) {
        $clean = ("$line").Trim().TrimEnd(',')
        $parsed = 0.0
        if ([double]::TryParse($clean, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) {
            $values.Add($parsed)
        }
    }
    return $values.ToArray()
}

$resolvedInput = (Resolve-Path -LiteralPath $InputPath).Path
$projectRoot = Split-Path -Parent $PSScriptRoot
$ffmpegCandidate = Join-Path $projectRoot '.tools\ffmpeg\ffmpeg-9.0.1-full_build-shared\bin\ffmpeg.exe'
$ffprobeCandidate = Join-Path $projectRoot '.tools\ffmpeg\ffmpeg-9.0.1-full_build-shared\bin\ffprobe.exe'
if (-not (Test-Path -LiteralPath $ffmpegCandidate)) {
    $ffmpegCandidate = Get-Command ffmpeg.exe -ErrorAction Stop | Select-Object -ExpandProperty Source -First 1
}
if (-not (Test-Path -LiteralPath $ffprobeCandidate)) {
    $ffprobeCandidate = Get-Command ffprobe.exe -ErrorAction Stop | Select-Object -ExpandProperty Source -First 1
}
$script:ffmpegPath = $ffmpegCandidate
$script:ffprobePath = $ffprobeCandidate

$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$safeBaseName = [IO.Path]::GetFileNameWithoutExtension($resolvedInput) -replace '[^\p{L}\p{N}._-]', '_'
$outputDirectory = Join-Path $OutputRoot "$safeBaseName-proxy-$timestamp"
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$proxyPath = Join-Path $outputDirectory 'proxy-720p.mp4'
$encodeLogPath = Join-Path $outputDirectory 'proxy-encode.log'
$nvencLogPath = Join-Path $outputDirectory 'nvenc-encode-attempt.log'

$sourceInfo = Get-FirstVideoStream -Path $resolvedInput -ProbeOutputPath (Join-Path $outputDirectory 'source-probe.json')
$sourceFps = Convert-RationalToDouble $sourceInfo.stream.avg_frame_rate
$sourceDuration = [double]$sourceInfo.stream.duration
if ($sourceDuration -le 0) { $sourceDuration = [double]$sourceInfo.data.format.duration }

$bitrate = "${VideoBitrateMbps}M"
$maxrate = "$(($VideoBitrateMbps * 2))M"
$bufsize = "$(($VideoBitrateMbps * 4))M"
$encodeArguments = @(
    '-hide_banner', '-y', '-nostdin', '-v', 'info', '-benchmark', '-stats',
    '-hwaccel', 'cuda', '-hwaccel_output_format', 'cuda',
    '-i', $resolvedInput,
    '-map', '0:v:0', '-map', '0:a:0?',
    '-vf', 'scale_cuda=1280:720',
    '-c:v', 'h264_nvenc', '-preset', 'p4', '-tune', 'hq',
    '-rc', 'vbr', '-cq', '24', '-b:v', $bitrate, '-maxrate', $maxrate, '-bufsize', $bufsize,
    '-c:a', 'aac', '-b:a', '128k',
    '-movflags', '+faststart',
    $proxyPath
)

$stopwatch = [Diagnostics.Stopwatch]::StartNew()
$previousErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$encodeOutput = & $script:ffmpegPath @encodeArguments 2>&1 | ForEach-Object { "$_" }
$nvencExitCode = $LASTEXITCODE
$ErrorActionPreference = $previousErrorActionPreference
$stopwatch.Stop()
$nvencSeconds = $stopwatch.Elapsed.TotalSeconds
$encodeOutput | Set-Content -LiteralPath $nvencLogPath -Encoding UTF8
$encodeBackend = 'h264_nvenc'
$encodeExitCode = $nvencExitCode
$nvencFailure = $null

if ($nvencExitCode -ne 0) {
    $nvencText = $encodeOutput -join "`n"
    $apiMatch = [regex]::Match($nvencText, 'Required:\s*([0-9.]+)\s+Found:\s*([0-9.]+)')
    $driverMatch = [regex]::Match($nvencText, 'minimum required Nvidia driver for nvenc is\s+([0-9.]+)')
    if ($apiMatch.Success) {
        $nvencFailure = "NVENC API mismatch: required $($apiMatch.Groups[1].Value), found $($apiMatch.Groups[2].Value)"
        if ($driverMatch.Success) {
            $nvencFailure += "; FFmpeg reports minimum driver $($driverMatch.Groups[1].Value)"
        }
    } else {
        $nvencFailure = "NVENC initialization failed with exit code $nvencExitCode"
    }
    $encodeBackend = 'libx264-fallback'
    $fallbackArguments = @(
        '-hide_banner', '-y', '-nostdin', '-v', 'info', '-benchmark', '-stats',
        '-hwaccel', 'cuda', '-hwaccel_output_format', 'cuda',
        '-i', $resolvedInput,
        '-map', '0:v:0', '-map', '0:a:0?',
        '-vf', 'scale_cuda=1280:720,hwdownload,format=nv12',
        '-c:v', 'libx264', '-preset', 'veryfast', '-crf', '23',
        '-c:a', 'aac', '-b:a', '128k',
        '-movflags', '+faststart',
        $proxyPath
    )
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $encodeOutput = & $script:ffmpegPath @fallbackArguments 2>&1 | ForEach-Object { "$_" }
    $encodeExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorActionPreference
    $stopwatch.Stop()
    $encodeOutput | Set-Content -LiteralPath $encodeLogPath -Encoding UTF8
} else {
    $encodeOutput | Set-Content -LiteralPath $encodeLogPath -Encoding UTF8
}

if ($encodeExitCode -ne 0) {
    throw "Proxy encoding and fallback both failed. See $outputDirectory"
}

$proxyInfo = Get-FirstVideoStream -Path $proxyPath -ProbeOutputPath (Join-Path $outputDirectory 'proxy-probe.json')
$proxyFps = Convert-RationalToDouble $proxyInfo.stream.avg_frame_rate
$proxyDuration = [double]$proxyInfo.stream.duration
if ($proxyDuration -le 0) { $proxyDuration = [double]$proxyInfo.data.format.duration }

$sourceTimestamps = Get-FrameTimestamps -Path $resolvedInput
$proxyTimestamps = Get-FrameTimestamps -Path $proxyPath
$compareCount = [math]::Min($sourceTimestamps.Count, $proxyTimestamps.Count)
$maxTimestampDrift = 0.0
$sumTimestampDrift = 0.0
for ($index = 0; $index -lt $compareCount; $index++) {
    $drift = [math]::Abs($sourceTimestamps[$index] - $proxyTimestamps[$index])
    $sumTimestampDrift += $drift
    if ($drift -gt $maxTimestampDrift) { $maxTimestampDrift = $drift }
}
$meanTimestampDrift = if ($compareCount -gt 0) { $sumTimestampDrift / $compareCount } else { [double]::PositiveInfinity }
$frameDuration = if ($sourceFps -gt 0) { 1.0 / $sourceFps } else { 0.04 }
$durationDifference = [math]::Abs($sourceDuration - $proxyDuration)
$frameCountDifference = [math]::Abs($sourceTimestamps.Count - $proxyTimestamps.Count)

$seekFractions = @(0.05, 0.25, 0.5, 0.75, 0.95)
$seekResults = @()
foreach ($fraction in $seekFractions) {
    $seekTime = [math]::Round($sourceDuration * $fraction, 3)
    $seekText = [string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0:0.###}', $seekTime)
    $null = & $script:ffmpegPath -hide_banner -nostdin -v error -ss $seekText -i $proxyPath -frames:v 1 -an -f null NUL 2>&1
    $seekResults += [ordered]@{
        requestedSeconds = $seekTime
        exitCode = $LASTEXITCODE
        success = ($LASTEXITCODE -eq 0)
    }
}

$dimensionPass = ([int]$proxyInfo.stream.width -eq 1280) -and ([int]$proxyInfo.stream.height -eq 720)
$fpsPass = [math]::Abs($sourceFps - $proxyFps) -lt 0.01
$frameCountPass = $frameCountDifference -le 1
$durationPass = $durationDifference -le ([math]::Max(0.05, $frameDuration))
$timestampPass = $compareCount -gt 0 -and $maxTimestampDrift -le ($frameDuration + 0.001)
$seekPass = @($seekResults | Where-Object { -not $_.success }).Count -eq 0
$timelinePass = $dimensionPass -and $fpsPass -and $frameCountPass -and $durationPass -and $timestampPass -and $seekPass
$hardwareEncodeAvailable = $nvencExitCode -eq 0
$verdict = if (-not $timelinePass) {
    'FAIL'
} elseif (-not $hardwareEncodeAvailable) {
    'PASS_WITH_WARNING'
} else {
    'PASS'
}
$encodeRealtimeFactor = if ($stopwatch.Elapsed.TotalSeconds -gt 0) { $sourceDuration / $stopwatch.Elapsed.TotalSeconds } else { 0 }

$report = [ordered]@{
    schemaVersion = 1
    generatedAt = (Get-Date).ToString('o')
    verdict = $verdict
    source = [ordered]@{
        path = $resolvedInput
        width = [int]$sourceInfo.stream.width
        height = [int]$sourceInfo.stream.height
        fps = [math]::Round($sourceFps, 3)
        durationSeconds = [math]::Round($sourceDuration, 6)
        frameCount = $sourceTimestamps.Count
    }
    proxy = [ordered]@{
        path = $proxyPath
        fileSizeBytes = (Get-Item -LiteralPath $proxyPath).Length
        width = [int]$proxyInfo.stream.width
        height = [int]$proxyInfo.stream.height
        fps = [math]::Round($proxyFps, 3)
        durationSeconds = [math]::Round($proxyDuration, 6)
        frameCount = $proxyTimestamps.Count
        encodeBackend = $encodeBackend
        encodeSeconds = [math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
        encodeRealtimeFactor = [math]::Round($encodeRealtimeFactor, 2)
    }
    timeline = [ordered]@{
        comparedFrames = $compareCount
        frameCountDifference = $frameCountDifference
        durationDifferenceSeconds = [math]::Round($durationDifference, 6)
        meanTimestampDriftSeconds = [math]::Round($meanTimestampDrift, 6)
        maxTimestampDriftSeconds = [math]::Round($maxTimestampDrift, 6)
        seekChecks = $seekResults
    }
    checks = [ordered]@{
        dimensions1280x720 = $dimensionPass
        fpsPreserved = $fpsPass
        frameCountWithinOne = $frameCountPass
        durationWithinOneFrame = $durationPass
        timestampsWithinOneFrame = $timestampPass
        randomSeekSucceeded = $seekPass
        hardwareEncodeAvailable = $hardwareEncodeAvailable
        hardwareEncodeFailure = $nvencFailure
        nvencAttemptExitCode = $nvencExitCode
        nvencAttemptSeconds = [math]::Round($nvencSeconds, 3)
    }
}

$jsonReportPath = Join-Path $outputDirectory 'proxy-validation-report.json'
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $jsonReportPath -Encoding UTF8
$proxySizeMB = [math]::Round($report.proxy.fileSizeBytes / 1MB, 2)
$markdown = @"
# Proxy Timeline Validation

- Verdict: **$verdict**
- Source: $resolvedInput
- Proxy: $proxyPath
- Source format: $($report.source.width)x$($report.source.height) at $($report.source.fps) fps
- Proxy format: $($report.proxy.width)x$($report.proxy.height) at $($report.proxy.fps) fps
- Proxy size: $proxySizeMB MB
- Encode backend: $encodeBackend
- Encode time: $($report.proxy.encodeSeconds) seconds ($($report.proxy.encodeRealtimeFactor)x real-time)

## Timeline comparison

| Check | Result |
|---|---:|
| Source frames | $($report.source.frameCount) |
| Proxy frames | $($report.proxy.frameCount) |
| Frame-count difference | $frameCountDifference |
| Duration difference | $([math]::Round($durationDifference, 6)) s |
| Mean timestamp drift | $([math]::Round($meanTimestampDrift, 6)) s |
| Maximum timestamp drift | $([math]::Round($maxTimestampDrift, 6)) s |
| Five random seek checks | $(if ($seekPass) { 'PASS' } else { 'FAIL' }) |
| NVENC available with current FFmpeg/driver | $(if ($hardwareEncodeAvailable) { 'YES' } else { 'NO - software encode fallback used' }) |

This validates proxy generation and timeline preservation only. It does not validate image-model accuracy.
"@
$markdownPath = Join-Path $outputDirectory 'proxy-validation-report.md'
$markdown | Set-Content -LiteralPath $markdownPath -Encoding UTF8

Write-Host "Proxy validation complete: $verdict"
Write-Host "Report directory: $outputDirectory"
Write-Host "Proxy: $proxyPath ($proxySizeMB MB)"
Write-Host "Encode: $($report.proxy.encodeSeconds) seconds, $($report.proxy.encodeRealtimeFactor)x real-time"
Write-Host "Frames: $($report.source.frameCount) -> $($report.proxy.frameCount)"
Write-Host "Max timestamp drift: $($report.timeline.maxTimestampDriftSeconds) seconds"
