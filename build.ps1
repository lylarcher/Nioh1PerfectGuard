<#
    build.ps1 — 编译 + 校验 + 打包（仁王1 精防 MOD）

    用法：
        .\build.ps1                 只编译、跑全部检查、生成 dist\
        .\build.ps1 -Install        额外复制到游戏 mods\ 目录（游戏必须已退出）
        .\build.ps1 -SkipTests      跳过检查（不推荐，仅调试用）
        .\build.ps1 -GameDir "D:\...\Nioh"

    设计原则：
      * 检查不通过就**中止并且不产出包**，绝不发布未经验证的 DLL。
      * dist\ 完全由本脚本生成，因此不入库（.gitignore 已排除）。
      * 字节级稳定：dist\ 里的文件与 mod\ 里的源文件逐字节相同，
        SHA256SUMS.txt 因此可与已安装副本对账。

    注意：本文件保存为 **UTF-8 带 BOM**。Windows PowerShell 5.1 在没有 BOM 时会
    按 ANSI 解码 .ps1，中文会变成乱码并破坏引号配对 —— 这一点踩过一次。
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [switch]$Install,
    [switch]$SkipTests,
    [string]$GameDir = 'E:\SteamLibrary\steamapps\common\Nioh'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$Root       = $PSScriptRoot
$ModDir     = Join-Path $Root 'mod'
$ToolsDir   = Join-Path $Root 'tools'
$DistRoot   = Join-Path $Root 'dist'
$DistDir    = Join-Path $DistRoot 'Nioh1PerfectGuard'
$ZigDir     = Join-Path $Root '_tools_dl'
$ZigUrlFile = Join-Path $Root '_work\zig_url.txt'
$FallbackZigUrl = 'https://ziglang.org/download/0.16.0/zig-x86_64-windows-0.16.0.zip'

$script:Failures = 0

function Write-Head([string]$Text) {
    Write-Host ''
    Write-Host "== $Text" -ForegroundColor Cyan
}
function Write-Ok([string]$Text)   { Write-Host "   [ok]   $Text" -ForegroundColor Green }
function Write-Bad([string]$Text)  { Write-Host "   [FAIL] $Text" -ForegroundColor Red }
function Write-Note([string]$Text) { Write-Host "   $Text" -ForegroundColor DarkGray }

# Run a native command and capture its output WITHOUT letting stderr become a
# terminating error. With $ErrorActionPreference='Stop', `2>&1` on a native
# command turns any stderr output into an exception -- which made a failing check
# abort the whole script instead of reporting "[FAIL]".
function Invoke-Capture {
    param([string]$Exe, [string[]]$ArgList = @())
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out  = & $Exe @ArgList 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
    return [pscustomobject]@{ Out = @($out); Code = $code }
}

