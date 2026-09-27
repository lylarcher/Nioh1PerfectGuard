# Launch Nioh: Complete Edition, inject the hardware-breakpoint probe, and
# optionally capture a decrypted memory dump.
#
# Nothing is written into the game folder: the probe is injected from outside with
# CreateRemoteThread, so it can be attached at any time, including mid-mission.
#
# Modes
#   observe   (default) pure logging: confirm which code runs when you block
#   freeguard           observe + zero the guard Ki cost, so blocking costs no Ki.
#                       This is a real, playable prototype of the mod's first
#                       feature and it settles the anchor question at the same time.
#
# Usage:
#   pwsh -File tools\probe_session.ps1                  # observe
#   pwsh -File tools\probe_session.ps1 -Mode freeguard
#   pwsh -File tools\probe_session.ps1 -Dump
param(
    [ValidateSet('observe', 'freeguard')][string]$Mode = 'observe',
    [switch]$Dump,
    [string]$GameDir = '',
    [string]$Root    = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$work  = Join-Path $Root '_work'
$tools = $PSScriptRoot
New-Item -ItemType Directory -Force -Path $work | Out-Null

function Get-Nioh { Get-Process -Name nioh -ErrorAction SilentlyContinue | Select-Object -First 1 }

$proc = Get-Nioh
if (-not $proc) {
    Write-Host "[1] launching nioh.exe with SteamAppId=485510"
    $env:SteamAppId  = '485510'
    $env:SteamGameId = '485510'
    Start-Process -FilePath (Join-Path $GameDir 'nioh.exe') `
        -ArgumentList '--disable-d3d-debug' -WorkingDirectory $GameDir | Out-Null
    Write-Host "[2] waiting for startup"
    Start-Sleep -Seconds 32
    $proc = Get-Nioh
} else {
    Write-Host "[1] using running nioh.exe (pid=$($proc.Id))"
}
if (-not $proc) { throw "nioh.exe is not running" }

$actionLine = ''
if ($Mode -eq 'freeguard') {
    # Target3 is the 'call 0x7B4C40' that subtracts the guard Ki cost.
    # Zeroing xmm1 (the cost in a float register) makes the subtraction a no-op.
    $actionLine = 'Action3=xmm1=0.0'
}

$ini = Join-Path $work 'probe.ini'
@"
[Probe]
Module=nioh.exe
; --- breakpoints (max 4: DR0..DR3) ---
; t1 guard flag write #1:  [[char+0x40]+0x90]+0xC8 = 1
Target1=0x74DD09
; t2 guard flag write #2 (the other guard branch in the same resolver)
Target2=0x74DD92
; t3 the guard Ki-cost subtraction:  xmm1 = [entry+0x10] * 0.2 ; call 0x7B4C40
Target3=0x74DE51
; t4 the script button query QueryButton(ctx, mask, charIdx). Logging rdx/r8d here
;    reveals which button masks the game polls - press guard and the mask for it
;    appears, which is exactly the missing guardButtonIndex for anchor A.
Target4=0x6ECC00
$actionLine
MaxHits=40000
PerTargetCap=300
; --- memory watches (sampled ~6/s, logged only on change) ---
; player slot table -> player object, first 24 bytes: NULL until you are in a mission
Watch1=player_slot0;0x18A0490;+0x0;24
; [[player+0x240]+0xBA8] is the resource table; entry 7 is 0x50 bytes:
;   +0x00 enabled, +0x0C current value (this is what a guard subtracts), +0x10 cost
Watch2=guard_res7;0x18A0490;+0x240;#0xBA8;#0x238;16
; [[player+0x240]+0x40] is the (cur,max) pair used by the RecoverStamina node
Watch3=gauge40;0x18A0490;+0x240;#0x40;16
Log=probe.log
"@ | Set-Content -LiteralPath $ini -Encoding ascii
Write-Host "[3] wrote $ini  (mode=$Mode)"
Get-Content $ini | Where-Object { $_ -match '^(Target|Action|Watch)' } | ForEach-Object { "     $_" }

$log = Join-Path $work 'probe.log'
if (Test-Path $log) {
    Move-Item -Force $log (Join-Path $work ("probe.{0:yyyyMMdd-HHmmss}.log" -f (Get-Date)))
}

Write-Host "[4] injecting probe.dll into pid $($proc.Id)"
python (Join-Path $tools 'inject_dll.py') --pid $proc.Id --dll (Join-Path $work 'probe.dll')

if ($Dump) {
    Write-Host "[5] dumping decrypted image (best done while in a mission)"
    python (Join-Path $tools 'dump_module.py') --pid $proc.Id --wait-decrypted `
        --out (Join-Path $work 'nioh1.mem.exe')
}

Write-Host ""
Write-Host "================= NOW JUST PLAY ================="
Write-Host " The probe is armed already and stays quiet until you"
Write-Host " are actually in a mission with enemies."
Write-Host ""
Write-Host "  1. get into a mission (no save exists yet, so New Game"
Write-Host "     and play the opening until you can move/attack/block)"
Write-Host "  2. stand still 5s              (idle baseline)"
Write-Host "  3. attack 5 times              (Ki spending)"
Write-Host "  4. get hit WITHOUT guarding    (baseline)"
Write-Host "  5. hold guard, get hit 5x      (BLOCKED path)"
Write-Host "  6. tap guard as they hit you   (TIMELY GUARD)"
Write-Host "  7. stand still 5s              (closing baseline)"
if ($Mode -eq 'freeguard') {
    Write-Host ""
    Write-Host " >>> FREEGUARD MODE IS ON: blocking should now cost NO Ki."
    Write-Host "     Watch your Ki bar while blocking - if it stays put,"
    Write-Host "     the anchor is confirmed and the mod's core works."
}
Write-Host ""
Write-Host " Leave 2-3s between steps so the log segments cleanly,"
Write-Host " then tell me and I will summarise."
Write-Host ""
Write-Host " Log: $log"
Write-Host "================================================="
