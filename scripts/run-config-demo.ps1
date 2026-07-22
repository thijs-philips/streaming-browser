[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$ProducerConfig = 'config\producer.example.yaml',
    [string]$ViewerConfig = 'config\viewer.example.yaml'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\bin\$Configuration"
$producer = Join-Path $bin 'streaming_browser.exe'
$viewer = Join-Path $bin 'streaming_viewer.exe'

function Resolve-ConfigPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) {
        return (Resolve-Path $Path).Path
    }
    return (Resolve-Path (Join-Path $root $Path)).Path
}

$producerConfigPath = Resolve-ConfigPath $ProducerConfig
$viewerConfigPath = Resolve-ConfigPath $ViewerConfig

if (-not (Test-Path $producer) -or -not (Test-Path $viewer)) {
    throw "Build $Configuration first using .\scripts\build.ps1 -Configuration $Configuration"
}

Get-Process streaming_browser, streaming_viewer -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

Start-Process -FilePath $producer -WorkingDirectory $bin `
    -ArgumentList "--config=`"$producerConfigPath`""
Start-Process -FilePath $viewer -WorkingDirectory $bin `
    -ArgumentList "--config=`"$viewerConfigPath`""

Write-Host "Producer started with: $producerConfigPath"
Write-Host "Viewer started with:   $viewerConfigPath"
