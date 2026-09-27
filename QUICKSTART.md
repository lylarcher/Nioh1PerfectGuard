# 一页说明 —— 只看这一页也能用

## 它是什么

《仁王1 完全版》（1.24.8）的**精准防御（精防）** MOD：
在被击中的**一瞬间**按下防御，就能触发精防并拿到收益
（回精 / 格挡耗精减免 / 对敌削精与伤害 / 后摇取消 / 音效）。

它**不改动任何游戏文件**，一个字节都不改 `.text`：用的是 CPU 的硬件执行断点 + 异常处理，
只在断点命中时改内存里的几个数值字段。

## 三步装上

1. 退出游戏。
2. 把 `dinput8.dll`（通用 MOD 加载器）丢进游戏根目录
   （`...\steamapps\common\Nioh\`），
   再把 `Nioh1PerfectGuard\` **整个文件夹**放进该目录下的 `mods\`。
3. 用 **Steam** 启动游戏（直接双击 exe 会在约 28 秒后自己退出）。

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

启动游戏后打开同目录的 `Nioh1PerfectGuard.gameplay.log`，看到这三行就是好的：

```
ANCHOR 4/4 verified
SELFTEST breakpoint: OK: 5/5 probe calls trapped by the VEH
STATUS ACTIVE anchors=4/4 ...
```

- `ANCHOR` 不对 → 游戏版本不是 1.24.8（MOD 会拒绝安装，这是**故意**的）。
- `SELFTEST` 是 MOD 每次启动对自己做的验证（它真的能让断点触发）。
  显示 `FAILED` 就不用往下试了，把日志发回来。

进关卡后每打一次精防会写 `PERFECT GUARD #n`；想随时看累计，
按 **`Ctrl+Shift+F10`**。

## 最短的验收（约 5 分钟）

1. **进关卡**（标题画面不会真正开始；不过它在演示模式里其实也会产生格挡事件）。
2. 被打时**看准时机**按防御，做 5 次 → 日志应有 `PERFECT GUARD`。
3. **按住防御不放**挨打 → 应出现 `PLAYER BLOCK ... not within 250ms`，**不**发奖励。
4. 打敌人让它格挡你 → 应出现 `GUARD (other) ... ignored`，**不**发奖励。
5. 完整细节见 `ACCEPTANCE_TEST.md`。

## 常用调节

| 想做什么 | 改哪个键 |
| --- | --- |
| 精防窗口太窄/太宽 | `WindowMs`（默认 250） |
| 键鼠玩家：防御键没反应 | `LearnButtons=1` → 按一下防御键 → 抄 `LEARN key VK=0x..` 到 `GuardKeyVK` → 改回 0 |
| 觉得减免不够/过头 | `KiDamageReductionPercent`（默认 100 = 完全不耗精） |
| 回精方式 | `KiRecoveryMode`（0 不回 / 1 返还本次 / 2 固定 / **3 最大精力 1/6**） |
| 想要对敌效果 | `EnemyKiDamage` / `EnemyHpDamage` 设成非 0（默认关闭，不写内存） |
| 后摇取消（实验性） | `CancelRecovery=1` |
| 音效 | `SoundEnabled` / `SoundVolume` / `SoundFile`（换成你自己的 16-bit PCM 44.1/48k WAV） |

**除 `Enabled` 外所有键都是热更新**（改完约 1 秒生效）；`Enabled` 需要重启。

## 想关掉 / 卸载

- **临时关闭**：`Enabled=0` 然后**重启游戏** → 日志显示 `STATUS BYPASS`，**一个断点都不装**。
- **完全卸载**：删 `mods\Nioh1PerfectGuard\`（不再需要加载器就再删 `dinput8.dll`）。

## 还有一件事想请你确认

GR 里有一个**静态分析回答不了**的问题：格挡耗精到底改了哪个精力字段。
MOD 会自动给出结论，但**要先把 `KiDamageReductionPercent` 改成 50**（默认 100 时两种模型数值重合，答不出来）：

```
KIV block #1 charge D=.. visible loss L=.. scaled D*(1-r)=.. residual=..% -> 结论
```

测完把日志发回即可，不需要你自己算。详见 `ACCEPTANCE_TEST.md`。

## 出问题时

| 现象 | 先看 |
| --- | --- |
| 日志里完全没有 `ANCHOR` | `mods\loader.log` 里有没有加载本 MOD；DLL 位置对不对 |
| 一堆"不算精防"、从没 `PERFECT GUARD` | 防御键没配对（键鼠请看上面 `LearnButtons`） |
| 没有精防音效 | 日志里的 `SOUND ...` 行，对照 `README_CN.md` 的「音效排查表」 |
| 游戏崩溃 | 把崩溃时间和 `%LOCALAPPDATA%\CrashDumps\nioh.exe.*.dmp` 发回；必要时用 `DiagDisable` 逐项排查 |
