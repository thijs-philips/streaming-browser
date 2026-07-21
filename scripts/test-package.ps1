[CmdletBinding()]
param(
    [string]$Version = '0.1.0',
    [int]$TimeoutSeconds = 45
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$package = Join-Path $root "out\streaming-browser-$Version-windows-x64"
$delay = [Threading.ManualResetEvent]::new($false)

function Stop-DemoProcesses {
    Get-Process streaming_browser, streaming_viewer -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path (Join-Path $package 'streaming_browser.exe'))) {
    throw "Package directory not found: $package"
}

Stop-DemoProcesses
try {
    & (Join-Path $package 'prepare-sandbox.ps1')
    & (Join-Path $package 'run-demo.ps1')
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $viewer = Get-Process streaming_viewer -ErrorAction SilentlyContinue |
            Where-Object { $_.MainWindowTitle -match 'frame (\d+)' -and $_.Responding } |
            Select-Object -First 1
        if ($viewer) {
            Write-Host "Packaged demo passed: $($viewer.MainWindowTitle)"
            return
        }
        $null = $delay.WaitOne(100)
    }
    throw 'Packaged viewer did not receive a frame'
}
finally {
    Stop-DemoProcesses
}
