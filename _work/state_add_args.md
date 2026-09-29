# `add()` (0x7A2890) — 全部引擎调用点的五个实参 · 只读静态逆向

- 目标：`nioh.exe` 1.24.8，镜像 `_work\nioh1.mem.exe`（file offset == RVA；本机 dump 的 PE ImageBase 字段
  为 `0x7FF7AA2D0000`，下文**一律用 RVA**）。工具：`tools\dump_pe.py`、`tools\xref.py`（`_work\nioh1.xref.pkl`）。
- 全程静态；没有任何实机结论。每条断言都标了 **confirmed / probable / guess**。
- 前置报告：`_work\dmgrate_report.md`（本报告修正/确认了它若干条）。

---

## 0. 结论速览（先看这个）

| # | 问题 | 结论 | 置信度 |
|---|---|---|---|
| 1 | 引擎调用点的 **key (rdx)** 都有哪些 | 直接调用 `0x7A2890` 的**确实 43 处**（原报告数字对）；另有 **6 处**经包装函数 `0x7A2AD0` 间接进入，所以引擎合计 **49** 个 `add()` 调用点。常量的 key 集合 = `{0,1,3,0x17,0x18,0x19,0x2b,0x31,0x32,0x33,0x34,0x35,0x36,0x3b,0x3c,0x3e,0x3f,0x40,0x41,0x42,0x44,0x45,0x46,0x4d,0x50,0xb0,0x13c}` 外加若干**运行期变量**（`r15d`、`[rec+0x40]`、`word[rec+0xa]-1`、`r9+0x39..0x3b`、`r15+0x34`、`r15+0x46`）。**原报告漏了 `0,0x17,0x31,0x32,0x33,0x3b,0x46,0xb0,0x13c`；`0x44/0x45` 经核对是对的（走包装函数）**。**`0x33` 真的被引擎用了**（0x724777）⇒ 与 MOD 的 Armor key 撞键；`0x1E`、`0x50` 之外没找到 `0x1E`。 | **confirmed**（每个点的 key 都有指令级证据） |
| 2 | **r9d（第 4 参）** | **49 个点里 38 个（78%）传常量 `-1`（0xFFFFFFFF）**；其余是可变值（`0xA83F97` 内 11 处取 `[rsp+0x60]`/`r13d`，一处 `lea r9d,[r15-2]`）。编译器的写 -1 惯用法是 `lea r9d,[rdx - (key+1)]`（edx 刚存了 key，直接算成 -1）。**`add()` 对它的唯一动作是 `mov [rdi+0x24], ebx`（0x7A2941）——写进 `obj+0x24`(dword)**；ctor 已经把 `obj+0x24` 初始化成 `-1`，tick 链（0x7AD260 → 0x7ACCA0 → slot5/slot6）**完全不读 `obj+0x24`**。⇒ **MOD 传 -1 就是引擎的默认形状。** | **confirmed** |
| 3 | **stack0（第 5 参，第一个栈参）** | **49 个点里 44 个传 `0`**；非 0 的只有 5 处：`0x74CBCF`(r15b)、`0x7AA459/0x7AA554/0x7AA651`(r14b)、`0xA8425A`(r13b)。**`add()` 对它的唯一动作是：非 0 时 `mov byte [rdi+0x22], 1`（0x7A294E）**。`obj+0x22` 有真实语义：`0x7A3AF0`（唯一调用者 `0x749640` 格挡/受击状态机）遍历 `[mgr+0x170]` 树，只统计 `slot3(obj)!=0 && [obj+0x1c]!=0 && [obj+0x22]==0` 的对象 ⇒ **`stack0` 是"不要把我算进减伤计数"的标记**，所以对真正的减伤状态传 `0` 是对的。 | **confirmed** |
| 4 | **除 `add()` 外还需要别的注册调用吗？** | **不需要。** `add()` **只**做 `[mgr+0x170]` 红黑树插入；它**不**调 slot 5（onActivate），**不**碰活动链表 `[mgr+0x198]`/计数 `[mgr+0x1a0]`。但**每帧 tick `0x7ACCA0` 会自己懒激活**：`[obj+0x1c]!=0` 是唯一闸门，非 0 则先调一次 slot 5（并把 `[obj+0x1d]=1`）再每帧调 slot 6。而**所有类的 ctor 已经把 `dword [obj+0x1c] = 1`**（DamageRate ctor `0x79F450` 的 `c7401c01000000`）。`0x7A2C80` 是**另一条互斥的注册路径**（把对象挂进 `[mgr+0x198]` 状态 ID 链表，供 `find_by_state_id 0x7A3C40` 用），引擎在 `key == -1` 时走它、否则走 `add()`（`0x71F1AC`/`0x71F1C6` 二选一），两个容器**每帧都被 tick**。 | **confirmed**（代码级） |
| 5 | **MOD 传的实参形状对不对** | **对，和引擎主导形状逐位一致**：`add(mgr, key, obj, -1, 0)` + 用引擎 ctor 造的对象 = 引擎在 38/49 个点上的原样写法。**"参数形状不对" 这条假设可以排除。** | **confirmed** |

**最终答案（详见 §9）**：实参形状**不需要改**；**不需要**额外注册调用；`add()` 返回 1 且节点在树里**不能**证明效果生效——因为生效只取决于"每帧 tick 有没有跑"，而 tick 链把效果写进 `mgr+0x24/+0x28`（damage）和 `mgr+8`（armor flag）。**一个样本外挂进去的节点变哑，最可能的原因是：它被挂进了一个"消费者不读"的 state manager 实例**（`add()` 无法察觉），其次是**同 key 破坏性替换**（Armor 的 key `0x33` 被引擎自己的 ability 分支 `0x724777` 占用）。

---

## 1. `add()` 本体（0x7A2890）—— 逐条读，确定它对五个参数的每一处使用

函数外形（前 10 字节同时是 MOD 的字节校验锚）：`48 8B C4 57 41 56 41 57 48 83`（`mov rax,rsp; push rdi; push r14; push r15; sub rsp,0x50`）。

