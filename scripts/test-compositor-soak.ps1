[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [int]$DurationSeconds = 60
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$delay = [Threading.ManualResetEvent]::new($false)
function Stop-Demo {
    Get-Process streaming_browser, streaming_viewer, streaming_compositor -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}
try {
    Stop-Demo
    & (Join-Path $PSScriptRoot 'run-compositor-demo.ps1') -Configuration $Configuration
    $deadline = (Get-Date).AddSeconds(30)
    do {
        $compositor = Get-Process streaming_compositor -ErrorAction SilentlyContinue |
            Where-Object { $_.MainWindowTitle -match 'overlay connected' -and $_.Responding } |
            Select-Object -First 1
        $producer = Get-Process streaming_browser -ErrorAction SilentlyContinue |
            Where-Object MainWindowTitle | Select-Object -First 1
        if ($compositor -and $producer) { break }
        $null = $delay.WaitOne(100)
    } while ((Get-Date) -lt $deadline)
    if (-not $compositor -or -not $producer) { throw 'Compositor soak could not start' }
    $producerStart = $producer.WorkingSet64
    $compositorStart = $compositor.WorkingSet64
    $null = $delay.WaitOne($DurationSeconds * 1000)
    $producer.Refresh(); $compositor.Refresh()
    if ($producer.HasExited -or $compositor.HasExited -or
        -not $producer.Responding -or -not $compositor.Responding) {
        throw 'A compositor soak process exited or stopped responding'
    }
    Write-Host ("Compositor soak passed ({0}s): producer {1:N1}->{2:N1} MiB; compositor {3:N1}->{4:N1} MiB" -f `
        $DurationSeconds, ($producerStart / 1MB), ($producer.WorkingSet64 / 1MB), `
        ($compositorStart / 1MB), ($compositor.WorkingSet64 / 1MB))
}
finally {
    Stop-Demo
}
