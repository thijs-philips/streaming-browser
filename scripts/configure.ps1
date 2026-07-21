[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string]$Architecture = 'x64',
    [switch]$NoTests
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$testOption = if ($NoTests) { 'OFF' } else { 'ON' }

cmake -S $root -B (Join-Path $root 'build') `
    -G 'Visual Studio 17 2022' -A $Architecture `
    "-DSTREAMING_BROWSER_BUILD_TESTS=$testOption"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE" }
