# 仁王1 (nioh.exe 1.24.8) — 九十九武器 / Living Weapon 计量表 逆向报告

- 目标：定位 **LW(九十九/99) 计量表**的存储位置、**LW 是否开启**的可读判据、以及**引擎给该表充能的函数/状态**
- 方式：**纯静态**（`_work\nioh1.mem.exe`，50,782,208 字节，文件偏移 == RVA，image base `0x140000000`）。**没有实机会话**，因此下文一切结论都标注为「已确认 / 较可信 / 推测 / 未能确定」，并附原始字节与反汇编行。
- 本报告**只写** `_work\re_lw_report.md`，未修改任何其他项目文件。

---

## 0. 一句话结论

| 问题 | 结论 | 置信度 |
| --- | --- | --- |
| ① LW 计量表在哪 | **未能唯一确定**。已确认整套计量表数据结构与三个访问原语；`[[char+0x240]]+0x40`（记录 0）是本次普查中**唯一被引擎「补满」、且「上限」被硬编码 100.0f** 的 0–100 表（但这与既有笔记"`param+0x40/+0x44` = 精力"冲突，见 §3.1），`[[char+0x240]]+0x48` 是**LW 模块唯一直接读的 param 浮点字段**。二者同属记录 0。 | 结构：**已确认**；"哪条是 99 表"：**未能确定** |
| ② 如何判断 LW 正在开启 | **已找到两套可用判据**：(a) 状态对象 **`AddStateObjectCallSpirit`，状态 ID = 0x22 (34)**（调用守护灵＝开 99）；(b) 一组全局状态块 `0x191DE28`，其中 `[0x191DE34]` 是一个 0/1/2 子状态机，其 `==2` / `==1` 查询函数在 257 / 84 处被调用，并且**恰好是 LW 模块自己用的判据**。 | (a) **较可信**（类名+调用点证据强）；(b) **已确认存在且属于 LW 模块**，语义为**推测** |
| ③ 引擎如何给该表充能 / 续时 | **已确认**：LW/阿米粒塔一族共 6 个状态类，全部由 **`0xA83F97`** 一个函数构造并施加到状态容器 `[[char+0x240]]+0x10B0`；另有 `0x8A5B50` 一条「阿米粒塔信息 → 玩家 param → 把记录 0 补满」的短函数。状态构造签名 `ctor(容器, 时长秒, 倍率)` 已由本项目既有笔记实测确认。 | **已确认（调用点/类名/目标字段）**；"哪个状态对应哪次充能"：**较可信** |

**最重要的可执行结论**：判断 LW 是否开启**不需要新的偏移**。MOD 已经有 `buff_state_present()`（`docs\RE_NOTES.md` §4.52.8），只需把容器 `[[char+0x240]]+0x10B0` 的 DFS 里匹配的 `[stateobj+0x10]` 值改成 **`0x22`**，就能判定 LW 开启。

---

## 1. 数据与工具（原始字节证据）

`image_base = 0x7FF7AA2D0000`（运行时），`RVA = 运行时地址 - 0x7FF7AA2D0000`。
`.text`: `va=0x1000, vsize=0x1199498`。以下所有 `; -> 0xXXXX` 都是 **RVA**。

### 1.1 计量表访问原语（本题的"骨架"，已确认）

**`0x7B4C20` — 把 `cur` 补到 `max`（"补满"）**

```asm
007B4C20  f30f104904        movss  xmm1, dword ptr [rcx + 4]   ; max
007B4C25  f30f1001          movss  xmm0, dword ptr [rcx]       ; cur
007B4C29  0f2fc1            comiss xmm0, xmm1
007B4C2C  7203              jb     0x7B4C31
007B4C2E  32c0              xor    al, al
007B4C30  c3                ret
007B4C31  f30f1109          movss  dword ptr [rcx], xmm1       ; cur = max
007B4C35  b001              mov    al, 1
007B4C37  c3                ret
```

**`0x7B4C40` — 从 `+0x0C` 扣减，下限 0（"消耗"）**

```asm
007B4C40  f30f10510c        movss  xmm2, dword ptr [rcx + 0xc]
007B4C45  f30f5cd1          subss  xmm2, xmm1
007B4C49  0f57c9            xorps  xmm1, xmm1
007B4C4C  0f28c2            movaps xmm0, xmm2
007B4C4F  f30f5cc1          subss  xmm0, xmm1
007B4C53  0f2fc1            comiss xmm0, xmm1
007B4C56  7206              jb     0x7B4C5E
007B4C58  f30f11510c        movss  dword ptr [rcx + 0xc], xmm2
007B4C5D  c3                ret
007B4C5E  f30f11490c        movss  dword ptr [rcx + 0xc], xmm1   ; clamp 0
007B4C63  c3                ret
```

> 两者都取 **记录指针** 于 `rcx`。因此「调用方加到某个基址上的位移」直接给出记录下标。
> **重要**：`0x7B4C20` 作用于 `rec+0x00`（`cur`）与 `rec+0x04`（`max`），而 `0x7B4C40` 作用于 `rec+0x0C`。
> 也就是说**同一张表里有两套"当前值"字段**：`+0x00/+0x04` 是一对 (cur,max)，`+0x0C` 是另一个独立的量。

**`0x7B4CB0` — 遍历 16 条记录并清空**

