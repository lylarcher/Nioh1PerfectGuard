# DamageRate（承受伤害倍率）叠加语义 — 只读静态逆向报告

- 目标：`nioh.exe` 1.24.8，镜像 `_work\nioh1.mem.exe`（file offset == RVA，image base `0x140000000`；
  本机 dump 的 PE ImageBase 字段为 `0x7FF7AA2D0000`，下文一律用 **RVA**）。
- 问题：MOD 的 10% 减伤与装备自带的 10% 减伤，最终是 **叠加(sum) 还是相乘(product)**；
  若是相乘，MOD 该传什么 magnitude。
- 结论：**引擎的承伤修正层是「相乘」，不是相加**。所以传 10% 的朴素百分比**不是**加法叠加。
  但修正公式所需的 `r_gear` 可以从**引擎自己的状态容器**里精确读出来（见 §3 表达式）。
- 全程静态分析，未运行游戏。所有"推测"都标注了置信度。

---

## 0. 一句话结论表

| # | 问题 | 结论 | 置信度 |
|---|---|---|---|
| 1 | 多个承伤修正是相加还是相乘？ | **相乘**。所有 DamageRate 家族的状态对象各自把自己参数乘进管理器同一对 float 累加器（`mgr+0x24`/`+0x28`）。全二进制**没有**任何"减伤求和"的类或代码路径 | **confirmed**（机制）/ **probable**（这是唯一的承伤通道） |
| 2 | 运行时在哪读"当前承伤值"？ | 首选（可枚举、可自证）：**遍历状态容器 `[mgr+0x170]` 红黑树，取所有 `[obj+0x10]==0x1E` 的对象的 `[obj+0x50]`，相乘**。次选（字段级）：`*(float*)(mgr+0x24)` / `*(float*)(mgr+0x28)`（apply 就是写这两个字段），但静态上**没找到读取它的游戏代码** → 语义需一次实机确认 | 枚举法 **confirmed**；`mgr+0x24` 作为"最终生效值" **probable（未证实）** |
| 3 | 该传什么 magnitude？ | `our = (F_other − r_mod) / F_other`，其中 `F_other = Π [obj+0x50]`（所有**非自身**的 0x1E 状态对象） | 公式 **probable**（前提是 §1 的乘积语义成立） |
| 4 | 有引擎侧上限/钳制吗？ | **未发现**承伤总量的钳制；有两处容易误认的常量（`1.25f` 是移速类累加器的钳制，`1.25/1.33/0.5` 是攻击侧 `mgr+0x48` 的乘数） | "未发现"（不是"证明不存在"） |

顺带得到 3 条对现有笔记的**修正**（§6），其中一条解释了"为什么 `0x7A25C0` 一调就崩"。

---

## 1. Q1：引擎怎么合并多个承伤修正 —— 相乘

### 1.1 承伤修正的落点：`AddStateObjectDamageRate` 的 apply

类的 vtable `0x11A8280`（RTTI `.?AVAddStateObjectDamageRate@Character@@`，`_work\nioh1.mem.rtti.json`），
9 个槽位（索引来自 rtti.json，逐个反汇编核对过）：

| slot | vt+ | RVA | 作用 |
|---|---|---|---|
| 0 | +0x00 | `0x79C6B0` | 析构（`lea vtable 0x11A80F0`；`test dl,1` → 走分配器释放 0x58） |
| 1 | +0x08 | **`0x7AC550`** | `movss [rcx+0x50],xmm1` → **写参数（倍率）** |
| 2 | +0x10 | `0x7A3DC0` | `movss xmm0,[rcx+0x50]; ret` → 读参数 |
| 3 | +0x18 | `0x7A4B40` | `al = (1.0f > [rcx+0x50])` → "这是不是一个减免" |
| 4 | +0x20 | `0x7A5720` | `al = (rdx && [rdx+0x10] == [rcx+0x10])` → 同状态 ID |
| 5 | +0x28 | `0x7A6190` | onActivate（转 `0x7AC190`） |
| **6** | **+0x30** | **`0x7A8C30`** | **apply ← 全部算术在这里** |
| 7 | +0x38 | `0x7A7980` | `ret 0`（onDeactivate 空实现） |
| 8 | +0x40 | `0x7A8110` | `ret 0` |

`0x7A8C30` 全文（14 字节，无 `.pdata`，叶子函数）：

```asm
007A8C30  f30f104224   movss xmm0, dword ptr [rdx + 0x24]
007A8C35  f30f594150   mulss xmm0, dword ptr [rcx + 0x50]     ; × 状态参数
007A8C3A  f30f104a28   movss xmm1, dword ptr [rdx + 0x28]
007A8C3F  f30f114224   movss dword ptr [rdx + 0x24], xmm0     ; 写回
007A8C44  f30f594950   mulss xmm1, dword ptr [rcx + 0x50]
007A8C49  f30f114a28   movss dword ptr [rdx + 0x28], xmm1     ; 写回
007A8C4E  c3           ret
```

`rcx` = 状态对象，`rdx` = 作用目标。**关键点：这里是 `mulss`，不是 `subss`/`addss`。
所以任意多个 DamageRate 状态对象 ⇒ 累加器被连乘。**

