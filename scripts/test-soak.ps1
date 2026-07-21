[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [int]$DurationSeconds = 600,
    [int]$MinimumFps = 15
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\bin\$Configuration"
$producerExe = Join-Path $bin 'streaming_browser.exe'
$viewerExe = Join-Path $bin 'streaming_viewer.exe'
$fixture = (Resolve-Path (Join-Path $root 'tests\fixtures\frame-stress.html')).Path
$url = [Uri]::new($fixture).AbsoluteUri
$delay = [Threading.ManualResetEvent]::new($false)
$log = Join-Path $env:LOCALAPPDATA 'StreamingBrowser\app.log'

function Stop-DemoProcesses {
    Get-Process streaming_browser, streaming_viewer -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

function Wait-ViewerFrame([int]$TimeoutSeconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $process = Get-Process streaming_viewer -ErrorAction SilentlyContinue |
            Where-Object { $_.MainWindowTitle -match 'frame (\d+)' -and $_.Responding } |
            Select-Object -First 1
        if ($process) { return $process }
        $null = $delay.WaitOne(100)
    }
    throw 'Timed out waiting for viewer frame'
}

function Frame-Number([System.Diagnostics.Process]$Process) {
    $Process.Refresh()
    if ($Process.MainWindowTitle -match 'frame (\d+)') { return [uint64]$Matches[1] }
    return [uint64]0
}

Stop-DemoProcesses
Remove-Item $log -Force -ErrorAction SilentlyContinue
try {
    $producer = Start-Process -FilePath $producerExe -WorkingDirectory $bin `
        -ArgumentList @("--url=$url", '--force-transparency', '--visible') -PassThru
    $viewer = Start-Process -FilePath $viewerExe -WorkingDirectory $bin -PassThru
    $viewerWindow = Wait-ViewerFrame 30
    $firstFrame = Frame-Number $viewerWindow
    $maxProducerMemory = [int64]0
    $maxViewerMemory = [int64]0
    $deadline = [DateTime]::UtcNow.AddSeconds($DurationSeconds)

    while ([DateTime]::UtcNow -lt $deadline) {
        $producer.Refresh()
        $viewer.Refresh()
        if ($producer.HasExited -or $viewer.HasExited) { throw 'A soak process exited early' }
        if (-not $viewer.Responding) { throw 'Viewer stopped responding during soak' }
        $maxProducerMemory = [Math]::Max($maxProducerMemory, $producer.WorkingSet64)
        $maxViewerMemory = [Math]::Max($maxViewerMemory, $viewer.WorkingSet64)
        $null = $delay.WaitOne(1000)
    }

    $lastFrame = Frame-Number $viewerWindow
    $delivered = [int64]$lastFrame - [int64]$firstFrame
    $minimumFrames = $DurationSeconds * $MinimumFps
    if ($delivered -lt $minimumFrames) {
        throw "Only $delivered frames advanced; expected at least $minimumFrames"
    }
    $errors = if (Test-Path $log) {
        Select-String -Path $log -Pattern 'failed|timed out|WAIT_ABANDONED' -CaseSensitive:$false
    }
    if ($errors) { throw "Runtime error diagnostics found: $($errors[0].Line)" }

    Write-Host ("Soak passed: {0}s, {1} frames ({2:N1} fps), producer peak {3:N1} MiB, viewer peak {4:N1} MiB" -f `
        $DurationSeconds, $delivered, ($delivered / $DurationSeconds), `
        ($maxProducerMemory / 1MB), ($maxViewerMemory / 1MB))
}
finally {
    Stop-DemoProcesses
}