```asm
007B4CBF  488d5940      lea    rbx, [rcx + 0x40]     ; 起始 = 基址 + 0x40
007B4CC3  bf10000000    mov    edi, 0x10             ; 16 条
007B4CD0  488b0b        mov    rcx, qword ptr [rbx]        ; rec+0x38 = 指针
007B4CD3  c743c802000000 mov   dword ptr [rbx - 0x38], 2   ; rec+0x00 = 2
007B4CDA  8973d4        mov    dword ptr [rbx - 0x2c], esi ; rec+0x0C = 0
007B4CE2  8b5308        mov    edx, dword ptr [rbx + 8]    ; rec+0x40 = int 键
007B4CE5  e8b663ffff    call   0x7AB0A0                    ; 按 (rec+0x38, rec+0x40) 移除状态
007B4CEA  4883c350      add    rbx, 0x50                   ; 步长 0x50
007B4CEE  4883ef01      sub    rdi, 1
007B4CF2  75dc          jne    0x7B4CD0
```

**`0x7B4D10` — 同类遍历，但先"查询该状态是否存在"**

```asm
007B4D30  488b0b        mov    rcx, qword ptr [rbx]        ; rec+0x38
007B4D33  4885c9        test   rcx, rcx
007B4D36  7434          je     0x7B4D6C                    ; 空指针 => 跳过
007B4D38  8b5308        mov    edx, dword ptr [rbx + 8]    ; rec+0x40
007B4D3B  e870ebfeff    call   0x7A38B0                    ; 查询 (容器, 状态ID)
007B4D40  4885c0        test   rax, rax
007B4D43  7427          je     0x7B4D6C                    ; 查不到 => 跳过
007B4D45  488b10        mov    rdx, qword ptr [rax]
007B4D48  488bc8        mov    rcx, rax
007B4D4B  ff5218        call   qword ptr [rdx + 0x18]      ; 状态对象虚函数 slot3 => bool
007B4D4E  84c0          test   al, al
007B4D50  741a          je     0x7B4D6C
007B4D52  488b0b        mov    rcx, qword ptr [rbx]        ; 成立才清空该记录
007B4D55  c743c802000000 mov   dword ptr [rbx - 0x38], 2
007B4D5C  8973d4        mov    dword ptr [rbx - 0x2c], esi
007B4D67  e83463ffff    call   0x7AB0A0
```

### 1.2 `0x7A38B0`：记录里的 `{+0x38, +0x40}` 是"状态容器 + 状态 ID"句柄（**已确认**）

```asm
007A38B0  83faff          cmp    edx, -1
007A38B3  7409...         je     -> xor eax,eax; ret      ; -1 => 空
007A38B5  4c8b41170...    mov    r8, qword ptr [rcx + 0x170]   ; ★ 树根在 rcx+0x170
007A38C3  80 79 19 00     cmp    byte ptr [rcx + 0x19], 0      ; 红黑树哨兵
007A38D0  39 51 20        cmp    dword ptr [rcx + 0x20], edx   ; ★ 节点+0x20 = 排序键
007A38F9  488b4028        mov    rax, qword ptr [rax + 0x28]   ; ★ 节点+0x28 = 状态对象
```

与 `docs\RE_NOTES.md` §4.52.8 记录的容器布局（`[管理器+0x170]` 树根、`[节点+0x20]` 键、`[节点+0x28]` 状态对象）**完全一致** —— 这是一次独立的相互印证。
⇒ **资源表每条记录的 `+0x38`（容器指针）与 `+0x40`（int 键 = 状态 ID）就是"该计量表由哪个状态驱动"的绑定。**

### 1.3 计量表布局（**已确认**）

- 表基址 = `[[char+0x240]] + 0xBB0`（`0x7B21F0` 的 `lea rax,[rcx+8]; add rax,idx*0x50`，调用方 `0x74DDD9: add rcx,0xba8`）
- **记录 0x50 字节**，`rec_j = param + 0xBB0 + j*0x50`，遍历 16 条（`0x7B4CB0` / `0x7B4D10`）
- 记录字段：

| 偏移 | 类型 | 证据 |
| --- | --- | --- |
| `+0x00` | int 状态（遍历器置 2；格挡路径与 1 比较） | `0x7B4CD3 mov dword [rbx-0x38],2`；`0x7B4D55` |
| `+0x04` | float（"Up"类状态在此上做 `+ rate/帧`） | `AddStateObject*Up` 构造函数 |
| `+0x0C` | float 当前值（被 `0x7B4C40` 扣减） | `0x7B4C40` 原文 |
| `+0x38` | 指针 → 状态容器 | `0x7B4D30` + `0x7A38B5` |
| `+0x40` | int → 状态 ID（送 `0x7A38B0` 当键） | `0x7B4D38` |

- 相邻还有一张 **"(cur,max) 对"表**：`rec_i = param + 0x40 + i*0x50`，`+0x00 = cur`，`+0x04 = max`。
  `param+0xBB0 + j*0x50` 落在 `param+0x40 + i*0x50` 上当 `i = j + 23`。

### 1.4 格挡路径（对照，**已确认**，用来标定下标含义）

```asm
0074DDD2  488b8940020000  mov rcx, qword ptr [rcx + 0x240]   ; param
0074DDD9  4881c1a80b0000  add rcx, 0xba8                     ; 表基址
0074DDE0  ba06000000      mov edx, 6                         ; 下标 6
0074DDE5  e806440600      call 0x7B21F0                      ; -> base+8+6*0x50 = param+0xDD0
```

⇒ 既有笔记里"格挡扣精改的是 `param+0xBB0 + 7*0x50 = param+0xDE0`"，就是 **记录 7**；
而这里访问的是 **记录 6**（走 `0x7B4310` / `0x7B5930`，另一条路径）。

---

## 2. 问题 ③：引擎如何充能 —— **已确认**（状态对象族 + 施加点）

### 2.1 完整状态 ID → 类名映射（**已确认**，全部为直接证据）

方法（三步，全部可复现，见 `_work\re_lw29.py`）：