### 1.2 apply 的 `rdx` 是谁 —— 是角色状态管理器（`param+0x10B0`）

通用状态 tick `0x7ACCA0`（`.pdata` 0x7ACCA0-0x7ACDAF，271 字节）在每个 tick **无条件**调用 slot 6：

```asm
007ACCAA  80791c00      cmp byte ptr [rcx + 0x1c], 0
007ACCAE  488bfa        mov rdi, rdx                  ; 保存传入的"目标"
...
007ACCEE  f30f104334    movss xmm0, [rbx + 0x34]      ; 计时器
007ACCF3  f30f5cc7      subss xmm0, xmm7              ; -= dt
...
007ACD0E  488b03        mov rax, [rbx]                ; vtable
007ACD11  0f28d7        movaps xmm2, xmm7             ; dt
007ACD14  488bd7        mov rdx, rdi                  ; ★ rdx = tick 的第二个参数
007ACD17  488bcb        mov rcx, rbx
007ACD1A  ff5030        call qword ptr [rax + 0x30]   ; ★ slot 6 = apply
...
007ACD8F  f30f104328    movss xmm0, [rbx + 0x28]      ; 寿命计数
007ACD97  0f97c0        seta al                       ; al = (寿命 > 0) → 还活着
```

8 个 tick 调用点，逐个反汇编确认 `rdx` 全部 = **管理器对象**（=`[char+0x240]+0x10B0`）：

| tick 调用点 | 所属函数 | tick 的 rdx | 性质 |
|---|---|---|---|
| `0x7AD393` | `0x7AD260` | `mov rdx,rbp`，rbp=rcx=`mgr` | 遍历红黑树 `[mgr+0x170]`（**`add()` 插入的那棵树**） |
| `0x7AC866` | `0x7AC6E0` | `mov rdx,rsi`，rsi=`mgr` | 单个对象 `[mgr+0x180]` |
| `0x7AC90F` | `0x7AC6E0` | `mov rdx,rsi` | 链表 `[mgr+0x188]` |
| `0x7AC9CF` | `0x7AC6E0` | `mov rdx,rsi` | 链表 `[mgr+0x198]` |
| `0x7AD1B1` | `0x7ACDB0` | `mov rdx,rsi` | 树 `[mgr+0x1a8]` |
| `0x7AD4F4` | `0x7AD470` | `mov rdx,rdi`，rdi=rcx=`mgr` | 单对象 `[mgr+0x180]` 的另一种处理 |
| `0x7AD60F` / `0x7AD70F` | `0x7AD540` / `0x7AD640` | 同形状（未逐条展开） | — |

并且 `add()`（`0x7A2890`，见 §1.4）把新对象插入的正是 `[mgr+0x170]` 那棵树 —— 与 `0x7AD260` 遍历的是同一棵。
⇒ **MOD 用 `add()` 挂上去的 DamageRate，每 tick 都会把 `mgr+0x24` 和 `mgr+0x28` 各乘一次自己的参数。**

### 1.3 家族内所有"承伤/消耗"类 apply 全是乘法（84 个 `AddState*` 刷了一遍）

用 `nioh1.mem.rtti.json` 里 86 个 `AddStateObject*` 的 slot 6 逐个反汇编，按 `rdx`（管理器）目标偏移归类：

| 管理器偏移 | 谁在乘 | 语义（按类名） |
|---|---|---|
| `+0x0c` | `Attack` | 输出伤害 |
| **`+0x24`** | **`DamageRate`**、**`PhysicalDamageRate`**、`Chaos`、`Water` | **承伤倍率 #1（物理）** |
| **`+0x28`** | **`DamageRate`**（只它写第二个） | **承伤倍率 #2** |
| `+0x2c`/`+0x30` | `Earth`、`Shouheki`、`StaminaDamageRate` | 精力伤害倍率 |
| `+0x34`/`+0x38` | `AttackDamageRate` | 攻击伤害倍率 |
| `+0x3c` | `DoubleAttackDamage` | |
| `+0x40` | `AttackStaminaDamageRate` | |
| `+0x48` | `Fire`、`StaminaRecover` | → 发布到 `param+0x6c` |
| `+0x4c`/`+0x50`/`+0x54` | `UseStaminaRate` / `…Attack` / `…Dodge` | |
| `+0x60` | `Dirty` | |
| `+0x64` | `AmritaGaugeUp` | |
| `+0xf0` | `MoveSpeed`、`Paralysis`、`Thunder`、`Slow` | 移速 |
| `+0xfc`/`+0x100`/`+0x104`/`+0x110`/`+0x114` | `MoneyUp`、`YashiroBonus`、`AmritaUpPlus`、`FavoriteUp`、`DamageSacrifice`、`BulletDamageSacrifice` | **唯一一批 `addss`（加法）** —— 都是"资源/金钱/精华收益"，**没有一个与承伤有关** |
| `+0x108`/`+0x10c`/`+0x11c`/`+0x154` | `ShotSometimeFree`、`CallSpirit`、`Blind`、`Stealth` | |

