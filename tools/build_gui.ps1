# =============================================================================
#  仁王1 精防 MOD · 配置界面（GUI）构建脚本 / build script for the config GUI
# =============================================================================
#  这份脚本只负责 GUI，和 MOD 本体的 build.ps1 完全独立（互不影响）：
#
#     powershell -ExecutionPolicy Bypass -File tools\build_gui.ps1
#     powershell ... -File tools\build_gui.ps1 -TestOnly     # 只跑检查，不打包 exe
#     powershell ... -File tools\build_gui.ps1 -Clean        # 先清掉旧的构建产物
#
#  做四件事：
#     1. 检查/准备项目内虚拟环境 .venv（项目约定：Python 依赖一律装在 .venv）
#     2. 跑 GUI 的自检 ini_gui.py --selftest（原文回写必须字节一致）
#     3. 用 PyInstaller 打一个免装 Python 的单文件 exe 到 tools\gui_dist\
#     4. 校验 exe（大小/SHA256/用 --selftest 实测它能自己找到 INI）
#
#  产物按决定**不打包进发布 zip**，只作为开发/用户工具。
# =============================================================================

[CmdletBinding()]
param(
    [switch]$TestOnly,   # 只做环境检查与自检，不构建 exe
    [switch]$Clean       # 构建前清掉 tools\gui_dist 与 _work\pyi
)

$ErrorActionPreference = 'Stop'
$script:steps = 0

function Write-Step([string]$msg) {
    $script:steps++
    Write-Host ('[{0}] {1}' -f $script:steps, $msg) -ForegroundColor Cyan
}
function Write-Ok([string]$msg) { Write-Host ('    [ok]   ' + $msg) -ForegroundColor Green }
function Write-Warn2([string]$msg) { Write-Host ('    [warn] ' + $msg) -ForegroundColor Yellow }
function Fail([string]$msg) {
    Write-Host ('    [FAIL] ' + $msg) -ForegroundColor Red
    Write-Host '构建中止 / build aborted.' -ForegroundColor Red
    exit 1
}

$root    = Split-Path -Parent $PSScriptRoot                     # 仓库根
$gui     = Join-Path $PSScriptRoot 'ini_gui.py'
$py      = Join-Path $root '.venv\Scripts\python.exe'
$outDir  = Join-Path $PSScriptRoot 'gui_dist'
$workDir = Join-Path $root '_work\pyi'
$exeName = 'Nioh1PerfectGuard-Config'
$exePath = Join-Path $outDir ($exeName + '.exe')

Write-Host ''
Write-Host '=== Nioh1PerfectGuard 配置界面构建 / config GUI build ===' -ForegroundColor White

# ---------------------------------------------------------------- 0. 自身编码
Write-Step '脚本自身编码 / script encoding'
$self = $MyInvocation.MyCommand.Path
if ($self -and (Test-Path $self)) {
    $b = [System.IO.File]::ReadAllBytes($self)
    if ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) {
        Write-Ok 'UTF-8 带 BOM（Windows PowerShell 5.1 正确解析中文的前提）'
    } else {
        Write-Warn2 '不是 UTF-8 带 BOM：Windows PowerShell 5.1 下中文可能显示为乱码'
    }
}

# ---------------------------------------------------------------- 1. 源码
Write-Step '源码检查 / source check'
if (-not (Test-Path $gui)) { Fail "找不到 $gui" }
Write-Ok ("ini_gui.py  {0:N0} B" -f (Get-Item $gui).Length)

# ---------------------------------------------------------------- 2. 虚拟环境
Write-Step '虚拟环境 .venv / virtualenv'
if (-not (Test-Path $py)) {
    $base = $null
    foreach ($cand in @('py', 'python')) {
        $c = Get-Command $cand -ErrorAction SilentlyContinue
        if ($c) { $base = $c.Source; break }
    }
    if (-not $base) { Fail '找不到 Python。请先安装 Python 3（勾选 Add to PATH），或手动创建 .venv。' }
    Write-Warn2 ("未找到 .venv，正在用 {0} 创建…" -f $base)
    if ($base -like '*py.exe') { & $base -3 -m venv (Join-Path $root '.venv') } else { & $base -m venv (Join-Path $root '.venv') }
    if (-not (Test-Path $py)) { Fail '.venv 创建失败。' }
}
Write-Ok ("python: {0}" -f (& $py -c "import sys;print(sys.version.split()[0])"))