# ─────────────────────────────────────────────────────────── 0. 定位工具链 ──
function Find-Zig {
    if (Test-Path -LiteralPath $ZigDir) {
        $hit = Get-ChildItem -LiteralPath $ZigDir -Recurse -Filter 'zig.exe' -File -ErrorAction SilentlyContinue |
               Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    $url = $FallbackZigUrl
    if (Test-Path -LiteralPath $ZigUrlFile) {
        $t = (Get-Content -LiteralPath $ZigUrlFile -Raw).Trim()
        if ($t) { $url = $t }
    }
    Write-Note "未找到 zig.exe，正在下载：$url"
    New-Item -ItemType Directory -Force -Path $ZigDir | Out-Null
    $zip = Join-Path $ZigDir 'zig.zip'
    $old = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'   # PS 5.1 的进度条会让下载慢十倍
    try { Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing } finally { $ProgressPreference = $old }
    Expand-Archive -LiteralPath $zip -DestinationPath $ZigDir -Force
    Remove-Item -LiteralPath $zip -Force
    $hit = Get-ChildItem -LiteralPath $ZigDir -Recurse -Filter 'zig.exe' -File | Select-Object -First 1
    if (-not $hit) { throw "下载后仍未找到 zig.exe，请手动放置到 $ZigDir" }
    return $hit.FullName
}

# ──────────────────────────────────────────────────────── 1. 编译 ──
function Build-All([string]$Zig) {
    Write-Head '编译'
    $target = 'x86_64-windows-gnu'

    $r = Invoke-Capture $Zig @('cc', '-target', $target, '-O2',
                               '-o', (Join-Path $ToolsDir 'test_logic.exe'),
                               (Join-Path $ToolsDir 'test_logic.c'))
    if ($r.Code -ne 0) { $r.Out | ForEach-Object { Write-Note $_ }; throw '编译 test_logic.exe 失败' }
    Write-Ok ('tools\test_logic.exe  {0:N0} B' -f (Get-Item (Join-Path $ToolsDir 'test_logic.exe')).Length)

    $r = Invoke-Capture $Zig @('cc', '-target', $target, '-shared', '-O2',
                               '-o', (Join-Path $ModDir 'Nioh1PerfectGuard.dll'),
                               (Join-Path $ModDir 'Nioh1PerfectGuard.c'), '-lole32')
    if ($r.Code -ne 0) { $r.Out | ForEach-Object { Write-Note $_ }; throw '编译 Nioh1PerfectGuard.dll 失败' }
    Write-Ok ('mod\Nioh1PerfectGuard.dll  {0:N0} B' -f (Get-Item (Join-Path $ModDir 'Nioh1PerfectGuard.dll')).Length)
}

# ──────────────────────────────────────────────────────── 2. 跑检查 ──
# 每个检查都必须以 0 退出；任何一项失败都会让整体中止。
# 注意参数约定各不相同：多数脚本收「仓库根目录」，INI 检查收「mod 目录」，
# 导出表收「DLL 路径」—— 一开始统一按仓库根目录传，结果 INI 检查直接崩了。
function Run-Checks([string]$Phase) {
    $pyCmd = Get-Command python -ErrorAction SilentlyContinue
    if (-not $pyCmd) { throw '找不到 python，无法运行检查脚本' }
    $py = $pyCmd.Source

    if ($Phase -eq 'pre') {
        $checks = @(
            @{ Name = '锚点字节对账';       Exe = $py; ArgList = @((Join-Path $ToolsDir 'test_anchors.py'),       $Root) },
            @{ Name = '纯逻辑单元测试';     Exe = (Join-Path $ToolsDir 'test_logic.exe'); ArgList = @() },
            @{ Name = '测试算术==发货算术'; Exe = $py; ArgList = @((Join-Path $ToolsDir 'test_logic_digest.py'),  $Root) },
            @{ Name = '文档数字==实测值';   Exe = $py; ArgList = @((Join-Path $ToolsDir 'test_doc_counts.py'),    $Root) },
            @{ Name = 'INI 编码与默认值';   Exe = $py; ArgList = @((Join-Path $ToolsDir 'test_ini_encodings.py'), $ModDir) },
            @{ Name = '文档标记/日志解释';  Exe = $py; ArgList = @((Join-Path $ToolsDir 'test_doc_markers.py'),   $Root) },
            @{ Name = '日志调用点均有上限'; Exe = $py; ArgList = @((Join-Path $ToolsDir 'audit_log_sites.py'),    $Root) }
        )
    } else {
        $checks = @(
            @{ Name = '文档文件可达性';     Exe = $py; ArgList = @((Join-Path $ToolsDir 'test_doc_paths.py'),     $Root) },
            # 导出清单很长，摘要取首行（"exports (N):"）
            @{ Name = '导出表';             Exe = $py; ArgList = @((Join-Path $ToolsDir 'list_exports.py'), (Join-Path $ModDir 'Nioh1PerfectGuard.dll')); Summary = 'first' }
        )
    }

    Write-Head "检查（$Phase）"
    foreach ($c in $checks) {
        $r = Invoke-Capture $c.Exe $c.ArgList
        $nonEmpty = @($r.Out | Where-Object { $_ -match '\S' })
        if ($c.ContainsKey('Summary') -and $c.Summary -eq 'first') {
            $line = $nonEmpty | Select-Object -First 1
        } else {
            $line = $nonEmpty | Select-Object -Last 1
        }
        if ($r.Code -eq 0) {
            Write-Ok ('{0,-18} {1}' -f $c.Name, $line)
        } else {
            $script:Failures++
            Write-Bad ('{0,-18} (exit {1})' -f $c.Name, $r.Code)
            $r.Out | Select-Object -Last 15 | ForEach-Object { Write-Note "      $_" }
        }
    }
}

# ──────────────────────────────────────────────────── 3. 生成 dist\ ──
function New-Dist {
    Write-Head '打包 dist\Nioh1PerfectGuard\'
    if (Test-Path -LiteralPath $DistDir) { Remove-Item -LiteralPath $DistDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path (Join-Path $DistDir 'Sounds') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $DistDir 'source') | Out-Null

    # 源 → 交付文件（显式列出，避免漏项或混入多余文件）
    $map = @(
        @{ From = (Join-Path $ModDir 'Nioh1PerfectGuard.dll'); To = 'Nioh1PerfectGuard.dll' },
        @{ From = (Join-Path $ModDir 'Nioh1PerfectGuard.ini'); To = 'Nioh1PerfectGuard.ini' },
        @{ From = (Join-Path $ModDir 'Sounds\parry.wav');      To = 'Sounds\parry.wav' },
        @{ From = (Join-Path $Root   'QUICKSTART.md');         To = 'QUICKSTART.md' },
        @{ From = (Join-Path $ModDir 'README_CN.md');          To = 'README_CN.md' },
        @{ From = (Join-Path $ModDir 'README_EN.md');          To = 'README_EN.md' },
        @{ From = (Join-Path $Root   '验收测试说明.md');        To = 'ACCEPTANCE_TEST.md' },
        @{ From = (Join-Path $Root   'CHANGELOG.md');          To = 'CHANGELOG.md' },
        @{ From = (Join-Path $ModDir 'Nioh1PerfectGuard.c');   To = 'source\Nioh1PerfectGuard.c' },
        @{ From = (Join-Path $ModDir 'pg_logic.h');            To = 'source\pg_logic.h' }
    )
    foreach ($m in $map) {
        if (-not (Test-Path -LiteralPath $m.From)) { throw "缺少源文件：$($m.From)" }
        $dest = Join-Path $DistDir $m.To
        Copy-Item -LiteralPath $m.From -Destination $dest -Force
        Write-Note ('{0,-32} {1,8:N0} B' -f $m.To, (Get-Item -LiteralPath $dest).Length)
    }

    # SHA256SUMS.txt：按相对路径排序，UTF-8 无 BOM，且不含自身
    $files = Get-ChildItem -LiteralPath $DistDir -Recurse -File |
             Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
             ForEach-Object { [pscustomobject]@{ Rel = $_.FullName.Substring($DistDir.Length + 1); Full = $_.FullName } } |
             Sort-Object -Property Rel
    $lines = foreach ($f in $files) {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $f.Full -Algorithm SHA256).Hash, $f.Rel
    }
    [System.IO.File]::WriteAllLines((Join-Path $DistDir 'SHA256SUMS.txt'), $lines,
                                    (New-Object System.Text.UTF8Encoding($false)))
    Write-Ok ('SHA256SUMS.txt  {0} 个文件' -f $lines.Count)
}