> 目标寄存器已抽查核对（避免"把别处的同偏移当成管理器"）：`StaminaRecover`(0x7A9EF0) 直接写 `[rdx+0x48]`；
> `Fire`(0x7A9000) 首两条即 `mov rbx,rdx` 后写 `[rbx+0x48]`；`MoveSpeed`(0x7A9760) 为 `mov rbx,rdx; mov rdi,rcx`，
> 之后虽然用 `[rcx+8]`→角色→`[char+0x230]` 查条件，但写的是 `[rbx+0xf0]`=`mgr+0xf0` ✓。

**没有任何一个类的 apply 对承伤做 `addss` 或 `subss`。** 引擎也没有"减伤求和"的聚合函数
（如果存在，它必须遍历状态并累加到一个字段 —— 全二进制找不到这样的代码；见 §1.5 的负结果）。

### 1.4 引擎自己也是这么挂减伤的（强旁证）

1. **格挡减伤就是 DamageRate**。格挡/命中状态机 `0x749640`（26,632 字节）内：

```asm
0074D869  f30f10159bb0e300  movss xmm2, [rip+0xe3b09b]  ; -> 0x158890C = 0.95f
0074D871  f30f100ddb73a500  movss xmm1, [rip+0xa573db]  ; -> 0x11A4C54 = 300.0f
0074D879  498bce            mov rcx, r14               ; r14 = [char+0x240]+0x10b0
0074D87C  e8cf1b0500        call 0x79F450              ; DamageRate ctor(mgr, 300, 0.95)
0074D881  ba45000000        mov edx, 0x45              ; 树键 = 0x45
0074D886  e919010000        jmp  <add>                 ; 0x7A2890
```
  即"格挡时承受伤害 ×0.95"就是往同一棵树上挂一个 DamageRate。**MOD 的做法和引擎同构。**
   同一函数里还用 `find_by_key` 查 `0x44`/`0x45`（`0x74D693` / `0x74D7AD`）。

2. **引擎从角色 ability 数据推导 DamageRate**（`0xA83F97` 巨型函数内）：

```asm
00A84205  f30f108050030000  movss xmm0, [rax+0x350]    ; rax = ability 块（见下）
00A8420D  410f2fc2          comiss xmm0, xmm10         ; > 0 ?
00A84211  764c              jbe  skip
00A84222  f30f100d9ac87100  movss xmm1, [rip+0x71c89a] ; -> 0x11A0AC4 = 600.0f
00A8422A  488d8bb0100000    lea rcx, [rbx+0x10b0]      ; mgr
00A84231  f30f5cb050030000  subss xmm6, [rax+0x350]    ; ★ magnitude = xmm6 − stat
00A84239  0f28d6            movaps xmm2, xmm6
00A8423C  e80fb2d1ff        call 0x79F450              ; DamageRate(600, 1−stat)
00A84246  488d8bb0100000    lea rcx, [rbx+0x10b0]
00A8424D  4c8bc0            mov r8, rax
00A84255  ba01000000        mov edx, 1                 ; ★ 树键 = 1
00A8425A  e831e6d1ff        call 0x7A2890              ; add()
```
   ability 块取值：`p = [param+0xB98] ?: (param+0x9D8)`，再经 `0x7B2D40(p)`
  返回统计块（`0x7B2D40`：`[rcx+0x198]`，为空则返回一个静态默认块；1155 个调用者）。

3. **数据表驱动**（装备/技能特殊效果最可能走的路径）。`0x7241E0` 是"按记录类型建状态对象"的分发器
   （**唯一调用者 `0x71F186`**）：

```asm
007241E0  488b01            mov rax,[rcx]              ; rcx = 效果记录
007241EC  4c8b9040020000    mov r10,[rax+0x240]
00724200  4981c2b0100000    add r10,0x10b0             ; r10 = mgr
007241FC  0fb64109          movzx eax, byte ptr [rcx+9]   ; 记录 +9 = 状态类别(1..0x58)
0072421B..             查表 [0x724404] / [0x7243D8] → 88 个 ctor 之一
...
00724335  0fbf410e          movsx eax, word ptr [rcx+0xe] ; ★ magnitude 原始值(int16)
0072433D  0fb6410c          movzx eax, byte ptr [rcx+0xc] ; ★ 寿命原始值(byte, 分钟)
0072434B  mulss xmm2,[0x1588710]  ; × 0.001  → magnitude = int16/1000
00724356  mulss xmm1,[0x1588AEC]  ; × 60     → 寿命 = byte*60 秒
0072435E  jmp  0x79F450           ; ★ DamageRate ctor
```
   调用方随后（`0x71F183` 分支）：

```asm
0071F186  e855500000   call 0x7241E0               ; 造对象（rax）
0071F190  0fb7570a     movzx edx, word ptr [rdi+0xa]
0071F194  488bcb       mov rcx, rbx                ; rbx = mgr
0071F197  ffca         dec edx                     ; ★ 树键 = 记录 +0xa − 1
0071F19E  41b9ffffffff mov r9d, -1
0071F1A4  c644242000   mov byte [rsp+0x20], 0
0071F1A9  4c8bc0       mov r8, rax
0071F1AC  e8df360800   call 0x7A2890               ; add(mgr, key, obj, -1, 0)
```
   → **装备特殊效果很可能就是"记录 +0xe = 900 → magnitude 0.9 → DamageRate"**。
   （记录字段：`+8` 低 3 位 = 类型 1..4，`+9` = 状态类别+1，`+0xa` = 键+1，`+0xc` = 寿命/数值，
   `+0xe` = magnitude×1000。）