$tk = & $py -c "import tkinter;print(tkinter.TkVersion)" 2>&1
if ($LASTEXITCODE -ne 0) { Fail '这个 Python 没有 tkinter。请换一个带 tkinter 的 Python 3（官方安装包默认带）。' }
Write-Ok ("tkinter {0}" -f $tk)

# ---------------------------------------------------------------- 3. 语法 + 自检
Write-Step '语法与自检 / syntax + selftest'
& $py -c "import ast,sys;ast.parse(open(sys.argv[1],encoding='utf-8').read())" $gui
if ($LASTEXITCODE -ne 0) { Fail 'ini_gui.py 语法错误。' }
Write-Ok '语法 OK'

& $py $gui --selftest
if ($LASTEXITCODE -ne 0) { Fail '自检失败：未改动时的回写与原文不一致，或改动落错了行。' }
Write-Ok '自检通过（原文回写字节一致 / 改动只影响目标行）'

if ($TestOnly) {
    Write-Host ''
    Write-Host '仅测试模式，未构建 exe。/ test-only, exe not built.' -ForegroundColor Yellow
    exit 0
}

# ---------------------------------------------------------------- 4. PyInstaller
Write-Step 'PyInstaller（装在 .venv 内）/ PyInstaller inside .venv'
$oldEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$ver = (& $py -m PyInstaller --version 2>&1 | Out-String).Trim()
$ErrorActionPreference = $oldEap
if ($ver -notmatch '^\d') {
    Write-Warn2 '未安装，正在装入 .venv…'
    & $py -m pip install --quiet pyinstaller
    if ($LASTEXITCODE -ne 0) { Fail 'PyInstaller 安装失败（需要网络）。' }
    $ver = & $py -m PyInstaller --version
}
Write-Ok ("PyInstaller {0}" -f $ver)

# ---------------------------------------------------------------- 5. 构建
Write-Step '构建 exe / building'
if ($Clean) {
    foreach ($d in @($outDir, $workDir)) { if (Test-Path $d) { Remove-Item $d -Recurse -Force } }
    Write-Ok '已清理旧产物'
}
& $py -m PyInstaller --noconfirm --onefile --windowed --name $exeName `
    --distpath $outDir --workpath $workDir --specpath $workDir $gui |
    Select-String -Pattern 'completed successfully|ERROR' | Select-Object -Last 1 | ForEach-Object { $_.Line }
if (-not (Test-Path $exePath)) { Fail 'exe 未生成。' }

# ---------------------------------------------------------------- 6. 校验
Write-Step '校验 exe / verifying'
$fi = Get-Item $exePath
$sha = (Get-FileHash $exePath -Algorithm SHA256).Hash
Write-Ok ("{0}  {1:N1} MB" -f $fi.Name, ($fi.Length / 1MB))
Write-Ok ("SHA256 {0}" -f $sha)

# exe 里 __file__ 在临时目录，脚本会改用"可执行文件所在位置"查找 INI；
# 用 --selftest 实测这一条（能自己找到 mod\Nioh1PerfectGuard.ini 才算过）。
& $exePath --selftest | Select-Object -Last 3
if ($LASTEXITCODE -ne 0) { Fail 'exe 自检失败：它没能找到/解析 INI。' }
Write-Ok 'exe 自检通过（能在本机布局下找到 INI 并原样回写）'

Write-Host ''
Write-Host '=== 完成 / done ===' -ForegroundColor Green
Write-Host ('  输出: ' + $exePath)
Write-Host  '  注意: 该 exe 按决定不打包进发布 zip。/ not shipped inside the release zip.'
Write-Host  '  重建: powershell -ExecutionPolicy Bypass -File tools\build_gui.ps1'
exit 0
