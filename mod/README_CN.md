# 仁王 1 · 精准防御（精防）MOD — v0.1.2

在《仁王 1 完全版》（Nioh: Complete Edition，`nioh.exe` 1.24.8）中实现类似仁王 3
「Guard Parry / 精准防御」的格挡收益。

> **当前状态：四个锚点全部在实机验证通过（4/4），MOD 已能在游戏内激活运行。**
> 已实现：精防窗口判定（真实按键边沿）、格挡耗精减免、回精、音效、INI 热更新、日志。
> 尚未实现：对敌削精 / HP、提前接续（对应锚点仍在定位）。

---

## 实机验证记录

```
ANCHOR ok   guard_flag_1       rva=0x74DD09
ANCHOR ok   guard_flag_2       rva=0x74DD92
ANCHOR ok   guard_ki_cost      rva=0x74DE51
ANCHOR ok   inputmgr_getter    rva=0xE6BE90
INPUT manager slot = 0x7FF7ABE48658 (from getter 0xE6BE90 + disp 0xD0C7C1)
ANCHOR 4/4 verified
ARM pass: 372 threads seen, 372 newly armed
STATUS ACTIVE anchors=4/4 reduction=100% recovery=1 gate=1 mask=0x0100 slot=0 sound=1
CONFIG reloaded: ...   ← INI 热更新生效
```

注意输入管理器地址是**从 getter 自己的指令字节解出来的**（`mov rax,[rip+disp32]` 的
`disp32`），不是硬编码，因此换版本会自动对不上并拒绝安装。

---

## 安装

需要支持 `mods\` 目录的 x64 DLL 加载器（例如 `NiohNativeModLoader` / `dinput8.dll` 代理）。

1. 退出游戏。
2. 把 `Nioh1PerfectGuard` 整个文件夹放到游戏的 `mods\` 目录下：
   ```
   nioh.exe
   dinput8.dll            <- 加载器
   mods\
     Nioh1PerfectGuard\
       Nioh1PerfectGuard.dll
       Nioh1PerfectGuard.ini
       Sounds\parry.wav
   ```
3. 启动游戏。

**卸载**：退出游戏，删除 `mods\Nioh1PerfectGuard` 文件夹即可。不改动任何游戏文件。
如果连加载器也不再需要，再把游戏根目录的 `dinput8.dll` 一并删掉。

### 想关掉 MOD / 回退

| 目的 | 做法 | 效果 |
| --- | --- | --- |
| **临时关闭（保留文件）** | INI 里 `Enabled=0`，**然后重启游戏** | 日志显示 `STATUS BYPASS: Enabled=0`，**一个断点都不装**，不写任何内存 |
| **完全卸载** | 删 `mods\Nioh1PerfectGuard\`（及不再需要的 `dinput8.dll`） | 游戏回到原生状态 |
| **只想关掉音效/某类收益** | 改对应 INI 键 | 立即生效 |

> ⚠️ `Enabled` 是**唯一需要重启**的键：装/卸断点是启动时的一次性决定，
> 运行时改它不会生效（MOD 会打一条 `NOTICE: Enabled changed to ...` 提醒你）。
> 其余所有键都是热更新，改完约 1 秒生效。

**这个 MOD 不改动任何游戏文件**，也不修改 `.text` 里的一个字节：
它用的是 CPU 的硬件执行断点（DR0–DR3）+ 异常处理器，
只在断点命中时改内存里的若干数值字段。所以卸载就是删文件夹。

---

## 工作原理（为什么它是安全的）

本 MOD **不修改游戏的任何一个代码字节**。它使用 CPU 的硬件执行断点
（x64 的 DR0–DR3 调试寄存器）挂在格挡判定的几条指令上，在异常处理里
**改写被保存的线程上下文**，然后放行。线程恢复执行时看到的就是被改过的结果。

好处：

- 代码段保持原样 —— 不存在完整性校验被触发的问题；
- 不需要指令长度解码、不需要 trampoline 重定位；
- 启动时会逐条校验锚点处的**期望指令字节**，不匹配就**拒绝安装**，
  宁可完全不生效也不会写坏任何东西。

---

## 配置（`Nioh1PerfectGuard.ini`）

INI 内每一项都有中英双语注释、取值范围和默认值。大多数设置有改动后
**约 1 秒内自动生效**（不需要重启游戏）。

| 键 | 默认 | 含义 |
| --- | --- | --- |
| `Enabled` | 1 | 总开关。0 = 不安装任何断点 |
| `WindowMs` | **450** | 精防窗口（毫秒），从"新按下防御键"起算（2026-09-28 起由 250 调宽） |
| `CancelRecovery` | 0 | 精防后提前接续（**实验性，默认关**，见下） |
| `CancelRecoveryFrames` | 30 | 接续时推进的动画帧数 |
| `RequireTimelyGuard` | 1 | 1 = 仅 WindowMs 内新按下防御的格挡算精防；0 = 每次格挡都算 |
| `GuardButtonMask` | 0x0100 | 手柄防御键位掩码（标准 XInput 位）。`0x0100` = LB/L1 |
| `PadSlot` | 0 | 读取哪个手柄槽位 0～3 |
| `GuardKeyVK` | 0 | 键盘防御键的虚拟键码，0 = 不启用。与手柄掩码是"或"关系 |
| `LearnButtons` | 0 | 按键学习模式：把新出现的按键位/键码写进日志，用于反查上面两项 |

### 编码支持

配置文件读取器**自己做了编码探测**，以下写法都能正确解析：

- UTF-8（带或不带 BOM）
- UTF-16 LE（带 BOM）
- ANSI / cp1252

分区 `[PerfectGuard]` 之外的键会被忽略；`;` 与 `#` 开头的注释会被跳过。
已用 5 组用例实测通过（含把 `KiDamageReductionPercent=999` 写进注释和其他分区，
均被正确忽略）。