### 1.5 负结果（重要）

用 capstone 扫全 `.text`（按 `.pdata` 函数逐个线性反汇编）搜索"承伤聚合/钳制"：

- 所有出现 `+0x10b0` 位移的 104 个有界函数中，**没有一个**把管理器指针派生出来后再以 float 读写 `+0x24`/`+0x28`
  （唯一命中 `0x749D75` 是假阳性：`rbx` 已被 `find_by_key` 的返回值覆盖成状态对象）。
- 全二进制搜 `movss xmm0,[rcx+0x24]; ret` 之类的 getter（`0x730370`/`0x7300D0` 等）经 RTTI 反查
  属于 `CActModuleInput*` / `CActModuleAction*`（动作模块），**不是**状态管理器。
- 结论：**除了 apply 自己，静态上看不到任何代码读 `mgr+0x24`/`+0x28`。**
  这**不**推翻"相乘"结论（apply 的语义是确定的），但它意味着"最终把伤害乘上这个值"的那一步
  要么在这些状态 tick 链内部完成，要么通过我未能钉住的接口读取 —— 需要一次实机读数（§5）。

---

## 2. Q2：运行时"当前承伤值"在哪

### 2.1 字段级（apply 直接写的两个字段）—— probable

```c
char* player = *(char**)(base + 0x18A0490);   // 玩家槽位表第 0 项（4 项 × 24 字节）
char* param  = *(char**)(player + 0x240);     // 参数管理器
char* mgr    = param + 0x10B0;                // 角色状态管理器（容器）
float F24 = *(float*)(mgr + 0x24);            // 承伤倍率 #1（DamageRate + PhysicalDamageRate + Chaos + Water 都乘它）
float F28 = *(float*)(mgr + 0x28);            // 承伤倍率 #2（只有 DamageRate 乘它）
```
即 `*(float*)( *(char**)( *(char**)(base+0x18A0490) + 0x240 ) + 0x10B0 + 0x24 )`。

- 字段存在性与"apply 写它"是 **confirmed**（§1.1）。
- "它就是伤害公式最后用到的那个值" 是 **probable，未证实**（§1.5 的负结果）。
- 相加/相乘的默认值：同族 ctor `0x7AED80` 把 `+0x24/+0x28/+0x30/+0x38/+0x3c/+0x40` 全部初始化成 `1.0f`
  （`c741240000803f` 等），符合"倍率累加器默认 1.0"。

### 2.2 容器级（**推荐**，可枚举、可自证）—— confirmed

引擎自己在 `add()` 里就是这么找对象的（`0x7A2952` 起）：树头 `[mgr+0x170]`，节点键 `[node+0x20]`，
对象指针 `[node+0x28]`，对象状态 ID `[obj+0x10]`，参数 `[obj+0x50]`。

所以 DLL 侧最稳的"当前承伤"读法是**直接枚举**（项目已有的 `buff_state_present()` 就是同一棵树的
`ReadProcessMemory` 版 DFS，见 `RE_NOTES` §4.52.8）：

```c
// DFS over the RB-tree at mgr+0x170 (bounded, all reads via ReadProcessMemory)
float F_dmgrate = 1.0f;           // product of every DamageRate state except ours
for (node in DFS(*(void**)(mgr + 0x170))) {
    char* obj = *(char**)(node + 0x28);
    int   key = *(int*)(node + 0x20);
    if (*(int*)(obj + 0x10) == 0x1E && key != OUR_KEY)
        F_dmgrate *= *(float*)(obj + 0x50);
}
// r_gear = 1 - F_dmgrate
```

同时可用的**引擎自带查找器**（比手写 DFS 更强，但要调游戏代码）：

| 函数 | 签名 | 语义 | 证据 |
|---|---|---|---|
| `0x7A38B0` | `void* f(void* mgr, int key)` | **按键查状态对象**（红黑树 `[mgr+0x170]`，比较 `[node+0x20]`，返回 `[node+0x28]`；key==-1 → NULL） | `007A38B0 cmp edx,-1 / 007A38B5 mov r8,[rcx+0x170] / 007A38D0 cmp [rcx+0x20],edx / 007A38F9 mov rax,[rax+0x28]` |
| `0x7A3C40` | `void* f(void* mgr, int state_id)` | **按状态 ID 查状态对象**（遍历链表 `[mgr+0x198]`，计数 `[mgr+0x1a0]`，比较 `[obj+0x10]`） | `007A3C40 cmp qword [rcx+0x1a0],0 / 007A3C4D mov rcx,[rcx+0x198] / 007A3C64 cmp [rdx+0x10],r8d` |
| `0x7A3DC0` | `float (obj*)` | DamageRate 的 slot 2 = 读 `[obj+0x50]` | vtable slot 2 |

### 2.3 顺带确认的其它"发布"字段（避免误读）

