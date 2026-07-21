[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$fixturePath = (Resolve-Path (Join-Path $root 'tests\fixtures\frame-stress.html')).Path
$fixtureUrl = [System.Uri]::new($fixturePath).AbsoluteUri

& (Join-Path $PSScriptRoot 'run-capture-spike.ps1') `
    -Configuration $Configuration `
    -Url $fixtureUrl `
    -ForceTransparency `
    -Visible
