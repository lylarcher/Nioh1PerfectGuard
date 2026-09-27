<#
  make_nexus_zip.ps1 —— 生成用于分发的 ZIP（Nexus Mods 等）

  产物布局 = "解压到游戏根目录"：

      Nioh1PerfectGuard-<版本>-nexus.zip
        readme.txt                       （安装说明：解压到哪、怎么验证、协议、鸣谢）
        dinput8.dll                      （balfa 的 Nioh Native Mod Loader，原样）
        mods\Nioh1PerfectGuard\...       （dist 里的 12 个文件）

  用法：
      .\tools\make_nexus_zip.ps1 -GameDir "E:\SteamLibrary\steamapps\common\Nioh"
      .\tools\make_nexus_zip.ps1 -GameDir "<游戏目录>" -NoLoader   # 不含加载器的版本
      .\tools\make_nexus_zip.ps1 -LoaderPath "D:\somewhere\dinput8.dll"

  GameDir 用来取 dinput8.dll（它是第三方二进制，不入库，只在打包时拷进来）。
#>
param(
    [string]$GameDir = '',
    [string]$LoaderPath = '',
    [switch]$NoLoader,
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
$Root    = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$DistDir = Join-Path $Root 'dist\Nioh1PerfectGuard'
if (-not $OutDir) { $OutDir = Join-Path $Root 'dist' }

if (-not (Test-Path -LiteralPath $DistDir)) {
    throw "找不到 $DistDir —— 请先跑一次 .\build.ps1"
}

# version straight out of the source, so it cannot drift from the DLL
$src = Get-Content -LiteralPath (Join-Path $Root 'mod\Nioh1PerfectGuard.c') -Raw
$m = [regex]::Match($src, '#define\s+MOD_VERSION\s+"([^"]+)"')
$Version = if ($m.Success) { $m.Groups[1].Value } else { 'unknown' }
Write-Host "版本：$Version"

# staging
$stage = Join-Path $env:TEMP ('pg_nexus_' + [guid]::NewGuid().ToString('N'))
$stageMod = Join-Path $stage 'mods\Nioh1PerfectGuard'
New-Item -ItemType Directory -Force -Path $stageMod | Out-Null

Copy-Item -Path (Join-Path $DistDir '*') -Destination $stageMod -Recurse -Force
Copy-Item -LiteralPath (Join-Path $Root 'mod\PACKAGE_README.txt') -Destination (Join-Path $stage 'readme.txt') -Force

# the loader (third-party binary: copied in at packaging time, never committed)
$loaderIncluded = $false
if (-not $NoLoader) {
    $cand = $LoaderPath
    if (-not $cand) {
        if (-not $GameDir) {
            throw '请用 -GameDir 指定游戏目录（用来取 dinput8.dll），或用 -LoaderPath 直接给文件路径，或加 -NoLoader 生成不含加载器的包'
        }
        $cand = Join-Path $GameDir 'dinput8.dll'
    }
    if (-not (Test-Path -LiteralPath $cand)) {
        throw "找不到加载器：$cand（可用 -LoaderPath 指定，或 -NoLoader 跳过）"
    }
    Copy-Item -LiteralPath $cand -Destination (Join-Path $stage 'dinput8.dll') -Force
    $loaderHash = (Get-FileHash -LiteralPath $cand -Algorithm SHA256).Hash
    $loaderIncluded = $true
    Write-Host ("加载器：{0}  {1} B  sha256 {2}" -f $cand, (Get-Item $cand).Length, $loaderHash)
} else {
    Write-Host '按 -NoLoader：不包含 dinput8.dll'
}

# zip
$suffix = if ($loaderIncluded) { 'nexus' } else { 'nexus-noloader' }
$zip = Join-Path $OutDir ("Nioh1PerfectGuard-{0}-{1}.zip" -f $Version, $suffix)
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $false)
Remove-Item -LiteralPath $stage -Recurse -Force

Write-Host ''
Write-Host ("ZIP : {0}" -f $zip)
Write-Host ("大小: {0:N0} B   sha256: {1}" -f (Get-Item $zip).Length, (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash)
Write-Host ''
Write-Host '包内条目：'
Add-Type -AssemblyName System.IO.Compression
$z = [System.IO.Compression.ZipFile]::OpenRead($zip)
$z.Entries | Sort-Object FullName | ForEach-Object { Write-Host ("  {0,9:N0}  {1}" -f $_.Length, $_.FullName) }
$z.Dispose()
Write-Host ''
Write-Host '分发说明：用户把整个 ZIP 解压到游戏根目录（nioh.exe 所在的文件夹）。'