- `param+0x68` ← `mgr+0x44`，`param+0x6c` ← `mgr+0x48`（`0x7ACBE0` / `0x7ACBE9`）；两者每帧先被
  `0x7B51B0(rcx=param)` 重置为 `-1.0f` / `1.0f`（`007B51B0 c74168000080bf` / `007B51B9 c7416c0000803f`）。
  `mgr+0x48` 是**攻击侧**倍率（被 `×1.25`(`0x11A0964`)、`×1.33`(`0x11A0B48`)、`×0.5`(`0x11A0D0C`) 缩放）——
  **不要**把它当承伤。
- `mgr+0x70 + i*4`（16 个 uint16）由 `0x7B54D0(rcx=param+0xBA8, rdx=mgr)` 镜像进 16 条状态记录的 `+0x22`。
- `mgr+0x124..+0x150`（11 个 float）是"对 UI 可见的量表块"，`0x7AC6E0` 开头快照、结尾逐项比较，
  有变化就调 `0x7A97EB0` 通知。**也不是承伤。**

---

## 3. Q3：MOD 该传什么 magnitude —— 修正公式

因为合并是**乘法**，朴素传 `0.9` 的结果是 `(1−r_gear)×0.9`（10%+10% → 实际 19%）。

要让**总量**变成加法 `1 − (r_gear + r_mod)`：

```
F_other = Π [obj+0x50]   over all DamageRate objects except ours      ( = 1 − r_gear )
our     = (1 − (r_gear + r_mod)) / (1 − r_gear) = (F_other − r_mod) / F_other
```
- 例：`r_gear = 0.10`，`r_mod = r_mod = 0.10` → `our = (0.9 − 0.1)/0.9 = 0.888…`，
  总量 `0.9 × 0.8889 = 0.8` ✅（正好 20%）。
- 边界：`F_other − r_mod ≤ 0`（总减免 ≥ 100%）时把 `our` 钳到 `0`；`F_other` 读不到时退回朴素 `1 − r_mod`
  （即接受 19%，绝不乱写）。
- `r_gear` 必须来自 **§2.2 的枚举**（所有非自身的 0x1E 对象），而不是 `mgr+0x24` —— 后者语义未证实。

**实现要点（与现有架构一致）**

1. 现有 `add(mgr, 0x1E, obj, -1, 0)` 用的树键 = 自己的状态 ID `0x1E`。
   我统计了全部 43 个引擎 `add()` 调用点的 `edx`：`1, 3, 0x18, 0x19, 0x2b, 0x34, 0x35, 0x36, 0x3c,
   0x3e, 0x3f, 0x40, 0x41, 0x42, 0x44, 0x45, 0x4d, 0x50`（另有记录驱动键 `[rec+0x40]` 与 `r15d`）。
   **`0x1E` 不在其中** ⇒ 目前与引擎的 DamageRate 不撞键（record 驱动的键来自数据，属 probable）。
   但**必须在运行时校验**：挂之前先 `find_by_key(mgr, 0x1E)`，若返回了一个**不属于自己**的
   `[obj+0x10]==0x1E` 对象，就换一个空闲键（撞键的后果见 §6.2，是破坏性的）。
2. 每帧（或每次 buff 刷新时）重算 `F_other` 并更新自己对象的 `[obj+0x50]`
   （也可以直接调 slot 1 setter `0x7AC550`，但那要走虚调用；直接写字段与项目"只写数值"的惯例一致）。
   枚举走 `ReadProcessMemory`，安全。
3. 寿命：见 §6.1 —— ctor 第 1 个 float 是**寿命**（`obj+0x28` 以 1.0/s 递减，归 0 后
   tick 返回"不活"，管理器自己把节点摘掉并释放）。所以不需要调 `remove()`。
4. 局限（必须写进文档）：本公式只对**DamageRate 层**做到加法。如果玩家的减伤有一部分走
   `AddStateObjectDefense`/元素抗性或伤害公式里的独立项，则实际总量是
   `r_gear_layer + r_mod`，其中 `r_gear_layer < r_gear_total`。§5 的实机测量能量化这个差额。

---

## 4. Q4：有无引擎侧上限/钳制

**未发现对承伤总量的钳制。** 逐项排查结果：

| 候选 | 实情 |
|---|---|
| DamageRate 的 apply | 纯 `mulss`，**无 min/max**（`0x7A8C30`） |
| 状态参数 setter / ctor | 直接存值，无钳制（`0x7AC550` / `0x79F450`） |
| slot 3 `0x7A4B40` | 只是 `al = (1.0f > param)`（"这是减免吗"），**不是钳制** |
| `0x11A0964 = 1.25f` | 用在 `0x7ACA39`：`mgr+0xf0`（就是 `MoveSpeed` 类在乘的那个字段，`0x7A9760` 已确认目标是 `rdx`=mgr）超过 1.25 就压回 1.25。**是管理器里真实存在的钳制，但钳的是移速，不是承伤** |
| `0x1588938 = 1.0f`、`0x1588B98 = FLT_MAX` | 通用常量，未参与承伤钳制 |
| `0x11A0B48 = 1.33f`、`0x11A0D0C = 0.5f` | 乘进 `mgr+0x48`（攻击侧） |
| 格挡路径 | 只是又挂一个 0.95 的 DamageRate，无额外钳制 |

⇒ 结论：**"未发现"**（不是"证明不存在"）。若要坐实，需要在实机上把 `F_other` 一路压到 0.1 以下，
观察伤害是否出现非线性（例如伤害被钳到 1）。