1. `.rdata` 的 RTTI 给出 `vftable_rva`；
   **先验证**：对 6 个已独立确知构造函数的类，RTTI 的 `vftable_rva` 与构造函数里 `lea rXX,[rip+vt]` 的**实测**目标**偏差恰为 0**（`_work\re_lw28.py` 输出：`distinct biases: [0]`）。
   ⇒ 可以直接用 RTTI 的 `vftable_rva` 作为"真实虚表地址"。
   （**被否定的假设**：我一开始按"类名列表与构造函数列表的排序位置一一对应"来配对，锚点校验 8/8 通过但**结果是错的**——见 §5.1。）
2. 字节级扫描所有 `REX.W 8D /r`（`mod=00, rm=101`）取得 `lea rXX,[rip+vt]` 的**唯一**引用者＝构造函数。
3. 构造函数内 `mov qword ptr [rXX+0x10], imm` 即状态 ID（与 `docs\RE_NOTES.md` §4.51 记录的对象布局 `+0x10 = 状态 ID` 一致）。

产物：`_work\re_lw29_states.json`（181 行 / 179 个构造函数 / **84 个不同 ID**）。

**与 LW / 阿米粒塔相关的条目（逐条为本报告的关键证据）：**

| 状态 ID | 类名 | 构造函数 RVA | 虚表 RVA |
| --- | --- | --- | --- |
| **0x20 (32)** | `AddStateObjectAmritaGaugeUp::Character` | `0x79E870` | `0x11A8B90` |
| 0x06 (6) | `AddStateObjectAmritaGaugeUp::Character`（第二个构造函数，同一虚表） | `0x7AA395` | `0x11A8B90` |
| **0x27 (39)** | `AddStateObjectAmritaUpPlus::Character` | `0x79E920` | `0x11A8DC0` |
| **0x25 (37)** | `AddStateObjectAttackHitRecoverAmrita::Character` | `0x79ED80` | `0x11A8D20` |
| **0x42 (66)** | `AddStateObjectAmritaGaugeRecover::Character` | `0x79E7B0` | `0x11A95E0` |
| 0x38 (56) | `AddStateObjectDyingAmritaUp::Character` | `0x79FB40` | `0x11A9400` |
| **0x22 (34)** | `AddStateObjectCallSpirit::Character` | `0x79F1B0` | `0x11A8C30` |
| **0x26 (38)** | `AddStateObjectTransformSpirit::Character` | `0x7A1EE0` | `0x11A8D70` |
| 0x24 (36) | `AddStateObjectArtOfSacrifice::Character` | `0x79EA80` | `0x11A8CD0` |
| 0x55 (85) | `AddStateObjectGuardianPowerless::Character` | `0x79FF80` | `0x11A9B80` |
| 0x18 (24) | `AddStateObjectYashiroBonus::Character` | `0x7A2420` | `0x11A8910` |
| 0x2D (45) | `AddStateObjectSpiritSealedUp::Character` | `0x7A1A30` | `0x11A8FA0` |

构造函数级证据（`AmritaGaugeUp`，ID 0x20，`_work\re_lw33_out.txt`）：

```asm
0079E8AF  call   qword ptr [r9 + 0x28]        ; 管理器分配器（尺寸 0x58）
0079E8B3  mov    rcx, qword ptr [rbx + 0x168] ; 属主引用
0079E8BF  movss  dword ptr [rax + 0x28], xmm6 ; 参数 A = 时长（秒）
0079E8C4  movss  dword ptr [rax + 0x2c], xmm6 ; 参数 B = 倍率
0079E8CE  mov    qword ptr [rax + 8], rcx
0079E8D2  lea    rcx, [rip + 0xA0A2B7]        ; -> 0x11A8B90 = AmritaGaugeUp 虚表
          mov    qword ptr [rax + 0x10], 0x20 ; ★ 状态 ID = 0x20
```

### 2.2 施加点：**全部集中在 `0xA83F97`**（**已确认**）

直接调用（`E8 rel32`）统计：

| 构造函数 | 调用点数 | 所在函数 |
| --- | --- | --- |
| `0x79E7B0` AmritaGaugeRecover (0x42) | 1 | `0xA83F97` @ `0xA8AD40` |
| `0x79E870` AmritaGaugeUp (0x20) | 2 | `0xA83F97` |
| `0x79E920` AmritaUpPlus (0x27) | 1 | `0xA83F97` |
| `0x79ED80` AttackHitRecoverAmrita (0x25) | 3 | `0xA83F97` |
| `0x79F1B0` CallSpirit (0x22) | 1 | `0xA83F97` @ `0xA86690` |
| `0x79EA80` ArtOfSacrifice (0x24) | 1 | `0xA83F97` |
| `0x79FF80` GuardianPowerless (0x55) | 1 | `0xA83F97` |
| `0x7A1020` NobunagaElement (0x53) | 1 | `0xA83F97` |
| `0x79FB40` DyingAmritaUp (0x38) | 1 | `0x714CE0` @ `0x7158F0` |
| `0x7A05B0` KusabiConnect (0x17) | 1 | `0x76C06D` |

`0xA83F97` 大小 **32,603 字节**（`0xA83F97..0xA8BEF2`），签名（由序言读出）：

```asm
00A83F97  movaps xmmword ptr [rsp + 0xae0], xmm12
00A83FA0  mov    eax, edx
00A83FA2  cvtsi2ss xmm7, rax        ; r8d -> float
00A83FA7  lea    eax, [r8 - 1]
```

函数体内反复出现的共同形态（**这是"充能"的核心**）：

