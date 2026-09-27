# 仁王1 精防 MOD —— 实机验收测试说明

> **只想快速装上用起来？** 看同目录的 **`QUICKSTART.md`**（一页）。
> 想知道"哪些已经验证过、修过什么"？看 **`CHANGELOG.md`**。
> 本文件只讲**你要动手做的那几步**。

## 一、装完之后应该长这样

```
E:\SteamLibrary\steamapps\common\Nioh\
  nioh.exe
  dinput8.dll                       ← 通用 MOD 加载器（balfa 的 NiohNativeModLoader）
  mods\
    Nioh1PerfectGuard\
      Nioh1PerfectGuard.dll         ← 本 MOD
      Nioh1PerfectGuard.ini
      Sounds\parry.wav
      source\Nioh1PerfectGuard.c    ← 源码（只是给你查阅用，运行不需要）
      source\pg_logic.h
```

**关键点**：加载器会在进游戏后自动扫描 `mods\` 并加载 DLL，**4/4 锚点校验通过**后日志出现
`STATUS ACTIVE`。已实测在游戏里稳定运行 72 秒以上，无崩溃、无卡顿。

**如果没生效**：先看 `mods\loader.log` 里有没有
`Loading mod: ...\mods\Nioh1PerfectGuard\Nioh1PerfectGuard.dll` 这一行 —— 没有就是加载器没扫到，
和本 MOD 无关。

---

## 二、怎么进游戏

1. 用 **Steam** 启动（`nioh.exe` 受 Steam DRM 保护，直接双击会在约 28 秒后自己退出）。
2. **建议离线模式游玩**：仁王1 有在线存档校验，避免云端存档被 MOD 影响。
3. 必须**新建游戏并进入关卡**。标题画面不会构造玩家对象，此时 MOD 只完成安装、不会打任何日志。

---

## 三、进关卡后请依次做这几件事

> **重要更新**：检测链已经在**游戏自己的标题画面演示模式**里被验证过了 ——
> 没人操作也会产生真实格挡事件，`PERFECT GUARD` / `LAYOUT` 都会正常输出（详见 `CHANGELOG.md`）。
> 所以你这一局的**主要目的不再是"验证能不能识别"**，而是：
> ① 用你自己的手感确认**窗口 250ms 是否合适**；② 回答 `KIV` 那个字段归属问题；
> ③ 确认几项收益在**真人操作**下的表现（回精手感、对敌效果、后摇取消）。

| 场景 | 怎么做 | 预期日志 |
| --- | --- | --- |
| 1 | 站着不动，被打 5 次（**不按防御**） | `KITRACE` 有输出 |
| 2 | 按防御，**按住不放**，被打 5 次 | 有 `PLAYER BLOCK`，但没有 `PERFECT GUARD` |
| 3 | **先把 `KiDamageReductionPercent` 改成 `50`**（见下方说明），然后看准敌人攻击**快打到你的一瞬间**再按防御，做 5 次 | `PERFECT GUARD` + `LAYOUT` + `KITRACE` + `KIV` |
| 4 | 打敌人，**让敌人成功格挡你** 5 次 | `GUARD (other) ... ignored`，**不应**出现 `PERFECT GUARD` |
| 5 | 把 INI 里 `EnemyKiDamage` / `EnemyHpDamage` 改成非 0，再精防 5 次 | `ENEMY ki damage` / `ENEMY hp damage` |
| 6 | 想试后摇取消：INI 里 `CancelRecovery=1`，再精防 | `RECOVERY cancel` |

**第 4 项很重要**：它验证的是上一轮修掉的语义 bug ——
"格挡标志位"表示的是**我的攻击被格挡了**，所以 MOD 必须只给**玩家自己格挡成功**发奖励。

### 每条详细日志都有上限，超了怎么办

为了让日志不至于越打越大，逐条事件的详细行都有数量上限（例如 `PERFECT GUARD` 前 60 次、
`KIV` 前 40 次、`LAYOUT` 前 20 次）。上限都留了充足余量，照上面做完**不会**触顶。

如果确实打久了、详细行不再出现：**随时按 `Ctrl+Shift+F10`**，
会打一条 `MANUAL EXPORT` 加一组 `STATE`，里面有累计的
`blocks= / perfect= / rewards= / freeguards= / rearmed=` 计数 —— 总量永远看得到。

### 场景 1 与场景 3 里的 `KITRACE` 用途不同

`KITRACE` 每行都会告诉你**这一行是哪个字段变了**：

```
KITRACE ki=210.5/250 | entry7 flag=1 cur=1180 cost=6.4  <-- changed: ki
```

- **场景 1（不防御挨打）**：主要看 `changed: ki` —— 说明"当前精力"字段确实在动；
- **场景 3（成功格挡）**：关键看格挡前后那几行是 `ki` 变了、`entry7` 变了、还是**两个都变**。
  这正是 `KIV` 行用来下结论的原始数据。

### 日志在哪

```
E:\SteamLibrary\steamapps\common\Nioh\mods\Nioh1PerfectGuard\Nioh1PerfectGuard.gameplay.log
```

---

## 四、日志标记对照表

| 标记 | 含义 |
| --- | --- |
| `ANCHOR 4/4 verified` | 锚点自校验通过（没有这行说明 DLL 没被加载） |
| `SELFTEST breakpoint: OK: 5/5 ...` | **硬件断点机制在 nioh.exe 进程内确实会触发**（每次启动自动验证） |
| `SELFTEST breakpoint: FAILED ...` | 断点机制在本机不工作 → 任何奖励都不可能触发，请把日志发回 |
| `STATUS ACTIVE ...` | MOD 已生效，括号里是当前配置 |
| `STATUS DEGRADED(no-reward)` | 扣精点锚点校验失败：**不会给任何奖励**，请把日志发回 |
| `GUARD pressed (pad=0x.... mask=0x.... key=0x.. key_down=..)` | 检测到防御键**新按下**（`mask` 显示的是你当前配的掩码） |
| `PLAYER BLOCK #n -- not within 250ms` | 玩家格挡了，但不在窗口内（不算精防，符合预期） |
| `NOTICE: a previous session established that the guard-cost event never fires on this build, ...` | **正常**：上次游玩已确认扣精点在本版本不触发，所以这次**一开局**就用旗标事件源（不再需要先打满 3 次）。删掉 `Nioh1PerfectGuard.state` 可重新判定 |
| `NOTICE: the guard-cost event fires after all, ...` | **正常**：扣精点其实会触发，MOD 已切回精确事件源（扣精缩放重新生效） |
| `SOUND ready for immediate playback (event driven, no polling delay)` | 音效已就绪，且**不再有轮询延迟**（触发即响） |
| `SOUND thread: the audio engine never became ready` | 音频引擎始终没起来（对照 README 的音效排查表） |
| `PERFECT GUARD #n via guard-cost/guard-flag (guard pressed Xms ago)` | 判定为精防，X 是按下到格挡的间隔；`via` 说明用的是哪个事件源 |
| `NOTICE: 3 player blocks were seen by the guard-flag site but the guard-cost site has never fired, ... It will switch back automatically if a guard-cost event ever appears.` | **正常**：自动模式发现扣精点不触发，已改用旗标事件源（奖励照常）；若扣精点后来出现会自动切回 |
| `GUARD (other) entry=0x... ignored` | 敌人格挡了**我的**攻击 —— 正确忽略，不发奖励 |
| `KITRACE ki=.. entry7=..` | 两个候选精力字段各自的数值，并**标明这一行是谁变了**（`changed: ki` / `entry7` / `ki AND entry7`）。限流 1 行/秒，共 1200 行上限 |
| `KITRACE capped at 1200 lines ...` | 正常维护（限流到顶），不是错误 |
| `KIRESTORE a -> b (snapshot .., attributed .. of charge .., peak loss .., gave ../..%)` | 可见精力被补回的金额与比例 |
| `FLAGDIAG via guard_flag_1 ctx=.. ctx160=.. A=.. ply=.. st=.. cur=.. cost=.. B=.. atk=..` | 诊断：某个"攻击被格挡"标志被置位时，命中上下文里双方 + 攻击方的状态（`ply=1` 表示那一侧就是玩家；`st` 是格挡资源项的状态字） |
| `KIV block #n charge D=.. visible loss L=.. scaled D*(1-r)=.. residual=..% first loss after Nms peak=.. -> ...` | **自动判定**：格挡耗精到底改的是哪个字段，以及是否可信 |
| `LAYOUT player=0x.. hp=../.. ki=../.. action=.. frame=..` | 整张字段表是否与推测一致 |
| `ENEMY ki damage .. / ENEMY hp damage ..` | 对敌效果写入成功（需把 `EnemyKiDamage`/`EnemyHpDamage` 设为非 0） |
| `ENEMY effects skipped: the player (0x..) is neither ...` | 拒绝猜测目标（玩家不在命中上下文两个角色里），**没有写任何内存** |
| `RECOVERY cancel: motion frame X -> Y` | 后摇被提前接续（需 `CancelRecovery=1`） |
| `MANUAL EXPORT: version=.. blocks=.. perfect=.. rewards=.. freeguards=..` | 你按了 `Ctrl+Shift+F10`，紧接着会打一组 `STATE`（见第七节） |
| `ARM purge: dropped n stale thread id(s)` | 正常维护（清理被系统回收的线程 id），不是错误 |
| `ARM rolling re-arm: first full cycle done (...)` | 正常维护（约 30 秒转完一圈，逐批重装断点，**不会卡顿**） |
| `WARNING: ... NO guard press was ever detected` | 键鼠玩家没标定防御键，精防门永远不会开 |
| `NOTICE: no controller is connected` | 没检测到手柄，需要按下面第五节做键盘标定 |
| `NOTICE: a trigger is being squeezed (LT=.. RT=..) ...` | 你在按扳机键（L2/R2），但 `GuardButtonMask` **只能匹配 14 个按钮位、匹配不到扳机** —— 换一个键做防御，或用 `GuardKeyVK` |
| `NOTICE: Enabled changed to N, but installing or removing the breakpoints happens only at startup` | 你运行中改了 `Enabled`；**需要重启游戏**才生效（其它键都是热更新） |
| `NOTICE: DiagDisable=1 -- no breakpoint will be armed ...` | 你打开了诊断开关位 1；此状态下 MOD 检测不到任何东西（仅供排查） |
| `NOTICE: DiagDisable=2 -- guard input polling is off ...` | 诊断开关位 2；不会检测任何按键 |