```asm
007A28B0  418bd9            mov  ebx, r9d        ; ★ 第4参缓存
007A28B3  498bf8            mov  rdi, r8         ; ★ 第3参 obj
007A28B6  8bea              mov  ebp, edx        ; ★ 第2参 key
007A28B8  488bf1            mov  rsi, rcx        ; ★ 第1参 mgr
007A28BB  4d85c0            test r8, r8
007A28BE  7450              je   0x7A2910       ; obj==NULL → 返回 0
; ---- 前置闸门：参数对象是否"可用" ----
007A28C0  488b8168010000    mov  rax, [rcx+0x168]   ; mgr+0x168 = 宿主角色
007A28C7  488b8840020000    mov  rcx, [rax+0x240]   ; [char+0x240] = param
007A28CE  4883c110          add  rcx, 0x10
007A28D2  e8b9190100        call 0x7B4290           ; 谓词(param+0x10)
007A28D7  84c0              test al, al
007A28D9  743c              je   0x7A2917          ; al==0 → 正常路径
; al!=0 → 反激活 + 析构 + 释放 obj，返回 0（0x7A28DB-0x7A2912）
007A2917  83fdff            cmp  ebp, -1
007A291A  7515              jne  0x7A2931
007A291C  4533c0            xor  r8d, r8d
007A291F  488bd7            mov  rdx, rdi
007A2922  488bce            mov  rcx, rsi
007A2925  e896fcffff        call 0x7A25C0           ; key==-1 → 析构 obj，返回 0
007A2931  8b4608            mov  eax, [rsi+8]       ; mgr 标志 dword
007A2934  c1e81d            shr  eax, 0x1d          ; bit 29
007A2937  a801              test al, 1
007A2939  7406              je   0x7A2941
007A293B  837f1045          cmp  dword [rdi+0x10], 0x45
007A293F  75db              jne  0x7A291C          ; (bit29 && id!=0x45) → 析构 obj，返回 0
; ---- ★★ 对 r9d / stack0 的唯一写入 ----
007A2941  895f24            mov  dword [rdi+0x24], ebx     ; 第4参 → obj+0x24
007A2944  80bc249000000000  cmp  byte [rsp+0x90], 0     ; ★第5参（见下）
007A294C  7404              je   0x7A2952
007A294E  c6472201          mov  byte [rdi+0x22], 1        ; 第5参 !=0 → obj+0x22 = 1
; ---- 按键在 [mgr+0x170] 树里找同 key 节点 ----
007A2952  488b8e70010000    mov  rcx, [rsi+0x170]   ; 树头哨兵
...
007A2966  396820            cmp  dword [rax+0x20], ebp    ; 比较 node+0x20（key）
007A298A  483bd9            cmp  rbx, rcx
007A298D  0f84df000000      je   0x7A2A72          ; 没找到 → 插入
; ---- 找到了：同 key 冲突处理（破坏性） ----
007A2993  488b4b28          mov  rcx, [rbx+0x28]    ; 已有 obj
007A2997  8b4110            mov  eax, [rcx+0x10]    ; 已有 state id
007A299A  448b4710          mov  r8d, [rdi+0x10]    ; 新 state id
007A299E  413bc0            cmp  eax, r8d
007A29A1  410f94c6          sete r14b               ; same_id
007A29A5  80791c00          cmp  byte [rcx+0x1c], 0 ; 已有 obj 是否 active
007A29A9  7435              je   0x7A29E0
007A29AB  80792000          cmp  byte [rcx+0x20], 0
007A29AF  742f              je   0x7A29E0
007A29B1  80792100          cmp  byte [rcx+0x21], 0
007A29B5  740e              je   0x7A29C5
007A29B7  413bc0            cmp  eax, r8d
007A29BA  7509              jne  0x7A29C5
007A29BC  488b01            mov  rax, [rcx]
007A29BF  488bd7            mov  rdx, rdi
007A29C2  ff5040            call [rax+0x40]         ; old.slot8(new)
007A29C5  4533c0            xor  r8d, r8d
007A29C8  488bd7            mov  rdx, rdi
007A29CB  488bce            mov  rcx, rsi
007A29CE  e8edfbffff        call 0x7A25C0          ; ★析构"新"对象
...
007A2A00  450fb6c6          movzx r8d, r14b
007A2A04  488bce            mov  rcx, rsi
007A2A07  e8b4fbffff        call 0x7A25C0          ; ★析构"旧"对象
007A2A0C  488bc3            mov  rax, rbx
... (内联 _Tree::erase：找后继 + 再平衡)
007A2A4D  488bd3            mov  rdx, rbx
007A2A50  488d8e70010000    lea  rcx, [rsi+0x170]
007A2A57  e8f4afffff        call 0x79DA50          ; 摘除该节点，返回节点
007A2A5F  e88c458000        call 0xFA6FF0
007A2A6D  41ff5058          call [r8+0x58]         ; free(节点)
; ---- 插入（无论"键已存在被摘掉"还是"本来没有"）----
007A2A72  896c2438          mov  dword [rsp+0x38], ebp     ; { key
007A2A76  48897c2440        mov  qword [rsp+0x40], rdi     ;   obj }
007A2A7B  488d542438        lea  rdx, [rsp+0x38]
007A2A80  488d8e70010000    lea  rcx, [rsi+0x170]
007A2A87  e8e487ffff        call 0x79B270                 ; 建节点
007A2A8C  4c8d4820          lea  r9, [rax+0x20]           ; node+0x20 = key 字段
007A2A95  4533c0            xor  r8d, r8d
007A2AA4  e82791ffff        call 0x79BBD0                 ; 挂上（再平衡）
007A2AA9  b001              mov  al, 1                    ; ★返回 1
007A2AAB  ... 恢复现场 / ret
```

**从这里能直接得出的三件事（confirmed）：**

1. **第 4 参 (r9d) 只写 `obj+0x24`**（0x7A2941），**第 5 参 (stack0) 只在非 0 时写 `obj+0x22=1`**（0x7A294E）。
   两条之后都不再被 `add()` 读。
2. **第 5 参的取址证明**：`add()` 入口 rsp=R；`push rdi/r14/r15` 后 R-0x18，`sub rsp,0x50` 后 = R-0x68。
   caller 的第 5 个实参在 `[R+0x28]` ⇒ `[R+0x28] = [rsp+0x68+0x28] = [rsp+0x90]`，与 `0x7A2944` 的
   `cmp byte ptr [rsp+0x90], 0` 完全吻合。**`add()` 的第 5 个参数就是这里的 `[rsp+0x90]`。**（confirmed）
3. **`add()` 里没有任何对手册之外的注册**：没有 `[mgr+0x198]`、没有 `[mgr+0x1a0]`、
   没有任何 `call [rax+0x28]`（slot 5 / onActivate）。它出现的虚调用只有
   `[rax+0x38]`(slot7 反激活)、`[rax]`(slot0 析构)、`[rax+0x40]`(slot8) 以及分配器的 `[r8+0x58]`。
   **⇒ `add()` = "按键插入/替换到 keyed 容器"。**（confirmed）

---

## 2. Q1 — 每个调用点的 `rdx`（key）

### 2.1 直接 `call 0x7A2890` 的 43 处（和原报告计数一致）

探测方式：在 `.text` 上扫 `E8/E9 rel32`，`rva_next + rel == 0x7A2890`；再用 `.pdata` 边界做**函数内反向切片**
（跟踪 rcx/edx/r8/r9d 与 `[rsp+0x20]` 的最近一次定义，遇 `call` 按易失寄存器处理；`rsp` 偏移按 push/pop/sub/add 折算）。