### 如果按防御键没反应（键鼠用户）
手柄掩码对键盘不适用。两步搞定：

1. 把 `LearnButtons=1` 存盘（游戏内热更新，不用重启）
2. 进游戏按几下你的防御键，然后看日志：

```
LEARN pad bit 8 pressed -> GuardButtonMask=0x0100 (raw word 0x0100)
LEARN key VK=0xA0 pressed -> GuardKeyVK=160
```

把日志里的数字填回 INI，再把 `LearnButtons` 改回 0 即可。
常见 VK：左 Shift=160(0xA0)、右 Shift=161、Ctrl=17、空格=32、鼠标左键=1。
| `KiDamageReductionPercent` | 100 | 格挡耗精减免 0～100%。**100 = 完全免耗精** |
| `KiTopUp` | 1 | 是否额外把可见精力**补回**。见下方「两条减免机制」——**一般保持 1** |
| `KiTopUpPreEventMs` | 100 | 补回时的基准精力往前回溯多少毫秒（0 = 关闭）。旗标事件源在扣精**之后**才触发，基准取错就一分钱都不还，见下方「两条减免机制」 |
| `KiRecoveryMode` | 3 | 0 不回精 / 1 返还本次消耗 / 2 固定值 / **3 回复最大精力的 1/6** |
| `FixedRecovery` | 50 | 模式 2 使用的固定回精量 |
| `HpRecoveryMode` | 1 | **精防回血**：0 关 / **1 按最大 HP 百分比（默认）** / 2 固定值 / 3 两者相加 |
| `HpRestorePercent` | 3 | 每次精防回复**最大 HP 的 3%**（向下取整：3% of 880 = 26） |
| `HpRestoreFixed` | 50 | 模式 2 或 3 使用的固定回血量 |
| `SpeedBuffPercent` / `SpeedBuffMs` | **0** / 10000 | **精防后移速增益**（0 = 关）。想启用设成 4。⚠ 本轮默认关：这是唯一调用游戏代码的功能，尚未实机验证 |
| `DamageCutPercent` / `DamageCutMs` | **0** / 10000 | **精防后承受伤害降低**（0 = 关）。想启用设成 4 |
| `ArmorBuff` / `ArmorBuffMs` | 0 / 5000 | **霸体**（已决定不使用，见下） |
| `LivingWeaponGaugeOnGuard` / `LivingWeaponGaugePercent` / `LivingWeaponGaugeMax` / `LivingWeaponGaugeOffset` | **1** / 10 / **515** / **0x100** | **九十九槽（精华量表 / 守护灵槽）积累**：每次精防加 N% 槽，**默认开**、默认 10% |
| `LivingWeaponExtendOnGuard` / `LivingWeaponExtendPercent` | **1** / **50** | **九十九状态中续烧条**：在九十九状态下每次精防续 N%，**默认开**、默认 10% |
| `CancelActionOnGuard` | **1** | **单按防御键取消当前动作**（0 = 关）：攻击/武技、喝药、上阴阳符、上咒术忍术、丢道具都算。防御+X/Y/A 这类组合键**不算**；移动不影响 |
| `AttackButtonMask` / `ComboGuardWindowMs` | 0xF000 / 100 | 哪些键算攻击键 / 与防御键相隔多少毫秒内算“组合键” |
| `CancelActionStrictHold` / `CancelActionFrames` / `CancelActionRecentMs` | 0 / 30 / **0** | 严格模式（按着就不取消）/ 动画帧推进量 / **0 = 任何防御按下都取消**（不区分精防与普通防御）|
| `EnemyKiDamage` | 0 | 对敌削精（float，`[[char+0x240]+0x40]`） |
| `EnemyHpDamage` | 0 | 对敌 HP（整数，`[[char+0x240]+0x20]`，可致死） |
| `SoundEnabled` / `SoundVolume` / `SoundFile` | 1 / 1 / parry.wav | 精防提示音 |
| `DiagnosticHotkey` | 1 | 游戏前台按 `Ctrl+Shift+F10` 写一行累计统计 |
| `KiTrace` | 1 | 诊断：持续采样两个候选精力字段（**验收完可以改 0**） |
| `BlockEventSource` | 2 | **哪个事件代表"玩家格挡成功"**：`2`=自动（推荐）/ `0`=只用扣精点 / `1`=只用旗标点。见下节 |
| `DiagDisable` | 0 | **诊断位掩码，平时保持 0**：`1`=不装断点 / `2`=不启动输入线程 / `4`=开启"滚动重装"（**已知会弄崩游戏**）/ `8`=不跑自检 |