```asm
00A86643  mulss  xmm7, xmm8
00A86648  xorps  xmm0, xmm0
00A8664B  cvtsi2ss xmm0, rax                       ; 某个整数（阿米粒塔量）
00A86650  mulss  xmm0, xmm9
00A86655  addss  xmm0, xmm7
00A86659  mulss  xmm1, xmm0
00A8665D  test   bl, bl
00A8665F  je     0xA86674
00A86661  mov    ecx, dword ptr [rsi + 0x10]
00A86664  mov    eax, 0x51eb851f
00A86669  imul   ecx, dword ptr [rsi + 8]          ; x * 0x51eb851f >> 37 = x/100
00A8666D  mul    ecx
00A8666F  shr    edx, 5
00A86672  jmp    0xA86677
00A86674  mov    edx, dword ptr [rsi + 8]
00A86677  xorps  xmm2, xmm2
00A8667A  mov    eax, edx
00A8667C  lea    rcx, [r15 + 0x10b0]              ; ★ 状态容器 = param + 0x10B0
00A86683  cvtsi2ss xmm2, rax
00A86688  mulss  xmm2, dword ptr [rip + 0xB02470]  ; -> 0x1588B00 = 100.0f
00A86690  call   0x79F1B0                          ; CallSpirit，xmm1=时长，xmm2=量
```

`0x1588B00` 处的常量实测 = **`100.0f`**（`_work\re_lw23` 的常量表）。

同一函数内 `AmritaGaugeRecover` 的施加点：

```asm
00A8AD39  lea    rcx, [r15 + 0x10b0]        ; 状态容器
00A8AD40  call   0x79E7B0                   ; AddStateObjectAmritaGaugeRecover
```

⇒ **问题 ③ 的答案**：引擎不是"直接写计量表"，而是**施加一个状态对象**到 `[[char+0x240]]+0x10B0`：
- `AmritaGaugeUp`(0x20) —— 让阿米粒塔计量表在一段时间内**增长**（`rec+0x04 += rate/帧`）
- `AmritaGaugeRecover`(0x42) —— **恢复/补回**该表
- `AttackHitRecoverAmrita`(0x25) —— 命中时回收阿米粒塔
- `AmritaUpPlus`(0x27) / `DyingAmritaUp`(0x38) —— 增益/死亡时的阿米粒塔上升
- **`CallSpirit`(0x22) / `TransformSpirit`(0x26)** —— 守护灵召唤/化身（＝99 的施加机制，见 §3.2）

### 2.3 一条显式的"给记录 0 充能"短函数（**已确认**）

```asm
008A5B50  sub    rsp, 0x148
008A5B57  mov    rax, qword ptr [rip + 0xF5243A]   ; -> 0x17F7F98 (栈 cookie)
008A5B69  mov    rdx, rcx
008A5B6C  lea    rcx, [rsp + 0x20]
008A5B71  call   0xDDF4E0                          ; 构造"阿米粒塔信息"结构
008A5B76  xor    ecx, ecx
008A5B78  call   0x755DC0                          ; ★ 玩家槽位访问器（既有锚点）
008A5B7D  test   rax, rax
008A5B80  je     0x8A5B97
008A5B82  mov    rcx, qword ptr [rax + 0x240]       ; param
008A5B89  test   rcx, rcx
008A5B8C  je     0x8A5B97
008A5B8E  add    rcx, 0x40                          ; ★ 记录 0
008A5B92  call   0x7B4C20                           ; ★ 补满（cur = max）
```

配套的 **记录 0 初始化/复位** 在 param 构造函数 `0x7698E0` 里：

```asm
0076A08C  mov    rcx, qword ptr [rdi + 0x240]
0076A093  mov    edx, 0x64                            ; 100
0076A098  add    rcx, 0x10
0076A09C  call   0x7B57C0                             ; 用 100 初始化 sub+0x10
0076A0A1  mov    rcx, qword ptr [rdi + 0x240]
0076A0A8  add    rcx, 0x10
0076A0AC  call   0x7B4BE0
0076A0B1  mov    rax, qword ptr [rdi + 0x240]
0076A0B8  mov    dword ptr [rax + 0x44], 0x42C80000    ; ★ param+0x44 = max = 100.0f
0076A0BF  mov    rcx, qword ptr [rdi + 0x240]
0076A0C6  add    rcx, 0x40
0076A0CA  call   0x7B4C20                             ; ★ 补满：param+0x40 = 100.0f
```

**这是全镜像里唯一一处把某个字段写成 `100.0f`、紧接着对该字段所在记录调用"补满"的地方**
（`_work\re_lw15_out.txt` A1、`_work\re_lw23_out.txt` A 节）。
`100.0f` 作为立即数写入的**全部** 40 处（`_work\re_lw23_out.txt`）里，只有 `0x76A0B8` 的位移是 `+0x44`。

---

## 3. 问题 ① 与 ②：计量表位置 与 开启判据

### 3.1 计量表候选（记录 0；**结构已确认，归属未能确定**）

**22 个 `0x7B4C20`（补满）的直接调用者，全部指向记录 0**（`_work\re_lw13_out.txt`）：

- `0x7103DE lea rcx,[r15+0x40]` / `0x7103FA call 0x7B4C20`
- `0x71523F lea rcx,[rdi+0x40]` / `0x71525A call`
- `0x767AF0 mov rcx,[rbx+0x240]; add rcx,0x40` / `0x767AFB call`
- `0x76A0BF..0x76A0CA`（param 构造）
- `0x87D883..0x87D88E`、`0x88A672..0x88A67D`、`0x894630..0x89463B`、`0x8A2AB6..0x8A2AC1`、`0x8A5B8E..0x8A5B92`、`0x8AFC76..0x8AFC7A`、`0x754B34..0x754B38` …
- `0x70ECE0` 里成对出现：`0x7B4BE0(sub+0x10)` → `0x7B5330(sub+0x40)` → `0x7B4C20(sub+0x40)`