| # | 调用点 | 所属函数 | key (edx) | r9d | stack0 | 备注 |
|---|---|---|---|---|---|---|
| 1 | `0x715881` | `0x714CE0` | **0x50** | -1 | 0 | obj = 上一条 `0x79E9E0`(Armor, id 0x33) 的返回值 |
| 2 | `0x71F1AC` | `0x71EFEC` | **`word[rec+0xa] − 1`** （运行期） | -1 | 0 | 记录驱动；`key==-1` 时改走 `0x7A2C80`（见 §5） |
| 3 | `0x7201CA` | `0x7200DF` | **0x2b** | -1 | 0 | |
| 4 | `0x72024B` | `0x7200DF` | **0** | -1 | 0 | |
| 5 | `0x7202F9` | `0x7200DF` | **0x4d** | -1 | 0 | |
| 6 | `0x745CD5` | `0x745C60` | **0x36** | -1 | 0 | ability 驱动（`0x7A1F80` 造对象） |
| 7 | `0x745D79` | `0x745C60` | **0x3c** | -1 | 0 | |
| 8 | `0x745DAA` | `0x745C60` | **0x3f** | -1 | 0 | 用 `0x79F7D0`(id 0x01) 造对象 |
| 9 | `0x745E18` | `0x745C60` | **0x3b** | -1 | 0 | |
| 10 | `0x74CBCF` | `0x749640` | **0x3e** | -1 | **r15b（可变）** | 本组里 stack0 唯一的非 0 常量路径 |
| 11 | `0x74EFE6` | `0x749640` | **r15d（可变）** | -1 | 0 | r15d 在函数内被多处赋值，静态不唯一 |
| 12 | `0x766ED0` | `0x766E93` | **0x17** | -1 | 0 | |
| 13 | `0x76720B` | `0x76712D` | **0x42** | -1 | 0 | |
| 14 | `0x7672BC` | `0x76712D` | **0x40** | -1 | 0 | |
| 15 | `0x767385` | `0x76712D` | **0** | -1 | 0 | |
| 16 | `0x767423` | `0x76712D` | **0** | -1 | 0 | |
| 17 | `0x7674F8` | `0x76712D` | **1** | -1 | 0 | |
| 18 | `0x76751C` | `0x76712D` | **0x34** | -1 | 0 | 紧接着 `0x767503` 的 Armor ctor |
| 19 | `0x7675CD` | `0x76712D` | **0x41** | -1 | 0 | |
| 20 | `0x767624` | `0x76712D` | **0** | -1 | 0 | |
| 21 | `0x7A2AF7` | `0x7A2AD0` | **（透传包装函数的 edx）** | **-1（包装函数写死）** | **0（写死）** | 见 §2.3 |
| 22 | `0x7AA459` | `0x7AA395` | **`r9 + 0x39`（可变）** | -1 | **r14b（可变）** | |
| 23 | `0x7AA554` | `0x7AA395` | **`r9 + 0x3a`（可变）** | -1 | **r14b** | |
| 24 | `0x7AA651` | `0x7AA575` | **`r9 + 0x3b`（可变）** | -1 | **r14b** | |
| 25 | `0x7B0ACF` | `0x7B0AA0` | **`[rec+0x40]`（可变）** | -1 | 0 | rec = 传入的结构 |
| 26 | `0x7B0B3A` | `0x7B0B13` | **`[rec+0x40]`** | -1 | 0 | |
| 27 | `0x7B7A22` | `0x7B76E7` | **`[rec+0x40]`** | -1 | 0 | |
| 28 | `0x91C730` | `0x91C4A0` | **0x19** | -1 | 0 | |
| 29 | `0x91C8CE` | `0x91C7E0` | **0x19** | -1 | 0 | |
| 30 | `0x93EF65` | `0x93EBF1` | **0x19** | -1 | 0 | |
| 31 | `0xA8425A` | `0xA83F97` | **1** | `[rsp+0x60]`（可变） | **r13b（可变）** | 原报告引用过此点 |
| 32 | `0xA8441C` | `0xA83F97` | **3** | `[rsp+0x60]` | 0 | |
| 33 | `0xA84C40` | `0xA83F97` | **0x18** | **r13d（可变）** | 0 | |
| 34 | `0xA85163` | `0xA83F97` | **r15d（可变）** | `[rsp+0x60]` | 0 | |
| 35 | `0xA854F5` | `0xA83F97` | **r15d** | `[rsp+0x60]` | 0 | |
| 36 | `0xA87716` | `0xA83F97` | **0x35** | `[rsp+0x60]` | 0 | |
| 37 | `0xA877BF` | `0xA83F97` | **`r15 + 0x34`** | `[rsp+0x60]` | 0 | |
| 38 | `0xA8786A` | `0xA83F97` | **0x35** | `[rsp+0x60]` | 0 | |
| 39 | `0xA8790F` | `0xA83F97` | **0x35** | `[rsp+0x60]` | 0 | |
| 40 | `0xA8846D` | `0xA83F97` | **0x34** | `[rsp+0x60]` | 0 | 紧接 `0xA88453` 的 Armor ctor |
| 41 | `0xA886C9` | `0xA83F97` | **`r15 + 0x46`** | **`r15 − 2`** | 0 | |
| 42 | `0xA886F9` | `0xA83F97` | **0x46** | -1 | 0 | |
| 43 | `0xD92F30` | `0xD924B0` | **0x3e** | -1 | 0 | |

### 2.2 原报告列表的核对（confirm / correct）

原报告：`1, 3, 0x18, 0x19, 0x2b, 0x34..0x36, 0x3c, 0x3e..0x42, 0x44, 0x45, 0x4d, 0x50`。

- **确认**：`1, 3, 0x18, 0x19, 0x2b, 0x34, 0x35, 0x36, 0x3c, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x4d, 0x50` 全部用过（逐个给了调用点）。
- **`0x44` / `0x45` 是对的**，但**不在**那 43 个直接调用点里：它们在 `0x7A2AD0`（包装函数）的调用点
  `0x74D767`(0x44) 与 `0x74D9AA`(0x45，由 `0x74D886` 跳入) 上，包装函数把 edx 原样递给 `add()`。
  （原报告写 "`0x74D886 jmp <add>`" 不够精确：那里是 `jmp 0x74D9A4`，最终 `call 0x7A2AD0`；见 §2.3。）
- **更正/补充（原报告漏项）**：`0`（4 处）、`0x17`、`0x31`、`0x32`、**`0x33`**、`0x3b`（3 处）、`0x46`、
  `0xb0`、`0x13c`，以及全部**运行期变量** key。
- **`0x1E` 确认不在**任何（可直接静态定值的）引擎 key 里。`0x31/0x32/0x33` 与 `0xb0/0x13c` 是新增的。
- **`0x33` 是撞键风险点**：`0x724777` 分支用 key `0x33` 挂 **id = 0x4C**（ctor `0x7A0A40`）的对象；
  同一调用点另外两个分支用 `0x32`(id 0x4B, ctor `0x7A0AE0`) / `0x31`(id 0x4A, ctor `0x7A0B80`)。
  **key 与 state id 完全无关**（这三条是真凭实据：key 0x31/0x32/0x33 ↔ id 0x4A/0x4B/0x4C）。
  而 MOD 的 Armor 用 `key = 0x33 = 自己的 state id` ⇒ 一旦引擎这条 ability 分支跑起来，
  `add()` 的**同 key 破坏性替换**会摘除并析构 MOD 的 Armor 状态对象（详见 §7.2）。置信度 **probable**（撞键事实 confirmed；运行时是否真发生取决于该分支是否被走到）。

### 2.3 经包装函数 `0x7A2AD0` 的 6 处（合计 49）

`0x7A2AD0` 是 `add(mgr, key, obj, -1, 0)` 的薄包装（**逐字节给定 r9d/stack0**）：

```asm
007A2AD0  ...
007A2AE4  41b9ffffffff      mov  r9d, 0xffffffff          ; ★ 恒 -1
007A2AEA  c644242000        mov  byte [rsp+0x20], 0       ; ★ 恒 0
007A2AEF  498bf8            mov  rdi, r8
007A2AF2  8bf2              mov  esi, edx                 ; key 透传
007A2AF4  488be9            mov  rbp, rcx                 ; mgr 透传
007A2AF7  e894fdffff        call 0x7A2890
007A2AFC  0fb6d8            movzx ebx, al
...
007A2B31  e83a24faff        call 0x744F70                 ; add 成功后额外的效果/通知
```

