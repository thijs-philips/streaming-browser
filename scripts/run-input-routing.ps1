[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$Synthetic
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Bin = Join-Path $Root "build\bin\$Configuration"
$Producer = Join-Path $Bin 'streaming_browser.exe'
$Viewer = Join-Path $Bin 'streaming_viewer.exe'
$Probe = Join-Path $Root "build\usb-routing\bin\$Configuration\usb_routing_probe.exe"
$ProducerConfig = Join-Path $Root 'config\producer.input-routing.yaml'
$ViewerConfig = Join-Path $Root 'config\viewer.browser.yaml'
$Fixture = (Resolve-Path (Join-Path $Root 'tests\fixtures\input-routing.html')).Path
$FixtureUrl = [Uri]::new($Fixture).AbsoluteUri

foreach ($path in @($Producer, $Viewer, $Probe)) {
    if (-not (Test-Path $path)) { throw "Build output not found: $path" }
}

Get-Process streaming_browser, streaming_viewer -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

Start-Process -FilePath $Producer -WorkingDirectory $Bin -ArgumentList @(
    "--config=`"$ProducerConfig`"",
    "--url=$FixtureUrl"
)
Start-Process -FilePath $Viewer -WorkingDirectory $Bin -ArgumentList @(
    "--config=`"$ViewerConfig`""
)

$deadline = (Get-Date).AddSeconds(15)
do {
    if (Get-NetTCPConnection -LocalPort 17831 -State Listen -ErrorAction SilentlyContinue) {
        break
    }
    [Threading.ManualResetEvent]::new($false).WaitOne(100) | Out-Null
} while ((Get-Date) -lt $deadline)

if (-not (Get-NetTCPConnection -LocalPort 17831 -State Listen -ErrorAction SilentlyContinue)) {
    throw 'Producer input-routing WebSocket did not start'
}

if ($Synthetic) {
    & $Probe --synthetic
    exit $LASTEXITCODE
}

Start-Process -FilePath $Probe -WorkingDirectory (Split-Path $Probe)
Write-Host 'USB routing probe started. Route only the intended USB keyboard and mouse.'
Write-Host 'EMERGENCY STOP / DISABLE ROUTING: Ctrl+Alt+Shift+F12 entirely on the routed USB keyboard.'
Write-Host 'The test page also displays this chord and the expected isolation checks.'
