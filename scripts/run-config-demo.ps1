[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$Config = 'streaming-browser.example.yaml'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\bin\$Configuration"
$producer = Join-Path $bin 'streaming_browser.exe'
$viewer = Join-Path $bin 'streaming_viewer.exe'
$configPath = if ([IO.Path]::IsPathRooted($Config)) {
    (Resolve-Path $Config).Path
} else {
    (Resolve-Path (Join-Path $root $Config)).Path
}

if (-not (Test-Path $producer) -or -not (Test-Path $viewer)) {
    throw "Build $Configuration first using .\scripts\build.ps1 -Configuration $Configuration"
}

Get-Process streaming_browser, streaming_viewer -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

Start-Process -FilePath $producer -WorkingDirectory $bin `
    -ArgumentList "--config=`"$configPath`""
Start-Process -FilePath $viewer -WorkingDirectory $bin `
    -ArgumentList "--config=`"$configPath`""

Write-Host "Producer and viewer started with YAML configuration: $configPath"
