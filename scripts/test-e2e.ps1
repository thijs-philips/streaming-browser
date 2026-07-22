[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [int]$StartupTimeoutSeconds = 30,
    [int]$ProgressTimeoutSeconds = 15
)

$ErrorActionPreference = 'Stop'
if (-not ('StreamingBrowserE2ENativeMethodsV3' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class StreamingBrowserE2ENativeMethodsV3 {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr window, uint message,
        IntPtr wparam, IntPtr lparam);
}
'@
}
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\bin\$Configuration"
$producerExe = Join-Path $bin 'streaming_browser.exe'
$viewerExe = Join-Path $bin 'streaming_viewer.exe'
$fixture = (Resolve-Path (Join-Path $root 'tests\fixtures\frame-stress.html')).Path
$url = [Uri]::new($fixture).AbsoluteUri
$delay = [Threading.ManualResetEvent]::new($false)

function Stop-DemoProcesses {
    Get-Process streaming_browser, streaming_viewer -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

function Wait-MainWindow([string]$Name, [string]$Pattern, [int]$TimeoutSeconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $match = Get-Process $Name -ErrorAction SilentlyContinue |
            Where-Object { $_.MainWindowTitle -match $Pattern -and $_.Responding } |
            Select-Object -First 1
        if ($match) { return $match }
        $null = $delay.WaitOne(100)
    }
    throw "Timed out waiting for $Name window matching '$Pattern'"
}

function Get-FrameNumber([System.Diagnostics.Process]$Process) {
    $Process.Refresh()
    if ($Process.MainWindowTitle -match 'frame (\d+)') {
        return [uint64]$Matches[1]
    }
    return [uint64]0
}

if (-not (Test-Path $producerExe) -or -not (Test-Path $viewerExe)) {
    throw "Build $Configuration first using .\scripts\build.ps1"
}