---

## 5. 未定案的部分 + 一次就能定案的实机测量

### 5.1 仍未钉住的一件事

`mgr+0x24`/`+0x28` 的**基准值是谁、什么时候写回去的**：我在每帧管理器更新函数 `0x7AC6E0`
（唯一调用者 `0x76EB45`）里逐条看过全部 420 条指令，**没有对 `[rsi+0x24]`/`[rsi+0x28]` 的写**；
tick 前的两次调用（`0x6E320` = 全局数组初始化、`0x7B51B0(param)` = 只重置 `param+0x68/+0x6c` 和 16 条记录）
也都不写它。可能的解释：
- **H1**：它们由某个"管理器构造/复位"函数写 1.0（`0x7AE920` / `0x7AE990` / `0x7AED80` 都往
  `+0x24`（及邻域）写 `1.0f`，但**没有直接调用者** → 经无 `.pdata` 的跳转表间接调用），
  于是每帧被复位 → `mgr+0x24` 就等于"所有活跃 DamageRate 参数的乘积"，可直接当 `r_total` 读；
- **H2**：从复位、而且是被消费者**用过即还原**（每次伤害事件前后各一次），此时它不是"稳态量"，
  只在伤害结算的瞬间有意义。

两者都**不影响** Q1 的结论（乘法），只影响 Q2 的"字段级"读法是否可用。

### 5.2 一次实机 session 就能定案（全部只读，`ReadProcessMemory`，符合项目安全标准）

在现有诊断里加 `DmgRateProbe=1`，在**输入线程**（8ms）每 ~250ms 打一行：

```
DMRATE F24=%.6f F28=%.6f hp=%d | states=[ key=0x%X id=0x%X param=%.4f life=%.2f ] ...
```
- `F24 = *(float*)(mgr+0x24)`，`F28 = *(float*)(mgr+0x28)`；
- `states` = `[mgr+0x170]` DFS 的每一项（`key=[node+0x20]`、`id=[obj+0x10]`、`param=[obj+0x50]`、
  `life=[obj+0x28]`）；
- `hp = *(int*)(param+0x20)`。

判定规则（事先写死，避免事后解释）：

| 观察 | 结论 |
|---|---|
| 装上"承受伤害 −10%"装备后，列表里**多出一个 `id=0x1E, param=0.9`** 的项 | 装备减伤就走 DamageRate 层 ⇒ 枚举法（§2.2）能拿到 `r_gear`，§3 公式直接可用 |
| 该装备生效时 `F24` 在 0.9 附近（而不是 1.0） | H1 成立 ⇒ `mgr+0x24` 可直接当 `r_total` 读，`F_other` 也可由它反推 |
| `F24` 只在某些帧 ≠ 1.0，其余帧 == 1.0（每帧被复位） | 也是 H1，但**采样必须对准"状态列表非空且伤害即将结算"的时刻** |
| `F24` 长期单调下降（0.9、0.81、0.729…） | 没有复位 + 每次 apply 都乘 ⇒ 说明消费者不复位，`mgr+0x24` 不可直接读，只能用枚举法 |
| 装备生效但列表里**没有** 0x1E 项，且 `F24` 也不动 | 装备减伤在 DamageRate 层之外（Defense/公式项）⇒ 枚举法拿不到 `r_gear`，需要另找；此时 §3 公式只能把"引擎自己挂的那部分"做加法 |
| 同时用 `LAYOUT`（已有）记一次已知攻击的 HP 掉量，带/不带装备各一次 | 得到**实际总倍率**，与上面推出的层内倍率对比，即可量化"层外减伤"占多少 |

配套的第二次采样（可选，用来坐实"哪个字段被真正用于结算"）：在已有的 `0x74DE51` 断点处（或
命中结算分发器 `0x6FE5B0`）打印当时的 `F24/F28`，与随后一帧的 `param+0x20` 变化比对。

---

## 6. 对现有笔记的三条修正（都有一手反汇编证据）

### 6.1 `ctor(mgr, a, b)`：`a` 是**寿命**，`b` 才是**参数（倍率）**

`0x79F450` 全文关键行：

```asm
0079F45F  488bd9              mov rbx, rcx
0079F467  0f28f1              movaps xmm6, xmm1        ; a
0079F46A  0f28fa              movaps xmm7, xmm2        ; b
0079F493  488b8b68010000      mov rcx, [rbx + 0x168]   ; 宿主角色
0079F49F  f30f117028          movss [rax + 0x28], xmm6 ; ★ a → +0x28（寿命计数）
0079F4A4  f30f11702c          movss [rax + 0x2c], xmm6 ; ★ a → +0x2c（下限/夹取）
0079F4AE  48894808            mov [rax + 8], rcx       ; 宿主
0079F4B2  488d0dc78da000      lea rcx, [rip+0xa08dc7]  ; -> 0x11A8280（DamageRate vtable）
0079F4B9  48c740101e000000    mov qword [rax+0x10], 0x1e
0079F4DA  48c740300000803f    mov qword [rax+0x30], 0x3f800000  ; +0x30 = 衰减率 1.0f，+0x34 = 0
0079F4EC  f30f117850          movss [rax + 0x50], xmm7 ; ★ b → +0x50（apply 读的参数）
```
- `+0x34/+0x38` 被 ctor 清零 ⇒ "时长"不在这里；
- tick 用 `[rbx+0x30]`（1.0f）当衰减率去减 `[rbx+0x28]`，并用 `seta al`（`[rbx+0x28] > 0`）判定存活
  ⇒ `a` 是**以 1.0/s 递减的寿命**，归 0 时管理器自己摘除并释放（`0x7AD2E6`-`0x7AD316`）。