它的 6 个调用点（扫 `E8/E9 → 0x7A2AD0`）：

| 调用点 | 所属函数 | 传给包装函数的 key (edx) |
|---|---|---|
| `0x724777` | `0x7246CF` | **0x33 / 0x32 / 0x31**（三个分支汇到同一条 `call`） |
| `0x730ECB` | `0x730AD0` | **0x3b** |
| `0x74B0AE` | `0x749640` | **0x3b** |
| `0x74D767` | `0x749640` | **0x44** |
| `0x74D9AA` | `0x749640` | **0x45**（来自 `0x74D881: mov edx,0x45`）或 **0xb0**（来自 `0x74D99F: mov edx,0xb0`） |
| `0x74DAC4` | `0x749640` | **0x13c** |

> 注意 `0x74D881` 那条 `mov edx,0x45` 后面挂的是 **DamageRate ctor `0x79F450`**（格挡减伤 0.95、寿命 300s），
> 但它是**经包装函数**进 `add()` 的 —— **这正好证明"引擎也用 `add(mgr,key,obj,-1,0)` 挂 DamageRate"**，
> 与 MOD 的写法同构。

---

## 3. Q2 — 第 4 参 r9d：引擎传什么、`add()` 拿它干嘛

**引擎传什么（49 点统计）**

- **常量 `-1`（0xFFFFFFFF）：38 / 49（78%）**。包含全部 6 个包装函数点。
  编译器惯用法：`lea r9d,[rdx − (key+1)]`，例如 `0x71587A: lea r9d,[rdx-0x51]`（当时 edx=0x50）
  ⇒ `0x50 − 0x51 = 0xFFFFFFFF`。也有直接 `mov r9d, 0xffffffff`（如 `0xA84241`、`0x7B0AC9`）。
- **可变：11 / 49**，全部集中在 `0xA83F97`（ability/统计驱动，26 个 add 相关调用）：
  10 处取 `dword [rsp+0x60]`（该槽是 `0xA83F97` 的一个**入参/局部标志字**，不是常量），
  1 处 `lea r9d,[r15-2]`（0xA886C5），1 处 `mov r9d, r13d`（0xA84C30）。
  **不是位掩码**，就是"调用方标志"原样透传。

**`add()` 拿它干嘛（confirmed，全文已读）**

```asm
007A28B0  418bd9            mov  ebx, r9d
007A2941  895f24            mov  dword ptr [rdi+0x24], ebx     ; 唯一用处
```

- **唯一落点 = `obj+0x24`（4 字节）**。ctor 已经把 `obj+0x24` 初始化成 `-1`
  （DamageRate `0x79F4D3: mov dword [rax+0x24], 0xffffffff`；Armor `0x79EA5B` 同）。
- **它不是 "merge vs insert" 的判据**：合并/插入由 **key** 决定（0x7A2966 `cmp [rax+0x20], ebp`），
  同 key 时再看**双方的 state id** 决定丢弃谁（§1 的 0x7A2993-0x7A2A0C）。
- **它不触发 onActivate**：`add()` 全文没有任何 `call [rax+0x28]`。
- **tick 链不读它**：`0x7AD260`（树遍历）→ `0x7ACCA0`（通用 tick）→ slot5/slot6 的代码里，
  只有 `[obj+0x1c]/[obj+0x1d]/[obj+0x1e]/[obj+0x1f]/[obj+0x28]/[obj+0x2c]/[obj+0x30]/[obj+0x34]/[obj+0x38]`。
  所以 `obj+0x24` 只是留给**类特定代码/消费者**读的一个 flag 槽。

**⇒ 结论：MOD 传 `-1` 与引擎 78% 的点、以及与"引擎自己挂 DamageRate"的那两个点（键 1 / 键 0x45）完全一致。
没有理由改。**（confirmed）

---

## 4. Q3 — 第 5 参 stack0：引擎传什么、`add()` 拿它干嘛

**引擎传什么（49 点统计）**

- **常量 `0`：44 / 49（90%）**。包含全部 6 个包装函数点（`0x7A2AEA: mov byte [rsp+0x20],0`）。
- **非 0 / 可变：5 处**：
  | 调用点 | 值 | 说明 |
  |---|---|---|
  | `0x74CBCF` | `r15b` | `0x749640` 格挡/受击状态机内 |
  | `0x7AA459` | `r14b` | `0x7AA395` 内三个点都是 `r14b` |
  | `0x7AA554` | `r14b` | |
  | `0x7AA651` | `r14b` | `0x7AA575` |
  | `0xA8425A` | `r13b` | ability 路径 |

**`add()` 拿它干嘛（confirmed）**

```asm
007A2944  80bc2490000000    cmp  byte ptr [rsp + 0x90], 0    ; 第5参
007A294C  7404              je   0x7A2952
007A294E  c6472201          mov  byte ptr [rdi + 0x22], 1    ; 唯一用处：obj+0x22 = 1
```

- **唯一落点 = `obj+0x22`（1 字节）= 1（当且仅当 stack0 != 0）**。ctor 已把 `obj+0x22` 清 0。
- **它有真实语义**（这是"0 才是正常值"的证据）：`0x7A3AF0`（**唯一调用者 `0x749640`**）在 keyed 树里做计数：

  ```asm
  007A3AF0  ...  mov rax,[rcx+0x170] ; 树
  007A3B13  488b4b28   mov  rcx,[rbx+0x28]     ; obj
  007A3B17  488b01     mov  rax,[rcx]
  007A3B1A  ff5018     call [rax+0x18]         ; slot3(obj)（DamageRate: 0x7A4B40 = "1.0f > param"）
  007A3B25  80781c00   cmp  byte [rax+0x1c], 0 ; active?
  007A3B29  7408       je   skip
  007A3B2B  80782200   cmp  byte [rax+0x22], 0 ; ★只统计 +0x22 == 0 的
  007A3B2F  7502       jne  skip
  007A3B31  ffc7       inc  edi                ; 计数++
  ```
  ⇒ **`stack0 != 0` 等价于"把我排除在减伤状态计数之外"**。传 `0` 是"我是一个正常、要被计入的减伤状态"。
- 另：`obj+0x22` 也被 `0x7AB250`(`cmp byte [rbx+0x22],0`)、`0x7AB3C0`、`0x7AB530`、`0x7A3AF0` 读；
  被 `0x7AA395`(×3)、`0x7ACDB0`(×2) 写。

**⇒ 结论：MOD 传 `0` 正确（引擎 90% 的选择、也是"我是个真减伤"的正确语义）。**（confirmed）

---

## 5. Q4 — `add()` 之外还需要什么，才能让每帧 tick 调 slot 6？

### 5.1 `add()` **不**做的两件事（confirmed）

- **不调 slot 5（onActivate）**：`add()` 全文虚调用只有 slot0 / slot7 / slot8 / 分配器 free。
- **不碰活动链表**：全文没有 `+0x198` / `+0x1a0` 位移。
  `[mgr+0x198]`（表头 + 元素计数在 `+0x1a0`）**只由 `0x7A2C80` 填充**（内部 `0x7A2D6F call 0x7AAB30`，
  `0x7AAB9B inc rax / mov [rdi+8],rax` 就是 `[mgr+0x1a0]++`）。

