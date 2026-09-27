# Launch Nioh: Complete Edition and dump its decrypted module image.
#
# Why the env vars: Steam launches nioh_launcher.exe, which passes SteamAppId to
# its children. Launching nioh.exe directly without them makes SteamAPI_Init fail
# and the process exits after ~28s (Steam DRM validation).
#
# Usage:  pwsh -File tools\dump_nioh1.ps1 [-KeepRunning]
param(
    [switch]$KeepRunning,
    [string]$GameDir = 'E:\SteamLibrary\steamapps\common\Nioh',
    [string]$OutDir  = (Join-Path (Split-Path -Parent $PSScriptRoot) '_work')
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$exe = Join-Path $GameDir 'nioh.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "nioh.exe not found at $exe" }

$running = Get-Process -Name nioh -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "[i] nioh.exe already running (pid=$($running[0].Id)); reusing it"
    $proc = $running[0]
} else {
    Write-Host "[1] launching nioh.exe with SteamAppId=485510"
    $env:SteamAppId  = '485510'
    $env:SteamGameId = '485510'
    $proc = Start-Process -FilePath $exe `
        -ArgumentList '--disable-d3d-debug' `
        -WorkingDirectory $GameDir -PassThru
    Write-Host "[2] pid=$($proc.Id), waiting for it to settle"
    Start-Sleep -Seconds 5
}

Write-Host "[3] dumping (polls until the code section stops looking encrypted)"
python (Join-Path $PSScriptRoot 'dump_module.py') `
    --pid $proc.Id --wait-decrypted --wait-timeout 180 `
    --out (Join-Path $OutDir 'nioh1.mem.exe') `
    --raw (Join-Path $OutDir 'nioh1.mem.raw.bin')

if ($LASTEXITCODE -ne 0) { throw "dump failed with exit code $LASTEXITCODE" }

if (-not $KeepRunning) {
    Write-Host "[4] stopping nioh.exe"
    Get-Process -Name nioh -ErrorAction SilentlyContinue | Stop-Process -Force
} else {
    Write-Host "[4] leaving nioh.exe running (-KeepRunning)"
}

Write-Host "[5] done. image: $(Join-Path $OutDir 'nioh1.mem.exe')"
