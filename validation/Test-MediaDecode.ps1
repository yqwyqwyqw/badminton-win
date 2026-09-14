[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,

    [ValidateRange(5, 600)]
    [int]$DurationSeconds = 30,

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

function Quote-NativeArgument {
    param([string]$Value)

    if ($Value -notmatch '[\s"]') { return $Value }
    return '"' + ($Value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

function Invoke-Benchmark {
    param(
        [string]$Name,
        [string[]]$Arguments,
        [string]$LogPath,
        [int]$ExpectedFrames,
        [double]$MediaSeconds
    )

    $stdoutPath = "$LogPath.stdout"
    $argumentLine = ($Arguments | ForEach-Object { Quote-NativeArgument $_ }) -join ' '
    $process = Start-Process -FilePath $script:ffmpegPath `
        -ArgumentList $argumentLine `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $LogPath `
        -WindowStyle Hidden `
        -PassThru

    $startedAt = Get-Date
    $peakWorkingSet = 0L
    while (-not $process.HasExited) {
        try {
            $process.Refresh()
            if ($process.WorkingSet64 -gt $peakWorkingSet) {
                $peakWorkingSet = $process.WorkingSet64
            }
        } catch {
            # The process may exit between HasExited and Refresh.
        }
        Start-Sleep -Milliseconds 100
    }
    $process.WaitForExit()
    $exitCode = $process.ExitCode
    $process.Refresh()
    $elapsed = ((Get-Date) - $startedAt).TotalSeconds
    $cpuSeconds = $process.TotalProcessorTime.TotalSeconds
    $logText = if (Test-Path -LiteralPath $LogPath) {
        Get-Content -LiteralPath $LogPath -Raw -ErrorAction SilentlyContinue
    } else { '' }
    if ($null -eq $exitCode) {
        $exitMatch = [regex]::Match($logText, 'Exiting with exit code (\d+)')
        if ($exitMatch.Success) {
            $exitCode = [int]$exitMatch.Groups[1].Value
        }
    }
    $benchmarkTime = [regex]::Match($logText, 'bench:\s+utime=([0-9.]+)s\s+stime=([0-9.]+)s')
    if ($benchmarkTime.Success) {
        $cpuSeconds = [double]$benchmarkTime.Groups[1].Value + [double]$benchmarkTime.Groups[2].Value
    }
    $benchmarkMemory = [regex]::Match($logText, 'bench:\s+maxrss=(\d+)KiB')
    if ($benchmarkMemory.Success) {
        $ffmpegPeak = [long]$benchmarkMemory.Groups[1].Value * 1KB
        if ($ffmpegPeak -gt $peakWorkingSet) { $peakWorkingSet = $ffmpegPeak }
    }
    $processedFrames = 0
    $frameMatches = [regex]::Matches($logText, 'frame=\s*(\d+)')
    if ($frameMatches.Count -gt 0) {
        $processedFrames = [int]$frameMatches[$frameMatches.Count - 1].Groups[1].Value
    } elseif ($ExpectedFrames -gt 0 -and $exitCode -eq 0) {
        $processedFrames = $ExpectedFrames
    }
    if ($null -eq $exitCode -and $ExpectedFrames -gt 0 -and $processedFrames -ge ([math]::Floor($ExpectedFrames * 0.99))) {
        $exitCode = 0
    }
    $fps = if ($elapsed -gt 0) { $processedFrames / $elapsed } else { 0 }
    $realtimeFactor = if ($elapsed -gt 0) { $MediaSeconds / $elapsed } else { 0 }
    $logicalProcessors = [Environment]::ProcessorCount
    $normalizedCpuPercent = if ($elapsed -gt 0 -and $logicalProcessors -gt 0) {
        100.0 * $cpuSeconds / ($elapsed * $logicalProcessors)
    } else { 0 }

    if (Test-Path -LiteralPath $stdoutPath) {
        $stdout = Get-Content -LiteralPath $stdoutPath -Raw -ErrorAction SilentlyContinue
        if (-not [string]::IsNullOrWhiteSpace($stdout)) {
            Add-Content -LiteralPath $LogPath -Value "`n--- stdout ---`n$stdout" -Encoding UTF8
        }
        Remove-Item -LiteralPath $stdoutPath -Force
    }

    return [ordered]@{
        name = $Name
        exitCode = $exitCode
        elapsedSeconds = [math]::Round($elapsed, 3)
        mediaSeconds = [math]::Round($MediaSeconds, 3)
        processedFrames = $processedFrames
        throughputFps = [math]::Round($fps, 2)
        realtimeFactor = [math]::Round($realtimeFactor, 2)
        processCpuSeconds = [math]::Round($cpuSeconds, 3)
        normalizedCpuPercent = [math]::Round($normalizedCpuPercent, 2)
        peakWorkingSetMB = [math]::Round($peakWorkingSet / 1MB, 1)
        log = $LogPath
    }
}

$resolvedInput = (Resolve-Path -LiteralPath $InputPath).Path
$projectRoot = Split-Path -Parent $PSScriptRoot
$ffmpegCandidates = @(@(
    (Join-Path $projectRoot '.tools\ffmpeg\ffmpeg-9.0.1-full_build-shared\bin\ffmpeg.exe'),
    (Get-Command ffmpeg.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1)
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) })
$ffprobeCandidates = @(@(
    (Join-Path $projectRoot '.tools\ffmpeg\ffmpeg-9.0.1-full_build-shared\bin\ffprobe.exe'),
    (Get-Command ffprobe.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1)
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) })

if ($ffmpegCandidates.Count -eq 0 -or $ffprobeCandidates.Count -eq 0) {
    throw 'FFmpeg/ffprobe not found. Install the project-local portable FFmpeg package first.'
}

$script:ffmpegPath = $ffmpegCandidates[0]
$ffprobePath = $ffprobeCandidates[0]
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$safeBaseName = [IO.Path]::GetFileNameWithoutExtension($resolvedInput) -replace '[^\p{L}\p{N}._-]', '_'
$outputDirectory = Join-Path $OutputRoot "$safeBaseName-$timestamp"
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

$probePath = Join-Path $outputDirectory 'media-probe.json'
$probeText = (& $ffprobePath -v error -show_format -show_streams -of json -- $resolvedInput) -join "`n"
if ($LASTEXITCODE -ne 0) { throw 'ffprobe could not read the input video.' }
$probeText | Set-Content -LiteralPath $probePath -Encoding UTF8
$probe = $probeText | ConvertFrom-Json
$videoStream = $probe.streams | Where-Object codec_type -eq 'video' | Select-Object -First 1
if (-not $videoStream) { throw 'The input file has no video stream.' }

$fps = Convert-RationalToDouble $videoStream.avg_frame_rate
$nominalFps = Convert-RationalToDouble $videoStream.r_frame_rate
$sourceDuration = [double]$videoStream.duration
if ($sourceDuration -le 0) { $sourceDuration = [double]$probe.format.duration }
$testDuration = [math]::Min([double]$DurationSeconds, $sourceDuration)
$expectedFrames = [int][math]::Round($fps * $testDuration)
$frameCountRate = if ($videoStream.nb_frames -and $sourceDuration -gt 0) {
    [double]$videoStream.nb_frames / $sourceDuration
} else { 0 }
$probablyCfr = ([math]::Abs($fps - $nominalFps) -lt 0.001) -and (
    $frameCountRate -eq 0 -or [math]::Abs($fps - $frameCountRate) -lt 0.05
)

$gpus = @(Get-CimInstance Win32_VideoController | ForEach-Object {
    [ordered]@{ name = $_.Name; driverVersion = $_.DriverVersion }
})
$hwaccels = @(& $script:ffmpegPath -hide_banner -hwaccels 2>&1 | ForEach-Object { "$_" })
$ffmpegVersion = (& $script:ffmpegPath -version | Select-Object -First 1) -join ''

$commonTail = @(
    '-t', ([string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0:0.###}', $testDuration)),
    '-map', '0:v:0', '-an', '-f', 'null', 'NUL'
)
$softwareArguments = @(
    '-hide_banner', '-nostdin', '-v', 'info', '-benchmark', '-stats',
    '-i', $resolvedInput,
    '-vf', 'scale=1280:720:flags=fast_bilinear'
) + $commonTail
$cudaArguments = @(
    '-hide_banner', '-nostdin', '-v', 'verbose', '-benchmark', '-stats',
    '-hwaccel', 'cuda', '-hwaccel_output_format', 'cuda',
    '-i', $resolvedInput,
    '-vf', 'scale_cuda=1280:720'
) + $commonTail

$softwareResult = Invoke-Benchmark -Name 'software-scale-720p' `
    -Arguments $softwareArguments `
    -LogPath (Join-Path $outputDirectory 'software-decode.log') `
    -ExpectedFrames $expectedFrames `
    -MediaSeconds $testDuration
$cudaResult = Invoke-Benchmark -Name 'cuda-nvdec-scale-720p' `
    -Arguments $cudaArguments `
    -LogPath (Join-Path $outputDirectory 'cuda-decode.log') `
    -ExpectedFrames $expectedFrames `
    -MediaSeconds $testDuration

$warnings = @()
if ($videoStream.width -ne 3840 -or $videoStream.height -ne 2160) {
    $warnings += 'The source is not standard 3840x2160 4K.'
}
if ([math]::Abs($fps - 60.0) -gt 0.5) {
    $warnings += "The source is $([math]::Round($fps, 3)) fps, not the target 60 fps."
}
if (-not $probablyCfr) {
    $warnings += 'Basic metadata suggests possible VFR; inspect per-frame timestamps next.'
}

$cudaPass = ($cudaResult.exitCode -eq 0) -and ($cudaResult.realtimeFactor -ge 1.0)
$targetProfileMatch = ($videoStream.width -eq 3840) -and ($videoStream.height -eq 2160) -and ([math]::Abs($fps - 60.0) -le 0.5)
$verdict = if (-not $cudaPass) {
    'FAIL'
} elseif (-not $targetProfileMatch) {
    'PASS_WITH_WARNING'
} else {
    'PASS'
}
$report = [ordered]@{
    schemaVersion = 1
    generatedAt = (Get-Date).ToString('o')
    verdict = $verdict
    sourceDecodeVerified = $cudaPass
    target4k60Verified = ($cudaPass -and $targetProfileMatch)
    criterion = 'CUDA/NVDEC pipeline completes successfully at no less than real-time speed for this source'
    source = [ordered]@{
        path = $resolvedInput
        fileSizeBytes = [long]$probe.format.size
        durationSeconds = [math]::Round($sourceDuration, 3)
        codec = $videoStream.codec_name
        profile = $videoStream.profile
        pixelFormat = $videoStream.pix_fmt
        width = [int]$videoStream.width
        height = [int]$videoStream.height
        fps = [math]::Round($fps, 3)
        nominalFps = [math]::Round($nominalFps, 3)
        frameCount = if ($videoStream.nb_frames) { [int]$videoStream.nb_frames } else { $null }
        bitRate = [long]$videoStream.bit_rate
        probablyConstantFrameRate = $probablyCfr
    }
    machine = [ordered]@{
        logicalProcessors = [Environment]::ProcessorCount
        gpus = $gpus
        ffmpeg = $ffmpegVersion
        advertisedHardwareAcceleration = $hwaccels
    }
    benchmark = [ordered]@{
        requestedDurationSeconds = $DurationSeconds
        actualDurationSeconds = $testDuration
        software = $softwareResult
        cuda = $cudaResult
    }
    warnings = $warnings
}

$jsonReportPath = Join-Path $outputDirectory 'validation-report.json'
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $jsonReportPath -Encoding UTF8

$warningLines = if ($warnings.Count -gt 0) {
    ($warnings | ForEach-Object { "- $_" }) -join "`n"
} else { '- None' }
$markdown = @"
# Media and Hardware Decode Validation

- Verdict: **$($report.verdict)**
- Source: $resolvedInput
- Format: $($videoStream.width)x$($videoStream.height), $([math]::Round($fps, 3)) fps, $($videoStream.codec_name) / $($videoStream.profile)
- Duration: $([math]::Round($sourceDuration, 3)) seconds
- Initial frame-rate classification: $(if ($probablyCfr) { 'CFR' } else { 'possible VFR' })
- Test window: $([math]::Round($testDuration, 3)) seconds

## Throughput

| Pipeline | Exit code | Wall time | Throughput | Real-time factor | Normalized process CPU | Peak working set |
|---|---:|---:|---:|---:|---:|---:|
| CPU decode + CPU scale | $($softwareResult.exitCode) | $($softwareResult.elapsedSeconds) s | $($softwareResult.throughputFps) fps | $($softwareResult.realtimeFactor)x | $($softwareResult.normalizedCpuPercent)% | $($softwareResult.peakWorkingSetMB) MB |
| NVDEC/CUDA decode + CUDA scale | $($cudaResult.exitCode) | $($cudaResult.elapsedSeconds) s | $($cudaResult.throughputFps) fps | $($cudaResult.realtimeFactor)x | $($cudaResult.normalizedCpuPercent)% | $($cudaResult.peakWorkingSetMB) MB |

## Warnings

$warningLines

## Criterion

$($report.criterion). This report covers only the first A6 check; it does not validate trajectory recall or rally segmentation.
"@
$markdownPath = Join-Path $outputDirectory 'validation-report.md'
$markdown | Set-Content -LiteralPath $markdownPath -Encoding UTF8

Write-Host "Validation complete: $($report.verdict)"
Write-Host "Report directory: $outputDirectory"
Write-Host "CPU pipeline: $($softwareResult.throughputFps) fps, $($softwareResult.realtimeFactor)x real-time"
Write-Host "CUDA pipeline: $($cudaResult.throughputFps) fps, $($cudaResult.realtimeFactor)x real-time"
if ($warnings.Count -gt 0) {
    Write-Host 'Warnings:'
    $warnings | ForEach-Object { Write-Host "- $_" }
}