- 因此 MOD 传 `(300, 0.96)`：`300` = 300 秒寿命，`0.96` = 倍率 —— **用法是对的**，
  但笔记里"第一个 float 是时长，写在别处"的说法可以精确化：`a` 就存在 `obj+0x28`
  （与 apply 目标的 `mgr+0x28` 是**两个不同对象的同偏移**，别混淆）。

### 6.2 `add()` 的替换判据是**树键（edx）**，不是"状态 ID"

`0x7A2890` 里：`ebp = edx`（树键），`rdi = r8`（新对象），`rsi = rcx`（mgr）。

```asm
007A2917  83fdff            cmp ebp, -1                 ; 键 == -1 → 走移除路径并返回 0
007A291C  4533c0            xor r8d, r8d
007A291F  488bd7            mov rdx, rdi
007A2922  488bce            mov rcx, rsi
007A2925  e896fcffff        call 0x7A25C0               ; remove()
007A2931  8b4608            mov eax, [rsi+8]            ; 管理器标志
007A2934  c1e81d            shr eax, 0x1d
007A2937  a801              test al, 1
007A2939  7406              je 0x7A2941
007A293B  837f1045          cmp dword [rdi+0x10], 0x45  ; 特例：状态 ID 0x45
007A293F  75db              jne 0x7A291C                ;   → 直接移除路径
007A2941  895f24            mov [rdi+0x24], ebx         ; r9d 写入 obj+0x24
007A2952  488b8e70010000    mov rcx, [rsi+0x170]        ; ★ 树头
007A2966  396820            cmp dword [rax+0x20], ebp   ; ★ 按「键」查找
...
007A2993  488b4b28          mov rcx, [rbx+0x28]         ; 命中：已有对象
007A2997  8b4110            mov eax, [rcx+0x10]         ; 已有状态 ID
007A299A  448b4710          mov r8d, [rdi+0x10]         ; 新状态 ID
007A299E  413bc0            cmp eax, r8d
007A29A1  410f94c6          sete r14b                   ; same_id
007A29A5  80791c00          cmp byte [rcx+0x1c], 0
007A29A9  7435              je 0x7A29E0
007A29AB  80792000          cmp byte [rcx+0x20], 0
007A29AF  742f              je 0x7A29E0
007A29B1  80792100          cmp byte [rcx+0x21], 0
007A29B5  740e              je 0x7A29C5
007A29B7  413bc0            cmp eax, r8d
007A29BA  7509              jne 0x7A29C5
007A29BC  488b01            mov rax, [rcx]
007A29BF  488bd7            mov rdx, rdi
007A29C2  ff5040            call [rax+0x40]             ; 旧对象.slot8(新对象)
007A29C5  4533c0            xor r8d, r8d
007A29C8  488bd7            mov rdx, rdi
007A29CB  488bce            mov rcx, rsi
007A29CE  e8edfbffff        call 0x7A25C0               ; ★ 丢弃「新」对象
007A29D3  48837b2800        cmp qword [rbx+0x28], 0
007A29D8  0f95c0            setne al
...
007A29E0  488bd1            mov rdx, rcx                ; 另一条路径
007A29E9..（同上判断）
007A2A00  450fb6c6          movzx r8d, r14b
007A2A04  488bce            mov rcx, rsi
007A2A07  e8b4fbffff        call 0x7A25C0               ; ★ 丢弃「已有」对象
007A2A0C  488bc3            mov rax, rbx                ; 复用节点，装入新对象
```
⇒ **键冲突是破坏性的**（两者必去其一），而"状态 ID 相同"只是决定去哪一个。
所以「同一个状态 ID 会被替换」这句话不准确：真正决定"是否与引擎已有 buff 互相顶掉"的是**树键**。
对 MOD 的直接含义：`OUR_KEY = 0x1E` 目前与引擎已知键不冲突（§3 要点 1），但升级/数据变动后必须运行时校验。

### 6.3 为什么 `0x7A25C0` 一调就崩（**可能是**这个原因）

`0x7A25C0` 全文（74 字节）只做三件事：

