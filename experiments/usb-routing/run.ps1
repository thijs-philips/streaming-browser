[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$Root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$BuildDir = Join-Path $Root 'build\usb-routing'
$GameInputDir = Join-Path $BuildDir '_deps\gameinput-3.4.259'
$GameInputArchive = Join-Path $BuildDir '_deps\Microsoft.GameInput.3.4.259.nupkg'

if (-not (Test-Path (Join-Path $GameInputDir 'native\include\GameInput.h'))) {
    New-Item (Split-Path $GameInputArchive) -ItemType Directory -Force | Out-Null
    Invoke-WebRequest `
        -Uri 'https://api.nuget.org/v3-flatcontainer/microsoft.gameinput/3.4.259/microsoft.gameinput.3.4.259.nupkg' `
        -OutFile $GameInputArchive
    $actualHash = (Get-FileHash $GameInputArchive -Algorithm SHA256).Hash
    $expectedHash = 'D763A7CC2F72768B6206A2714681DEAC369FC87C4A8351689D051A22D9A2678E'
    if ($actualHash -ne $expectedHash) {
        Remove-Item $GameInputArchive -Force
        throw "Microsoft.GameInput package hash mismatch: $actualHash"
    }
    New-Item $GameInputDir -ItemType Directory -Force | Out-Null
    Expand-Archive $GameInputArchive $GameInputDir -Force
}

if (-not $SkipBuild) {
    cmake -S $PSScriptRoot -B $BuildDir -G 'Visual Studio 17 2022' -A x64
    if ($LASTEXITCODE -ne 0) { throw 'USB routing CMake configure failed' }

    cmake --build $BuildDir --config $Configuration --parallel 20
    if ($LASTEXITCODE -ne 0) { throw 'USB routing build failed' }

    ctest --test-dir $BuildDir -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'USB routing tests failed' }
}

$Executable = Join-Path $BuildDir "bin\$Configuration\usb_routing_probe.exe"
if (-not (Test-Path $Executable)) {
    throw "USB routing probe not found: $Executable"
}

& $Executable
exit $LASTEXITCODE