### 九十九槽（精华量表 / 守护灵槽）两个开关

| 开关 | 何时生效 | 默认 |
| --- | --- | --- |
| `LivingWeaponGaugeOnGuard`（+ `LivingWeaponGaugePercent`） | **不在**九十九状态时，每次精防给量表加 N% | **开** / 10 |
| `LivingWeaponExtendOnGuard`（+ `LivingWeaponExtendPercent`） | **在**九十九状态时，每次精防续 N%（烧条） | **开** / 10 |

机制上它们不是"改字段"，而是**调用引擎自己的那个状态对象**
（`Character::AddStateObjectAmritaGaugeUp`，构造函数 `0x79E870`，状态 id `0x20`）：
它的 apply 就是 `gauge = min(gauge + 幅度, 1.0)`，量表 0～1 归一化，所以
"10% 槽"＝幅度 `0.10`，上限由引擎夹紧。**是否在九十九状态**由两路相或判断：① 引擎状态
容器（`[[char+0x240]]+0x10B0`）里存在 `CallSpirit` 状态对象（状态 id `0x22`）—— 主判据；
② 引擎的激活标志字节（`Player::SetTsukumoWeaponActiveFlag` 写的那个字节）。任一路为真即
算"在"，两路都读不到则按"不在"处理（走攒槽），所以判据读错**不会**让攒槽静默失效。

日志里每次调用都会写明：

```
LW engine: the 99-gauge constructor at 0x... stamps state id 0x20 in its first 128 bytes
LW gauge: +10% via AmritaGaugeUp state 0x... (in 99 state=0 [container 0x22=0, flag=-1], add()=1, container check: present)
```

`add()=1` 且 `present` ＝ 引擎收下了这个状态对象。与限时增益一样，这条路会调用游戏
代码（由 `DiagDisable` 位 16 统一关闭），并且**永不调用"移除"**（那条路实测会崩），
时长交给引擎自己过期。

### 事件源会自动记住结论（Nioh1PerfectGuard.state）