### `SELFTEST` 那行是什么

整个 MOD 靠"硬件执行断点 + VEH"工作。**如果这套机制在本机不生效，表现是
"零反应、零崩溃、日志一片安静"** —— 和"还没进关卡"完全分不清。所以
MOD 每次启动都会在**自己的代码**上装一个断点、调用 5 次、数 VEH 是否全部捕获：

```
SELFTEST breakpoint: OK: 5/5 probe calls trapped by the VEH (DR0=..., Dr7=L0 exec)
```

看到 `OK` 就说明"断点会响"这件事**已经被证实**；如果出现 `FAILED`，
那么后面不管你怎么打都不会有奖励，请直接把这行发回。

### `KIV` 那行是本轮新增的关键诊断

静态分析无法断定"格挡耗精"改的 `entry+0x0C` 与可见精力条读的 `param+0x40`
是不是同一个东西（`Refer::StaminaRate` 读的是后者）。MOD 现在直接把两个数摆在一起：

- `charge D` = 引擎被告知要扣的**原始**耗精量（缩放前抓的）
- `visible loss L` = 确认过的精力字段**实际**掉了多少
  （只统计格挡后 **250ms 内**看到的掉落 —— 更晚的掉落很可能是你自己反攻花的精力，
  不会被算成格挡耗精，也不会被"补回"）
