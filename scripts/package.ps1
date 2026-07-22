[CmdletBinding()]
param(
    [string]$Version = '0.1.0'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$release = Join-Path $root 'build\bin\Release'
$out = Join-Path $root 'out'
$packageName = "streaming-browser-$Version-windows-x64"
$package = Join-Path $out $packageName
$archive = Join-Path $out "$packageName.zip"
$cefRoot = Join-Path $root 'third_party\cef\cef_binary_150.0.14+g7c1aa68+chromium-150.0.7871.129_windows64'
$yamlCppLicense = Join-Path $root 'build\_deps\yaml-cpp-src\LICENSE'

if (-not (Test-Path (Join-Path $release 'streaming_browser.exe'))) {
    throw 'Build Release first using .\scripts\build.ps1 -Configuration Release'
}

Remove-Item $package -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $archive -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $package -Force | Out-Null
Copy-Item (Join-Path $release '*') $package -Recurse -Force
Remove-Item (Join-Path $package '*.pdb') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $package 'configuration_tests.exe') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $package 'protocol_tests.exe') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $package 'pipe_tests.exe') -Force -ErrorAction SilentlyContinue

Copy-Item (Join-Path $cefRoot 'LICENSE.txt') (Join-Path $package 'CEF_LICENSE.txt')
Copy-Item (Join-Path $cefRoot 'CREDITS.html') $package
Copy-Item $yamlCppLicense (Join-Path $package 'YAML_CPP_LICENSE.txt')
Copy-Item (Join-Path $root 'PROGRESS.md') $package
Copy-Item (Join-Path $root 'FINAL_REPORT.md') $package
Copy-Item (Join-Path $root 'streaming-browser.example.yaml') $package
New-Item -ItemType Directory -Path (Join-Path $package 'fixtures') -Force | Out-Null
Copy-Item (Join-Path $root 'tests\fixtures\*.html') (Join-Path $package 'fixtures')

@'
$ErrorActionPreference = 'Stop'
& icacls.exe $PSScriptRoot /grant '*S-1-15-2-2:(OI)(CI)(RX)' | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Failed to apply CEF sandbox LPAC ACL" }
Write-Host 'Sandbox permissions prepared.'
'@ | Set-Content (Join-Path $package 'prepare-sandbox.ps1') -Encoding UTF8

@'
$ErrorActionPreference = 'Stop'
$fixture = (Resolve-Path (Join-Path $PSScriptRoot 'fixtures\frame-stress.html')).Path
$url = [Uri]::new($fixture).AbsoluteUri
Get-Process streaming_browser, streaming_viewer -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Process (Join-Path $PSScriptRoot 'streaming_browser.exe') `
    -WorkingDirectory $PSScriptRoot `
    -ArgumentList @("--url=$url", '--force-transparency', '--visible')
Start-Process (Join-Path $PSScriptRoot 'streaming_viewer.exe') `
    -WorkingDirectory $PSScriptRoot
'@ | Set-Content (Join-Path $package 'run-demo.ps1') -Encoding UTF8

@'
[CmdletBinding()]
param([string]$Config = 'streaming-browser.example.yaml')
$ErrorActionPreference = 'Stop'
$configPath = if ([IO.Path]::IsPathRooted($Config)) {
    (Resolve-Path $Config).Path
} else {
    (Resolve-Path (Join-Path $PSScriptRoot $Config)).Path
}
Get-Process streaming_browser, streaming_viewer -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Process (Join-Path $PSScriptRoot 'streaming_browser.exe') `
    -WorkingDirectory $PSScriptRoot `
    -ArgumentList "--config=`"$configPath`""
Start-Process (Join-Path $PSScriptRoot 'streaming_viewer.exe') `
    -WorkingDirectory $PSScriptRoot `
    -ArgumentList "--config=`"$configPath`""
'@ | Set-Content (Join-Path $package 'run-config-demo.ps1') -Encoding UTF8

& (Join-Path $package 'prepare-sandbox.ps1')
Compress-Archive -Path (Join-Path $package '*') -DestinationPath $archive -CompressionLevel Optimal
Write-Host "Package created: $archive"