第一次用自动模式游玩时，若打满 3 次格挡都没见到扣精点触发，MOD 会改用旗标事件源，
并把**这个结论写进 DLL 同目录的 Nioh1PerfectGuard.state**（一行文本）。
这样**下次启动就一开局用正确的事件源**，不用再等 3 次。

想重新判定（例如换了角色、换了关卡、或更新了游戏）：删掉这个文件即可。
反向也成立：如果哪次扣精点真的触发了，MOD 会把结论改回来（costsite=fires）并切回精确源。

### 「格挡事件源」与「两条减免机制」

MOD 有**两个**可以代表"玩家格挡成功"的时机，各有取舍，`BlockEventSource` 决定用哪个：

| 取值 | 事件 | 优点 | 代价 |
| --- | --- | --- | --- |
| `0` | 格挡扣精点 | 最精确；**只有这里能缩放引擎真正要扣的精力** | 引擎自己有三道前置条件，不满足时整段代码不执行，什么都看不到 |
| `1` | "攻击被格挡"旗标点 | 触发条件宽得多；已实测能在本游戏里触发 | 拿不到扣精量：`KiRecoveryMode` 只能是 0 或 3，扣精缩放不生效，`KIV` 不输出（改为输出 `KIV-FLAG`） |
| `2`（默认） | **自动** | 先试扣精点；若它从未触发而旗标点已看到 3 次玩家格挡，就改用旗标点，**并在日志里说明**；扣精点后来出现会自动切回 | 无 |

**两条减免机制**（`KiDamageReductionPercent`）：

1. 在扣精点**缩放**引擎要扣的量 —— **只有事件源为 `0`（或自动模式判定扣精点可用）时才有**；
2. 把可见精力**补回**损失的 `减免%` —— 由 `KiTopUp` 控制。

**这两条都要看清时间顺序**：扣精点的事件发生在引擎扣精**之前**，所以那里
"现读的精力"就是正确的基准；而旗标点发生在扣精**之后**（实测：精防那一毫秒
日志里已经是 `ki=71.03/105`，之前是满的 105），若还在事件时刻现读，基准里就
已经含了这次扣除，损失恒为 0、补回等于没开。

所以旗标事件源的基准取自 **`KiTopUpPreEventMs`**（默认 100ms）窗口内的最高精力
值 —— 由 8ms 的输入线程持续记录样本，事件发生时取扣精之前的值。日志里：

- `KIREF pre-event reference 105 vs live 71.03` → 基准取对了；
- `KIV-FLAG ... handed back 33.97/100%` → 已经把这 33.97 还回去了。

代价：若某次与格挡无关的消耗恰好落在窗口内，它也会被当作格挡耗精补回 ——
**最可能撞上的正是本 MOD 自己的防御取消打法**（先出一刀、再按防御取消，出刀
瞬间就扣了精）：两者相隔不到窗口时，那一刀的精耗会被一起补回。`KIREF` 行会
打出取到的基准值，若明显高于格挡前的实际精力就是这个情况，把
`KiTopUpPreEventMs` 调小（例如 50）或设 `0` 关闭。

**非法值会被拒绝并保留上一份有效配置**，日志里会写明原因，例如：

```
CONFIG reject KiDamageReductionPercent=150 (allowed 0..100); keeping 100
CONFIG gameplay group rejected; previous settings kept
```

### 「回精」的说明

三种方式可选：不回精 / 返还本次格挡消耗的精力 / 固定数值 /
**回复最大精力的 1/6（默认）**。最大精力取自引擎自己的
`Refer::Stamina` 字段域（`[[char+0x240]+0x44]`），不是猜的。

`KiDamageReductionPercent`（格挡耗精减免）有**两条**并行机制，因为静态分析
无法证明扣精锚点写的资源表项就是精力条显示的值：

1. 在扣精点**缩放** `xmm1`（`0x74DE51`）；
2. 在 400ms 窗口内把可见精力字段 `[[char+0x240]+0x40]` **补回**损失量的
   `减免%`（只补差额、只增不减、永不超过快照）。