- `peak` = 整个 400ms 窗口里见过的最大掉落（仅作参考）
- `first loss after Nms` = **引擎花了多久才把耗精写进这个字段** ——
  这个数字直接告诉你 400ms 的观察窗口够不够
- `residual` = 这个损失与所选模型差了多少（占 `D` 的百分比；越小越可信）

然后自动给出结论：

| 日志结论 | 说明 |
| --- | --- |
| 字段**没动** | 那张资源表和精力条无关 |
| 掉了**全额 D** | 缩放锚点够不到精力条，**补回**才是真正生效的机制 |
| 掉了 **D×(1-减免%)** | 缩放锚点**已经**够到精力条，补回属于**重复计算** |
| `INCONCLUSIVE by configuration` | **配置本身无法区分**（见下），改了减免%再来 |
| `INCONCLUSIVE: ... margin ...` | 这个损失落在两个模型之间，分不出来 |
| `INCONCLUSIVE: no model fits` | 既不是全额也不是缩放后（部分传导 / 精力自然回复吃掉了样本） |

> ### ⚠️ 一定要把 `KiDamageReductionPercent` 设成 `50` 再来测这一项
>
> 默认是 `100`，此时"缩放后的耗精" = `D×(1-100%)` = **0**，
> 和"字段根本没动"是**同一个数字** —— 数学上无法区分。
> 旧版本的判定会在这种情况下武断地选一个（而且是错的），
> 现在它会明确告诉你 `INCONCLUSIVE by configuration` 并建议改成 50。
>
> 经验上 **20–80 之间**才可分辨（MOD 也按这个范围判断），**50 最好**。
> 测完这一项再改回你想要的数值即可（热更新，立刻生效）。
>
> 换句话说：**默认配置下这一项是答不出来的**，这不是 bug，是这道题的固有限制。

