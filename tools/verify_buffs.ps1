<#
  verify_buffs.ps1 —— 一条命令做「精防限时增益」的实机验证

  为什么要这个脚本：
  限时增益是本 MOD 唯一会**调用游戏代码**的功能（见 RE_NOTES §4.52）。
  它的验证需要"装新 DLL → 启动游戏 → 等标题画面演示模式自己打出精防 → 看日志/看有没有崩"，
  步骤多、要盯两三分钟，而且必须复现得一模一样才有比较价值。所以把它脚本化。

  用法（游戏必须已退出）：
      .\tools\verify_buffs.ps1                 # 默认最多等 180 秒
      .\tools\verify_buffs.ps1 -WaitSeconds 240
      .\tools\verify_buffs.ps1 -KeepRunning    # 验完不关游戏

  退出码：0 = 通过；1 = 失败（有崩溃/进程死亡）；2 = 前置条件不满足（游戏还在跑等）。

  注意：脚本会**在 [PerfectGuard] 段里把这两个增益临时打开**（默认 0 = 关闭），
  验完会把 INI 恢复原样。改动只发生在游戏 mods 目录里的那一份副本上。
#>
param(
    [int]$WaitSeconds = 180,
    [switch]$KeepRunning,
    [string]$GameDir = 'E:\SteamLibrary\steamapps\common\Nioh',
    [int]$SteamAppId = 485510
)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$modDir = Join-Path $GameDir 'mods\Nioh1PerfectGuard'
$log = Join-Path $modDir 'Nioh1PerfectGuard.gameplay.log'
$ini = Join-Path $modDir 'Nioh1PerfectGuard.ini'
$exe = Join-Path $GameDir 'nioh.exe'
$crashDir = Join-Path $env:LOCALAPPDATA 'CrashDumps'

function Say([string]$m)  { Write-Host "   $m" }
function Ok([string]$m)   { Write-Host "   [ok] $m"   -ForegroundColor Green }
function Bad([string]$m)  { Write-Host "   [BAD] $m"  -ForegroundColor Red }
function Head([string]$m) { Write-Host "`n== $m" -ForegroundColor White }

Head '前置检查'
if (Get-Process -Name nioh -ErrorAction SilentlyContinue) {
    Bad '游戏正在运行 —— 请先退出游戏（DLL 被占用，无法安装）'
    Say '如果本机的 nioh.exe 结束不掉（Access is denied，可能被保护/提权），'
    Say '请用任务管理器以管理员身份结束，或改到另一台机器上跑这个脚本。'
    exit 2
}
if (-not (Test-Path -LiteralPath $exe)) { Bad "找不到 $exe"; exit 2 }
if (-not (Test-Path -LiteralPath $modDir)) { Bad "找不到 $modDir（先跑一次 build.ps1 -Install）"; exit 2 }
Ok "游戏目录：$GameDir"

Head '安装当前构建'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'build.ps1') -Install -GameDir $GameDir
if ($LASTEXITCODE -ne 0) { Bad '安装/构建失败，已中止'; exit 2 }
if (-not (Test-Path -LiteralPath $ini)) { Bad "安装后仍找不到 $ini"; exit 2 }

