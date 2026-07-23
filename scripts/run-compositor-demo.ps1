[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$EnableInputRouting,
    [switch]$StartCaptureProbe
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\bin\$Configuration"
$producer = Join-Path $bin 'streaming_browser.exe'
$compositor = Join-Path $bin 'streaming_compositor.exe'
$fixture = (Resolve-Path (Join-Path $root 'tests\fixtures\viewport-manager.html')).Path
$url = ([Uri]::new($fixture).AbsoluteUri + '?mode=overlay')
$compositorConfig = (Resolve-Path (Join-Path $root 'config\compositor.example.yaml')).Path
$producerConfigName = if ($EnableInputRouting) {
    'producer.compositor-input.yaml'
} else {
    'producer.compositor.yaml'
}
$producerConfig = (Resolve-Path (Join-Path $root "config\$producerConfigName")).Path

if (-not (Test-Path $producer) -or -not (Test-Path $compositor)) {
    throw "Build $Configuration first using .\scripts\build.ps1 -Configuration $Configuration"
}

Get-Process streaming_browser, streaming_viewer, streaming_compositor -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

$shutdownDeadline = (Get-Date).AddSeconds(15)
do {
    $remaining = Get-Process streaming_browser -ErrorAction SilentlyContinue
    if (-not $remaining) { break }
    [Threading.ManualResetEvent]::new($false).WaitOne(100) | Out-Null
} while ((Get-Date) -lt $shutdownDeadline)
if (Get-Process streaming_browser -ErrorAction SilentlyContinue) {
    throw 'Previous CEF subprocesses did not stop before compositor launch'
}

$compositorProcess = Start-Process -FilePath $compositor -WorkingDirectory $bin `
    -ArgumentList "--config=`"$compositorConfig`"" -PassThru
$producerProcess = Start-Process -FilePath $producer -WorkingDirectory $bin `
    -ArgumentList @("--config=`"$producerConfig`"", "--url=$url") -PassThru

Write-Host "Compositor demo started (compositor PID $($compositorProcess.Id), producer PID $($producerProcess.Id))."
Write-Host 'Drag separators in the compositor window; source blocks should follow beneath the alpha overlay.'
Write-Host 'Press F11 in the compositor window to toggle borderless fullscreen.'
Write-Host 'Server scaling (responsive CEF) is the default; press F12 to toggle client scaling (fixed 4K CEF).'

if ($StartCaptureProbe) {
    if (-not $EnableInputRouting) {
        throw '-StartCaptureProbe requires -EnableInputRouting'
    }
    & (Join-Path $root 'experiments\usb-routing\run.ps1') `
        -Configuration $Configuration -SkipBuild
}