```asm
007A25C0  4885d2        test rdx, rdx ; je ret
007A25CA  807a1d00      cmp byte [rdx+0x1d], 0
007A25D3  488b02        mov rax, [rdx]
007A25DD  ff5038        call [rax+0x38]      ; slot 7（onDeactivate）
007A25E4  488b03        mov rax, [rdx]
007A25F0  ff10          call [rax]           ; slot 0（析构）
007A25F2  e869488000    call 0x...276e60    ; 取进程级分配器
007A2600  41ff5058      call [r8+0x58]       ; free(对象)
```
**它自己不做任何链表/红黑树摘除。** 摘除动作全部由调用方完成（例如 `0x7AD260` 的
`0x7AD2E6`-`0x7AD316` 先把节点从树里摘下、`dec [mgr+0x190]`/`[mgr+0x1a0]`，再调 0x7A25C0）。
所以从 DLL 直接对"仍在容器里的"对象调它 ⇒ 容器里留下悬垂指针 ⇒ 之后遍历/析构时崩溃。
**这条解释与实机观察一致**（`RE_NOTES` §4.52），置信度 **probable**（未做实机复现）。
⇒ 结论不变，而且理由更清楚：**继续让引擎自己按寿命过期**（§6.1），不要调 remove。

---

## 7. 被排除的假设（写下来避免重走）

1. ❌「引擎把减伤求和」/「存在 `1 − Σr` 的聚合」：全二进制无此类代码；承伤修正全部是
   对同一 float 的 `mulss`（§1.1/§1.3）。
2. ❌「多个 DamageRate 只能存在一个，所以必然互相顶掉」：树键不同即可共存
   （引擎自己就同时用键 1、0x45，还可能用记录键）。顶替只在**同键**时发生（§6.2）。
3. ❌「`mgr+0x24` 是每帧复位、被伤害代码读取的稳态值」：**没能证实**（§1.5 负结果 + §5.1）。
   不能把它当既成事实写进实现。
4. ❌「`param+0x68/+0x6c` 是承伤」：它们是 `mgr+0x44/+0x48`（攻击侧）的发布副本（§2.3）。
5. ❌「`mgr+0xf0` 的 1.25f 钳制是承伤上限」：那是移速/麻痹类累加器的钳制（§4）。
6. ❌「ctor 第一个 float 写在 `+0x34/+0x38`」：ctor 把这两个清零，第一个 float 落在 `+0x28/+0x2c`（§6.1）。
7. ❌「`0x7A25C0` 崩是因为"引擎不允许移除"」：它只是缺少摘链步骤（§6.3）。

---

## 8. 下一步（单一最佳动作）

**给 MOD 加一个只读诊断 `DmgRateProbe=1`（§5.2 的那行 `DMRATE ...`），复用已有的
`buff_state_present()` 树 DFS（只多读 `[obj+0x50]`、`[obj+0x28]`），并在同一行打印
`*(float*)(mgr+0x24)` / `*(float*)(mgr+0x28)`；打一场战斗，装备"承受伤害 −10%"前后各取一份日志。**

- 零风险：全程 `ReadProcessMemory`，不调游戏函数、不写任何内存。
- 一次 session 同时定案：装备减伤是否走 DamageRate 层（决定 `r_gear` 从哪读）、
  `mgr+0x24` 是否可直接读（H1 vs H2）、以及是否存在引擎侧钳制。
- 得到结果后按 §3 公式落地：把 MOD 挂的那个 0x1E 对象的 `[obj+0x50]` 每次设为
  `(F_other − r_mod)/F_other`，即可实现真正的加法叠加。

---

## 附：本次分析用到的关键 RVA 速查

| RVA | 名称/语义 |
|---|---|
| `0x18A0490` | 玩家槽位表（4 × 24 字节） |
| `0x7A2890` | `add(mgr, key, obj, r9d, byte)` — 插入 `[mgr+0x170]`，同键破坏性替换 |
| `0x7A25C0` | 对象销毁（无摘链，不可直接调用） |
| `0x7A38B0` | `find_by_key(mgr, key) -> obj` |
| `0x7A3C40` | `find_by_state_id(mgr, id) -> obj`（遍历 `[mgr+0x198]`） |
| `0x7ACCA0` | 通用状态 tick（slot 6 apply 每 tick 无条件调用） |
| `0x7AC6E0` | 每帧管理器更新（唯一调用者 `0x76EB45`）：tick 三处 + `0x7AD260` + `0x7ACDB0` + 发布 |
| `0x7AD260` | 遍历 `[mgr+0x170]` 红黑树并 tick（rdx = mgr） |
| `0x7B51B0` | 每帧重置 `param+0x68=-1.0f` / `param+0x6c=1.0f` + 16 条记录 |
| `0x7B54D0` | 把 `mgr+0x70+i*4`（uint16 ×16）镜像进记录 `+0x22` |
| `0x79F450` | DamageRate ctor(mgr, 寿命, 倍率)（175 字节，`.pdata` 起点） |
| `0x7A8C30` | DamageRate apply：`[rdx+0x24] *= p; [rdx+0x28] *= p` |
| `0x7A9920` | PhysicalDamageRate apply：`[rdx+0x24] *= p` |
| `0x74D869`/`0x74D881` | 引擎自己的"格挡减伤"= DamageRate(300, 0.95)，键 `0x45` |
| `0xA8423C`/`0xA8425A` | 引擎从 ability `+0x350` 推导 DamageRate(600, 1−stat)，键 `1` |
| `0x7241E0` / `0x71F183` | 记录→状态对象 分发器 + 调用方（键 = `word[rec+0xa]−1`） |
| `0x7B2D40` | ability/统计块访问器（`[obj+0x198]`，默认静态块） |
| `0x11A8280` | `AddStateObjectDamageRate::Character` vtable |
