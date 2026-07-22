[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\bin\$Configuration"
$producer = Join-Path $bin 'streaming_browser.exe'
$viewer = Join-Path $bin 'streaming_viewer.exe'
$fixture = (Resolve-Path (Join-Path $root 'tests\fixtures\feedback-slider.html')).Path
$url = [Uri]::new($fixture).AbsoluteUri

if (-not (Test-Path $producer) -or -not (Test-Path $viewer)) {
    throw "Build $Configuration first using .\scripts\build.ps1 -Configuration $Configuration"
}

Get-Process streaming_browser, streaming_viewer -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

Start-Process -FilePath $producer -WorkingDirectory $bin -ArgumentList @(
    "--url=$url",
    '--force-transparency',
    '--visible'
)
Start-Process -FilePath $viewer -WorkingDirectory $bin

Write-Host 'Feedback demo started. Drag the slider in the viewer to test the full interaction loop.'