### 拿到 `KIV` 结论之后该怎么办（已经替你排好）

三种结论对应三种不同的后续处理。**其中两种你现在就能自己改 INI 完成**，
不需要等我出新版本：

| `KIV` 结论 | 含义 | 你该改成 |
| --- | --- | --- |
| 掉了**全额 D** | 缩放锚点**够不到**精力条，真正省精力靠"补回" | **保持默认**（`KiTopUp=1`）即可 |
| 字段**根本没动** | 那张资源表和精力条无关，省精力完全靠"补回" | **保持默认**（`KiTopUp=1`）即可 |
| 掉了 **D×(1-减免%)** | 缩放锚点**已经**够到精力条，补回属于**重复计算** | 把 **`KiTopUp` 改成 `0`**（只保留缩放），热更新立即生效 |
| `INCONCLUSIVE ...` | 没测出来 | 按上面那条提示调整（多半要把减免%设 50）后重试 |

**为什么必须知道这个**：如果结果是第三种而 `KiTopUp` 仍是 1，
那么你设 50% 减免时会实际减掉 75%（`1-0.5×0.5`）—— 不是按你写的数字生效。
把 `KiTopUp` 设成 0 之后，减免比例才精确等于你配的值。

---

## 五、默认配置（`Nioh1PerfectGuard.ini`）