**没有任何调用者指向记录 ≠ 0。**
⇒ **`param+0x40` 是引擎唯一会"补满"的表**，且 `param+0x44` 被硬编码为 `100.0f`。

**而 LW 模块自己读的正是记录 0 里的另一个字段 `param+0x48`**（`0x714CE0` @ `0x71592D`）：

```asm
00715903  mov    rax, qword ptr [rsi + 0x50]
00715907  mov    rcx, qword ptr [rax + 0x240]
0071590E  add    rcx, 0xb0
00715915  call   0x7B4220                      ; 某些前置查询
0071591A  test   al, al
0071591C  jne    0xA85A1B
00715922  mov    rax, qword ptr [rsi + 0x50]
00715926  mov    rax, qword ptr [rax + 0x240]
0071592D  movss  xmm0, dword ptr [rax + 0x48]   ; ★ param+0x48
00715932  comiss xmm0, xmm10                   ; xmm10 = 某阈值常量
00715936  jbe    0xA85A1B
0071593C  comiss xmm10, dword ptr [rax + 0x40]  ; ★ 与 param+0x40 比较
00715941  jb     0xA85A1B                       ; 仅当 0x48 > T 且 param+0x40 < T 才继续
... 继续做一次 HUD/通知（0xA8CE40，事件 id 0xEDBE / 0x925F）
```

`param+0x48` 属于记录 0（`param+0x40` 记录的第 8..11 字节）。全镜像的直接读点只有 2 处
（`0x71592D` 在 LW/阿米粒塔相关的 `0x714CE0` 里——**已确认**；另有 `0xC59DCC`——本报告未分析），
也就是说在"经 `[X+0x240]` 抵达 param 的浮点访问"这一普查口径下，`param+0x48` 几乎只被 LW 侧读。
而 `0x714CE0` 正是：调用 `0x8D4C20`/`0x8D4C00`（LW 模块查询）、调用 `0x79FB40`
(`AddStateObjectDyingAmritaUp`)、构造后送 `0x7A2C80`。

**诚实结论**：
- **已确认**：记录 0 = `param+0x40` 是唯一被补满、且 `max` 被硬编码 100.0f 的 0–100 表；
  `param+0x48` 是它的第二个 float 字段，且是 LW 模块唯一直接读的 param 字段。
- **未能确定**：LW(99) 条是否**就是**记录 0，还是记录 0 是**精力(Ki)**、而 99 条在我没能定位的
  别的子对象里。既有笔记（§4.17）已用引擎自己的 getter（`Refer::Stamina` `0x6EC7C0` 读
  `param+0x40`、`Refer::StaminaRate` `0x6EC800` 读 `param+0x44`）**强证据**地证明
  `param+0x40/+0x44` 是**精力 (当前, 上限)**。这一点与本节的"唯一被补满"结论冲突，
  冲突未解决 ⇒ 我**不主张** `param+0x40` 就是 99 条。

**被否定的假设**：`param+0xBB0+...` 的 16 条记录里某一条是 99 条（因为"资源表"的
`+0x00/+0x0C/+0x38/+0x40` 字段全都用于状态绑定，且**没有任何** `0x7B4C20` 调用者指向它）。

### 3.2 判据 ②-A：状态对象 **`CallSpirit` (ID 0x22)** —— **较可信**

**命名链（`docs\RE_NOTES.md` 未记录，本报告新发现）**：

| 字符串 RVA | 内容 |
| --- | --- |
| `0x12C0FD8` | `TSUKUMO_WEAPON` |
| `0x1399E78` | `movie/TUT_TSUKUMO.wmv`（教程影片文件名） |
| `0x11F35B8` | `Player::SetTsukumoWeaponActiveFlag` |
| `0x11F6308` | `Tutorial::GetCountTsukumoUse` |
| `0x12C2BE0` | `Living Weapon Activated`（成就名） |
| `0x11A8C30` | `.?AVAddStateObjectCallSpirit@Character@@` 的虚表 |

⇒ **"Tsukumo" ＝ 开发代号 ＝ 英文版 Living Weapon**；`Living Weapon Activated` 是成就项。
（**已确认**：字符串本身。**较可信**：`Tsukumo` ↔ LW 的对应。）

`CallSpirit` 的状态对象被施加到 `[[char+0x240]]+0x10B0`，参数
`xmm1 = 时长(秒)`、`xmm2 = 量`，且施加点 `0xA86690` 位于阿米粒塔处理函数 `0xA83F97` 内、
其量值由 `整数阿米粒塔量 × 100.0f` 算出。
`TransformSpirit`(0x26) 是同类（"化身"）。

⇒ **建议的 LW 开启判据**：状态容器 `[[char+0x240]]+0x10B0` 的红黑树（根 `+0x170`，
节点 `+0x00/+0x10` 子节点、`+0x20` 键、`+0x28` 状态对象，对象 `+0x10` = 状态 ID）
里是否存在 **状态 ID `0x22`**（若不够，再连同 `0x26`、`0x42`、`0x20` 一起记录）。

**风险**：`TransformSpirit` 的施加点是 **0 个**（`_work\re_lw12_out.txt`），说明它由别的机制
（脚本/无名调用）施加；`CallSpirit` 只有 1 个施加点。若 `0x22` 不是 LW，则这条判据为假阳性。
⇒ 必须在实机确认（见 §4）。

### 3.3 判据 ②-B：全局状态块 `0x191DE28`（**已确认存在**，语义**推测**）

LW/成就模块（`0x8D4Bxx–0x8D4Dxx`，一组极小的 getter/setter）读写一个全局块：

