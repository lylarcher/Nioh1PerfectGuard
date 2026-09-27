# Capture the Nioh window (or the whole screen) to a PNG so the agent can look at
# what state the game is in. Used to drive the game to a mission without a human.
#
# Usage:  pwsh -File tools\shot.ps1 [-Out <png>] [-FullScreen] [-WhatWindow nioh]
param(
    [string]$Out = '',
    [switch]$FullScreen,
    [string]$WhatWindow = 'nioh'
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@

if (-not $Out) {
    $dir = Join-Path (Split-Path -Parent $PSScriptRoot) '_work\shots'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $Out = Join-Path $dir ("shot-{0:yyyyMMdd-HHmmss}.png" -f (Get-Date))
}

$rect = $null
if (-not $FullScreen) {
    $p = Get-Process -Name $WhatWindow -ErrorAction SilentlyContinue |
         Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($p) {
        if ([W]::IsIconic($p.MainWindowHandle)) { [W]::ShowWindow($p.MainWindowHandle, 9) | Out-Null }
        [W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
        Start-Sleep -Milliseconds 400
        $r = New-Object W+RECT
        if ([W]::GetWindowRect($p.MainWindowHandle, [ref]$r)) { $rect = $r }
        Write-Host "window: $($p.MainWindowTitle)  rect=$($r.L),$($r.T) $($r.R - $r.L)x$($r.B - $r.T)"
    } else {
        Write-Host "no window with handle found for '$WhatWindow'; falling back to full screen"
    }
}

if ($rect -and ($rect.R - $rect.L) -gt 0) {
    $x = $rect.L; $y = $rect.T
    $w = $rect.R - $rect.L; $h = $rect.B - $rect.T
} else {
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $x = $b.X; $y = $b.Y; $w = $b.Width; $h = $b.Height
}

$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size $w, $h))
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "saved: $Out ($w x $h)"