### 5.2 `0x7A2C80` 是什么，和 `add()` 什么关系（confirmed）

```asm
007A2C80  4889542410        mov  qword [rsp+0x10], rdx   ; 保存 obj
007A2CA8  4885d2            test rdx, rdx
007A2CAB  7507              jne  0x7A2CB4
007A2CAD  32c0              xor  al, al ; ret 0           ; obj==NULL → 0
007A2CB4  4584c0            test r8b, r8b
007A2CB7  0f84a6000000      je   0x7A2D63              ; r8b==0：跳过查重
; --- r8b!=0：在 [mgr+0x198] 里找 slot4(obj)==同 state id 的旧项，摘除+析构 ---
007A2CBD  488b8198010000    mov  rax, [rcx+0x198]
007A2CC4  488b38            mov  rdi, [rax]              ; 表头->next
007A2CCA  0f8493000000      je   0x7A2D63
007A2CD0  488b4f10          mov  rcx, [rdi+0x10]         ; node+0x10 = obj
007A2CD4  488b01            mov  rax, [rcx]
007A2CD7  488bd5            mov  rdx, rbp
007A2CDA  ff5020            call [rax+0x20]              ; slot4(obj, target) = 同 state id?
...
007A2D34  48ff8ea0010000    dec  qword [rsi+0x1a0]       ; 计数--
...
007A2D63  488d8e98010000    lea  rcx, [rsi+0x198]        ; ★ 表头
007A2D6A  488d542448        lea  rdx, [rsp+0x48]         ; &obj
007A2D6F  e8bc7d0000        call 0x7AAB30                ; ★ 追加到 [mgr+0x198]，[mgr+0x1a0]++
007A2D74  b001              mov  al, 1
```

`0x7AAB30`（唯一 3 个调用者：`0x7A2C50`、`0x7A2C80`、`0x7A2D90`）是那个容器的插入：
`0x7AAB6E call [rdx+8]` 取容量、`0x7AAB95 cmp rdx,1 / jb 断言`、`0x7AAB9B inc rax / 0x7AAB9E mov [rdi+8],rax`（`[mgr+0x1a0]++`）。

**`add()` 与 `0x7A2C80` 是"互斥的两条注册路径"**，证据是引擎唯一同时涉及两者的点 `0x71EFEC`
（记录→状态分发器的调用方）：

```asm
0071F186  e855500000        call 0x7241E0          ; 由记录造状态对象 → rax
0071F190  0fb7570a          movzx edx, word [rdi+0xa]
0071F197  ffca              dec  edx               ; key = word[rec+0xa] − 1
0071F199  83faff            cmp  edx, -1
0071F19C  7415              je   0x71F1B3          ; ★ key == -1 → 走 0x7A2C80
0071F19E  41b9ffffffff      mov  r9d, 0xffffffff
0071F1A4  c644242000        mov  byte [rsp+0x20], 0
0071F1A9  4c8bc0            mov  r8, rax
0071F1AC  e8df360800        call 0x7A2890          ; ★ 有 key → 走 add()
0071F1B1  eb18              jmp  0x71F1CB
0071F1B3  440fb64708        movzx r8d, byte [rdi+8]
0071F1B8  488bd0            mov  rdx, rax
0071F1BB  41c0e805          shr  r8b, 5
0071F1BF  41f6d0            not  r8b
0071F1C2  4180e001          and  r8b, 1            ; 查重开关
0071F1C6  e8b53a0800        call 0x7A2C80          ; ★ 无 key → 走活动链表注册
```

⇒ `0x7A2C80(mgr, obj, bool search)` 的原型也从这段读出来：**`unsigned char f(void *mgr, void *obj, unsigned char search)`**。
它在**两条**路径上都被引擎当"注册动作"用（另一个例子见下），所以它不是 `add()` 的后续步骤，而是**替代容器**。

### 5.3 每帧 tick 真的会跑到 slot 6 —— 三个容器都被 tick（confirmed）

`0x7AC6E0`（每帧 manager 更新；**调用者 `0x76E579`**，原报告写 `0x76EB45`，此处更正）的关键序列：

```asm
007AC7D3  e8481bffff        call 0x79E320          ; ★★ 先复位：mgr+8=0，mgr+0xc..+0x64 = 1.0f
007AC7EB  e8c0890000        call 0x7B51B0          ; param+0x68/-0x6c + 16 条记录复位
007AC7F6  e8650a0000        call 0x7AD260          ; ★ tick keyed 树 [mgr+0x170]  ← add() 插入的那棵
007AC7FB  488b9e80010000    mov  rbx, [rsi+0x180]  ; 单对象槽
007AC866  e835040000        call 0x7ACCA0          ;    tick 它
007AC86B  488b8688010000    mov  rax, [rsi+0x188]  ; 链表
007AC90F  e88c030000        call 0x7ACCA0
007AC924  488b8698010000    mov  rax, [rsi+0x198]  ; ★ 活动链表（0x7A2C80 的容器）
007AC9CF  e8cc020000        call 0x7ACCA0
007AC9EA  e8c1030000        call 0x7ACDB0          ; 另一棵 [mgr+0x1a8]
```

`0x79E320`（**每帧复位器**，这是原报告 §5.1 里 H1 的精确答案）：

```asm
0079E320  c7410c0000803f    mov  dword [rcx+0xc], 1.0f
0079E329  894108            mov  dword [rcx+8], eax      ; ★ mgr+8 = 0（标志位每帧清零）
0079E353  c741240000803f    mov  dword [rcx+0x24], 1.0f  ; ★ 承伤累加器 #1
0079E35A  c741280000803f    mov  dword [rcx+0x28], 1.0f  ; ★ 承伤累加器 #2
...       （+0x10..+0x64 全部 = 1.0f，+0x44 = -1.0f）
```
调用者：`0x79BD50`(manager ctor)、`0x79E6D0`、`0x7A2EF0`(清空全部状态)、`0x7A4620`、**`0x7AC6E0`（每帧）**。

`0x7AD260`（keyed 树遍历）→ `0x7ACCA0(rcx=obj, rdx=mgr, xmm2=dt)`：

```asm
007ACCA0  ...
007ACCAA  80791c00          cmp  byte [rcx+0x1c], 0      ; ★★ 唯一的"是否 apply"闸门
007ACCC8  0f849d000000      je   0x7ACD6B              ; ==0 → 跳过 slot5/slot6（但仍然累计时间并返回"活着"）
007ACCCE  84c0              test al, al                  ; al = [obj+0x1d]
007ACCD0  750e              jne  0x7ACCE0
007ACCD2  488b01            mov  rax, [rcx]
007ACCD5  c6411e01          mov  byte [rcx+0x1e], 1
007ACCD9  ff5028            call [rax+0x28]              ; ★ slot 5 = onActivate（懒激活）
007ACCDC  c6431d01          mov  byte [rbx+0x1d], 1
007ACCE0  f30f104b38        movss xmm1, [rbx+0x38]
007ACCE5  0f2fce            comiss xmm1, xmm6
007ACCEC  7620              jbe  0x7ACD0E
...
007ACD0E  488b03            mov  rax, [rbx]
007ACD11  0f28d7            movaps xmm2, xmm7
007ACD14  488bd7            mov  rdx, rdi                ; ★ rdx = 传进来的目标 = mgr
007ACD17  488bcb            mov  rcx, rbx
007ACD1A  ff5030            call [rax+0x30]              ; ★ slot 6 = apply
```