> 早期版本第 2 条是"每 tick 重算并补回"，而 worker 每 8ms 跑一次，
> 于是 400ms 内约 50 次迭代会让 `0.8^50 ≈ 1e-5` —— **任何非零减免都会
> 变成"完全不耗精"**，百分比形同虚设。现已改为锁存峰值损失、只补差额，
> 减免比例被真正遵守。哪条机制在生效由 `KIV` 日志行自动判定。

### 手柄按键与扳机键（一段被证实过的内存布局）

MOD 读的是输入管理器里**每个手柄槽位 20 字节**的记录，其中内嵌的正是
Windows 标准的 `XINPUT_GAMEPAD`：

| 偏移（+ 槽位×20） | 字段 |
| --- | --- |
| `+0x49E04` | 该槽位是否已连接（字节） |
| `+0x49E0C` | **`wButtons`（16 位按钮位掩码）** |
| `+0x49E0E` / `+0x49E0F` | `bLeftTrigger` / `bRightTrigger`（各 1 字节） |
| `+0x49E10` … | 左右摇杆（4 个 16 位有符号量） |

这不是猜的 —— 引擎自己的按键测试函数 `0xE6BCF0` 就是
`test word ptr [padstate+4], cx`（`padstate = 管理器+0x49E08+槽位×20`，
所以按钮字正好落在 `+0x49E0C`），索引 14/15 则分别测
`byte [padstate+6]` 与 `byte [padstate+7]`，而摇杆读的是 `+8`/`+0xa` ——
三处偏移同时对上 `XINPUT_GAMEPAD`。

它用的位表（RVA `0x12BF690`，14 项）前 12 项与标准 XInput 完全一致：
`0x0001/0002/0004/0008` 十字键、`0x0010` START、`0x0020` BACK、
`0x0040/0080` 左右摇杆按下、**`0x0100` = L1/LB（默认防御键）**、
`0x0200` = R1/RB、`0x1000` = X、`0x2000` = Y。

> ⚠️ **`GuardButtonMask` 匹配不到扳机键。** L2/R2 在那两个**独立的字节**里，
> 不在 16 位按钮字里，所以无论掩码怎么写都不可能命中。
> 如果你把游戏里的防御绑成了 L2/R2：要么在游戏里改绑到别的键，
> 要么用 `GuardKeyVK` 走键盘。MOD 检测到你在捏扳机而始终没有防御按下时，
> 会直接打一条 `NOTICE: a trigger is being squeezed (LT=.. RT=..) ...` 告诉你这件事。

### 「提前接续」的说明（实验性）

取消后摇的实现方式是**推进角色动画帧**（`[[char+0x38]+0x60]`，引擎的
`Refer::MotionFrame` 读的就是这个字段）。**这一项尚未经过实机验证**，
可能与某些动作冲突，因此**默认关闭**。

打开方式：`CancelRecovery=1`，用 `CancelRecoveryFrames` 调力度。
如果出现动作卡住或表现异常，改回 0 即可（热更新立即生效）。

### 对敌效果的说明

作用于**本次被格挡攻击的来源**，不是锁定目标。命中上下文里同时带攻守双方角色指针
（`[ctx+0x100]` 与 `[ctx+0xE8]`），取其中不是玩家的那个作为攻击者。

- 削精写 `[[char+0x240]+0x40]`（`Refer::Stamina` 读的字段）
- HP 写 `[[char+0x240]+0x20]`（`Refer::Hp` 读的字段）

写入前都有合理性闸门（HP 必须在 `0..1e8`、精力必须在 `0..1e5`），
不合理就跳过而不是乱写。默认值为 0，**不配置就不会写任何内存**。

### 音效没声音？先看日志里那一行 `SOUND`

MOD 的音频路径刻意**从不猜测**：任何一步不对都会打印一条明确的
`SOUND disabled: ...` 并静音，而不会假装成功或让游戏崩掉。对照表：

