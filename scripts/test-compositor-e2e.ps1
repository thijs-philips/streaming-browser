[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [int]$StartupTimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'
if (-not ('CompositorE2ENativeV3' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CompositorE2ENativeV3 {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")]
    public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr window, uint message,
        IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr window, IntPtr insertAfter,
        int x, int y, int width, int height, uint flags);
}
'@
}
[CompositorE2ENativeV3]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$root = Split-Path -Parent $PSScriptRoot
$log = Join-Path $env:LOCALAPPDATA 'StreamingBrowser\app.log'
$delay = [Threading.ManualResetEvent]::new($false)

function Stop-Demo {
    Get-Process streaming_browser, streaming_viewer, streaming_compositor -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

try {
    Stop-Demo
    Remove-Item $log -Force -ErrorAction SilentlyContinue
    & (Join-Path $PSScriptRoot 'run-compositor-demo.ps1') -Configuration $Configuration
    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    $compositor = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        $compositor = Get-Process streaming_compositor -ErrorAction SilentlyContinue |
            Where-Object { $_.MainWindowTitle -match 'overlay connected' -and $_.Responding } |
            Select-Object -First 1
        if ($compositor -and (Test-Path $log)) {
            $contents = Get-Content $log -Raw
            if ($contents -match 'Layout WebSocket client connected' -and
                $contents -match 'Compositor applied layout revision \d+ with 6 viewports' -and
                $contents -match 'Compositor layout sources: FP1 \.\.\. FP6') {
                break
            }
        }
        $null = $delay.WaitOne(100)
    }
    if (-not $compositor) { throw 'Compositor did not connect to the overlay producer' }
    $before = Get-Content $log -Raw
    $matches = [regex]::Matches($before, 'Compositor applied layout revision (\d+)')
    if ($matches.Count -eq 0) { throw 'Initial layout snapshot was not applied' }
    $revision = [int]$matches[$matches.Count - 1].Groups[1].Value

    # Local storage can contain a prior manual layout. Click the overlay's
    # Presets/reset button before targeting the known default separator.

    $rect = [CompositorE2ENativeV3+RECT]::new()
    if (-not [CompositorE2ENativeV3]::GetClientRect(
            $compositor.MainWindowHandle, [ref]$rect)) {
        throw 'Could not read compositor client rectangle'
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    $scale = [Math]::Min($width / 3840.0, $height / 2160.0)
    $offsetX = ($width - 3840.0 * $scale) / 2.0
    $offsetY = ($height - 2160.0 * $scale) / 2.0
    function Convert-Point([double]$PageX, [double]$PageY) {
        $x = [int][Math]::Round($offsetX + $PageX * $scale)
        $y = [int][Math]::Round($offsetY + $PageY * $scale)
        return [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
    }
    $reset = Convert-Point 335 34
    [CompositorE2ENativeV3]::SendMessage(
        $compositor.MainWindowHandle, 0x0201, [IntPtr]1, $reset) | Out-Null
    [CompositorE2ENativeV3]::SendMessage(
        $compositor.MainWindowHandle, 0x0202, [IntPtr]0, $reset) | Out-Null
    $null = $delay.WaitOne(300)
    $start = Convert-Point 1337 540
    [CompositorE2ENativeV3]::SendMessage(
        $compositor.MainWindowHandle, 0x0201, [IntPtr]1, $start) | Out-Null
    for ($step = 1; $step -le 20; $step++) {
        $point = Convert-Point (1337 + $step * 12) 540
        [CompositorE2ENativeV3]::SendMessage(
            $compositor.MainWindowHandle, 0x0200, [IntPtr]1, $point) | Out-Null
        $null = $delay.WaitOne(10)
    }
    [CompositorE2ENativeV3]::SendMessage(
        $compositor.MainWindowHandle, 0x0202, [IntPtr]0,
        (Convert-Point 1577 540)) | Out-Null

    $dragDeadline = [DateTime]::UtcNow.AddSeconds(10)
    $nextRevision = $revision
    while ([DateTime]::UtcNow -lt $dragDeadline) {
        $contents = Get-Content $log -Raw
        $matches = [regex]::Matches($contents, 'Compositor applied layout revision (\d+)')
        if ($matches.Count -gt 0) {
            $nextRevision = [int]$matches[$matches.Count - 1].Groups[1].Value
        }
        if ($nextRevision -gt $revision) { break }
        $null = $delay.WaitOne(100)
    }
    if ($nextRevision -le $revision) {
        throw 'Divider drag did not produce a newer compositor layout revision'
    }

    # F12 toggles server scaling. The producer keeps its shared ring allocated
    # at 4K and only resizes the CEF view into a sub-rectangle, so no stream
    # reset happens; ResizeObserver sends a fresh normalized layout snapshot.
    $contents = Get-Content $log -Raw
    $baselineGenerations = ([regex]::Matches($contents,
        'creating a new stream generation')).Count
    [CompositorE2ENativeV3]::SendMessage(
        $compositor.MainWindowHandle, 0x0100, [IntPtr]0x7B,
        [IntPtr]1) | Out-Null
    [CompositorE2ENativeV3]::SendMessage(
        $compositor.MainWindowHandle, 0x0101, [IntPtr]0x7B,
        [IntPtr]0xC0000001) | Out-Null
    $serverDeadline = [DateTime]::UtcNow.AddSeconds(20)
    $serverScaled = $false
    $serverWidth = $width
    $serverHeight = $height
    while ([DateTime]::UtcNow -lt $serverDeadline) {
        $contents = Get-Content $log -Raw
        if ($contents -match "Compositor switched to server scaling; requested CEF viewport $($serverWidth)x$($serverHeight)" -and
            $contents -match "Compositor received CEF frame $($serverWidth)x$($serverHeight) for client $($serverWidth)x$($serverHeight)") {
            $serverScaled = $true
            break
        }
        $null = $delay.WaitOne(100)
    }
    if (-not $serverScaled) { throw 'F12 did not activate server scaling' }

    # Server mode is persistent: exercise large aspect-ratio changes and
    # require the delivered frame—not only the request—to converge exactly.
    $resizeCases = @(
        @{ Name = 'wide'; Width = 1200; Height = 400 },
        @{ Name = 'tall'; Width = 400; Height = 900 },
        @{ Name = 'regular'; Width = 1000; Height = 700 }
    )
    foreach ($case in $resizeCases) {
        if (-not [CompositorE2ENativeV3]::SetWindowPos(
                $compositor.MainWindowHandle, [IntPtr]::Zero, 0, 0,
                $case.Width, $case.Height, 0x0002 -bor 0x0004)) {
            throw "Could not apply $($case.Name) compositor resize"
        }
        $resizeDeadline = [DateTime]::UtcNow.AddSeconds(20)
        $resized = $false
        while ([DateTime]::UtcNow -lt $resizeDeadline) {
            $currentRect = [CompositorE2ENativeV3+RECT]::new()
            [CompositorE2ENativeV3]::GetClientRect(
                $compositor.MainWindowHandle, [ref]$currentRect) | Out-Null
            $currentWidth = $currentRect.Right - $currentRect.Left
            $currentHeight = $currentRect.Bottom - $currentRect.Top
            $contents = Get-Content $log -Raw
            if ($contents -match "Compositor switched to server scaling; requested CEF viewport $($currentWidth)x$($currentHeight)" -and
                $contents -match "Compositor received CEF frame $($currentWidth)x$($currentHeight) for client $($currentWidth)x$($currentHeight)") {
                $resized = $true
                break
            }
            $null = $delay.WaitOne(100)
        }
        if (-not $resized) {
            throw "Server scaling did not converge after $($case.Name) resize"
        }
    }

    [CompositorE2ENativeV3]::SendMessage(
        $compositor.MainWindowHandle, 0x0100, [IntPtr]0x7B,
        [IntPtr]1) | Out-Null
    [CompositorE2ENativeV3]::SendMessage(
        $compositor.MainWindowHandle, 0x0101, [IntPtr]0x7B,
        [IntPtr]0xC0000001) | Out-Null
    $clientDeadline = [DateTime]::UtcNow.AddSeconds(20)
    $clientScaled = $false
    while ([DateTime]::UtcNow -lt $clientDeadline) {
        $contents = Get-Content $log -Raw
        if ($contents -match 'Compositor switched to client scaling; requested CEF viewport 3840x2160') {
            $clientScaled = $true
            break
        }
        $null = $delay.WaitOne(100)
    }
    if (-not $clientScaled) { throw 'F12 did not restore client scaling' }

    # The persistent 4K ring must survive every server-scaling resize without
    # a stream reset/reconnect (that is what made resizing jerky before).
    $contents = Get-Content $log -Raw
    $finalGenerations = ([regex]::Matches($contents,
        'creating a new stream generation')).Count
    if ($finalGenerations -gt $baselineGenerations) {
        throw 'Server scaling caused a stream generation change; the persistent ring should absorb sub-4K resizes'
    }

    Write-Host "Compositor E2E passed: layout revision $revision -> $nextRevision; smooth server-scaling resize (no stream reset) and F12 client restore passed"
}
finally {
    Stop-Demo
}