树遍历的 tick 分支（`rdx = rbp = mgr`，`rbp` 在 `0x7AD294 mov rbp,rcx` 设定）：

```asm
007AD2C4  f30f107328        movss xmm6, [rbx+0x28]       ; obj+0x28 = 寿命
007AD2C9  0f2f7b2c          comiss xmm7, [rbx+0x2c]
007AD2CD  0f87b6000000      ja   0x7AD389              ; 0 > obj+0x2c → tick
007AD2D6  0f97c0            seta al                      ; al = 寿命 > 0
007AD2DB  0f85a8000000      jne  0x7AD389              ; 寿命 > 0 → tick
...
007AD389  410f28d0          movaps xmm2, xmm8
007AD38D  488bd5            mov  rdx, rbp                ; ★ mgr
007AD390  488bcb            mov  rcx, rbx                ; ★ obj
007AD393  e808f9ffff        call 0x7ACCA0
```

**闸门是可满足的**：所有 `AddStateObject*` 的 ctor 都用 **dword** 写 `obj+0x1c`:

```asm
; DamageRate ctor 0x79F450
0079F4C1  897818            mov  dword [rax+0x18], edi      ; +0x18 = 0
0079F4C4  c7401c01000000    mov  dword [rax+0x1c], 1        ; ★ +0x1c=1, +0x1d=0, +0x1e=0, +0x1f=0
; Armor ctor 0x79E9E0 同样：0x79EA4C  c7401c01000000
```

> 注：`obj+0x1c` 的"清 0"出现在所有析构/摘除路径（`0x7A25C0`、`0x7A2C80`、`0x7AD260`、`0x7A2EF0`、`add()` 的
> 失败路径 0x7A28F0 等），**没有任何地方写 1，除了各类 ctor 的这条 dword 写**。所以"对象是否活着"完全由 ctor 决定。

### 5.4 结论

**`add()` 之后不需要任何额外注册调用**（confirmed）：
`add()` → 树；下一帧 `0x7AC6E0` → `0x79E320` 复位 → `0x7AD260` 遍历树 → `0x7ACCA0` → 首帧 slot 5、此后每帧 slot 6。
`0x7A2C80` 只是**另一条**注册路径（把对象放进状态 ID 链表供 `find_by_state_id 0x7A3C40`），
**不是 `add()` 的前置/后续**；两者是 `key == -1` 与 `key != -1` 的二选一（`0x71F19C`）。

> **重要修正（原报告 §2.2 / §5.1 的两处）**
> 1. **`mgr+0x24`/`+0x28` 确实被消费者读**：`0x72688C`（伤害结算，~6297 条指令）
>    `0x7273DE: mulss xmm14, [rsi+0x24]` 与 `0x7278C7: mulss xmm7, [rsi+0x28]`，
>    其中 `rsi = [r13+0x10b0]`（`0x726FA2`/`0x727845` 设定；`r13 = [actor+0x240]`，`0x726D60`）。
>    ⇒ 原报告 §1.5 的"静态上看不到任何代码读 `mgr+0x24/+0x28`"**被推翻**：承伤累加器确有消费者。
>    **confirmed**。
> 2. **`mgr+8` 的 bit 11（0x800）= Armor 效果**，消费者 `0x73CF00`：

```asm
0073CF00  4053              push rbx
0073CF06  488b4150          mov  rax, [rcx+0x50]      ; char = [actor+0x50]
0073CF0D  488b9040020000    mov  rdx, [rax+0x240]     ; param
0073CF14  8b82b8100000      mov  eax, [rdx+0x10b8]    ; ★ = mgr+8（param+0x10B0+8）
0073CF1A  c1e80b            shr  eax, 0xb             ; ★ bit 11
0073CF1D  a801              test al, 1
0073CF1F  7553              jne  0x73CF74             ; ★→ 返回 1（不受身）
... 其它条件 ...
0073CF74  b001              mov  al, 1
```
    调用者 `0x726690`、`0x736CD7`（受击反应链）。Armor 的 slot6 正是
    `007A8300  814a08 00080000  or dword [rdx+8], 0x800`（`rdx` = tick 传入的 mgr）。**两边位数/基址完全对得上。confirmed。**

---

## 6. Q5 — MOD 用的两个类的 vtable / slot 5,6,7 / key 与 id 的关系

### 6.1 `AddStateObjectDamageRate`（id `0x1E`，ctor `0x79F450`）

RTTI：`.?AVAddStateObjectDamageRate@Character@@`，vftable **`0x11A8280`**（`_work\nioh1.mem.rtti.json`）。

| slot | vt+ | RVA | 已验证的作用 |
|---|---|---|---|
| 0 | +0x00 | `0x79C6B0` | 析构 |
| 1 | +0x08 | `0x7AC550` | `movss [rcx+0x50], xmm1; ret` = 写参数 |
| 2 | +0x10 | `0x7A3DC0` | 读参数 |
| 3 | +0x18 | `0x7A4B40` | `al = (1.0f > [rcx+0x50])` = "这是减免吗" |
| 4 | +0x20 | `0x7A5720` | `al = (rdx && [rdx+0x10] == [rcx+0x10])` = 同 state id |
| **5** | **+0x28** | **`0x7A6190`** | `→ 0x7AC190(obj,mgr)`（生成特效 + `or dword [mgr+8],2`）再尾跳 `0x7AC2B0` |
| **6** | **+0x30** | **`0x7A8C30`** | **apply**（见下） |
| 7 | +0x38 | `0x7A7980` | `ret 0`（空） |
| 8 | +0x40 | `0x7A8110` | `ret 0`（空） |

```asm
007A8C30  f30f104224        movss xmm0, [rdx+0x24]
007A8C35  f30f594150        mulss xmm0, [rcx+0x50]
007A8C3A  f30f104a28        movss xmm1, [rdx+0x28]
007A8C3F  f30f114224        movss [rdx+0x24], xmm0      ; mgr+0x24 *= param
007A8C44  f30f594950        mulss xmm1, [rcx+0x50]
007A8C49  f30f114a28        movss [rdx+0x28], xmm1      ; mgr+0x28 *= param
007A8C4E  c3                ret
```
ctor 关键行（原报告 §6.1 的"a=寿命、b=参数"结论**确认**）：
```asm
0079F49F  f30f117028    movss [rax+0x28], xmm6     ; a → +0x28 寿命
0079F4A4  f30f11702c    movss [rax+0x2c], xmm6     ; a → +0x2c 下限（tick 的存活判据之一）
0079F4B9  48c740101e000000  mov qword [rax+0x10], 0x1e   ; ★ state id
0079F4EC  f30f117850    movss [rax+0x50], xmm7     ; b → +0x50 倍率
```

### 6.2 `AddStateObjectArmor`（id `0x33`，ctor `0x79E9E0`）

RTTI：`.?AVAddStateObjectArmor@Character@@`，vftable **`0x11A9130`**。