| 日志 | 原因 / 怎么办 |
| --- | --- |
| `SOUND ready (XAudio2 engine=xaudio2_9.dll)` | 正常，音效可用 |
| `SOUND disabled by configuration` | INI 里 `SoundEnabled=0` —— 正常 |
| `SOUND disabled: xaudio2_9.dll / XAudio2Create unavailable` | 系统缺 XAudio2 运行库（装最新 DirectX 终端用户运行时） |
| `SOUND disabled: cannot open <路径>` | `Sounds\parry.wav` 不在 DLL 同目录，或 `SoundFile` 写错了名字 |
| `SOUND disabled: expected RIFF WAVE` | 那个文件不是 WAV |
| `SOUND disabled: only 16-bit PCM supported` / `use 16-bit integer PCM WAV (format 1)` | WAV 不是 16-bit PCM（自己换的音效需转码） |
| `SOUND disabled: need 44100 or 48000 Hz (got N)` | 采样率不是 44100/48000 |
| `SOUND disabled: WAV missing fmt or data chunk` / `cannot read complete WAV` / `WAV size N out of range` | WAV 损坏或被截断 |
| `SOUND disabled: CreateMasteringVoice hr=0x...` | 没有可用音频输出设备（换默认设备 / 插上耳机） |
| `SOUND disabled: CoInitializeEx hr=0x...` / `XAudio2Create hr=0x...` | COM 或 XAudio2 初始化失败 |
| `SOUND submit failed` | 缓冲提交失败（一般是设备被独占） |