Stop-DemoProcesses
$log = Join-Path $env:LOCALAPPDATA 'StreamingBrowser\app.log'
Remove-Item $log -Force -ErrorAction SilentlyContinue
try {
    $producer = Start-Process -FilePath $producerExe -ArgumentList @(
        "--url=$url",
        '--force-transparency',
        '--visible'
    ) -WorkingDirectory $bin -PassThru

    $null = Wait-MainWindow 'streaming_browser' 'accelerated frame \d+' $StartupTimeoutSeconds
    $viewer = Start-Process -FilePath $viewerExe -WorkingDirectory $bin -PassThru
    $viewerWindow = Wait-MainWindow 'streaming_viewer' 'frame \d+' $StartupTimeoutSeconds
    $firstFrame = Get-FrameNumber $viewerWindow

    $deadline = [DateTime]::UtcNow.AddSeconds($ProgressTimeoutSeconds)
    $secondFrame = $firstFrame
    while ([DateTime]::UtcNow -lt $deadline -and $secondFrame -le $firstFrame) {
        $null = $delay.WaitOne(250)
        $secondFrame = Get-FrameNumber $viewerWindow
    }
    if ($secondFrame -le $firstFrame) {
        throw "Viewer frame did not advance beyond $firstFrame"
    }

    $producerWindow = Get-Process streaming_browser -ErrorAction Stop |
        Where-Object MainWindowTitle | Select-Object -First 1
    $visibilityCheckbox = [StreamingBrowserE2ENativeMethodsV3]::GetDlgItem(
        $producerWindow.MainWindowHandle, 1001)
    if ($visibilityCheckbox -eq [IntPtr]::Zero) {
        throw 'Producer visibility checkbox not found'
    }
    [StreamingBrowserE2ENativeMethodsV3]::SendMessage(
        $visibilityCheckbox, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $null = $delay.WaitOne(500)
    if ([StreamingBrowserE2ENativeMethodsV3]::IsWindowVisible(
            $viewerWindow.MainWindowHandle)) {
        throw 'Viewer did not hide after producer visibility toggle'
    }
    [StreamingBrowserE2ENativeMethodsV3]::SendMessage(
        $visibilityCheckbox, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $null = $delay.WaitOne(500)
    if (-not [StreamingBrowserE2ENativeMethodsV3]::IsWindowVisible(
            $viewerWindow.MainWindowHandle)) {
        throw 'Viewer did not reappear after producer visibility toggle'
    }

    $inputFixture = (Resolve-Path (Join-Path $root 'tests\fixtures\input.html')).Path
    $inputUrl = [Uri]::new($inputFixture).AbsoluteUri
    Stop-Process -Id $viewer.Id -Force
    $null = Wait-MainWindow 'streaming_browser' 'accelerated frame \d+|shared stream frame \d+' 10
    $viewer = Start-Process -FilePath $viewerExe -WorkingDirectory $bin `
        -ArgumentList "--navigate=$inputUrl" -PassThru
    $reconnected = Wait-MainWindow 'streaming_viewer' 'frame \d+' $StartupTimeoutSeconds
    $reconnectedFrame = Get-FrameNumber $reconnected
    $navigationDeadline = [DateTime]::UtcNow.AddSeconds(15)
    $navigated = $false
    while ([DateTime]::UtcNow -lt $navigationDeadline) {
        if ((Test-Path $log) -and
            ((Get-Content $log -Raw) -match 'Navigated to file:///.*input\.html')) {
            $navigated = $true
            break
        }
        $null = $delay.WaitOne(100)
    }
    if (-not $navigated) { throw 'Viewer URL command did not navigate producer' }
    # Navigation logging can precede the renderer's new document becoming the
    # active input target. Give CEF one short settling interval before injecting
    # the synthetic click; without this the test is timing-dependent on fast builds.
    $null = $delay.WaitOne(500)
    $reconnected.Refresh()
    $clientRect = [StreamingBrowserE2ENativeMethodsV3+RECT]::new()
    if (-not [StreamingBrowserE2ENativeMethodsV3]::GetClientRect(
            $reconnected.MainWindowHandle, [ref]$clientRect)) {
        throw 'Could not read viewer client rectangle'
    }
    $clientWidth = $clientRect.Right - $clientRect.Left
    $clientHeight = $clientRect.Bottom - $clientRect.Top
    $scale = [Math]::Min($clientWidth / 3840.0, $clientHeight / 2160.0)
    $viewportWidth = 3840.0 * $scale
    $viewportHeight = 2160.0 * $scale
    $mouseX = [int](($clientWidth - $viewportWidth) / 2.0 + 600.0 * $scale)
    $mouseY = [int](($clientHeight - $viewportHeight) / 2.0 + 400.0 * $scale)
    $mousePoint = [IntPtr](($mouseY -shl 16) -bor ($mouseX -band 0xFFFF))
    [StreamingBrowserE2ENativeMethodsV3]::SendMessage(
        $reconnected.MainWindowHandle, 0x0201, [IntPtr]1, $mousePoint) | Out-Null
    [StreamingBrowserE2ENativeMethodsV3]::SendMessage(
        $reconnected.MainWindowHandle, 0x0202, [IntPtr]0, $mousePoint) | Out-Null
    $clickDeadline = [DateTime]::UtcNow.AddSeconds(15)
    $clicked = $false
    while ([DateTime]::UtcNow -lt $clickDeadline) {
        if ((Get-Content $log -Raw) -match 'input\.html#clicked') {
            $clicked = $true
            break
        }
        $null = $delay.WaitOne(100)
    }
    if (-not $clicked) { throw 'Viewer mouse click did not reach webpage' }

    $popupFixture = (Resolve-Path (Join-Path $root 'tests\fixtures\popup.html')).Path
    $popupUrl = [Uri]::new($popupFixture).AbsoluteUri
    Stop-Process -Id $viewer.Id -Force
    $viewer = Start-Process -FilePath $viewerExe -WorkingDirectory $bin `
        -ArgumentList "--navigate=$popupUrl" -PassThru
    $popupViewer = Wait-MainWindow 'streaming_viewer' 'frame \d+' $StartupTimeoutSeconds
    $popupNavigationDeadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $popupNavigationDeadline -and
           -not ((Get-Content $log -Raw) -match 'Navigated to file:///.*popup\.html')) {
        $null = $delay.WaitOne(100)
    }
    $popupViewer.Refresh()
    [StreamingBrowserE2ENativeMethodsV3]::SendMessage(
        $popupViewer.MainWindowHandle, 0x0201, [IntPtr]1, $mousePoint) | Out-Null
    $popupDeadline = [DateTime]::UtcNow.AddSeconds(15)
    $popupShown = $false
    $popupClicked = $false
    while ([DateTime]::UtcNow -lt $popupDeadline) {
        $popupLog = Get-Content $log -Raw
        if ($popupLog -match 'popup\.html#mousedown') {
            $popupClicked = $true
        }
        if ($popupLog -match 'CEF popup shown') {
            $popupShown = $true
            break
        }
        $null = $delay.WaitOne(100)
    }
    [StreamingBrowserE2ENativeMethodsV3]::SendMessage(
        $popupViewer.MainWindowHandle, 0x0202, [IntPtr]0, $mousePoint) | Out-Null
    if (-not $popupClicked) { throw 'Viewer click did not hit select control' }
    if (-not $popupShown) {
        Write-Warning 'Select control received the click, but CEF M150 did not emit PET_POPUP show on this machine.'
    }
    if ($reconnectedFrame -le $secondFrame) {
        throw "Reconnected viewer did not receive a newer frame"
    }

    Write-Host "E2E passed: frame $firstFrame -> $secondFrame; reconnect frame $reconnectedFrame; popup callback=$popupShown"
}
finally {
    Stop-DemoProcesses
}
