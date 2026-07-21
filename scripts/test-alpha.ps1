[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\bin\$Configuration"
$producerExe = Join-Path $bin 'streaming_browser.exe'
$fixture = (Resolve-Path (Join-Path $root 'tests\fixtures\alpha.html')).Path
$url = [Uri]::new($fixture).AbsoluteUri
$log = Join-Path $env:LOCALAPPDATA 'StreamingBrowser\app.log'
$delay = [Threading.ManualResetEvent]::new($false)

Get-Process streaming_browser, streaming_viewer -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Remove-Item $log -Force -ErrorAction SilentlyContinue
try {
    $producer = Start-Process -FilePath $producerExe -WorkingDirectory $bin `
        -ArgumentList @("--url=$url", '--force-transparency', '--alpha-probe') `
        -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($producer.HasExited) { throw 'Producer exited during alpha probe' }
        if ((Test-Path $log) -and
            ((Get-Content $log -Raw) -match 'Alpha probe passed')) {
            Write-Host 'Alpha probe passed: opaque, 50%, and transparent pixels preserved.'
            return
        }
        $null = $delay.WaitOne(100)
    }
    throw 'Timed out waiting for alpha probe result'
}
finally {
    Get-Process streaming_browser, streaming_viewer -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}