| slot | vt+ | RVA | 作用 |
|---|---|---|---|
| 0 | +0x00 | `0x79C3B0` | 析构 |
| 1 | +0x08 | `0x7AC530` | setter |
| 2 | +0x10 | `0x7A3DA0` | getter |
| 3 | +0x18 | `0x7A4A50` | 谓词 |
| 4 | +0x20 | `0x7A5720` | 同 state id |
| **5** | **+0x28** | **`0x7A59F0`** | 同 DamageRate：`→0x7AC190` 再尾跳 `0x7AC2B0`（只生成特效，无算术） |
| **6** | **+0x30** | **`0x7A8300`** | **apply = `or dword [rdx+8], 0x800; ret`**（`rdx` = mgr） ⇒ 置 `mgr+8` bit 11 |
| 7 | +0x38 | `0x7A7980` | `ret 0` |
| 8 | +0x40 | `0x7A8110` | `ret 0` |

ctor `0x79E9E0`：
```asm
0079E9F2  0f28f1            movaps xmm6, xmm1        ; ★ 只吃 1 个 float
0079EA32  f30f117028        movss [rax+0x28], xmm6   ; 寿命
0079EA37  f30f11702c        movss [rax+0x2c], xmm6
0079EA41  48c7401033000000  mov qword [rax+0x10], 0x33   ; ★ state id = 0x33
0079EA4C  c7401c01000000    mov dword [rax+0x1c], 1
```
**要点**：Armor ctor **只接收 1 个 float（xmm1 = 寿命）**，`xmm2` 被完全忽略，而且**从不写 `obj+0x50`**
（该类的 apply 也不读 `+0x50`）。所以 MOD 的 `buff_rate(2) = 1.5f`（写进 `xmm2`）**是一个无效实参**——
无害，但注释里"the engine's own value (1.5)"的说法没有对应的语义。（confirmed）

### 6.3 `add()` 路径里有没有"key 必须等于 state id"的依赖？—— **没有**

`add()` 全文只比较**双方 state id**（`0x7A299E cmp eax,r8d`），key 只用于树内排序/查同 key（`0x7A2966 cmp [rax+0x20], ebp`）。
加上 §2.2 的实证（key 0x31/0x32/0x33 ↔ id 0x4A/0x4B/0x4C；key 0x50 ↔ id 0x33；key 1 ↔ id 0x1E；key 0x45 ↔ id 0x1E）：
**key 与 state id 是两个独立的量，`add()` 不要求它们相等。**（confirmed）

**但 key 的"唯一性"是硬约束**：同 key ⇒ **破坏性替换**（析构一方 + 摘除节点 + 重新插入）。
MOD 目前 `key = state_id`，这在**语义上没必要**，而且恰好在 Armor（0x33）上与引擎撞键。

---

## 7. 那么"节点在树里却没效果"最可能是什么？

前面已经把"参数形状"和"缺少注册调用"两条**静态排除**了。按静态证据强弱排序：

### 7.1 ★最可能（probable）：节点被挂进了**消费者不读的那个 manager 实例**

- 消费者读的基址是**它们自己算出来的**，不是从容器里找的：
  - Armor：`0x73CF00` → `[actor+0x50]` → `+0x240` → `+0x10B0`，再读 `+8`。
  - DamageRate：`0x72688C` → `[r13+0x240]` → `+0x10B0`，再读 `+0x24`/`+0x28`
    （`r13` 在 `0x726D60` 由 `[r15+0x100]` 选出的 actor 派生；`r15` 是战斗管理器）。
  - 引擎自己的状态代码也一律这么取：`0x714CE0`/`0x749640` 都是
    `mov rax,[rcx+0x50] / mov rbx,[rax+0x240] / lea rXX,[rbx+0x10b0]`。
- MOD 取的是**全局** `[base+0x18A0490]` → `+0x240` → `+0x10B0`。
  两者只有在"`[base+0x18A0490]` 恰好就是那次命中里被选中的那个 character"时才相同。
- **一个错的 manager 是完全静默的**：`add()` 照样返回 1（树是真的、插入合法）、
  presence DFS 照样能找到节点、没有任何报错 —— **与观测现象逐条吻合**。
- 可行的静态派生诊断（下一次实现时加一行只读日志即可判定）：
  `host = *(void**)((char*)mgr + 0x168)`（manager 自带的宿主角色，`add()` 0x7A28C0 用的就是它）
  与 `*(void**)((char*)param + 0xB98 ?: param+0x9D8)`（`0x7B2D40` 的统计块访问方式）
  以及 `[[base+0x18A0490]+0x50]` 一起打印，看是否同一个对象。
  置信度 **probable**（机制 confirmed；"MOD 取错了对象"这一步是推断）。

### 7.2 次可能（probable）：**同 key 破坏性替换** —— Armor 的 key `0x33` 被引擎占用

- `add()` 的第 1 个动作就是"按键查树；同 key 就析构一方、摘除节点"（§1）。
- 引擎在 `0x724777`（`0x7246CF`，ability 分支）用 **key = 0x33** 挂 **id = 0x4C** 的对象；
  同一函数另两支用 0x32/0x31。**这条路径一旦在 MOD 的 Armor 生效期间运行，
  `add()` 会把 MOD 的 Armor 节点摘除、对象析构、再换成 id 0x4C 的对象** —— Armor 静默失效。
- DamageRate 用的 key `0x1E` 在**可静态定值**的 key 里不存在，但**记录驱动**的 key 来自游戏数据
  （`word[rec+0xa]−1`、`[rec+0x40]`），静态上无法排除 `0x1E`。
- **实现建议**：`key = state_id` 没有任何语义收益，却引入撞键风险。
  更稳的做法是每次挂之前先 `find_by_key(mgr, key)`（`0x7A38B0`，只读），确认返回的不是"别人的对象"；
  或者干脆用一个静态上确认未占用的 key（`0x1E` 目前安全）。置信度 **probable**。

### 7.3 其他静态可见的"会让样本外节点蒸发/不被处理"的机制（possible）

| 机制 | 证据 | 后果 |
|---|---|---|
| manager 被"清空" | `0x7AC6E0` 0x7AC74F：`cmp byte [rsi+0x1bc], al; je ...; call 0x7A2EF0` | `0x7A2EF0` 遍历 `[mgr+0x170]` 树 + `[mgr+0x188]` 链表**析构全部对象**；MOD 的节点会消失（MOD 只在刚 add 完检查一次，之后不再确认） |
| 整帧跳过 | `0x7AC742: call 0xA9FED0; test eax,eax; jne 0x7ACC5F` | 该帧不做任何状态 tick（暂停/非战斗态） |
| 效果是"帧内瞬时量" | `0x79E320` 每帧把 `mgr+8` 清零、`mgr+0xc..+0x64` 复位为 1.0 | 效果只在"本帧 tick 之后、消费者之前"有效；**这也解释了为什么"节点在树里"与"效果可见"是两件事** |
| 跨线程 | `add()` 从 MOD 线程调用，容器无锁（未见任何 lock/临界区）；`0x7AC6E0` 在游戏线程遍历同一棵树 | 并发插入可能让遍历漏掉/错乱（不必然崩） |

### 7.4 明确**不成立**的候选

- ❌ "r9d / stack0 传错"：MOD 的 `-1 / 0` 与引擎 78%/90% 一致，且 `add()` 只把它们写进
  `obj+0x24` / `obj+0x22`，tick 完全不读 `obj+0x24`；`obj+0x22=0` 正是 0x7A3AF0 计数想要的（§3/§4）。