```asm
008D4BE0  mov eax, dword ptr [rip + 0x104924A]   ; -> 0x191DE30
008D4BE6  ret
008D4BF0  mov eax, dword ptr [rip + 0x1049236]   ; -> 0x191DE2C
008D4C00  movzx ecx, byte ptr [rip + 0x1049224]  ; -> 0x191DE2B
008D4C07  xor  eax, eax
008D4C09  cmp  dword ptr [rip + 0x1049224], 2    ; -> 0x191DE34
008D4C10  cmove eax, ecx
008D4C13  ret
008D4C20  cmp  dword ptr [rip + 0x104920D], 2    ; -> 0x191DE34
008D4C27  sete al
008D4C2A  ret
008D4C30  movzx eax, byte ptr [rip + 0x1049201]  ; -> 0x191DE38
008D4C40  movzx eax, byte ptr [rip + 0x10491E3]  ; -> 0x191DE2A
008D4C50  movzx eax, byte ptr [rip + 0x10491D1]  ; -> 0x191DE28
008D4C60  movzx eax, byte ptr [rip + 0x10491C2]  ; -> 0x191DE29
008D4C70  cmp  dword ptr [rip + 0x10491BD], 1    ; -> 0x191DE34
008D4C77  sete al
008D4C80  mov  dword ptr [rip + 0x10491A6], ecx  ; -> 0x191DE2C
008D4C90  mov  byte ptr [rip + 0x1049193], 1     ; -> 0x191DE2A
008D4CA0  mov  byte ptr [rip + 0x1049181], 1     ; -> 0x191DE28
008D4CB0  mov  byte ptr [rip + 0x1049172], 1     ; -> 0x191DE29
008D4CC0  mov  byte ptr [rip + 0x1049165], cl    ; -> 0x191DE2B
008D4CD0  movzx eax, cl
008D4CD3  add  eax, eax
008D4CD5  mov  dword ptr [rip + 0x1049159], eax  ; -> 0x191DE34
008D4CE0  mov  dword ptr [rip + 0x104914A], ecx  ; -> 0x191DE30
008D4CF0  mov  byte ptr [rip + 0x1049142], cl    ; -> 0x191DE38
008D4D00  movzx eax, cl
008D4D03  mov  dword ptr [rip + 0x104912B], eax  ; -> 0x191DE34
```

关键点：`[0x191DE34]` 是一个 **0/1/2 子状态机**（`0x8D4CD0` 写 `2*cl`，
`0x8D4D00` 写 `cl`⇒ 0/1；`0x8D4C20` 判 `==2`；`0x8D4C70` 判 `==1`）。

调用热度（`E8` 直接调用计数，`_work\re_lw17_out.txt`）：

| 访问器 | 目标 | 调用点数 |
| --- | --- | --- |
| `0x8D4C20` (`[0x191DE34]==2`) | — | **257** |
| `0x8D4C30` (`[0x191DE38]`) | — | 287 |
| `0x8D4C70` (`[0x191DE34]==1`) | — | 84 |
| `0x8D4C00` (`[0x191DE2B]` 当 `[0x191DE34]==2`) | — | 47 |
| `0x8D4BE0` (`[0x191DE30]`) | — | 36 |

**它确实属于 LW**（**已确认**）：
唯一引用字符串 `Living Weapon Activated`（`0x12C2BE0`）的函数是 `0xE7ACF0`（成就字符串记录器），
而 `0xE7ACF0` 在**整个镜像里只有一个调用者**：`0x8D52B7`，位于函数 `0x8D51F0` 内。
`0x8D51F0` 自己就调用这一族访问器：

```asm
008D51F0  ...
008D5200  mov  ecx, 2
008D5205  call 0x8D4CE0          ; [0x191DE34] = 4
008D520A  call 0x8D4B20
```

（另有 `0x8D4C30`、`0x8D4C50` 等也在同一模块 `0x8C5A10` / `0x8CA920` / `0x8D04D0` / `0x8D5B70` 内被调用。）
并且 `0x79FB40`（`AddStateObjectDyingAmritaUp`）的施加路径 `0x714CE0` 里也用它：

```asm
007158C0  movss xmm6, dword ptr [rip + 0xE72EB0]  ; -> 0x1588778
007158C8  call  0x8D4C20                          ; LW 模块查询
007158CD  test  al, al
007158CF  je    0x7158E2
007158D1  call  0x8D4C00                          ; LW 模块查询 2
007158D6  test  al, al
007158D8  je    0x7158E2
007158DA  movss xmm6, dword ptr [rip + 0xA8F28A]  ; -> 0x11A4B6C
007158E2  movss xmm2, dword ptr [rip + 0xE73202]  ; -> 0x1588AEC = 60.0f
007158EA  movaps xmm1, xmm6
007158ED  mov  rcx, rbx
007158F0  call 0x79FB40                           ; 施加 DyingAmritaUp
```

**推测（明确标注）**：`[0x191DE34]` 的 `2` 很可能是"**任务/战斗中**"，而不是"LW 开启"；
`0x191DE2B` 的 `1` 由 `0x8D4CC0` 写入，也可能是"LW 曾开启过"的成就标志（因为这一族
还管理 `Mission Complete` / `Boss defeated` / `Kappa defeated` 等成就字符串，`0x12C2B58…`）。
⇒ **不建议**把它当 LW 判据，但**值得实机打印一次**（见 §4）。

---

## 4. 如果这些还不够：请实机只读打印这 4 项（一次游玩即可定案）

MOD 已有 `ReadProcessMemory` 版的状态容器 DFS（`buff_state_present()`，`docs\RE_NOTES.md` §4.52.8），
所以下面全部是"加几行只读打印"，不需要新机制：

