# 一页说明 —— 只看这一页也能用

《仁王1 完全版》（1.24.8）的**精准防御（精防）** MOD：在被击中的**一瞬间**按下防御，
就能触发精防并拿到收益。它**不改动任何游戏文件**，一个字节都不改 `.text` ——
用的是 CPU 的硬件执行断点 + 异常处理，只在断点命中时改内存里的几个数值字段。

包内还有英文完整说明 `README_EN.md`（中文为 `README_CN.md`）。

## 默认就有的功能

| 功能 | 默认 |
| --- | --- |
| 精防判定（防御按下沿 + 250ms 窗口） | **开** |
| 格挡耗精减免 100%（完全不耗精） | **开** |
| 回精（回复最大精力的 1/6） | **开** |
| 精防回血（回最大 HP 的 3%） | **开** |
| **单按防御键取消当前动作**（攻击/武技、喝药、上阴阳符、上咒术忍术、丢道具） | **开** |
| 精防音效 | **开** |
| 对敌削精 / 扣血 | **关**（`EnemyKiDamage` / `EnemyHpDamage`，默认不写内存）|
| 限时增益（移速 / 减伤 / 霸体） | **已停用**（默认全关；原因见开发目录里的 `docs\RE_NOTES.md` §4.52，不随包分发）|

## 三步装上

1. **退出游戏**。
2. 把 `dinput8.dll`（通用 MOD 加载器）放进游戏根目录
   （`...\steamapps\common\Nioh\`），再把 `Nioh1PerfectGuard\` **整个文件夹**放进 `mods\`。
3. 用 **Steam** 启动（直接双击 exe 会在约 28 秒后自己退出）。**建议离线模式**：仁王1 有在线存档校验。

```
Nioh\
  nioh.exe
  dinput8.dll            <- 加载器
  mods\
    Nioh1PerfectGuard\
      Nioh1PerfectGuard.dll
      Nioh1PerfectGuard.ini
      Sounds\parry.wav
```

## 怎么知道它生效了

启动游戏后打开 `mods\Nioh1PerfectGuard\Nioh1PerfectGuard.gameplay.log`，看到这三行就是好的：

```
ANCHOR 4/4 verified
SELFTEST breakpoint: OK: 5/5 probe calls trapped by the VEH
STATUS ACTIVE anchors=4/4 ...
```

- `ANCHOR` 不对 → 游戏版本不是 1.24.8（MOD 会**拒绝安装**，这是**故意**的）。
- `SELFTEST` 是 MOD 每次启动对**自己**做的验证（证明硬件断点机制在本机真的会触发）。
  显示 `FAILED` 就不用往下试了，把日志发回。
- 想随时看累计状态：游戏在前台时按 **`Ctrl+Shift+F10`**，会打一组 `STATE` 行。

## 最短的验收（约 5 分钟）

1. **进关卡**（标题画面不会真正开始，不过它在演示模式里也会产生真实格挡事件）。
2. 被打时**看准时机**按防御，做 5 次 → 日志应有 `PERFECT GUARD`。
3. **先掉点血再做** → 应看到 `HP restore 550 -> 576 (+26) max=880 mode=1`。
4. **单按防御键取消攻击**：先按 X/Y 打出去，动作还没结束时**只按防御**（别同时按攻击键）
   → 应看到 `ACTION CANCEL: action=..` 紧跟 `ACTION CANCEL follow-up: action .. -> ..`。
   按**防御+X**（武技的输入形状）时**不该**出现 `ACTION CANCEL`，只应看到
   `ACTION CANCEL skipped: guard+attack combination ...`。
5. 完整清单见 `ACCEPTANCE_TEST.md`。

## 常用调节（`Nioh1PerfectGuard.ini`；**除 `Enabled` 外全部热更新**，约 1 秒生效）

| 想做什么 | 改哪个键 |
| --- | --- |
| 精防窗口太窄 / 太宽 | `WindowMs`（默认 450）|
| 键鼠玩家：防御键没反应 | `LearnButtons=1` → 按一下防御键 → 抄 `LEARN key VK=0x..` 到 `GuardKeyVK` → 改回 0 |
| 觉得减免不够 / 过头 | `KiDamageReductionPercent`（默认 100 = 完全不耗精）|
| 回精方式 | `KiRecoveryMode`（0 不回 / 1 返还本次 / 2 固定 / **3 最大精力 1/6**）|
| 回血量与方式 | `HpRecoveryMode`（**1 按百分比** / 2 固定 / 3 两者 / 0 关）、`HpRestorePercent`（3）、`HpRestoreFixed`（50）|
| 关掉"防御取消攻击" | `CancelActionOnGuard=0` |
| 武技被误判成"单按防御" | `ComboGuardWindowMs` 从 100 调到 **150–250**（越大越保护武技，代价是取消稍晚）|
| 取消得不够干脆 | `CancelActionFrames`（默认 30，调大推进更多动画帧）|
| 想要对敌效果 | `EnemyKiDamage` / `EnemyHpDamage` 设非 0（默认关闭，不写内存）|
| 音效 | `SoundEnabled` / `SoundVolume` / `SoundFile`（换成自己的 16-bit PCM 44.1/48k WAV）|
| 关掉诊断日志 | `KiTrace=0`（验收完建议关，日志更小）|

## 想关掉 / 卸载

- **临时关闭**：`Enabled=0` 然后**重启游戏** → 日志显示 `STATUS BYPASS`，**一个断点都不装**、不写内存。
- **完全卸载**：删 `mods\Nioh1PerfectGuard\`（不再需要加载器就再删 `dinput8.dll`）。

## 出问题时先看哪里

| 现象 | 先看 |
| --- | --- |
| 日志里完全没有 `ANCHOR` | `mods\loader.log` 里有没有加载本 MOD；DLL 位置对不对 |
| 一堆"不算精防"、从没 `PERFECT GUARD` | 防御键没配对（键鼠请看上面 `LearnButtons`）|
| 按防御没取消攻击 | `ACTION CANCEL skipped:` 后面写的原因（组合键？最近没打过攻击？）|
| 没有精防音效 | 日志里的 `SOUND ...` 行，对照 `README_CN.md` 的「音效排查表」|
| 游戏崩溃 | 把 `%LOCALAPPDATA%\CrashDumps\nioh.exe.*.dmp` 与日志一起发回；必要时用 `DiagDisable` 逐项排查 |
| 装不上 / DLL 被占用 | 游戏必须先退出；被保护的残留 `nioh.exe` 需**管理员权限**结束或重启 |

> 作者 **lylarcher** ｜ 源码仓库：<https://github.com/lylarcher/Nioh1PerfectGuard>
>
> 协议：本包采用 **PolyForm Noncommercial 1.0.0**（见 `LICENSE.txt`）—— **允许非商业使用，禁止任何商业用途**（不得出售或打包进付费产品）。
>
> 包内文档：`README_CN.md` / `README_EN.md`（完整说明）、`ACCEPTANCE_TEST.md`（实机验收）、
> `CHANGELOG.md`（已验证什么、修过什么）、`source\`（源码，可自行核对改了哪些偏移）。