- ❌ "`add()` 不会调 onActivate，所以必须自己调 slot 5"：`0x7ACCA0` 会懒调（§5.3），
  唯一闸门 `[obj+0x1c]` 由 ctor 置 1。
- ❌ "必须调 `0x7A2C80` 注册进 `[mgr+0x198]` 才会生效"：两个容器**都被 tick**（§5.3），
  且引擎在 `key==-1` 时**只用** `0x7A2C80`、有 key 时**只用** `add()`（§5.2）。
- ❌ "key 必须等于 state id"：§6.3。

---

## 8. 附：本报告用到的关键 RVA

| RVA | 语义 |
|---|---|
| `0x7A2890` | `add(mgr, key, obj, r9d, stack0)`；只做 keyed 树插入；同 key 破坏性替换；返回 1/0 |
| `0x7A2AD0` | `add(mgr,key,obj,-1,0)` 包装（写死 r9d=-1、stack0=0）+ 成功后的额外通知 `0x744F70` |
| `0x7A2C80` | `reg(mgr, obj, search)` —— 注册进 `[mgr+0x198]` 状态 ID 链表（先按 slot4 查重） |
| `0x7AAB30` | `[mgr+0x198]` 链表追加（`[mgr+0x1a0]++`） |
| `0x7A25C0` | 对象析构 + free（不含摘链） |
| `0x79BD50` | state manager ctor（含 `+0xc..+0x120` 全 1.0f、四容器初始化） |
| **`0x79E320`** | **每帧复位器：`mgr+8=0`，`mgr+0xc..+0x64 = 1.0f`**；调用者含 `0x7AC6E0` |
| `0x7AC6E0` | 每帧 manager 更新（调用者 **`0x76E579`**）：复位 → tick 树 `+0x170` / `+0x180` / `+0x188` / `+0x198` / `0x7ACDB0` → 发布 |
| `0x7AD260` | 遍历 `[mgr+0x170]` 树并 `0x7ACCA0(obj, mgr, dt)` |
| `0x7ACCA0` | 通用 tick：`[obj+0x1c]` 闸门 → 懒 slot5 → 每帧 slot6 |
| `0x7A3AF0` | 树内"活跃减伤状态"计数（要求 slot3!=0、`[obj+0x1c]!=0`、`[obj+0x22]==0`）；调用者 `0x749640` |
| `0x7A3C40` | `find_by_state_id(mgr, id)`（遍历 `[mgr+0x198]`，先看 `[mgr+0x1a0]`） |
| `0x7A38B0` | `find_by_key(mgr, key)` |
| `0x79F450` | DamageRate ctor（寿命, 倍率）→ vtable `0x11A8280`，id 0x1E |
| `0x79E9E0` | Armor ctor（只吃 1 个 float = 寿命）→ vtable `0x11A9130`，id 0x33 |
| `0x7A8C30` | DamageRate slot6：`[mgr+0x24] *= p; [mgr+0x28] *= p` |
| `0x7A8300` | Armor slot6：`or dword [mgr+8], 0x800` |
| `0x73CF00` | Armor 效果消费者：读 `[param+0x10B8] >> 11 & 1` → "不受身" |
| `0x72688C` | 伤害结算（~6297 条指令）：`0x7273DE mulss xmm14,[mgr+0x24]`、`0x7278C7 mulss xmm7,[mgr+0x28]` |
| `0x714CE0` / `0x749640` / `0xA83F97` | 引擎自己挂状态的主函数（Armor ctor + add 同时出现） |
| `0x724777` | **用 key 0x33 挂 id 0x4C 对象**（撞键来源） |

---

## 9. 最终答案

### 9.1 MOD 应该传的实参形状（有证据）

```c
/* 与引擎 38/49 个直接调用点逐位一致；无需修改 */
void *obj = ctor(mgr, life_seconds, magnitude);   /* DamageRate: xmm1=寿命, xmm2=倍率
                                                     Armor:      xmm1=寿命, xmm2 被忽略 */
unsigned char ok = add(mgr, key, obj, -1, 0);
/*                    ^^^  ^^^  ^^^  ^^  ^
   第1参 树/管理器   ─┘    │    │   │   └ 第5参 stack0 = 0  → 非 0 会写 obj+0x22=1，
                            │    │   │                           让 0x7A3AF0 的减伤计数忽略本对象
                            │    │   └ 第3参 engine ctor 返回的 obj
                            │    └ 第2参 key：树内唯一；与 state id 无关；
                            │              同 key 会**破坏性替换**（析构一方）
                            └ 第4参 r9d = -1 → 写 obj+0x24（ctor 默认也是 -1；tick 不读）
*/
```
- **`-1 / 0` 必须保持**（= 引擎默认，`0x7A2AD0` 包装函数写死的就是这两个值）。**confirmed**
- `key` 可以自由选，但**建议不要用 `state_id`**：Armor 的 `0x33` 与 `0x724777` 撞键（§2.2/§7.2）。
- Armor 的 `magnitude`（MOD 现在传 1.5f）**是无效实参**，该类的效果完全由 slot6 置 `mgr+8` bit 11 决定。**confirmed**

### 9.2 是否需要额外的注册调用

**不需要。**（confirmed）
`add()` 之后，下一帧 `0x7AC6E0` 的 `0x79E320` 复位 → `0x7AD260` 遍历 keyed 树 → `0x7ACCA0` 会
**首帧调 slot 5、此后每帧调 slot 6**；闸门 `[obj+0x1c]` 由所有类的 ctor 置 1。
`0x7A2C80`（写入 `[mgr+0x198]`/`[mgr+0x1a0]`）**不是** `add()` 的前置或后续，而是引擎在
`key == -1` 时使用的**另一条互斥注册路径**（`0x71F19C`），并且那个容器同样每帧被 tick。

### 9.3 样本外 `add()` 节点变哑的**单一最可能原因**

> **它被挂进了一个"效果消费者不读"的 state manager 实例 —— 也就是 MOD 的 `mgr` 与本次命中
> 里游戏自己解析出来的那个 character 不是同一个对象。**
> 消费者（Armor `0x73CF00`、DamageRate `0x72688C`）和引擎全部状态代码都从
> `[actor+0x50] → +0x240 → +0x10B0` 现场推导 manager，而 MOD 用的是全局
> `[base+0x18A0490] → +0x240 → +0x10B0`。若两者不同，`add()` 依然返回 1、树依然合法、
> presence 检查依然"present"，但没有任何消费者会读那个 manager 的 `+0x24/+0x28/+8`
> —— **完全静默、与观测现象逐条吻合**。置信度 **probable**。

**次高（值得同时排掉）**：`key = state_id` 带来的**同 key 破坏性替换** —— Armor 的 `0x33` 与
`0x724777`（ability 分支，挂 id 0x4C）撞键，会把 MOD 的 Armor 节点连对象一起摘掉（§7.2）。

**一条只读、可立刻判定的诊断**（不需要实机结论，只要加一行日志）：
在安装成功后打印 `*(void**)((char*)mgr + 0x168)`（manager 自带的宿主角色）、
`*(void**)((char*)param + 0xB98)`（统计块，`0x7B2D40` 用）、`[[base+0x18A0490]+0x50]`，
看它们和"当前被命中的角色"是否同一个对象；三者一致 ⇒ 排除 9.3，转去查 7.2 的撞键与 7.3 的清空/暂停路径。