1. **状态容器里出现过的所有状态 ID**（关键）
   在 `buff_state_present()` 的 DFS 里，把每个节点的 `[stateobj+0x10]` 收集成集合，
   每次 LW 开/关前后各打印一次。
   - 若 **开 99 的瞬间**集合里新增 **`0x22`**（或 `0x26`/`0x42`/`0x20`）⇒ §3.2 判据成立，问题②解决。
   - 若都没出现 ⇒ `CallSpirit` 不是 LW，需要改查 `Refer::*` 节点表（MOD 已有 `NODEREG` 诊断）。

2. **记录 0 的完整 16 字节 + 记录 1..3**（`[[char+0x240]]+0x40 + i*0x10`，i=0..3，按 dword 打印）
   在三个时刻各打印一次：**LW 未开启**、**LW 开启瞬间**、**LW 持续中每秒**。
   - 只有 `+0x40`（Ki）在掉 ⇒ 记录 0 = 精力，99 表在别处。
   - `+0x44`（max）或 `+0x48` 在 LW 期间单调下降、LW 结束后回升 ⇒ **99 表就是记录 0**，
     且剩余时间/剩余量就在该字段（问题①②同时解决）。

3. **`param+0xBB0 + j*0x50`（j=0..15）的 `+0x00 / +0x04 / +0x0C / +0x38 / +0x40` 快照**
   同样在"99 开启前/中/后"各一次。
   - 找出 `+0x38` 非空且 `+0x40` ∈ 已知状态 ID 集合的那几条，就能**第一次**得到
     "记录下标 → 状态 ID"的真实映射；其中若 `+0x40 == 0x22` 的那条记录的
     `+0x04` 或 `+0x0C` 在 99 期间下降 ⇒ **那就是 99 计量表**。

4. **全局块 `0x191DE28..0x191DE3C` 的 20 字节**（每次 LW 开/关各一次）
   用来判定 `[0x191DE34]` 的 0/1/2 到底是不是 LW 状态。

> 最省事的一条：**只做第 2 项**（4 个 dword，两个时刻）。它能单独回答"记录 0 是不是 99 表"，
> 因为 `param+0x44` 被引擎硬编码为 `100.0f`（§2.3），如果它变成别的值或 `+0x40` 在 99 期间
> 单调下降，答案就唯一了。

---

## 5. 证据链自查（含一次被我自己否定的结论）

### 5.1 【已否定】"类名列表排序位置 ↔ 构造函数列表排序位置 一一对应"

我最初把 86 个 `AddStateObject*@Character` 的（按 `vftable_rva` 排序）与 89 个构造函数
（按 RVA 排序）按**位置**配对，并用 8 个"锚点"校验——**锚点 8/8 全部通过**。
但输出立刻自相矛盾（同一个 ID `0x1B` 同时被配成 `UseStaminaRate` 与 `MoveSpeed`；
同一个构造函数 `0x79E870` 同时被配成 `Defense` 与 `AmritaGaugeUp`）。
⇒ **锚点通过不能证明配对正确**（锚点是按同一套错误顺序查表，所以必然"通过"）。
已改用 §2.1 的三步直接证据法，并对 RTTI 地址偏差做了独立测量（偏差恰为 0）。
**教训**：排序位置配对必须用"配对结果内部一致性"来验证，不能用同一张表自证。

### 5.2 【已否定】"`vftable_rva` 与真实虚表地址有固定偏差"

曾怀疑 RTTI 的 `vftable_rva` 有偏移（因为看到 `0x11A8B90` vs 我误以为的值）。
用 6 个已独立确知构造函数→虚表配对测量后，**偏差恒为 0**（`_work\re_lw28.py`）。
⇒ 该假设被否定，RTTI 可直接使用。

### 5.3 【已否定 / 工具 bug 留档】扫描 `C7 /0 disp32 imm32` 时立即数取错一字节

`C7 /0` 的布局是 `[0]=C7 [1]=modrm [2..5]=disp32 [6..9]=imm32`。
我第一版在 `k+6` 读立即数（应为 `k+7`），于是把 `mov qword [rax+0x10], 0x20` 读成
`mov dword [rax+0xF10], 0`（同样地 `0x79F001` 的 `0x14` 被读成 `0`）。
⇒ §"记录 +0x40 ← 状态 ID"的**穷举扫描结果全部作废**，本报告**不使用**它。
（保留的结论只有：`rec+0x00/+0x0C/+0x38/+0x40` 的**位移**与语义，那些不依赖立即数。）

### 5.4 【已否定】"记录里的 `+0x38/+0x40` 是虚表 + 参数"

一开始以为 `0x7B4C70` 的 `mov rax,[rcx+rdx*8+0x40]` 是虚表调用。
`0x7A38B0` 的正文（§1.2）证明它是**红黑树查询**（键 `[节点+0x20]`、返回 `[节点+0x28]`，
树根 `[容器+0x170]`），与 §4.52.8 记录的状态容器布局逐字段吻合。

---

## 6. 附：关键 RVA 速查

