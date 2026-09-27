# Send synthetic keyboard input to the Nioh window (focus it first).
# Used to drive menus / gameplay without a human at the keyboard.
#
# Usage:
#   pwsh -File tools\keys.ps1 -Keys ENTER
#   pwsh -File tools\keys.ps1 -Keys W -Hold 1200          # hold W for 1.2s
#   pwsh -File tools\keys.ps1 -Keys "SPACE,ENTER" -Delay 400
param(
    [Parameter(Mandatory=$true)][string[]]$Keys,
    [int]$Hold = 60,
    [int]$Delay = 300,
    [int]$Repeat = 1,
    [string]$WhatWindow = 'nioh'
)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class K {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
"@

$p = Get-Process -Name $WhatWindow -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Host "game window not found"; exit 1 }
if ([K]::IsIconic($p.MainWindowHandle)) { [K]::ShowWindow($p.MainWindowHandle, 9) | Out-Null }
[K]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 500

$KEYUP = 0x0002
$map = @{
    'ENTER'=0x0D; 'ESC'=0x1B; 'SPACE'=0x20; 'TAB'=0x09; 'BACK'=0x08
    'UP'=0x26; 'DOWN'=0x28; 'LEFT'=0x25; 'RIGHT'=0x27
    'W'=0x57; 'A'=0x41; 'S'=0x53; 'D'=0x44
    'E'=0x45; 'Q'=0x51; 'F'=0x46; 'R'=0x52
    'J'=0x4A; 'K'=0x4B; 'L'=0x4C; 'I'=0x49; 'O'=0x4F; 'P'=0x50
    'SHIFT'=0x10; 'CTRL'=0x11; 'ALT'=0x12
    '1'=0x31; '2'=0x32; '3'=0x33; '4'=0x34; '5'=0x35
    'F1'=0x70; 'F2'=0x71; 'F10'=0x79
}

for ($r = 0; $r -lt $Repeat; $r++) {
    foreach ($k in $Keys) {
        $name = $k.Trim().ToUpper()
        if (-not $map.ContainsKey($name)) { Write-Host "unknown key: $name"; continue }
        $vk = [byte]$map[$name]
        [K]::keybd_event($vk, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds $Hold
        [K]::keybd_event($vk, 0, $KEYUP, [UIntPtr]::Zero)
        Write-Host "sent $name (hold ${Hold}ms)"
        Start-Sleep -Milliseconds $Delay
    }
}
