=====================================================================
 Nioh 1 Perfect Guard  /  仁王1 精防 MOD      version 0.1.1-nioh1
=====================================================================

*** WHERE TO EXTRACT / 解压到哪里 ***

  Extract EVERYTHING in this archive into your Nioh game folder:

      ...\steamapps\common\Nioh\          <-- this folder (next to nioh.exe)

  After extracting you should have exactly this:

      Nioh\
        nioh.exe
        dinput8.dll                  <- mod loader (from this archive)
        mods\
          Nioh1PerfectGuard\
            Nioh1PerfectGuard.dll
            Nioh1PerfectGuard.ini
            Sounds\parry.wav
            ... (docs, source)

  把压缩包里的**所有内容**解压到仁王1的游戏根目录
  （`...\steamapps\common\Nioh\`，也就是 `nioh.exe` 所在的那个文件夹），
  解压后应当如上图所示。**不要**只解压到 `mods\` 里面。


---------------------------------------------------------------------
 REQUIREMENTS / 前置条件
---------------------------------------------------------------------
* Nioh 1 Complete Edition, nioh.exe version 1.24.8 (Steam).
  The mod refuses to install on any other build (deliberately).
* Windows 64-bit. Offline play is recommended: Nioh 1 validates saves online.
* No other Nioh 1 mod loader needed - dinput8.dll is included.

* 《仁王1 完全版》Steam 版，`nioh.exe` 版本必须 **1.24.8**（其它版本会被拒绝安装）。
* 建议离线模式游玩（仁王1 有在线存档校验）。
* 加载器 `dinput8.dll` 已包含在压缩包里，不需要额外安装。


---------------------------------------------------------------------
 INSTALL / 安装
---------------------------------------------------------------------
1. Close the game completely.
2. Extract this archive into the Nioh game folder (see the box above).
3. Start the game **through Steam** (launching nioh.exe directly makes it exit
   after ~28 s).

1. 先**完全退出游戏**。
2. 把压缩包解压到游戏根目录（见最上面那个框）。
3. 用 **Steam** 启动游戏（直接双击 exe 会在约 28 秒后自己退出）。


---------------------------------------------------------------------
 HOW TO TELL IT WORKS / 怎么确认生效
---------------------------------------------------------------------
Open `mods\Nioh1PerfectGuard\Nioh1PerfectGuard.gameplay.log`. Healthy start:

    ANCHOR 4/4 verified
    SELFTEST breakpoint: OK: 5/5 probe calls trapped by the VEH
    STATUS ACTIVE anchors=4/4 ...

Press Ctrl+Shift+F10 in game to write a cumulative STATE block.


---------------------------------------------------------------------
 WHAT IT DOES / 功能
---------------------------------------------------------------------
Press guard at the moment you are hit -> perfect guard, with rewards:

* Ki cost reduction on block (100% = free blocking) and Ki recovery
* HP restore (3% of maximum HP by default)
* **A pure guard press cancels whatever action you are doing** - attacks and
  martial skills, drinking/using items, onmyo talismans, ninjutsu, thrown
  items - and takes over with guard immediately. Guard + X/Y/A (a
  martial-skill input) is left to the game; movement never blocks the cancel.
* Parry sound (replaceable WAV)
* Optional, off by default: Ki/HP damage to the enemy that was blocked

Everything is configured in `Nioh1PerfectGuard.ini` (hot-reloaded, except
`Enabled`). The mod changes **no game file** - not one byte of code.


---------------------------------------------------------------------
 UNINSTALL / 卸载
---------------------------------------------------------------------
Delete `mods\Nioh1PerfectGuard\` (and `dinput8.dll` if you do not need the
loader any more). Nothing else was added.


---------------------------------------------------------------------
 CREDITS / 鸣谢
---------------------------------------------------------------------
* dinput8.dll - the Nioh Native Mod Loader by **balfa**. It is redistributed
  here **unmodified** for convenience, exactly as shipped by its author
  (69,120 bytes, sha256 26EFCD7C67E21BC0070E141B1D82117D9FDDD9B1690D955E11822124D27F7FC4).
  If you prefer, delete it and get the loader from its original page instead.
* Everything else (the mod DLL, config, docs, source) is by **lylarcher**.
  Sources: https://github.com/lylarcher/Nioh1PerfectGuard (included in this package under source\, so you can check
  every memory offset it writes).

* `dinput8.dll` 是 **balfa** 的 Nioh Native Mod Loader，此处**原样重新分发**
  （未做任何修改）。不想用这份的话，删掉它、去作者原页面下载即可。
* 其余部分（MOD 本体、配置、文档、源码）作者 **lylarcher**，源码仓库：
  https://github.com/lylarcher/Nioh1PerfectGuard（源码也随包放在 source\ 下，你可以自行核对它到底写了哪些内存偏移）。


---------------------------------------------------------------------
 LICENSE / 协议
---------------------------------------------------------------------
**PolyForm Noncommercial License 1.0.0** - see `mods\Nioh1PerfectGuard\LICENSE.txt`.

Free for any NONCOMMERCIAL purpose (personal use, study, hobby projects;
charities, schools, public research and government bodies). **Commercial use is
not permitted**: you may not sell it, bundle it into a paid product, or ship it
as part of a monetised service.

This package contains **no game code, assets or data** from Nioh. Nioh and all
related marks belong to Koei Tecmo; this project is not affiliated with or
endorsed by them.

**PolyForm Noncommercial 1.0.0（仅限非商业使用）**：
允许个人使用、学习研究、业余项目，以及慈善/教育/公立研究/公共安全卫生/环保/政府
机构的非商业用途。**禁止任何商业用途**（不得出售、不得打包进付费产品、不得作为
收费服务的一部分分发）。本包含有**没有任何**来自《仁王》的游戏代码或资源；
《仁王》及相关标识归 Koei Tecmo 所有，本项目与其无隶属或背书关系。