# 把两个增益临时打开（默认是 0 = 关闭），并记下原值以便还原
Head '临时打开限时增益'
$iniOrig = Get-Content -LiteralPath $ini -Raw
$iniBak = "$ini.verifybak"
Copy-Item -LiteralPath $ini -Destination $iniBak -Force
$patched = $iniOrig -replace '(?m)^SpeedBuffPercent\s*=.*$', 'SpeedBuffPercent=4' `
                    -replace '(?m)^DamageCutPercent\s*=.*$', 'DamageCutPercent=4'
if ($patched -notmatch '(?m)^SpeedBuffPercent=4$') { Bad 'INI 里没有 SpeedBuffPercent 这一行'; }
[System.IO.File]::WriteAllText($ini, $patched, (New-Object System.Text.UTF8Encoding($false)))
Ok 'SpeedBuffPercent=4，DamageCutPercent=4（验完自动还原）'

# 记录日志起点与崩溃目录状态，这样"这次新出现的东西"才可判定
$logStart = 0
if (Test-Path -LiteralPath $log) { $logStart = (Get-Item -LiteralPath $log).Length }
$crashBefore = @()
if (Test-Path -LiteralPath $crashDir) {
    $crashBefore = Get-ChildItem -LiteralPath $crashDir -Filter 'nioh.exe.*.dmp' -ErrorAction SilentlyContinue |
                   Select-Object -ExpandProperty Name
}
$launchAt = Get-Date

Head '启动游戏（Steam 上下文）'
$env:SteamAppId = "$SteamAppId"
$env:SteamGameId = "$SteamAppId"
$proc = Start-Process -FilePath $exe -ArgumentList '--disable-d3d-debug' -WorkingDirectory $GameDir -PassThru
Say ("pid={0}，最多等 {1} 秒（标题画面演示模式会自己打出精防，不需要操作）" -f $proc.Id, $WaitSeconds)

$seen = @{}
$verdict = 'TIMEOUT'
$deadline = (Get-Date).AddSeconds($WaitSeconds)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    if (-not (Get-Process -Id $proc.Id -ErrorAction SilentlyContinue)) { $verdict = 'DIED'; break }
    if (-not (Test-Path -LiteralPath $log)) { continue }
    $text = ''
    try {
        $fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
        $fs.Seek([Math]::Min($logStart, $fs.Length), 'Begin') | Out-Null
        $sr = New-Object System.IO.StreamReader($fs)
        $text = $sr.ReadToEnd(); $sr.Close(); $fs.Close()
    } catch { continue }
    foreach ($k in @('BUFF engine functions verified', 'BUFF engine functions MISMATCH',
                     'container check: present', 'container check: NOT FOUND',
                     'after removal the state is gone', 'STILL PRESENT',
                     'BUFF speed start', 'BUFF dmgcut start', 'BUFF armor start',
                     'BUFF dmgcut end', 'PERFECT GUARD')) {
        if ($text -match [regex]::Escape($k)) { $seen[$k] = $true }
    }
    # 判定用**读回容器**得到的强信号，而不是 add() 的返回值
    if ($seen.ContainsKey('STILL PRESENT')) { $verdict = 'STUCK'; break }
    if ($seen.ContainsKey('after removal the state is gone')) { $verdict = 'DONE'; break }
    if ($seen.ContainsKey('container check: present')) {
        # 已证明引擎收下了：再多等 15 秒确认"到期能移除"，然后收工
        $deadline = (Get-Date).AddSeconds(15)
    }
}

Head '结果'
$crashNew = @()
if (Test-Path -LiteralPath $crashDir) {
    $crashNew = Get-ChildItem -LiteralPath $crashDir -Filter 'nioh.exe.*.dmp' -ErrorAction SilentlyContinue |
                Where-Object { $crashBefore -notcontains $_.Name } | Select-Object -ExpandProperty Name
}
$alive = [bool](Get-Process -Id $proc.Id -ErrorAction SilentlyContinue)

if ($crashNew.Count -gt 0) { $verdict = 'CRASH' }

switch ($verdict) {
    'DONE' {
        Ok '增益走完了完整生命周期，而且**读回容器确认**过：装上时 present、到期后 gone'
        if ($seen.ContainsKey('container check: NOT FOUND')) {
            Say '注意：安装时容器里没找到（布局推断可能不对）—— 这不算失败，但要告诉我'
        }
        Say '请肉眼确认：带增益时移速更快、被打掉血约为原来的 96%'
    }
    'STUCK' {
        Bad '增益到期了但状态对象**还在容器里**（移除没生效，增益会一直留着）—— 请把日志发回'
    }
    'CRASH' {
        Bad "游戏崩溃了，新崩溃转储：$($crashNew -join ', ')"
        Say '取证：python tools\analyze_dump.py "%LOCALAPPDATA%\CrashDumps\<那个 dmp>"'
        Say '脚本会打印异常地址与 Dr0-Dr7/Rip，足以定位是哪一次引擎调用出问题'
    }
    'DIED'  { Bad '游戏进程消失了，但没看到新的崩溃转储（可能被 Steam 回收或其它原因）' }
    default { Bad "等待 $WaitSeconds 秒内没有走完流程（可能是没等到精防，或增益没启动）" }
}
Say ''
Say ('看到的信号：' + (($seen.Keys | Sort-Object) -join ' / '))

Say ''
Say '本次新增的相关日志：'
if (Test-Path -LiteralPath $log) {
    Get-Content -LiteralPath $log | Select-String -Pattern 'BUFF |PERFECT GUARD|ANCHOR 4/4|STATUS ' |
        Select-Object -Last 20 | ForEach-Object { Say ('  ' + $_.Line) }
}

Head '还原 INI'
if (Test-Path -LiteralPath $iniBak) {
    Copy-Item -LiteralPath $iniBak -Destination $ini -Force
    Remove-Item -LiteralPath $iniBak -Force
    Ok 'INI 已还原（两个增益回到原来的设置）'
}
if (-not $KeepRunning -and $alive) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Say '已结束游戏进程（-KeepRunning 可保留）'
}

if ($verdict -eq 'DONE') { exit 0 } else { exit 1 }