换自己的音效：把 16-bit PCM、44.1/48kHz 的 WAV 放进 `Sounds\`，
再把 INI 的 `SoundFile` 改成文件名即可（热更新生效，**不需要重启**）。

### 实机排查：`LAYOUT` 与 `KIV` 日志行

每次精防（前 10 次）会写一行：

```
LAYOUT player=0x.. hp=1200/1200 ki=210.5/250 action=12 frame=4.17 | ctx=0x.. charA=0x.. charB=0x..
```

这一行能一次判定：HP/精力/精力上限字段对不对、动作 ID 与动画帧是否合理、
上下文里的两个角色指针哪个是玩家、以及哪一个是攻击者。

**`KIV` 行会自动判定「格挡耗精」到底改的是哪个字段。** 这是本 MOD 唯一
无法靠静态分析收口的问题：扣精锚点写的是资源表项 `entry+0x0C`，而精力条的
`Refer::StaminaRate` 读的是 `param+0x40`。所以 MOD 把两个数直接摆出来对比，
还会给出**拟合残差**作为可信度：

```
KIV block #1 charge D=6.4 visible loss L=6.4 scaled D*(1-r)=3.2 residual=0% -> 可见精力字段吃了全额
```

自动给出的结论：

| 结论 | 含义 |
| --- | --- |
| 字段**没动** | 资源表和精力条无关 |
| 掉了**全额 D** | 缩放锚点够不到精力条，**补回**才是生效机制 |
| 掉了 **D×(1-减免%)** | 缩放锚点已经够到精力条，补回属于**重复计算** |
| `INCONCLUSIVE by configuration` | 当前减免%让两个模型数值重合，**数学上分不出** |
| `INCONCLUSIVE: ... margin` / `no model fits` | 落在模型之间，不下结论 |

> **要让这一项真正有答案，得把 `KiDamageReductionPercent` 设成 50。**
> 默认的 100% 会让"缩放后耗精 = 0"与"字段没动"变成同一个数字，无法区分 ——
> 这种情况下 MOD 会明说 `INCONCLUSIVE by configuration` 而不是猜一个。
> 可分辨区间是 20–80，50 最清晰。测完再改回你想要的数值即可。

**拿到结论后该改什么（两种你自己就能改）**：

| `KIV` 结论 | 你该改成 |
| --- | --- |
| 掉了**全额 D** 或 字段**根本没动** | 保持默认 `KiTopUp=1`（省精力就靠"补回"这一步） |
| 掉了 **D×(1-减免%)**（缩放已够到精力条） | **`KiTopUp=0`** —— 否则两条机制都减，`KiDamageReductionPercent=50` 会实际减掉 75% |

关掉补回**不会**失去诊断能力：MOD 仍然采样并继续输出 `KIV` 行。

> **旗标事件源下 `KIV` 不适用**（那里看不到扣精量），看 `KIV-FLAG`：
> `reference` 是扣精前的基准值、`handed back` 是实际补回的量。若某次格挡
> 连 `KIREF` / `KIV-FLAG` 都没有，说明扣除落在 `KiTopUpPreEventMs` 窗口之外，
> 调大它即可（代价见上一节）。

---

## 日志与排查

- 日志文件：`Nioh1PerfectGuard.gameplay.log`（DLL 同目录）
- 启动时会打印每条锚点的校验结果：
  ```
  ANCHOR ok   guard_flag_1      rva=0x74DD09
  ANCHOR MISS guard_ki_cost     rva=0x74DE51 want=[...] got=[...]
  ANCHOR 3/4 verified
  SELFTEST breakpoint: OK: 5/5 probe calls trapped by the VEH (DR0=..., Dr7=L0 exec)
  STATUS DEGRADED(no-reward) anchors=3/4 reduction=100% recovery=3 gate=1 mask=0x0100 slot=0 sound=1
  ```
- **`SELFTEST` 行是关键**：MOD 每次启动都在**自己的代码**上装一个硬件执行断点、
  调用 5 次、数 VEH 是否全部捕获。因为"断点在本机不生效"的表现是
  **零反应、零崩溃、日志一片安静**，和"还没进关卡"完全分不清。
  看到 `OK` 才说明核心机制已被证实；`FAILED` 就不用往下试了，直接把日志发回。
- 四个锚点角色不同，**失效后果也不同**：

  | 锚点 | 角色 | 失效后果 |
  | --- | --- | --- |
  | `guard_flag_1` / `guard_flag_2` | 仅日志 | 少一些诊断行 |
  | `guard_ki_cost` | **权威事件源** | **任何奖励都不会触发** |

  所以当扣精锚点没通过时，状态会显示 `DEGRADED(no-reward)` 而不是 `ACTIVE`
  —— 避免出现"装上了但完全没用、日志又看不出原因"的沉默失败。
- `ANCHOR` 少于 2 条通过时 MOD 会**拒绝安装**，状态为
  `NOT INSTALLED: only n/4 anchors verified`；`Enabled=0` 时状态为 `BYPASS: Enabled=0`。

---

## 锚点（供参考）

| 名称 | RVA | 含义 |
| --- | --- | --- |
| `guard_flag_1` | `0x74DD09` | 置「本次攻击被格挡」标志位（分支一） |
| `guard_flag_2` | `0x74DD92` | 同函数的另一条格挡分支 |
| `guard_ki_cost` | `0x74DE51` | 格挡扣精调用；`xmm1` = 消耗量，`rcx` = 资源表项 |
| `inputmgr_getter` | `0xE6BE90` | 输入管理器单例 getter（**只校验字节、不装断点**） |

资源表：`[[char+0x240]+0xBA8]`，表项 0x50 字节，`entry = table + 8 + index*0x50`；
格挡用的是索引 7，表项 `+0x0C` 当前值、`+0x10` 消耗量；系数常量 `0x1588828` = `0.2f`。

输入管理器：全局指针槽 = getter 的 `disp32` 推出（本版本 = `base + 0x1B78658`）；
`+0x49E08 + slot*20` = 16 字节 `XINPUT_STATE`，**`+0x49E0C + slot*20` = `wButtons`**，
`+0x49E54` = 轮询计时器。防御键按下检测走这条**纯读取**路径，不调用任何游戏代码。

> ⚠️ `inputmgr_getter` 每帧被高频调用，**刻意不装断点**。早期版本装了它，
> 结果异常洪水把后台线程拖死、日志停在 `ANCHOR 4/4 verified`。锚点表现已加
> `armable` 标志区分「只校验」与「要挂钩」。

> ⚠️ 线程布点函数必须**跳过调用线程自己**，否则自我挂起 → 永久死锁。
> 这个 bug 已修复（探针里同样存在）。

---

## 已知限制

- 目前**每次成功格挡**都会触发收益（`RequireTimelyGuard=0`）。
  精确的「新按下防御」时间窗口需要锚点 A，正在定位中。
- 对敌削精 / HP、提前接续 尚未实现（锚点待定位）。
- 视觉特效**不在计划内**（仁王 2 版的金色特效依赖引擎 effect factory，
  移植性价比低），只保留音效反馈。
- 建议**离线**使用（仁王 1 有联机存档校验）。