> 下面是常用键；**完整键表与音效排查表见 `README_CN.md`**。
> 想换精防音效：放一个 16-bit PCM、44.1/48kHz 的 WAV 到 `Sounds\`，
> 改 `SoundFile` 即可（热更新生效，不用重启）。

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `WindowMs` | 250 | 精防窗口（毫秒） |
| `RequireTimelyGuard` | 1 | 只有窗口内新按下防御的格挡才算精防 |
| `KiDamageReductionPercent` | 100 | 格挡耗精减免 0～100%，**100 = 完全免耗精** |
| `KiTopUp` | 1 | 是否额外**补回**可见精力。**测 `KIV` 时把减免改成 50**；若 `KIV` 说"缩放已够到精力条"则改为 0，避免重复计算 |
| `KiRecoveryMode` | 3 | 回复最大精力的 1/6 |
| `GuardButtonMask` | 0x0100 | 手柄 L1/LB |
| `GuardKeyVK` | 0 | 键盘键位，**键鼠玩家必须标定，见下** |
| `EnemyKiDamage` / `EnemyHpDamage` | 0 / 0 | 对敌效果，默认关闭 |
| `CancelRecovery` | 0 | 后摇取消，**实验性**，默认关闭 |
| `KiTrace` | 1 | 诊断日志，验收完可以改 0 |
| `BlockEventSource` | 2 | **哪个事件代表"玩家格挡成功"**：`2`=自动（推荐，先试扣精点，不灵就自动改旗标点）/ `0`=只用扣精点 / `1`=只用旗标点。一般不用改 |
| `DiagDisable` | 0 | **诊断位掩码，平时保持 0**。用于定位崩溃来自哪一部分（1=不装断点 / 2=不启动输入线程 / 4=开启"滚动重装" / 8=不跑自检）。**位 4 已知会弄崩游戏，仅供排查** |

**INI 支持游戏内热更新**（约 1 秒生效，改 DLL 才需要重启）。

### 键盘玩家：两步标定防御键

`GuardButtonMask` 默认是手柄的 L1/LB。**键盘玩家的默认配置下不会有任何奖励触发**，
日志里只会刷"不算精防"，很容易误判成 MOD 坏了。所以：

1. 把 INI 里 `LearnButtons` 改成 `1`，回到游戏**按一下你的防御键**
2. 日志会出现（例）：
   ```
   LEARN key VK=0xA0 pressed -> GuardKeyVK=160
   ```
3. 把这个数值填进 `GuardKeyVK`，再把 `LearnButtons` 改回 `0`

> 参考：左 Shift = 160、右 Shift = 161、Ctrl = 17。数字是**十进制** VK 码。

---

## 六、请回传这些

1. **`KIV` 的结论** —— 这行直接决定"耗精减免"最终该用哪种实现
2. **`KIRESTORE` 是否触发** —— 以及补回时精力条看着是否稳定（有没有抖动/回弹）
3. **`LAYOUT` 各字段是否与推测一致** —— HP / 精力 / 上限 / 动作ID / 动画帧
4. **`PERFECT GUARD` 里的间隔毫秒数** —— 判断 250ms 窗口是否合适
5. **有没有出现第 4 项（敌人格挡）误发奖励**

一句话：**整个 `Nioh1PerfectGuard.gameplay.log` 发回来就行**，不用自己筛选。

---

## 七、随时按 `Ctrl+Shift+F10` 打一张 `STATE`

想知道 MOD 当时看到的完整状态，就在游戏里按 **Ctrl+Shift+F10**，日志会多出这样一组：

```
STATE (player object appeared - now in a mission) version=0.1.0-nioh1
STATE anchors=4/4 installed=1 mask=0x0100 slot=0 vk=0 gate=1 window=250ms reduction=100% recovery=3
STATE input slot=0x.. mgr=0x.. pad=0x0000 conn=[0,0,0,0] guard_down=0 presses=0 lt=0 rt=0
STATE player=0x.. param=0x.. hp=1200/1200 ki=210.5/250 guard_res flag=1 cur=1180 cost=6.4 frame=4.17 action=12
STATE ctx=0x.. charA=0x.. charB=0x.. | blocks=0 perfect=0 rewards=0 freeguards=0
```

**进关卡后第一次解析到玩家对象时也会自动打一次。**
`conn=[0,0,0,0]` 表示没接手柄；`pad` 是当前按键位掩码。

如果日志以 `STATE ... no module base yet` 结尾，说明连模块基址都还没拿到，
MOD 没有真正装上 —— 请连同 `mods\loader.log` 一起发回。
