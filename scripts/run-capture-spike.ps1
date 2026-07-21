[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [string]$Url = 'https://example.com',
    [switch]$ForceTransparency
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\bin\$Configuration\streaming_browser.exe"
if (-not (Test-Path $exe)) { throw "Build the producer first: $exe" }
$args = @("--url=$Url")
if ($ForceTransparency) { $args += '--force-transparency' }
& $exe @args