# ──────────────────────────────────────────────────── 4. 安装到游戏 ──
function Install-ToGame {
    Write-Head "安装到 $GameDir"
    if (Get-Process -Name nioh -ErrorAction SilentlyContinue) {
        throw '游戏正在运行，DLL 被占用；请先退出游戏再执行 -Install'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $GameDir 'nioh.exe'))) {
        throw "在 $GameDir 找不到 nioh.exe，请用 -GameDir 指定游戏目录"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $GameDir 'dinput8.dll'))) {
        Write-Note '警告：游戏根目录没有 dinput8.dll（MOD 加载器），MOD 不会被加载'
    }
    $target = Join-Path $GameDir 'mods\Nioh1PerfectGuard'
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    Copy-Item -Path (Join-Path $DistDir '*') -Destination $target -Recurse -Force
    Write-Ok "已复制到 $target"

    $bad = 0
    Get-ChildItem -LiteralPath $DistDir -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($DistDir.Length + 1)
        $dst = Join-Path $target $rel
        if (-not (Test-Path -LiteralPath $dst)) { Write-Bad "缺失 $rel"; $script:Failures++; $bad++ }
        elseif ((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash -ne
                (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash) {
            Write-Bad "哈希不一致 $rel"; $script:Failures++; $bad++
        }
    }
    if ($bad -eq 0) { Write-Ok '安装副本与交付包逐文件一致' }
}

# ───────────────────────────────────────────────────────────── main ──
Write-Host '仁王1 精防 MOD — 编译 / 校验 / 打包' -ForegroundColor White
Write-Note "项目根目录：$Root"

$watch = [System.Diagnostics.Stopwatch]::StartNew()
try {
    $zig = Find-Zig
    Write-Note "zig：$zig"
    Build-All $zig
    if (-not $SkipTests) {
        Run-Checks 'pre'
        if ($script:Failures -gt 0) { throw "有 $($script:Failures) 项检查未通过，已中止（未生成包）" }
    } else {
        Write-Head '检查'; Write-Note '已按 -SkipTests 跳过'
    }
    New-Dist
    if (-not $SkipTests) {
        Run-Checks 'post'
        if ($script:Failures -gt 0) { throw "打包后仍有 $($script:Failures) 项检查未通过" }
    }
    if ($Install) { Install-ToGame }
}
catch {
    Write-Host ''
    Write-Host "构建中止：$($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

Write-Head '结果'
Write-Ok ('DLL sha256 : {0}' -f (Get-FileHash -LiteralPath (Join-Path $DistDir 'Nioh1PerfectGuard.dll') -Algorithm SHA256).Hash)
Write-Ok ('交付包     : {0} 个文件' -f (Get-ChildItem -LiteralPath $DistDir -Recurse -File).Count)
Write-Ok ('用时       : {0:N1} 秒' -f $watch.Elapsed.TotalSeconds)
Write-Note 'dist\ 由本脚本生成，不入库；需要重新生成就再跑一次。'
exit 0