| RVA | 东西 |
| --- | --- |
| `0x7B4C20` | 记录：`if cur<max: cur=max`（补满）——22 个调用者，全部指向记录 0 |
| `0x7B4C40` | 记录：`rec+0x0C -= xmm1`，下限 0 |
| `0x7B4CB0` | 遍历 16 条记录（`base+0x40` 起，步长 0x50），清空 |
| `0x7B4D10` | 遍历 16 条，先查状态存在（`0x7A38B0`）再清空 |
| `0x7B4E10` | 显式逐条清空（记录 0..7，然后 `+0x378/+0x3B0`…） |
| `0x7B21F0` | `record(i) = table + 8 + i*0x50` |
| `0x7A38B0` | 状态容器查询 `(容器, 状态ID) -> 状态对象`，树根 `+0x170` |
| `0x7AB0A0` | 状态容器移除/通知 `(容器, 状态ID)` |
| `0x7A2890` | 状态施加（既有笔记），容器 `[[char+0x240]]+0x10B0` |
| `0x755DC0` | 玩家槽位访问器（既有锚点） |
| `0x7698E0` | param 构造函数（`param+0x44 = 100.0f` + 补满记录 0） |
| `0x8A5B50` | "阿米粒塔信息 → 玩家 param → 补满记录 0" |
| `0xA83F97` | 阿米粒塔/守护灵状态总施加函数（32,603 字节）——6 个相关状态全在此 |
| `0x714CE0` | 读 `param+0x48` 的 LW 分支；施加 `DyingAmritaUp` |
| `0x707BF0` | 构造"阿米粒塔信息"（`byte+0x12`/`byte+0x13` ÷ `100.0f`） |
| `0x8D4B20..0x8D4D09` | LW/成就模块的全局状态块访问器（全局 `0x191DE28`） |
| `0x8D51F0` | LW/成就模块函数；`0x8D52B7` 处调用 `0xE7ACF0` |
| `0xE7ACF0` | 成就字符串记录器（引用 `Living Weapon Activated` = `0x12C2BE0`）；全镜像唯一调用者 `0x8D52B7` |
| `0x1588B00` | **100.0f**（`0x707BF0` 用它归一化；`0xA86688` 用它乘阿米粒塔量） |
| `0x15886E0` | 1e-4f |
| `0x1588AEC` | 60.0f |

状态对象的构造函数（`ctor(容器, 时长秒, 倍率)`，对象 `+0x10` = 状态 ID）：

| ID | 构造函数 | 类 |
| --- | --- | --- |
| `0x20` | `0x79E870` | `AddStateObjectAmritaGaugeUp` |
| `0x22` | `0x79F1B0` | `AddStateObjectCallSpirit` |
| `0x25` | `0x79ED80` | `AddStateObjectAttackHitRecoverAmrita` |
| `0x26` | `0x7A1EE0` | `AddStateObjectTransformSpirit` |
| `0x27` | `0x79E920` | `AddStateObjectAmritaUpPlus` |
| `0x38` | `0x79FB40` | `AddStateObjectDyingAmritaUp` |
| `0x42` | `0x79E7B0` | `AddStateObjectAmritaGaugeRecover` |

复现脚本（均在 `_work\`，只读）：`re_lw4.py`（状态虚表引用）、`re_lw5.py`（构造函数/ID）、
`re_lw9.py`（LW 字符串引用）、`re_lw10/11/28/29.py`（类名↔构造函数↔ID 及偏差校验）、
`re_lw12.py`（施加点）、`re_lw13.py`（补满/扣减调用点）、`re_lw15/22/23.py`（`100.0f` 与归一化）、
`re_lw16/17.py`（LW 全局状态块）、`re_lw24.py`（param 浮点字段普查）、`re_lw25.py`（容器查询正文）、
`re_lw30/31/32.py`（记录字段扫描，见 §5.3 的作废说明）、`re_lw33.py`（本报告引用的字节证据）。

---

## 7. 交付摘要

**答出的**：

- **③（已确认）**：LW/阿米粒塔的"充能"不是直接写表，而是**施加状态对象**到
  `[[char+0x240]]+0x10B0`；一族 7 个状态类的 **ID 与构造函数 RVA 已逐个确认**
  （`AmritaGaugeUp 0x20/0x79E870`、`AmritaGaugeRecover 0x42/0x79E7B0`、
  `AttackHitRecoverAmrita 0x25/0x79ED80`、`AmritaUpPlus 0x27/0x79E920`、
  `DyingAmritaUp 0x38/0x79FB40`、**`CallSpirit 0x22/0x79F1B0`**、`TransformSpirit 0x26/0x7A1EE0`），
  且 6 个的施加点**全部集中在 `0xA83F97`**。另有一条显式补满记录 0 的短函数 `0x8A5B50`。
- **②（较可信 + 已确认的备选）**：最佳判据 = 状态容器里是否存在 **状态 ID `0x22`
  (`AddStateObjectCallSpirit`)**；命名链 `TSUKUMO_WEAPON` / `TUT_TSUKUMO.wmv` /
  `Player::SetTsukumoWeaponActiveFlag` / `Living Weapon Activated` 已确认 Tsukumo＝LW。
  备选：全局块 `0x191DE28`（LW/成就模块专用，`[0x191DE34]` 是 0/1/2 子状态机），
  语义**推测**，需实机确认。
- **①（结构已确认，具体字段未唯一确定）**：整套计量表结构、两个访问原语、记录字段布局
  已确认；记录 0（`param+0x40`）是**唯一被补满**且 `max` 硬编码 `100.0f` 的表，
  `param+0x48` 是 **LW 模块唯一直接读的 param 字段** —— 但既有笔记用引擎 getter 证明
  `param+0x40/+0x44` 是精力，冲突未解决，故**不宣称**它就是 99 表。

**未能确定的**：99 表的确切基址+偏移。

**最佳下一步（一步即可定案）**：用 MOD 现有的只读容器 DFS（`buff_state_present()`）
一次性打印两样东西——**(1)** 容器内出现过的全部状态对象 ID；**(2)**
`[[char+0x240]]+0x40` 起 4 个 dword（`+0x40/+0x44/+0x48/+0x4C`）——各在"99 开启前"与
"99 持续中"采样一次。前者直接验证 `0x22` 判据；后者直接判定记录 0 是不是 99 表
（`+0x44` 被硬编码 100.0f，若它在 99 期间变化或 `+0x40` 单调下降，答案唯一）。
