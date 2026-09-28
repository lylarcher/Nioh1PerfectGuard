# 仁王1 精防 MOD — 逆向工程笔记（阶段 0/1 成果）

> 目标版本：`nioh.exe` 1.24.8.0（Steam appid 485510）
> 磁盘 SHA256：`0C3508C6B4D0696D84423949DF9FACCB3F9C6D93833854E1E17A78D66DEFC389`
> 磁盘文件大小：26,943,456 字节，ImageBase `0x140000000`

---

## 1. 最重要的结论：磁盘代码是加密的，必须从内存取证

| 指标 | 磁盘 `nioh.exe` | 内存 dump |
| --- | --- | --- |
| `.text` 熵 | **8.000**（256 个字节值完全均匀） | **6.441** |
| `E8 rel32` 目标落在 `.text` 内 | **0.41%** | **88.59%**（312,882 条） |
| 与磁盘 `.text` 逐字节匹配率 | — | **0.39%** |
| `.pdata` / `.reloc` / `.rsrc` / `_RDATA` / `.bind` 与磁盘匹配率 | — | **100.00%** |

原因：Steam DRM（SteamStub）。特征就是「`.bind` 段存放装载 stub（入口点 `0x303B310` 在这里）+ `.text` 加密」。
**仁王1 没有 Denuvo**。Steam DRM 不是反作弊、不做完整性校验，启动时把 `.text` 解密到内存。

→ 所有 AOB 签名必须针对**运行态内存**，不能针对磁盘文件。

---

## 2. 启动游戏并抓取内存镜像（可复现配方）

关键点：Steam 启动的是 **`nioh_launcher.exe`**，由它再拉起 `nioh.exe`。
直接双击 `nioh.exe` 会因为缺少 appid 上下文导致 `SteamAPI_Init` 失败，**28 秒后自行退出**。

正确的做法是复制 Steam 传给启动器的环境变量：

```powershell
$env:SteamAppId='485510'; $env:SteamGameId='485510'
Start-Process -FilePath '<游戏目录>\nioh.exe' `
  -ArgumentList '--disable-d3d-debug' `
  -WorkingDirectory '<游戏目录>'
```

然后用 `.pdata`/工具抓取（见下），抓完关闭 `nioh.exe`。**不需要往游戏目录放任何文件。**

一键脚本：`tools\dump_nioh1.ps1`

---

## 3. 工具链（全部为自研 Python，无需 IDA/Ghidra/编译器）

| 工具 | 作用 |
| --- | --- |
| `tools\dump_module.py` | 用 ctypes `ReadProcessMemory` 从运行进程逐页抓取模块镜像，重建成「文件偏移 = RVA」的 PE。`--wait-decrypted` 会轮询 `.text` 熵直到 DRM 解密完成。 |
| `tools\compare_pe.py` | 内存 dump 与磁盘原版逐段比对（验证重建正确性） |
| `tools\analyze.py` | `stats` 段诊断 / `funcs` 从 `.pdata` 导出函数边界 / `rtti` 重建「类名→vftable→方法」 / `namerefs` 找引用某字符串的数据结构 / `nodes` 枚举 KTGL 脚本节点注册表 |
| `tools\xref.py` | `build` 全量反汇编建交叉引用库（389 万条指令、61,465 个调用目标、138,124 个数据引用，约 42 秒）；`callers` / `whorefs` / `dumpfn` / `strings` / `whorefs-str` 查询 |
| `tools\dump_pe.py` | 底层 PE 解析 |

已产出的数据（`_work\`）：
- `nioh1.mem.exe`（50,782,208 字节）—— **解密后的完整镜像**，后续所有分析的基础
- `nioh1.xref.pkl`（6.2 MB）—— 交叉引用库
- `nioh1.mem.rtti.json` —— 2,344 个类 / 2,394 个 vftable / 69,025 个方法
- `nioh1.mem.funcs.txt` —— 72,232 个函数边界
- `nioh1.nodes.tsv` —— 1,462 个脚本节点（名称 + 处理函数 RVA）

---

## 4. 已验证的锚点清单

### 4.1 玩家槽位访问器 —— RVA `0x755DC0` ✅

```asm
00755DC0  cmp ecx, 3
00755DC3  ja  0x755DD8          ; 越界返回 0
00755DC5  movsxd rax, ecx
00755DC8  lea rcx, [rip + 0x114A6C1]   ; -> 0x18A0490  全局玩家槽位表
00755DCF  lea rax, [rax + rax*2]       ; idx * 3
00755DD3  mov rax, [rcx + rax*8]       ; stride = 24 字节
00755DD7  ret
00755DD8  xor eax, eax
00755DDA  ret
```

- **语义**：`get_player_object(int idx)`，`idx ∈ 0..3`，`ecx=0` 即 1P。
- **全局表**：`0x18A0490`（4 项 × 24 字节）。
- 与仁王2 MOD 的 `player slot accessor` 锚点对应，是整条链的入口。

### 4.2 玩家精力组件 —— `[player + 0x240]`，字段在 `+0x40` ✅

来自 `Player::RecoverStamina` 节点处理函数（RVA `0x8A5B50`）：

```asm
008A5B69  mov  rdx, rcx
008A5B6C  lea  rcx, [rsp+0x20]
008A5B71  call 0xAB0AF4E0        ; 取节点输入
008A5B76  xor  ecx, ecx
008A5B78  call 0x755DC0          ; <<< 玩家访问器，idx=0
008A5B7D  test rax, rax
008A5B80  je   skip
008A5B82  mov  rcx, [rax + 0x240]   ; <<< 玩家对象 +0x240 = 精力组件
008A5B89  test rcx, rcx
008A5B8C  je   skip
008A5B8E  add  rcx, 0x40            ; <<< 组件内偏移
008A5B92  call 0x7B4C20             ; <<< 精力操作实现
```

精力操作原语（`0x7B4C20` 起，内联小函数，无 `.pdata` 记录）：

```asm
; 0x7B4C20: 若 [rcx] < [rcx+4] 则 [rcx] = [rcx+4]，返回是否变化
007B4C20  movss xmm1, [rcx+4]      ; 上限？
007B4C25  movss xmm0, [rcx]        ; 当前？
007B4C29  comiss xmm0, xmm1
007B4C2C  jb   0x7B4C31
007B4C2E  xor  al, al
007B4C30  ret
007B4C31  movss [rcx], xmm1
007B4C35  mov  al, 1
007B4C37  ret

; 0x7B4C40: [rcx+0xC] -= xmm1，下限截断到 0
007B4C40  movss xmm2, [rcx+0xC]
007B4C45  subss xmm2, xmm1
007B4C49  xorps xmm1, xmm1
007B4C4C  movaps xmm0, xmm2
007B4C4F  subss xmm0, xmm1
007B4C53  comiss xmm0, xmm1
007B4C56  jb    0x7B4C5E
007B4C58  movss [rcx+0xC], xmm2
007B4C5D  ret
007B4C5E  movss [rcx+0xC], xmm1     ; 截断到 0
007B4C63  ret
```

- `component + 0x40 + 0x00` = 当前精力（float）
- `component + 0x40 + 0x04` = 精力上限（float，待确认）
- `component + 0x40 + 0x0C` = 另一个可被扣减的 float 字段（待确认，可能是精力伤害累积）

### 4.3 KTGL 脚本节点注册表 —— 16 字节 `{name, handler}` ✅

结构（在 `.rdata` 中连续存放）：

```
struct NodeEntry {            // 16 字节
    const char* name;         // +0  VA，指向 ".rdata" 中的节点名字符串
    void*       handler;      // +8  VA，指向 ".text" 中的处理函数
};
```

共 **3 张表、1,462 个节点**。主要两张：

- `0x119E400` —— `Refer::*` / `Input::*` / `Measure::*` / `Math::*` / `Rand::*` / `Operate::*` 表
- `0x11F0AA0` —— `Player::*` / `UI::*` / `Tutorial::*` 等表
- `0x120CF90` —— 另一组

**防御/精力相关节点（可直接用作符号锚点）：**

| 节点名 | 处理函数 RVA |
| --- | --- |
| `Refer::IsAttackGuard` | `0x6F5A30` |
| `Refer::IsAttackHitAfter` | `0x6F5AC0` |
| `Refer::DefenseRange` | `0x6F51F0` |
| `Refer::Stamina` | `0x6F65D0` |
| `Refer::StaminaRate` | `0x6F6650` |
| `Refer::Hp` / `MaxHp` / `HpRate` | `0x6F5930` / `0x6F61C0` / `0x6F59B0` |
| `Refer::ActionId` / `PrevActionId` | `0x6F4EF0` / `0x6F6450` |
| `Refer::IsDead` | `0x6F5BF0` |
| `Refer::IsInTokoyoArea`（常世） | `0x6F5ED0` |
| `Refer::HighStanceRate` / `Mid` / `Low` | `0x6F58A0` / `0x6F6240` / `0x6F6130` |
| `Player::RecoverStamina` | `0x8A5B50` |
| `Input::Button` | `0x6F2450` |

### 4.4 角色属性访问器 —— 全局表 `0x1871740` ✅

`Refer::Stamina` 与 `Refer::IsAttackGuard` 结构完全同构，都走同一张全局表：

```asm
; Refer::IsAttackGuard (0x6F5A30)
007F...  mov  rcx, [rip + ...]   ; -> 0x1871740   全局属性表
         mov  edx, eax           ; 属性索引（由节点参数解析得到）
         call 0x9BD140           ; 取「是否被格挡」属性 → bool

; Refer::Stamina (0x6F65D0)
         mov  rcx, [rip + ...]   ; -> 0x1871740   同一张表
         mov  edx, eax
         call 0x9BC7C0           ; 取「精力」属性 → int
```

- **`0x1871740` = 角色属性访问器表**
- `0x9BC7C0` = get_stamina，`0x9BD140` = get_attack_guard（还有更多同族 getter）
- 这是「脚本属性 → C++ 实现」的桥，也是精防判定最可能的落点。

### 4.5 角色状态节点类（RTTI）✅

| 类 | vftable RVA | 类专属方法 |
| --- | --- | --- |
| `AddStateObjectAbsoluteGuard::Character` | `0x11A9450` | `0x79C2F0` … |
| `AddStateObjectStaminaDamageRate::Character` | `0x11A82D0` | `0x79D040`, `0x7A71B0`, `0x7A9ED0` |
| `AddStateObjectStaminaRecover::Character` | `0x11A8410` | `0x79D070`, `0x7A71E0`, `0x7A9EF0` |
| `AddStateObjectStaminaRecoverDown::Character` | `0x11A8460` | `0x79D0A0`, `0x7A7210`, `0x7A9F20` |
| `AddStateObjectUseStaminaRate::Character` | `0x11A91D0` | `0x79D190`, `0x7A7370`, `0x7AA960` |
| `AddStateObjectUseStaminaRateAttack/Dodge` | `0x11A9220` / `0x11A9270` | 同上模式 |
| `AddStateObjectDefense::Character` | `0x11A8190` | `0x79C7A0` … |
| `AddStateGuardPenetration::Character` | `0x11A97C0` | `0x79C2C0` … |

这些是「脚本节点类」，方法第 1/6/7 项是类专属的 get/set/apply 实现。

---

## 5. 尚未拿到的锚点（下一步目标）

| 编号 | 目标 | 状态 |
| --- | --- | --- |
| A | **玩家格挡输入边沿**（新按下防御） | ❌ 未定位。候选入口：`Input::Button` (`0x6F2450`)、`ActionInputButton` 相关类 |
| A0 | 引擎原生「及时格挡」状态位（若能复用可大幅简化） | ❌ 未验证是否存在 |
| C | **成功格挡事件 + 攻击来源** | ❌ 未定位。候选：`0x9BD140`（get_attack_guard）的**调用者链**、`Refer::IsAttackHitAfter` (`0x6F5AC0`) |
| B | 玩家精力**扣减**路径（格挡耗精） | 🟡 已定位精力字段与操作原语（4.2），但尚未找到「格挡时扣精」的调用点 |
| D | 敌人精力 / HP 结算 | ❌ 未定位。可复用 4.2 的字段布局推敌人同类组件 |
| E | 后摇取消 | ❌ 未定位 |
| F | 受击反应 ID（轻/重冲击） | ❌ 未定位，需重新标定（仁王2 的 18/4/1 不适用） |
| G | effect factory（视觉特效） | ⏸ 已决定砍掉，只保留音效 |

### 下一步的具体打法（建议顺序）

1. **建调用者图**：`0x9BD140`（get_attack_guard）没有直接调用者，因为节点处理函数是通过注册表指针间接调用的。突破口是找**谁读取了注册表项 `0x119E7E8`**（`Refer::IsAttackGuard` 的 handler 槽），或找 `0x1871740` 全局属性表的所有数据引用者。
2. **反推角色属性表结构**：`0x1871740` 是一个对象；找到它的构造函数/初始化处，就能拿到**属性索引枚举**——直接得到「攻击是否被格挡」「精力」「HP」等属性的编号，从而定位属性 getter 的调用方。
3. **格挡扣精**：在精力字段写入点上下断/扫描——找所有写 `[component+0x40]` 或调用 `0x7B4C40` 的函数，其中被「受击/格挡」路径调用的那个就是锚点 B。
4. **输入边沿**：从 `Input::Button` (`0x6F2450`) 顺藤摸瓜找到玩家动作输入表；或直接扫描读取防御键位状态的代码。
5. **及时格挡状态位**：在属性表枚举里搜索名字含 `Guard`/`Just`/`Timely`/`Parry` 的属性。

---

## 4.6 【本轮核心成果】格挡判定 + 格挡扣精 已定位（锚点 B / C）✅

### 4.6.1 调用链（全部由工具打印的 `; rva` 注记核对过，非手算）

```
Refer::IsAttackGuard 节点 (0x6F5A30)
   └─> 0x6ED140  (转发)
         └─> 0x6E58E0  (按 edx 取角色子对象，分支 0/1/2/0x15…)
               └─> 读 [子对象+0xB8] 侵入式链表
                     每个节点 +0x18 = 对象 X
                     若 [[X+0x90]+0xC8] != 0  ⇒  "本次攻击被格挡" = true
```

### 4.6.2 谁把这个标志位置 1 —— 全二进制只有 4 处

用新加的 `xref.py ops --disp 0xC8 --imm 1 --size 1` 扫描 18 MB 代码，
写 `byte [reg+0xC8] = 1` 的指令**只有 4 条、分布在 3 个函数**：

| 函数 | 指令位置 |
| --- | --- |
| **`0x749640`** | `0x74DD09`、`0x74DD92` |
| `0xD68150` | `+0x39` |
| `0x10242F0` | `+0x1BBB` |

其中 **`0x749640` 正是上一轮独立推断出的「首选锚点候选」**
（同时满足：调用玩家访问器 `0x755DC0`、使用位移 `+0x240`、调用精力扣减原语 `0x7B4C40`）。
**两条完全独立的证据线收敛到同一个函数** —— 这是本轮最强的交叉验证。

`0x749640` 范围 `0x749640 - 0x74FE48`（26,632 字节），**只有 1 个调用者：`0x6FE5B0`**（命中结算分发器）。

### 4.6.3 格挡标志位的完整写入上下文（两处同构）

```asm
0074DD04  test rcx, rcx
0074DD07  je   skip
0074DD09  mov  byte ptr [rcx + 0xc8], 1     ; ★ 格挡标志 = 1
```

前置：
```asm
0074DCF4  mov  rax, [rsi + 0x40]      ; 角色 +0x40 → 子对象
0074DCFD  mov  rcx, [rax + 0x90]      ; +0x90 → 持有标志的对象
```

与 `IsAttackGuard` 的读取路径 `[[X+0x90]+0xC8]` **逐字节对应**。
第二处（`0x74DD92`）走另一条判定：`call 0x7AB1D10` 或 `call 0x7A0E290` 返回真。

### 4.6.4 ★ 格挡扣精的确切位置

同一函数内、紧跟标志位之后：

```asm
0074DE0E  mov  rcx, [rsi + 0x100]     ; 选对象（rsi 或 rdi）
0074DE24  mov  rcx, [rcx + 0x240]     ; ★ 资源管理器
0074DE2B  add  rcx, 0xBA8             ; ★ +0xBA8 = 资源表
0074DE32  mov  edx, 7                 ; 索引 7
0074DE37  call 0x7B21F0               ; 取表项 -> rax
0074DE3C  cmp  dword ptr [rax], 1     ; 该资源启用？
0074DE3F  jne  skip
0074DE41  movss xmm1, dword ptr [rax + 0x10]   ; ★★ 消耗量
0074DE46  mulss xmm1, dword ptr [rip + ...]    ; -> 0x1588828 = 0.2f
0074DE4E  mov  rcx, rax
0074DE51  call 0x7B4C40               ; ★★★ 扣减原语
```

### 4.6.5 资源表结构（`0x7B21F0` 完全可读）

```asm
007B21F0  movsxd rax, edx
007B21F3  lea    rdx, [rax + rax*4]
007B21F7  shl    rdx, 4              ; rdx = index * 0x50
007B21FB  lea    rax, [rcx + 8]
007B21FF  add    rax, rdx            ; entry = table + 8 + index*0x50
007B2202  ret
```

- 表基地址 = `[[char + 0x240] + 0xBA8]`
- 表项 **0x50 字节**，`entry = table + 8 + index*0x50`
- 表项字段：`+0x00` 启用/类型（与 1 比较）、**`+0x0C` 当前值（被扣减）**、`+0x10` 消耗量/速率

### 4.6.6 系数常量

| RVA | 值 | 含义 |
| --- | --- | --- |
| `0x1588828` | **0.2f** | 格挡消耗 = 攻击精力伤害 × 20%（符合仁王设计） |
| `0x1588938` | 1.0f | |
| `0x1588B98` | FLT_MAX | |

### 4.6.7 同函数内索引 6 的处理（另一条资源路径）

```asm
0074DDD2  mov  rcx, [rcx + 0x240]
0074DDD9  add  rcx, 0xBA8
0074DDE0  mov  edx, 6
0074DDE5  call 0x7B21F0        ; 索引 6
0074DDED  mov  rcx, rax
0074DDF0  call 0x7B4310
0074DDF5  test al, al ; je
0074DDFC  call 0x7B5930
0074DE01  mov  r15b, 1
```

索引 6 与 7 是两种不同资源（6 走 `0x7B4310`/`0x7B5930`，7 走扣减原语）。

### 4.6.8 由此得到的 MOD 实现方案（全部依赖已验证能力）

| 功能 | 实现方式 | 位置 |
| --- | --- | --- |
| **精防/格挡事件检测** | 断点 | `0x74DD09`、`0x74DD92`（标志位置 1） |
| **格挡免耗精** | 上下文改写 `xmm1 = 0`（或按配置缩放） | `0x74DE51` |
| **格挡回精** | 断点处直接写 `[rcx+0xC] += amount` | `0x74DE51`（rcx = 表项） |
| **对敌削精 / HP** | 需要敌人侧同类资源表（同结构，`index` 不同） | 待定位 |
| **提前接续（取消后摇）** | 上下文改写 `skip=N` | 待定位 |

> **关键工程结论**：「格挡免耗精」就是在 `0x74DE51` 把 `xmm1` 置 0 —— 一个单点上下文改写，
> 而这正是上一轮已经本地验证过的 `ActionN=xmm1=0.0` 能力。整条链闭合。

### 4.6.9 自查：一次严重的计算错误（已修正，留作教训）

上一轮我曾把 `call 0x7ff7aa9bcc00` 之类的**绝对地址手算成 RVA**，
算错成 `0x9BCC00`（正确是 `0x6ECC00`），并据此得出了"这是被 ICF 合并的大分发器"的**错误结论**。
同类错误还出现在 `0x7B21F0`（曾误写为 `0x7821F0`）。

**已修正**：`xref.py dumpfn` 现在对每条 `call`/`jmp` 直接打印 `; rva 0xXXXX` 注记，
一律以工具输出为准，不再手算。上表中所有 RVA 均已用该注记核对。

---

## 4.7 MOD 本体 v0.1.0（已构建、已本地验证）✅

产物：`mod\Nioh1PerfectGuard.c` → `dist\Nioh1PerfectGuard\`（DLL 198 KB + INI + Sounds + README + SHA256SUMS）

### 架构：硬件断点 + 上下文改写（不改任何代码字节）

启动流程：

1. 读 `Nioh1PerfectGuard.ini`（中英双语注释、分组校验）
2. **逐条校验锚点处的期望指令字节**，不匹配则拒绝安装
   ```
   ANCHOR ok   guard_flag_1      rva=0x74DD09
   ANCHOR MISS guard_ki_cost     rva=0x74DE51 want=[E8 EA 6D 06 00] got=[...]
   ANCHOR 3/4 verified
   ```
3. 校验通过 ≥2 条才装 VEH + 硬件断点；否则 `NOT INSTALLED`（宁可不生效也不写坏东西）
4. 每 600ms 给新线程装断点（DR 是每线程状态），每 ~1s 轮询 INI 修改时间实现热更新

### 锚点表（含期望字节，用于启动自校验）

| 名称 | RVA | 期望字节 | 作用 |
| --- | --- | --- | --- |
| `guard_flag_1` | `0x74DD09` | `C6 81 C8 00 00 00 01` | 格挡标志位（分支一） |
| `guard_flag_2` | `0x74DD92` | `C6 81 C8 00 00 00 01` | 格挡标志位（分支二） |
| `guard_ki_cost` | `0x74DE51` | `E8 EA 6D 06 00` | 格挡扣精调用 |
| `ki_subtract_prim` | `0x7B4C40` | `F3 0F 10 51 0C F3` | 浮点扣减原语（备用） |

### 已实现的格挡收益

- **格挡耗精减免**：在 `0x74DE51` 按 `KiDamageReductionPercent` 缩放 `xmm1`。
  100% 时直接置 0（实测验证过的上下文改写路径）。
- **回精**：同一点上直接改资源表项 `[rcx+0x0C]`：
  - 模式 1 = 返还本次消耗（安全、有界）
  - 模式 2 = 加固定值
  - 注：仁王2 版的「最大精力/6」需要资源上限字段，尚未定位，故先用上述方式。
- **音效**：格挡时播放自定义 WAV。
- **日志**：`Ctrl+Shift+F10` 写累计统计。

### 音效实现（踩坑记录）

XAudio2 需要先 `CoInitializeEx`，否则 `CreateMasteringVoice` 返回
**`0x800401F0` (CO_E_NOTINITIALIZED)**。修正后又发现一个更要紧的设计问题：

> **不能在异常处理器（游戏线程）里调用 COM。** 改为 VEH 只递增一个请求计数，
> 所有 XAudio2 调用（建引擎、提交缓冲、启动播放）都由**自己的 worker 线程**完成 ——
> 该线程持有自己的 COM 单元，不干扰游戏的 apartment 模型。

其他实现细节：
- 用 `xaudio2_9.dll` 导出的 `XAudio2Create`，不依赖 import lib、不依赖 CLSID。
- COM 虚表顺序从 mingw 的 `xaudio2.h` **读出**而非凭记忆：
  IXAudio2 = 0-2 IUnknown / 5 CreateSourceVoice / 7 CreateMasteringVoice /
  8 StartEngine；IXAudio2Voice 基类 19 项（12 SetVolume、18 DestroyVoice）；
  IXAudio2SourceVoice 增量 19 Start / 20 Stop / 21 SubmitSourceBuffer。
- 该头文件是 XAudio2 **2.9** 布局（无 GetDeviceCount/Initialize），所以必须加载 `xaudio2_9.dll`。
- WAV 只接受 16 位整数 PCM、44100/48000 Hz、单/双声道，并逐条给出拒绝原因。

### 本地验证结果（不需要游戏）

| 测试 | 结果 |
| --- | --- |
| 正常运行 | `config_rc=0 enabled=1 window=250 reduction=100 recovery=1 gate=0 sound_enabled=1 vol=1.00 file=parry.wav \| wav=1 audio=1` |
| 非法值回退 | 5 项全被拒并逐条说明：`CONFIG reject KiDamageReductionPercent=150 (allowed 0..100); keeping 100`、`gameplay group rejected; previous settings kept`，`config_rc=1` |
| 合法自定义值 | `window=180 reduction=60 recovery=2 gate=1 sound_enabled=0`，`config_rc=0` |
| 音效链路 | WAV 184320 字节 / 44100 Hz / 2ch 加载成功 → `CoInitializeEx hr=0` → `SOUND ready (XAudio2 engine=xaudio2_9.dll)` → `audio=1` |

### 当前限制（如实记录）

- `RequireTimelyGuard=0`：**每次成功格挡**都触发收益。精确的「新按下防御」时间窗口
  需要锚点 A，尚未接线。
- 对敌削精 / HP（锚点 D）、提前接续 未实现。
- 视觉特效不在计划内，只保留音效。

---

## 4.8 锚点 A（格挡输入边沿）调研进展 —— 输入子系统已测绘 🟡

### 4.8.1 工具：导入表 → 调用点

新增 `tools/iat.py`：列出每个导入函数的 **IAT 槽 RVA**。因为 `call qword ptr [rip+X]`
在交叉引用库里就是"对 IAT 槽的数据引用"，所以拿到槽地址就能反查所有调用点。

| 导入 | IAT 槽 RVA | 跳转桩 |
| --- | --- | --- |
| `DINPUT8.dll!DirectInput8Create` | `0x119B038` | `0xFD1D09` |
| `XINPUT1_3.dll!#2` | `0x119BBD8` | `0xFD1D0F` |
| `XINPUT1_3.dll!#3` | `0x119BBD0` | `0xFD1D15` |
| `XINPUT1_3.dll!#5` | `0x119BBE0` | `0xFD1D1B` |
| `USER32.dll!GetAsyncKeyState` | `0x119BA60` | — |

跳转桩表位于 `0xFD1CF0 - 0xFD1D4B`（一串 `FF 25 disp32` = `jmp [rip+d]`）。
桩地址已用**磁盘镜像的槽内容**核对过（槽里确实指向 `DirectInput8Create` / 序数 2 等）。

### 4.8.2 已定位的输入模块

| RVA | 作用 |
| --- | --- |
| `0xA9A9E0`、`0xA9AEB0` | 调用 `DirectInput8Create`（DInput 设备创建） |
| `0xE6A5F0` | XInput 设备初始化：分配 0xD50 / 0xAF8 字节结构，调用 `XInputEnable`；被 `0xE6AB35` 调用 |
| **`0xE6AF34`** | **逐帧手柄轮询**（见下） |
| `0xE6AB50`、`0xE6ABE0`、`0xE6AD70`、`0xE6AE20`、`0xE6AF34` | 其他 XInput 调用者 |
| `0xE65F30`、`0xE6604B`、`0xE661E8` | 键盘路径（调用 `GetAsyncKeyState`） |

### 4.8.3 ★ 输入状态结构（`0xE6AF34`，257 字节）

```asm
00E6AF48  call 0x7E7D190                  ; 取时间增量
00E6AF4D  addss xmm0, [rbx + 0x49E54]     ; 轮询计时器
00E6AF57  comiss xmm0, [0x15889E4]        ; 节流常量 = 2.0f
00E6AF66  jb   skip                       ; 未到间隔
00E6AF92  lea rbp, [rdi + rdi*4]          ; rbp = slot * 5
00E6AF98  lea rdx, [rbx + 0x49E08]        ; ★ 输入状态数组基址
00E6AF9F  lea rdx, [rdx + rbp*4]          ; &state[slot]
00E6AFA3  call 0xFD1D0F                   ; ★ XInputGetState(slot, &state)
00E6AFAC  mov byte [rbx + rbp*4 + 0x49E04], 1   ; 已连接标志
```

由此得到**输入管理器对象 `rbx` 的字段布局**：

| 偏移 | 含义 |
| --- | --- |
| `+0x49E04 + slot*20` | 该槽"已连接"字节 |
| `+0x49E08 + slot*20` | **16 字节 `XINPUT_STATE`**（`dwPacketNumber` 在 +0） |
| **`+0x49E0C + slot*20`** | **`Gamepad.wButtons`（16 位按键位掩码）** |
| `+0x49E54` | 轮询计时器（float） |
| `+0x49E58` | 当前槽索引 |

> 槽步长 = 20 字节（4 字节标志+填充 + 16 字节状态），自洽。
> 这是**直接可用的原始按键位掩码**：`XINPUT_GAMEPAD_LEFT_SHOULDER = 0x0100`
> 即仁王 PC 默认的 L1/LB（防御键）。

### 4.8.4 脚本侧按键查询

`Input::Button` 节点 (`0x6F2450`)：
```asm
006F248A  call 0xDDF890        ; 读参数 -> eax = 按键索引
006F248F  mov  ecx, eax
006F2499  mov  ebx, 1
006F24A3  shl  rbx, cl         ; rbx = 1 << buttonIndex   （按键位掩码）
006F24AB  call 0xDDF890        ; 再读一个参数 -> eax = 角色索引
006F24B0  mov  rcx, [0x1871740]  ; 上下文对象
006F24B7  mov  r8d, eax
006F24BA  mov  rdx, rbx
006F24BD  call 0x6ECC00        ; ★ QueryButton(ctx, buttonMask, charIdx)
```

### 4.8.5 尚未解决的一步 + 两条可行路径

**卡点**：`0xE6AF34` **没有任何直接调用者**（原始字节扫 `E8 rel32` 也是 0），
也不在任何带 RTTI 的 vtable 里 —— 说明它是通过**无 RTTI 的函数指针表**间接调用的。
因此还差"输入管理器对象的单例指针"。

**路径一（推荐，最省事）**：`0x6ECC00` 是可以**直接调用**的普通函数
（`eax = f(ctx, mask, charIdx)`，遵循 Win64 ABI）。MOD 只需：
```c
void *ctx = *(void **)(base + 0x1871740);           // 上下文单例
int pressed = ((int(*)(void*,unsigned long long,int))(base + 0x6ECC00))
              (ctx, 1ull << guardButtonIndex, 0);
```
然后每帧轮询即可拿到"防御键是否按下"，自己做按下边沿与 `WindowMs` 计时。
**唯一未知量是 `guardButtonIndex`**，而它可以在一分钟内标定：让用户按住防御键，
MOD 依次试 `0..31`，哪个返回真就是它（并把结果写进日志/INI）。

**路径二**：找无 RTTI 的函数指针表 → 输入管理器单例 → 直接读
`[mgr + 0x49E0C + slot*20]` 的 `wButtons`。更直接但要先找到单例。

### 4.8.6 工具局限（已发现并部分弥补）

交叉引用库只反汇编 `.pdata` 覆盖的函数，**会漏掉没有 unwind 信息的代码**
（例如 `0xFD1CF0` 那一片输入 API 跳转桩、以及 `0xE6AF34` 的调用者）。

已弥补：`tools/ripref.py` 提供两个**原始字节扫描**模式，不受此限制：
```
python ripref.py <image> refs    0x119B038      # 找 RIP 相对引用（call/jmp/mov/lea [rip+d]）
python ripref.py <image> callers 0xE6AF34       # 找直接调用者（E8/E9 rel32）
```
`refs` 模式确实找回了库漏掉的 DInput/XInput 跳转桩引用。

---

### 4.8.7 运行时特征扫描输入管理器 —— 负结果（已排除一条路）

给探针加了 `HuntInputMgr=1`：扫描 `.data` 中所有指向**堆**的指针，
检查目标对象是否符合 `[+0x49E04+slot*20]` 已连接标志、`[+0x49E0C+slot*20]` 按键掩码、
`[+0x49E54]` 轮询计时器的布局（读操作全部走 `ReadProcessMemory`，非驻留页不会崩）。

- 第一版没排除镜像自身范围，得到 8 个候选，**全是 `.rdata` 内的假阳性**。
  修正为排除整个 `SizeOfImage` 范围后：
- 第二版只剩 2 个真堆指针候选，但**都不像**输入管理器：
  计时器恒为 `0.0000`，且"未连接"槽却有非零按键值。

**结论**：输入管理器**不是以 `.data` 直接指针形式可达的**（很可能它是某个更大对象的成员，
指针位于堆对象内部）。这条路作为"静态找单例"的方案已排除，除非改为全地址空间扫描。

### 4.8.8 ★ 锚点 A 的可行方案（下一步就做这个）

`0x6ECC00` 是个**干净、带 NULL 检查的小转发函数**：

```asm
006ECC00  488b4910    mov  rcx, [rcx + 0x10]     ; ctx->0x10
006ECC04  4885c9      test rcx, rcx
006ECC07  740e        je   0x6ECC17              ; NULL 直接返回，不碰任何东西
006ECC09  488b4918    mov  rcx, [rcx + 0x18]
006ECC0D  488b01      mov  rax, [rcx]
006ECC10  48ffa0d0000000  jmp qword ptr [rax + 0xD0]   ; 虚表槽 26
006ECC17  c3          ret
```

由此有两条实现路径：

**路径 A（推荐，被动且零风险）**：在 `0x6ECC00` 上装硬件断点，
**记录每次调用的 `rdx`（按键掩码）与 `r8d`（角色索引）**。
用户按防御键时出现的新掩码就是 `guardButtonIndex` 的答案 ——
不需要猜、不需要调用游戏代码、不需要改任何东西。
探针会话配置已把 t4 换成 `0x6ECC00`。

**路径 B（主动）**：由 MOD 每帧调用 `0x6ECC00(ctx, 1<<i, 0)` 扫描 32 位，
哪位返回真就是它。需要 `ctx = [0x1871740]` 非空（标题画面时为 NULL），
所以至少要进菜单/关卡。**风险**：这是调用游戏代码，ABI 或状态不对可能崩；
因此设计上做成 INI 开关（默认关）+ 仅标定期间启用。

两条路径都不需要精确的战斗时机，**只要按键被轮询到**即可标定。

---

## 4.9 【锚点 A 已落地】输入管理器单例 + 防御键按下检测 ✅

### 4.9.1 两个单例全局（静态读出来的）

`Pad::*` 节点族里的转发函数暴露了两个 getter：

```asm
00FA9580  mov rax, qword ptr [rip + 0x1CB1D41]   ; -> rva 0x2C5B2C8   手柄绑定对象
00E6BE90  mov rax, qword ptr [rip + 0xD0C7C1]    ; -> rva 0x1B78658   输入管理器
```

### 4.9.2 实机验证（关键，证据确凿）

用监视器读 `[0x1B78658] + 0x49E54`（轮询计时器）：

```
1.3333 → 1.4833 → 1.6333 → 1.7833     （步长恰好 = 我的 150ms 轮询间隔）
```

与反汇编预测的 `addss xmm0, dt; comiss xmm0, 2.0f` **完全吻合** → 该对象确实是输入管理器。

### 4.9.3 按键位序确认为标准 XInput

`[0x2C5B2C8] + 0x700` / `+0x704` 实测 = **`0x2000` / `0x4000`**，
即 `XINPUT_GAMEPAD_B` / `XINPUT_GAMEPAD_X` —— 正是日式布局的「确认=B、取消=X」。

由此确定掩码就是标准 XInput 位序，**防御键 L1/LB = `XINPUT_GAMEPAD_LEFT_SHOULDER = 0x0100`**。

`Pad::IsDecide`(`0x94AE00`) 的完整逻辑也印证了这点：
`ebx = f(g_pad, 0)`（当前按下的键）`& [g_pad+0x700]`（确认键掩码）。

### 4.9.4 MOD 如何用它（纯读取，零风险）

输入管理器地址**从 getter 自己的指令字节解出来**（取 `mov rax,[rip+disp32]` 的 disp32），
而不是硬编码 —— 换版本会自动对不上并拒绝安装：

```c
void *mgr = *(void **)(g_input_mgr_slot);            // = base + 0x1B78658
unsigned short btns = *(unsigned short *)
        ((char *)mgr + 0x49E0C + pad_slot * 20);     // wButtons
int down = (btns & guard_mask) != 0;                 // 按下边沿 + 时间戳
```

独立的 8ms 轮询线程做边沿检测（主循环 100ms 太粗），格挡事件发生时就地判定
`now - guard_press_ms <= WindowMs` 决定是否算精防。

### 4.9.5 实机激活验证

```
ANCHOR ok   guard_flag_1 / guard_flag_2 / guard_ki_cost / inputmgr_getter
INPUT manager slot = 0x7FF7ABE48658 (from getter 0xE6BE90 + disp 0xD0C7C1)
ANCHOR 4/4 verified
ARM pass: 372 threads seen, 372 newly armed
STATUS ACTIVE anchors=4/4 reduction=100% recovery=1 gate=1 mask=0x0100 slot=0 sound=1
CONFIG reloaded: ...        ← INI 热更新生效
```

### 4.9.6 两个我自己踩出来的 bug（已修，留档）

**Bug 1：给高频 getter 装了执行断点。**
`inputmgr_getter`（`0xE6BE90`）每帧被调用，装断点后异常洪水把后台线程饿死，
日志停在 `ANCHOR 4/4 verified`、`STATUS` 永远不出现，游戏却还活着。
修复：锚点表加 `armable` 标志，区分「只做字节校验」与「要占 DR 槽」。

**Bug 2：线程布点函数挂起了自己 → 永久死锁。**
`arm_new_threads()` 遍历进程内所有线程逐一 `SuspendThread`，**包括调用它的 worker 自己**，
于是永远走不到配对的 `ResumeThread`。现象同样是日志停在 `INSTALL step=arm`、
游戏存活但 MOD 停止工作。修复：循环里 `if (te.th32ThreadID == self) continue;`。
（探针里存在同一 bug，已一并修复。）

> 这两个 bug 都只有**实机运行**才能暴露 —— 本地自测、静态分析、单进程回归全部通过。
> 说明「实机验证」不是走过场。

---

## 4.10 设备无关性加固（本轮）

### 4.10.1 键盘路径与鼠标路径的定位

- `[0x1B78648]` 是**鼠标/窗口输入状态**单例（与输入管理器槽 `0x1B78658` 相隔 16 字节，
  是一族单例）。`0xE65F30` 会写它的 `+4/+5/+6`，读 `+0x10/+0x14`（光标位置钳制）、
  `+0x408`（疑似窗口句柄）、`+0x410`，并调用 `GetAsyncKeyState(1)`（鼠标左键）。
- 键盘的 `GetAsyncKeyState` 调用点在 `0xE65F30` / `0xE6604B` / `0xE661E8`。

**结论**：设备映射因玩家而异（手柄/键鼠），把 `GuardButtonMask` 硬编成 `0x0100`
是一个**可用性风险**，不能只靠"标准 XInput 位序"这一条推断。

### 4.10.2 用"学习模式"把它变成可标定，而不是可猜测

INI 新增两项：

| 键 | 作用 |
| --- | --- |
| `GuardKeyVK` | 键盘防御键的 VK 码，与手柄掩码**或**关系；0 = 不启用 |
| `LearnButtons` | 学习模式：日志里报告新出现的按键位与键码 |

学习模式会：
- 记录手柄按键字里**新出现的位** → `LEARN pad bit N pressed -> GuardButtonMask=0x....`
- 每 ~50ms 扫一遍 VK 0x08–0xFE，记录**新按下的键** → `LEARN key VK=0x.. pressed -> GuardKeyVK=..`

两项都只写日志、不改变任何行为，标定完改回 0 即可。
这样无论玩家用哪种设备，一次会话就能得到准确配置，不需要再来一轮猜。

### 4.10.3 配置读取器重写（修掉一个真实 bug）

实测中发现 `CONFIG reloaded: ... sound=0 vol=0.00` —— 明明 INI 里是
`SoundEnabled=1 / SoundVolume=1`。原因是用了 `GetPrivateProfileStringA`，
**它只认 ANSI 与带 BOM 的 UTF-16LE**；而这份 INI 是 UTF-8（还带中文注释），
一旦被编辑器以别的编码保存就会读错或静默忽略。

改为**自己读文件并做编码探测**：

- 识别 UTF-16LE BOM（对接 `WideCharToMultiByte`）、UTF-8 BOM、其余按 ANSI
- 简易行解析：跳过 `;` / `#` 注释、跟踪 `[PerfectGuard]` 分区、键值两侧去空白
- 分区外同名键会被忽略

验证（`tools/test_ini_encodings.py`，5/5 通过）：

| 用例 | 结果 |
| --- | --- |
| UTF-8 无 BOM / 带 BOM / UTF-16LE / ANSI | ✅ 全部解析出 `window=333 reduction=42 recovery=2 vol=0.25 file=my_parry.wav` |
| 非法值 `WindowMs=abc`、`reduction=999` | ✅ 被拒并回退默认值 |
| 注释里的假键 + `[OtherSection]` 的同名键 | ✅ 正确忽略 |

---

## 4.11 【锚点 D 落地】攻守双方指针 + HP/精力字段偏移 ✅

### 4.11.1 命中上下文里同时带攻守双方角色

格挡判定 `0x749640` 的唯一参数 `rsi` 是一个**命中上下文对象**（内部一直保留，
直到 `0x74DE51` 都没被覆盖）。构造它的分发器 `0x6FE5B0`：

```asm
006FE5B6  cmp   dword ptr [rcx + 0x11c], -1     ; 上下文有效性
006FE5BD  mov   rbx, rcx
006FE5C6  mov   rcx, qword ptr [rcx + 0x100]    ; ★ 角色 A（引擎会校验 word[+4]==0）
006FE5F3  mov   rax, qword ptr [rbx + 0xe8]     ; ★ 角色 B（同一套校验）
006FE612  mov   rax, qword ptr [rcx + 0x230]    ; 角色 +0x230 子对象
006FE65E  mov   dword ptr [rcx + 0xfc], 0x41000000   ; = 8.0f，某个计时/冷却
006FE66B  call  0x749640                        ; 送入格挡判定
```

两个角色都带 `+0x230` / `+0x240`，**与玩家角色同构**。

**攻击者的判定不需要猜**：玩家指针来自 `[0x18A0490]`（玩家槽位表），
两个候选里不是玩家的那个就是攻击者。

### 4.11.2 HP 与精力字段（从引擎自己的 getter 读出）

```asm
; Refer::Hp (0x6EC2D0)              ; Refer::Stamina (0x6EC7C0)
call 0x6E58E0                        call 0x6E58E0
mov  rax, [rax + 0x240]              mov  rax, [rax + 0x240]
mov  eax, [rax + 0x20]   ; ★ HP      movss xmm1, [rax + 0x40]  ; ★ 当前精力
                                     ; 钳制 >= 0 后 cvttss2si 返回
```

结合 `Player::RecoverStamina` 用的 `0x7B4C20`（"若 `[rcx] < [rcx+4]` 则提升到 `[rcx+4]`"），
得到**参数管理器的字段布局**：

| 偏移 | 含义 |
| --- | --- |
| `[[char+0x240]] + 0x20` | **HP（int32）** |
| `[[char+0x240]] + 0x40` | **当前精力（float）** |
| `[[char+0x240]] + 0x44` | **精力上限（float）** ← 「回复最大精力 1/6」所需 |
| `[[char+0x240]] + 0xBA8 + 8 + i*0x50` | 资源表（i=7 用于格挡扣精） |

### 4.11.3 MOD 实现

- **回精模式 3（Balanced，新默认）**：`cur += max/6`，钳制到上限；
  写入前后都有合理性闸门（`0 < max < 100000`、`0 <= cur <= max`）。
- **对敌削精 / HP**：从上下文取攻击者 → `[[enemy+0x240]+0x40]` 减精力、
  `+0x20` 减 HP；写入前逐项做合理性检查（HP 必须 `0 < hp < 1e8`），
  不合理就跳过而不是乱写。
- 默认两项都是 0，**不配置就不会写任何内存**。

### 4.11.4 新增格挡事件诊断

`LAYOUT` 行会打印玩家指针、精力、精力上限、HP，以及上下文里的两个角色指针：

```
LAYOUT player=0x.. ki=.. maxki=.. hp=.. | ctx=0x.. charA=0x.. charB=0x..
```

这条日志一次就能判定三件事：字段偏移对不对、谁是攻击者、以及
**格挡扣精实际改的是不是可见的精力条**（此前我对 `param+0x40` 与
资源表索引 7 的关系存疑，这条日志用于在实机上一次性settle）。

---

## 4.12 角色字段总表（全部从引擎自己的 getter 读出，非推断）

同一族小 thunk（都是 `sub rsp,0x28; call 0x6E58E0; ...`）把字段路径直接摊开了：

| 节点 | 处理函数 | 字段路径 | 类型 |
| --- | --- | --- | --- |
| `Refer::Hp` | `0x6EC2D0` | `[[char+0x240]] + 0x20` | int32 |
| `Refer::MaxHp` | `0x6EC450` | `[[char+0x240]] + 0x18` | int32 |
| `Refer::Stamina` | `0x6EC7C0` | `[[char+0x240]] + 0x40` | float |
| （精力上限） | — | `[[char+0x240]] + 0x44` | float（由 `Player::RecoverStamina` 用的 `0x7B4C20` 推得） |
| **`Refer::MotionFrame`** | `0x6EC4B0` | **`[[char+0x38]] + 0x60`** | float |
| **`Refer::ActionId`** | `0x6EB1C0` | `[[[[char+0x230]+8]+0x58]+0x20] + 0xC` | int16 |
| `Refer::GetActionPriorityType` | `0x6EB200` | `[char+0xF30]` 起始的查表 | int32 |
| （格挡资源表） | `0x7B21F0` | `[[char+0x240]+0xBA8] + 8 + i*0x50` | 0x50/项 |

### 4.12.1 「提前接续」的实现与风险判断

有了 `MotionFrame` 就可以**推进动画帧**来跳过当前动作剩余部分，即"取消后摇"。

但我要明确说明**这是一次有风险的实现**：盲写动画状态可能让角色卡在某个动作里。
因此按三条收口：

1. **默认关闭**（`CancelRecovery=0`），想用才开；
2. **只做加法**（`frame += N`）而不是覆盖成某个大值；
3. **合理性闸门**：只有 `0 ≤ 当前值 < 10000` 时才写，否则整个跳过。

并加了 `RECOVERY cancel: motion frame X -> Y` 日志行，便于确认它到底有没有生效。
INI 里也标注为实验性、未实机验证。

> 诚实说明：这一项目前是"**已实现但未验证**"，而不是"已完成"。
> 相比另外六项（都有实机或引擎字段级别的依据），它的置信度明显更低。

---

## 4.13 一个尚未定案的技术疑点（已用诊断覆盖）

### 问题

要让「格挡免耗精」真的生效，必须确认扣精点改的是**玩家看得见的精力条**。

- 格挡扣精点 `0x74DE51` 操作的是 `[[char+0x240]+0xBA8] + 8 + 7*0x50 + 0xC`
  = `[[char+0x240]+0x240+0xDEC]` → 即 **`param+0xDEC`**
- 引擎自己的 `Refer::Stamina` getter 读的是 **`param+0x40`**

**这两个地址不同**，所以"把消耗置 0 ⇒ 精力条不掉"这一步**在静态上没有闭合**。
我不能把它当成已验证的事实。

### 处理方式

新增 `KiTrace`（默认开启）在运行时同时采样两处字段：

```
KITRACE ki=210.5/250 | entry7 flag=1 cur=1180 cost=6.4   <-- changed
```

- `ki` 变化而 `entry7` 不变 → 说明两者独立，扣精减免需要改挂点（改 `param+0x40` 的写入方）
- 两者同向变化 → 说明 entry7 是累加器并最终驱动 `ki`，当前挂点正确
- 一次战斗会话即可定案

在标题画面 `KITRACE` 不输出（玩家对象不存在，函数提前返回），符合预期。

### 同类未验证项

`CancelRecovery`（推进动画帧）同样属于"已实现、未实机验证"。

> 我把这两项明确标为**未验证**，而不是混在"已完成"里。
> 其余六项都有实机（锚点 4/4、注入、音效链路）或引擎字段级（HP/精力/MotionFrame
> 均由 getter 反汇编读出）的依据。

---

## 4.14 【已静态定案】可见精力 ≠ 格挡扣精字段 —— 并已修正实现 ✅

### 4.14.1 证据

`0x7B4CB0` 是"遍历所有计量表"的函数：

```asm
007B4CBF  lea rbx, [rcx + 0x40]     ; ★ 表基址 = param + 0x40
007B4CC3  mov edi, 0x10             ; ★ 16 项
```

配合同簇的访问器 `0x7B4C70`（`mov rax, [rcx + rdx*8 + 0x40]`，`rdx = index*10`）
可知：**`param+0x40` 是一张 16 项 × 0x50 字节的表**，而

- `Refer::Stamina` 读的 `param+0x40` = **第 0 项的 `+0x00`**
- `param+0x44` = **第 0 项的 `+0x04`**（上限）
- `Player::RecoverStamina` 用的 `0x7B4C20` 正是"把第 0 项 `+0x00` 提升到 `+0x04`"

而格挡扣精改的是 `param+0xDE0+0x0C`。反算 `(0xDE0 - 0x40) / 0x50 = 43.7`，
**不是整数 → 两张表无关**。

**结论**：把 `0x74DE51` 处的消耗置 0，**并不能保证**玩家看得见的精力条不掉。
我此前几轮的假设是错的。

### 4.14.2 修正后的实现（双保险）

`KiDamageReductionPercent` 现在做两件事：

1. 仍在扣精点按比例缩放 `xmm1`（可能对内部那张表有效）；
2. **快照 + 补回可见精力**：格挡开始时快照 `[[char+0x240]+0x40]`（即
   `Refer::Stamina` 读的字段），随后 400ms 内按同一百分比把损失补回。

补回的三条安全约束：**只加不减**、**只补到快照值**、**只在 400ms 窗口内**。
并有 `KIRESTORE` 日志行。这样无论引擎内部用哪张表，可见精力条都会按预期减免。

这次修正的意义：把一个"看起来对、其实没有证据"的实现，
换成了**基于已确认字段**的实现，并保留了原路径作为保险。

---

## 4.15 【已实测】真实部署路径验证（加载器 + mods 目录）✅

在此之前我只用外部注入验证过 MOD，**从未验证过交付给用户的那套安装方式**。
这一环"东西都对但就是跑不起来"的风险最高，所以专门测了。

### 4.15.1 安装

```
<游戏目录>\
  nioh.exe
  dinput8.dll                       ← balfa 的 NiohNativeModLoader
  mods\Nioh1PerfectGuard\
      Nioh1PerfectGuard.dll
      Nioh1PerfectGuard.ini
      Sounds\parry.wav
```

### 4.15.2 结论（三条都是实测）

1. **加载器递归扫描子目录** —— 这是 README 没写清、示例却暗示的一点。
   加载器**自己的日志**给出了直接证据：

   ```
   [Loader] Scanning mods directory
   [Loader] Loading mod: ...\mods\Nioh1PerfectGuard\Nioh1PerfectGuard.dll
   [Loader] Finished loading mods
   ```

   （它导入了 `GetFileAttributesExW`，本就暗示会判断目录，现已证实。）
2. **整套流程走通且稳定**：两轮启动，
   `ANCHOR 4/4 verified` → `INSTALL step=arm` → `ARM pass: 372 threads seen` →
   `STATUS ACTIVE`，游戏连续稳定运行 72 秒以上。
3. 加载器把日志写在 **`mods\loader.log`**（不是游戏根目录），这点也记下来备用。

MOD 的日志写在**自己的目录**（`mods\Nioh1PerfectGuard\Nioh1PerfectGuard.gameplay.log`），
与 DLL 同级，符合设计。

### 4.15.3 为什么这一步重要

本地自测、静态分析、单进程回归、外部注入——**这四种验证都不会覆盖"用户照着 README 装"这件事**。
如果加载器不递归，用户按 README 装完会完全没反应，而日志一个字都不会有，
排查起来就是"毫无线索"。现在这个可能性被排除了。

---

## 4.16 资源表布局定案（三个访问器互相印证）✅

上一轮我从 `(0xDE0-0x40)/0x50` 不是整数推出"两张表无关"。这一轮找到了更直接的证据，
**修正了一处细节，并确认格挡扣精确实改的是格挡判定自己遍历的那张表**：

### 4.16.1 格挡判定自己就在遍历该表

`0x749640` 内部有两处调用计量表遍历器 `0x7B4CB0`，其中一处：

```asm
00749C1D  mov rcx, [r13 + 0x240]     ; 角色
00749C24  add rcx, 0xBA8             ; ★ 与格挡扣精同一个基址
00749C2B  call 0x7B4CB0
```

> 附带认识：`0x749640` 有 26KB，其实是**大型角色/命中更新状态机**，
> 格挡判定只是其中一段——这解释了它为什么会遍历计量表。

### 4.16.2 遍历器全文（步长与字段全部对上）

```asm
007B4CBF  lea rbx, [rcx + 0x40]      ; 起始 = base + 0x40
007B4CC3  mov edi, 0x10              ; 16 项
007B4CD0  loop:
007B4CD0    mov rcx, [rbx]           ; entry+0x38 = 指针
007B4CD3    mov dword [rbx - 0x38], 2 ; entry+0x00 = 2
007B4CDA    mov dword [rbx - 0x2c], esi ; entry+0x0C = 0
007B4CE2    mov edx, [rbx + 8]        ; entry+0x40 = int
007B4CEA    add rbx, 0x50            ; ★ 步长 0x50
```

以 **`entry = base + 8 + i*0x50`** 反推：`rbx-0x38` = `+0x00`、`rbx-0x2C` = `+0x0C`、
`rbx` = `+0x38`、`rbx+8` = `+0x40`。与另外两个访问器**完全一致**：

| 函数 | 计算 | 等价于 |
| --- | --- | --- |
| `0x7B21F0` | `base + 8 + i*0x50` | 表项起始 |
| `0x7B4C70` | `base + i*0x50 + {8, 0x14, 0x40}` | 表项 `{+0x00, +0x0C, +0x38}` |
| `0x7B4CB0` | 起始 `base+0x40`，步长 `0x50` | 同上 |

**表结构定案**（base = `[[char+0x240]+0xBA8]`，16 项 × 0x50 字节）：

| 偏移 | 含义 |
| --- | --- |
| `+0x00` | 状态（格挡路径与 1 比较；遍历器置 2） |
| `+0x0C` | **float 当前值——格挡扣精减的就是它** |
| `+0x38` | 指针 |
| `+0x40` | int |

### 4.16.3 与可见精力（`param+0x40`）的关系仍未证实

`Refer::Stamina` 读 `[[char+0x240]]+0x40`；本表项在 `param+0xBB0 + i*0x50`。
两者地址不同，**静态上仍无法断定谁是 UI 上那根精力条**。

这就是 MOD 里保留**双重实现**的原因：既缩放扣精点的 `xmm1`，也**快照并补回**
`param+0x40`。无论引擎用哪个字段，玩家看到的精力减免都成立。
`KITRACE` 日志用于在实机上最终确认。

---

## 4.17 `param+0x40 / +0x44` 是精力 (当前, 上限) —— 已确证 ✅

`0x6EC800`（就在 `Refer::Stamina` thunk `0x6EC7C0` 旁边）是 **`Refer::StaminaRate`**：

```asm
006EC816  mov   rcx, [rax + 0x240]
006EC81D  movss xmm1, dword ptr [rcx + 0x40]     ; 当前值
006EC83E  cvttss2si eax, dword ptr [rcx + 0x44]  ; 上限值
006EC84A  mulss xmm1, [0x1588B00]                ; × 常数
006EC855  divss xmm1, xmm0                       ; ★ 当前 / 上限
```

它算的是 **比率 `当前 / 上限`**，这正是 "Rate" 的语义。

**结论（硬证据）**：`[[char+0x240]] + 0x40` = **当前精力（float）**，
`+0x44` = **精力上限（float）**。

这条确认支撑了两项功能的地基：

- `KiRecoveryMode=3`（回复最大精力的 1/6）读的就是 `+0x44`；
- `KIRESTORE`（快照 + 补回"可见精力"）写在 `+0x40`。

### 仍存的一点：格挡扣精是否也改这个字段

资源表项在 `param+0xBB0 + i*0x50`，与 `param+0x40` 不同。静态上不能断定二者联动，
所以保留双重实现（缩放扣精量 + 补回可见精力），并用 `KITRACE` 在实机最终确认。
**但至少"恢复的那一侧用的是真精力字段"这一点，现在不再是猜测。**

### 4.17.1 顺手加的一条可用性防护

键鼠玩家若 `GuardButtonMask` 不匹配，精防门永远不会打开，日志里只会看到一堆
"不算精防"，很容易误判成 MOD 坏了。现在会在第 3 次格挡时给出一条明确警告：

```
WARNING: 3 blocks seen but NO guard press was ever detected, so the perfect-guard
gate can never open. GuardButtonMask is currently 0x0100 and GuardKeyVK is 0.
Set LearnButtons=1, press your guard key in game, and copy the LEARN line's value here.
```

把"沉默的失败"变成"可执行的提示"。

---

## 4.18 那张表更像是"状态/效果记录表"，不是计量表

看遍历器对每个表项做了什么（`0x7AB0A0`，`rcx` = 表项指针，`edx` = 表项 `+0x40` 的值）：

```asm
007AB0B9  cmp   edx, -1
007AB0C2  lea   rsi, [rcx + 0x170]      ; 表项 +0x170 是红黑树根
007AB0E0  cmp   dword ptr [rax + 0x20], edx   ; 按 key 比较
007AB0F1  cmp   byte ptr [rax + 0x19], 0      ; 颜色位 → 标准 rbtree 查找
007AB10D  mov   rdi, qword ptr [rbx + 0x28]   ; 找到的节点 +0x28 → 对象
007AB124  call  qword ptr [rax + 0x38]        ; 虚函数
007AB127  mov   byte ptr [rdi + 0x1d], 0      ; 清状态位
```

也就是说：表项 `+0x40` 被当作**键**去查一棵红黑树，找到对象后清标志、调虚函数。

**判断**：这张表更像**状态/效果记录表**（每条记录关联一个 map 里的对象），
而不像"当前精力/上限"这种计量表。这进一步支持了**双重实现是必要的**这个结论——
把 `entry+0x0C` 置 0 很可能只是清掉一个效果量，未必等于"格挡不掉精力"。

静态推理到此为止；剩下只能靠 `KITRACE` 的一次实机数据收口。

## 4.19 首次运行的可用性防护（基于实测证据）

**证据**：本项目所有实机测试里，输入管理器报告的四个槽位都是
`conn=[0,0,0,0]`、`pad=0x0000` —— **没有连接手柄**。

而本 MOD 的出厂默认 `GuardButtonMask=0x0100` 是手柄 L1/LB。**如果你是键鼠玩家，
默认配置下一个奖励都不会触发**，而日志里只会看到一堆"不算精防"，
非常容易误判成 MOD 坏了或锚点找错了。

现在在**首次解析到玩家对象时（即刚进关卡）**会主动判断并给出提示：

- 没有手柄连接 → 明确说明掩码不会匹配，并给出**两步标定法**
  （`LearnButtons=1` → 按防御键 → 抄走 `LEARN key VK=..` 的数值）
- 有手柄连接 → 报告连接了几个槽位、用的哪个槽位

配合 4.17.1 那条"3 次格挡但从未检测到按键"的警告，
把**首次运行最可能的两种沉默失败**都变成了可执行提示。

---

## 4.20 【已修 bug】格挡标志位的语义是"我的攻击被格挡了"，不是"我格挡成功"

### 问题

格挡标志位 `[[char+0x40]+0x90]+0xC8` 的语义由 `Refer::IsAttackGuard` 决定：
它是**对攻击方**查询"你这次的攻击有没有被格挡"。因此：

| 实际发生的事 | 标志位设在哪一方 |
| --- | --- |
| **我挡住敌人** | **敌人**对象上 |
| **敌人挡住我** | **我**对象上 |

而最初的 `on_block` 只看到"标志位被置 1"就计一次精防、放音效、发奖励——
**敌人挡下我的攻击时也会误触发**。同理，扣精点 `0x74DE51` 扣的是**格挡方**的资源，
敌人格挡时同样在跑。

> 也就是说：**我打敌人、敌人成功格挡的那一刻，MOD 会给我发精防奖励。** 这是必须修的。

### 修法：用资源表项的归属做权威判定

只有**格挡方自己的**资源表项会被扣。玩家自己的资源表范围是
`[[player+0x240]] + 0xBB0 .. +0x10B0`（16 项 × 0x50 字节）。所以：

```c
if (!entry_belongs_to_player(c->Rcx)) return;   // 别人在格挡，不是我们的事
```

这样就有了**权威的"玩家格挡成功"事件**。重构后：

| 断点 | 角色 |
| --- | --- |
| `0x74DD09` / `0x74DD92`（标志位） | **降级为纯日志**（两种情况都会打印，便于诊断） |
| `0x74DE51`（扣精点） | **权威事件**：过滤 → 精防窗口判定 → 发奖励 → 音效 → 快照回精 → 对敌效果 → LAYOUT |

并且把"否定的情况"也写清楚，便于实机核对：

```
GUARD (other) entry=0x... is outside the player's resource table; ignored
PLAYER BLOCK #3 -- not within 250ms of a guard press; no reward
```

---

## 4.21 【已修 bug】锚点部分失效时 MOD 会"静默哑掉"，且 DumpState 会改坏日志路径

第 18 轮自审（不写新功能，只审查自己的触发链）查出两个真问题。

### 4.21.1 三个锚点的依赖关系没有被表达出来

MOD 有 4 个锚点，但它们的**角色不同**：

| 锚点 | 角色 | 失效后果 |
| --- | --- | --- |
| `guard_flag_1` `0x74DD09` | 仅日志 | 只影响 `GUARD FLAG` 行 |
| `guard_flag_2` `0x74DD92` | 仅日志 | 同上 |
| `guard_ki_cost` `0x74DE51` | **权威事件源** | **一切奖励都不再触发** |
| `inputmgr_getter` `0xE6BE90` | 仅校验（不占 DR 槽） | 无 |

旧代码只报了 `ANCHOR n/4 verified`，**如果恰好是第 3 个（扣精点）校验失败**，
MOD 会照常报告安装成功、日志照常刷 `GUARD FLAG`，但精防永远不触发。
玩家看到的是"装了但没用"，而且日志里没有任何指向性线索 —— 这是最坏的一类失败。

**修法**：把依赖显式化，并在缺失时大声说出来：

```c
bool can_reward  = (g_anchor_count > 2) && g_anchor[2].enabled;  // 扣精点
bool can_observe = g_anchor[0].enabled || g_anchor[1].enabled;   // 标志位
```

- `can_reward == false` → 打印 `WARNING: the guard-cost anchor failed to verify;
  no perfect-guard reward can ever fire.`，状态串降级为 `DEGRADED(no-reward)`；
- 全部正常 → `STATUS ACTIVE anchors=4/4 ...`（实测输出见 4.21.3）。

状态串是给玩家一眼判断"到底装上了没有"用的，所以**必须区分"装上了"和"装上但不管用"**。

### 4.21.2 `PG_DumpState` 会永久改掉日志路径

`PG_DumpState` 为了导出快照，临时改写 `g_log_path`。旧代码**没有恢复**它，
于是每次调用之后，游戏内日志就被永久重定向到别处，后续诊断信息全丢。
修法：进入时保存、退出时恢复 `g_log_path`（含早退路径）。

### 4.21.3 修复后的实测输出

```
ARM pass: 372 threads seen, 372 newly armed
INSTALL step=arm done
INSTALL step=inputthread ith=00000000000027D0
STATUS ACTIVE anchors=4/4 reduction=100% recovery=3 gate=1 mask=0x0100 slot=0 sound=1
CONFIG reloaded: enabled=1 window=250ms reduction=100% recovery=3 gate_timely=1 sound=1 vol=1.00 file=parry.wav
```

回归：INI 编码/边界测试 **5/5 通过**（含拒绝 `WindowMs=abc`、
`KiDamageReductionPercent=999`、忽略注释键与其它 section 键）。

**本轮结论**：触发链本身（锚点校验 → 事件过滤 → 窗口判定 → 奖励）经审后无新缺陷；
剩下两个不确定项（扣精表项是否驱动可见精力条、`CancelRecovery` 是否生效）
在静态层面已无更多可挖证据，**必须靠一次实机游玩收口**。

---

## 4.22 精力条读的就是 `param+0x40`（强证据），并据此修好一个真 bug

### 4.22.1 `Refer::StaminaRate` 的身份确认

`.rdata 0x119F3B8` 存着字符串 `Refer::StaminaRate`，对应函数 `0x6EC800`：

```asm
006EC800  call 0x6E58E0            ; component(ctx, edx) —— 取出角色
006EC816  mov rcx, [rax + 0x240]   ; param
006EC81D  movss xmm1, [rcx + 0x40] ; 当前精力
006EC83E  cvttss2si eax, [rcx + 0x44] ; 上限
006EC84A  mulss xmm1, [0x1588B00]
006EC855  divss xmm1, xmm0
006EC865  ret                      ; 返回 0..1 的比率
```

它返回的是**归一化比率**，名字就叫 `Rate`，而且 `Refer::` 前缀是引擎的
**UI/脚本引用层**命名约定。**这就是精力条的填充比例。**

唯一的调用者 `0x6F6650`（128 字节，零引用 —— 它是函数指针注册进属性表的）做了这件事：

```asm
006F6669  mov rdx, rcx            ; 键对象
006F6688  call 0xDDF890           ; 键 -> 索引
006F668D  mov rcx, [0x1871740]    ; 属性注册表
006F6696  call 0x6EC800           ; StaminaRate(注册表, 索引)
006F66AB  call 0xDDFD20           ; 把 float 塞进 variant
```

"算出一个 float → 塞进 variant"是典型的 UI 数据绑定路径。

### 4.22.2 结论与它对实现的影响

| 字段 | 身份 | 证据强度 |
| --- | --- | --- |
| `[[char+0x240]]+0x40` / `+0x44` | **可见精力的当前值 / 上限** | 强（UI 层命名访问器直接读它并返回比率） |
| `[[char+0x240]+0xBA8]+8+7*0x50`，`+0x0C` | 格挡扣精**写入**的量 | 强（扣精点就在这） |

结合 §4.18（那张表是"状态/效果记录表"，表项 `+0x40` 当键查红黑树并调虚函数），
最合理的模型是：**`entry+0x0C` 是待生效的量，之后由效果对象写进真正的精力字段
`param+0x40`**。两条路径都值得保留 —— 这也正是双重实现存在的理由。

### 4.22.3 【已修 bug】`KIRESTORE` 的补回是"每 tick 重算"，导致减免百分比形同虚设

旧实现每个 tick 都算 `give = (snapshot - 当前) * 减免%` 再加回去。
而 worker 线程 **每 8ms 跑一次**，400ms 窗口 ≈ **50 次迭代**，于是：

```
剩余损失 = 初始损失 * (1 - r)^50
r = 20%  ->  0.8^50  ≈ 1e-5     // 几乎全补满
```

也就是说 `KiDamageReductionPercent` 实际只控制**精力条回弹多快**，
不控制**最终省下多少** —— 除了默认的 100%，其它取值全都近似"完全不耗精"。

**修法**：锁存"本窗口见过的最大损失 `peak`"和"已补回总量 `given`"，
每次只补差额，目标是 `snapshot - peak*(100-r)/100`：

```c
want  = g_ki_peak_loss * r / 100;
delta = want - g_ki_given;
if (delta <= 0) return;                 // 已经补够，不再补
target = min(now + delta, snapshot);    // 永不超过快照
```

数值仿真（`D=40`，`snapshot=100`）：

| 减免 r | 引擎**没**把缩放传到精力条 | 引擎**已**把缩放传到精力条 |
| --- | --- | --- |
| 0% | 损失 40 ✅（目标 40） | 损失 40 ✅ |
| 20% | 损失 32 ✅（目标 32） | 损失 25.6（多补，见下） |
| 50% | 损失 20 ✅（目标 20） | 损失 10（多补） |
| 100% | 损失 0 ✅ | 损失 0 ✅ |

右列是**唯一剩下的模型歧义**：若缩放本来就够得到精力条，补回就成了重复计算。
这件事静态上无法判定，所以改成**让日志自己给出答案**。

### 4.22.4 `KIV`：把模型歧义变成一条打印出来的结论

扣精点抓到的**缩放前**成本存进 `g_last_cost`（这就是引擎被要求扣的原始量 `D`），
窗口结束时拿它与确认过的精力字段实际损失 `L` 对比：

| 判据 | 结论 |
| --- | --- |
| `L ≈ 0` | 该字段根本没动 → 资源表和精力条无关 |
| `L ≈ D` | 缩放锚点够不到精力条 → **补回**才是生效机制 |
| `L ≈ D*(1-r)` | 缩放已够到精力条 → **补回重复计算** |
| 其它 | 部分传导 / 同时自然回复 → 不确定 |

容差取 `15%*D`（float 且会自然回复）。**这样一次实机游玩就能收口，
不需要再猜，也不需要玩家自己算比率。**

---

## 4.23 【已修】交付文档 `验收测试说明.md` 本身是坏的（双重编码乱码）

**发现过程**：核对文档里的日志标记是否与源码一致时，`read` 出来的是
`閿氱偣鑷牎楠岄€氳繃`。查文件字节：文件确实是合法 UTF-8，但**内容**是
"UTF-8 中文被按 GBK 解码后又存成 UTF-8"的双重编码 —— 也就是**乱码被固化进了文件**。

典型对照（"锚点" UTF-8 = `E9 94 9A E7 82 B9`，按 GBK 读）：

| 原字 | 文件里实际存的 |
| --- | --- |
| 锚点 | 閿氱偣 |
| 含义 | 鍚箟 |

**不可逆**：反解时出现 `U+20AC`(€)、`U+E0A1` 等 PUA 码位，
说明当初那次转换是**有损**的（63 字节的悬空尾字节被替换掉了），
`encode('cp936')` 直接抛异常，逐行反解 **0/57 行成功**。

**处置**：不赌反解，**按恢复出的 ASCII 骨架（表格键名、日志标记、配置名全都在）
重写整份文档**为干净 UTF-8，并顺手对齐本轮改动：

- `KIRESTORE` 行格式（`loss` → `peak loss` + `gave`）
- 新增 `KIV` 行说明，以及"它如何一次性判定精力字段归属"的表格
- 新增 `STATUS DEGRADED(no-reward)` 一条

**教训**：交付文档也要像二进制一样**校验编码**。以后所有中文文档统一用
`UTF8Encoding($false)` 或 `write` 工具写出，写完立刻用
`strict-utf8` 解码验证一次（本次已对 `mod\*.md`、`RE_NOTES.md`、
`验收测试说明.md` 全部验证通过）。

---

## 4.24 核心机制原来从未被真正验证过 —— 现在有了进程内自检

### 4.24.1 问题：一个无法与"玩家没玩"区分开的失效模式

整个 MOD 建立在一条**只能靠运行来确认**的假设上：

> DR0–DR3 里的 x64 执行断点，经我们的 VEH 分发后，**在 nioh.exe 里真的会触发**。

如果这条不成立（DR7 编码写错、或者内核/虚拟化策略把调试寄存器虚拟化掉了），
表现会是：**零事件、零崩溃、日志一片安静** —— 与"玩家还没进关卡""按键没标定"
**完全无法区分**。而之前所有实机测试都只验证到"安装成功"，从未验证过"断点会响"。

### 4.24.2 先做独立实验，确认编码本身是对的

写 `_work\dr7test.c`（zig cc 编译，console exe，不需要游戏），
三个变体各起一条一次性线程，在**自己的 probe 函数**上装 DR0：

| 变体 | 写入的 DR7 | 结果 |
| --- | --- | --- |
| A：`Dr7 = 0x1`（原 `arm_thread` 行为） | 清空一切其它位 | **5/5 命中** |
| B：保留保留位，只动 L0..G3 与 RW/LEN | `0x1`（本机初始 DR7 = 0） | **5/5 命中** |
| C：`Dr7 = 0x401`（置 bit 10） | `0x401` | **5/5 命中** |
| D：目标设为 0（不匹配） | `0x1` | **0/5 命中，但 5 次调用全部正常执行** |

结论：

1. **`Dr7 = L0`（RW=00、LEN=00 = 执行、1 字节）在本机确实会触发**；
2. **变体 D 是关键对照** —— 它证明计数器不是假阳性（不匹配时 0 命中，
   而且函数照常执行 5 次，说明断点机制没有破坏执行流）；
3. 本机新线程的初始 `DR7 = 0`，所以"`arm_thread` 清空 DR7 是个 bug"这个猜想
   **在本机不成立**（bit 10 并未被系统置位）。不过顺手改成"保留保留位"仍然是
   纯收益的加固 —— 成本为零，且不依赖"这个测量在所有机器上都成立"。

### 4.24.3 把实验搬进 MOD：启动即自检，结论直接写进日志

MOD 现在安装完成后立刻在**自己的代码**上做同样的验证（不涉及任何游戏状态）：
在一次性线程的 DR0 上装 `selftest_probe`，调用 5 次，数 VEH 分发了几次。

实机日志（**在 nioh.exe 进程内**）：

```
ARM pass: 372 threads seen, 372 newly armed
INSTALL step=arm done
SELFTEST breakpoint: OK: 5/5 probe calls trapped by the VEH (DR0=00007FFD2A905280, Dr7=L0 exec)
STATUS ACTIVE anchors=4/4 reduction=100% recovery=3 gate=1 mask=0x0100 slot=0 sound=1
```

**这条 `SELFTEST` 行把"断点机制是否可用"从假设变成了每次启动都重新验证的事实。**
失败时会明确说"execution breakpoints do not fire in this process,
so no anchor can ever trigger" —— 玩家若只看到 `STATUS ACTIVE` 却毫无反应，
现在有一行字可以直接区分"机制坏了"和"还没打起来"。

新增导出 `PG_GetSelfTest` 返回同一结论，便于自动化读取。

### 4.24.4 顺带修掉：`PG_SelfTest` 会永久改掉日志路径

和 §4.21.2 的 `PG_DumpState` 同一个 bug 类：它为了写自己的日志改写了
`g_log_path` 却**不恢复**。它同样是可被游戏内调用的导出，
调用一次之后游戏内诊断日志就全丢了。已改为保存/恢复 `g_ini_path` 与 `g_log_path`。

### 4.24.5 【已修】线程 id 复用会让某些线程永远装不上断点

`arm_new_threads` 用一个只增不减的 `g_armed[]` 记录"已装好断点的线程 id"，
之后靠 `is_armed(tid)` 跳过。但 **Windows 会回收线程 id**：一个已记录的 id
在其线程退出后可能被**新线程**复用，于是新线程被当成"已装过"而永远跳过 ——
那条线程上的格挡事件就**静默全部丢失**，不报任何错。

修法：每约 2 秒（64 个 arm pass × 32ms）做一次清理 —— 线程退出且句柄关闭后
其对象被释放，此时 `OpenThread(SYNCHRONIZE)` 会失败，据此把死 id 从表里摘掉，
让被回收的 id 有机会重新被装。

```c
HANDLE th = OpenThread(SYNCHRONIZE, FALSE, g_armed[i]);
alive = th && WaitForSingleObject(th, 0) == WAIT_TIMEOUT;
```

**残留窗口**：若 id 在两次清理之间就被复用，仍会漏掉（已如实记录，
不宣称彻底解决）。

---

## 4.25 把纯逻辑抽出来，做成离线可跑的单元测试

### 4.25.1 为什么

精防判定里有三块**纯逻辑**，它们不依赖引擎、不依赖 Win32，却一直没有被独立验证过：
窗口判定、按键边沿检测、精力补回算术。其中补回算术上一轮刚改过（§4.22.3），
当时只用一次性 Python 仿真验过 —— 仿真跑完就没了，**下次改动没有任何东西挡着**。

### 4.25.2 做法：单一来源 + 共享头文件

新建 `mod\pg_logic.h`，只放**无副作用**的纯函数（不分配、不记日志、不碰外部内存）：

| 函数 | 语义 |
| --- | --- |
| `pg_gate_open(gate_timely, press_ms, now, window_ms)` | 精防窗口；`press_ms==0`（从未按过）永不开启；时钟倒退也判为关闭而不是下溢成"很久以前" |
| `pg_down(buttons, mask, key_down)` | 手柄位或键盘键任一按下即为按下；**mask==0 永不判为按下** |
| `pg_is_new_press(was_down, down)` | 上升沿；按住不放不会反复续窗 |
| `PgKiRestore` + `pg_ki_begin/step/end` | 补回算术（锁存峰值损失与已补量，只补差额） |

**关键**：MOD 本体与测试 `tools\test_logic.c` **包含同一个头文件**，
所以测试通过是关于"实际发货的行为"的证据，而不是关于一份副本的证据。
本轮已把 `perfect_gate_open`、`poll_guard_button` 的边沿判定、
`ki_restore_tick` 全部改成调用这些函数（不再有第二份实现）。

### 4.25.3 测试内容（30 条断言，全部通过）

```
gate / window
press edge detection (50 frames)
button mask / keyboard
Ki top-up arithmetic

30 passed, 0 failed
```

覆盖的边界：

- 窗口：按下后 0ms / 恰好 `window_ms` / `window_ms+1` 三个边界；
  从未按下；陈旧按下（1 秒前的按下不得开门）；**时钟倒退**；`window_ms=0`
- 边沿：按住 50 帧只算 **1 次**按下；两次点按算 2 次
- 掩码：手柄 L1 命中 / 不同位不命中 / mask=0 永不命中 / 纯键盘（`pad=0`）也能命中
- 补回：`r=0/20/50/100` 各自落在精确目标；**永不超过快照**；
  已结算后幂等；窗口关闭时不改动；精力自然回复超过快照时不被拉回
- **回归护栏**：测试里显式复算了旧的"每 tick 补剩余损失×r%"公式，
  断言它确实会补满（`> 99.9`），并断言新公式**不会**补满。
  这样如果谁把旧写法改回来，测试立刻红。

编译运行（**不需要游戏**）：

```
zig cc -target x86_64-windows-gnu -O2 -o test_logic.exe test_logic.c
```

---

## 4.26 顺手修掉的两处安全/健壮性问题

### 4.26.1 对敌效果可能写到无关角色身上

`apply_enemy_effects` 原本的选敌规则是"两个候选里那个**不是玩家**的"。
但如果某个命中上下文里**玩家根本不在**这两个候选之中（例如 `ctx+0x100` 是攻击者、
`ctx+0xE8` 是别的东西），这条规则就会随手挑一个 Character 并把 **Ki/HP 伤害写进去**。

改为：**先确认玩家确实在这两个候选里**，确认之后再取另一个；确认不了就什么都不写，
并打印一次 `ENEMY effects skipped: the player (0x..) is neither ctx+0x100 (0x..) nor
ctx+0xE8 (0x..); refusing to guess`。

（默认 `EnemyKiDamage`/`EnemyHpDamage` 都是 0，所以这条路径本来就不写内存 ——
修的是"打开之后不要打错人"。）

### 4.26.2 对敌伤害日志会显示假的原始值

旧代码写完再算 `*ki + damage` 当"原值"。若发生钳位（原值小于伤害），
日志会把伤害值当成原值打印出来。改为写入前先存 `was`。

### 4.26.3 输入槽推导不再依赖硬编码下标

`anchor_verify()` 里用 `g_anchor[3]` 取 `inputmgr_getter` 来推导输入管理器指针槽。
**锚点表一旦重排，这段代码会照常编译，却从错误的锚点推导出一个错误的槽地址，
结果就是所有按键都检测不到。** 已改为**按名字查找**，并在缺失/字节数不足时
明确打印原因。

---

## 4.27 热更新 / 配置路径审计：找到 3 个真 bug，其中 1 个会静默关掉音效

### 4.27.1 【已修】`ini_str` 的默认值参数与输出缓冲区是同一块内存

唯一调用点是 `ini_str("SoundFile", c.sound_file, sizeof, c.sound_file)` ——
`def` 就是 `out`。而旧实现先 `ini_get(key, out, cap)`（**失败时会把 `out` 清空**），
再去 `lstrcpynA(out, def, cap)` 把"默认值"拷回来 —— 此时 `def` 已经被自己清空了。

**后果**：INI 里没有（或删掉了）`SoundFile` 这一行时，文件名变成空字符串
→ `"%sSounds\\%s"` 拼成 `...\Sounds\` → `wav_load` 失败 → **音效静默消失**，
日志里只有一句听起来很正常的 `SOUND disabled`。

修法：查找走局部缓冲，`def` 不再被污染。

### 4.27.2 【已修】编译内置默认值与随包 INI 不一致

`config_defaults()` 是"INI 缺失/不可读时"实际生效的配置，但它和 INI 对不上：

| 键 | 代码内置 | 随包 INI | 后果 |
| --- | --- | --- | --- |
| `RequireTimelyGuard` | **0** | 1 | INI 一丢，**每一次格挡都算精防** |
| `FixedRecovery` | **0.0** | 50 | 仅 mode 2 受影响，轻微 |

第二个尤其危险：`gate_timely=0` 意味着"门永远开着"，玩家随便按住防御挨打都会
发奖励，而日志看起来一切正常。

已改齐，并**加了一条测试把这条不变量钉住**：`test_ini_encodings.py` 现在会
分别用"有 INI"和"无 INI"跑一次 `PG_SelfTest`，逐字段比对，必须完全一致。

> 这条测试立刻又抓出了 4.27.1 —— 正是它把 `file=` 从 `parry.wav` 变成空串。
> 先修 4.27.2 再跑才暴露出来，说明"默认值与 INI 一致"这个不变量值得长期测。

### 4.27.3 【已修】配置热更新可能滚动回退一代

旧代码的解析基线是 `g_cfg_prev` —— **上一代**配置，而不是当前配置。
于是"INI 缺失或被编辑器截断"（编辑器保存文件时通常先清空再写）时，
所有键都读不到 → `c` 保持 `g_cfg_prev` → `g_cfg = c` → **配置悄悄退回一代**。

改为以**当前** `g_cfg` 为基线：没写的键保持现值，拒绝时本来就不会赋值
（回滚语义天然成立），`g_cfg_prev` 随之删掉。

### 4.27.4 【已修】`config_load` 可被并发调用 → 共享缓冲区 use-after-free

`ini_load_text()` 把整份 INI 解析到**一个共享堆缓冲** `g_ini_text`（先 `free` 再 `malloc`）。
而 `config_load` 有两个调用者：输入线程的 `ini_poll()`（文件一变就触发）和
导出 `PG_ReloadConfig()`（外层工具随时可调）。两者撞上就是 **double free / use-after-free**。

修法：`config_load` 外面套一把 `g_cfg_mutex`（DllMain 创建），
解析主体改名 `config_load_inner`。日志锁 `g_log_mutex` 仍然在内层获取，
不存在"日志 → 配置"的反向加锁，无死锁环。

### 4.27.5 【已修】`PG_SelfTest` 会把正在运行的配置重置成出厂默认

它为了自检而 `config_defaults(&g_cfg)` + `g_cfg_loaded = 0`，但**没有恢复**。
在游戏内调用一次，正在生效的配置就被打回默认。上一轮已经修过它的日志路径，
这一轮把它一并纳入保存/恢复（`g_cfg`、`g_cfg_loaded`、两条路径）。

### 4.27.6 【已修】每 20 秒把整进程线程挂起一次（周期性卡顿）

worker 循环里有一行：

```c
if (pass % 200 == 0) InterlockedExchange(&g_armed_n, 0);   // 每 20s 清空已装表
```

清空后会触发一次**全量重扫**：把当时进程里约 370 条线程逐个
`SuspendThread` → 改 DR → `ResumeThread`。实测初次全扫耗时 43–80ms，
也就是说**游戏每 20 秒被冻结约 50ms**（60fps 下掉 3 帧），而且是静默的。

而它想达到的两个目的其实都已被覆盖：

- **新线程**：正常扫描每 400ms 就会把不在表里的线程装上；
- **线程 id 被回收**：§4.24.5 的 `purge_dead_armed()` 每 6.4s 清一次死 id。

它唯一独有的是"修复被悄悄清掉的 DR 状态"。于是改成**滚动重装**：
每次 arm pass 重装 **6 条**已记录的线程（约 1ms），370 条约 25 秒转完一圈 ——
既保留了修复能力，又没有任何时刻会挂起整个进程。

完成第一圈时会打一行日志作为证据，`STATE` 行也新增了 `rearmed=` 计数：

```
ARM rolling re-arm: first full cycle done (372 writes over 374 tracked ids, no whole-process suspend)
```

---

## 4.28 音效延迟 100ms 与 KITRACE 无节流（日志会涨到几 MB/小时）

### 4.28.1 音效最多晚 100ms —— 听感上就是"音效坏了"

worker 循环是 `Sleep(100)`，而音效请求正是由它消费的（它拥有 COM apartment，
不能挪到别的线程去播）。于是**精防发生到听到声音最多隔 100ms**。

修正：worker 改成 `Sleep(25)`（延迟 ≤25ms），把原来的"每 4 个 pass"这类
昂贵节拍按比例放大 4 倍，所以各功能的实际周期完全不变：

| 工作 | 旧 | 新 |
| --- | --- | --- |
| 音效请求消费 | 100ms | **25ms** |
| `arm_new_threads` + 滚动重装 | 每 4 pass = 400ms | 每 16 pass = 400ms |
| `ini_poll`（INI 热更新） | 每 4 pass = 400ms | 每 16 pass = 400ms |
| 音效初始化重试 | 每 40 pass = 4s | 每 160 pass = 4s |

顺带确认了握手本身是对的：worker 先读 `g_play_req` 再写 `g_play_done`，
若期间 VEH 又自增，则新请求留到下一轮（**只会延后，不会丢**）。
多次精防落进同一个 25ms 窗口时会合并成一次播放 —— 人做不到 25ms 内两次精防，
且避免声音叠在一起，这是想要的行为。

### 4.28.2 KITRACE 会无限刷：约 10 行/秒、几 MB/小时

`ki_trace_sample()` 的触发条件是"entry-7 的值与上次采样不同"。但 §4.16.2 已经查明
引擎的计量表遍历器 `0x7B4CB0` **每轮都把每个表项清成 2 和 0**：

```asm
007B4CD3  mov dword [rbx - 0x38], 2     ; entry+0x00 = 2
007B4CDA  mov dword [rbx - 0x2c], esi   ; entry+0x0C = 0  ← 我们采样的是这个
```

所以"变了"这个条件**每一次采样都成立** —— 10 行/秒永远刷下去。
每行都要 `CreateFileA` + `WriteFile` + `CloseHandle`（还带一把互斥量），
一小时几 MB，而这正是要交给玩家回传的日志文件。

修正：节流 + 上限。真正关键的是"格挡那一刻的值"，而它已经被
`GUARD cost=` / `KIRESTORE` / `KIV` 三行直接带出来了，KITRACE 只是辅助。

- 变化行间隔 ≥ **1000ms**；心跳行（每 200 pass ≈ 5s）不受间隔限制；
- 总行数上限 **600**，到顶时打印一次说明然后静音。

### 4.28.3 节流逻辑抽进 `pg_logic.h`，因此可以被离线验证

这条修复本身在实机里**验证不了**：标题画面不会构造玩家对象，`ki_trace_sample()`
直接 return，所以再怎么跑游戏也不会有 KITRACE 行。既然无法用游戏验证，
就把它做成**可单元测试的纯函数** `pg_throttle()`（与 MOD 共用同一个头文件）：

```
diagnostic throttle
47 passed, 0 failed
```

新增断言（共 17 条）：

- 首次采样必发；间隔内连发全部丢弃；间隔一到就发；心跳可越过间隔；
- 到上限后**恰好一次**上限通告，之后永久静音，且计数不再增长；
- **回归护栏**：同一毫秒内 50 次采样只能出 **1** 行
  （这正是旧实现会出 50 行的那种输入）；
- 10 秒 @100Hz 的稳定流只出 **9–11** 行，而不是 1000 行。

> 写测试时我把"上限通告"的时机写错了（以为第 5 行那次调用就返回 CAPPED，
> 实际是第 6 次调用才返回）。是**测试的期望**错了，不是函数错了 ——
> 修正期望后 47/47。

---

## 4.29 文档与代码的一致性，改成机器检查

### 4.29.1 为什么

这个项目已经**两次**出现"文档说了代码没有的东西"：§4.23 整份验收文档是坏编码，
更早还有日志标记与源码不一致。靠人眼逐条核对不可持续，而且每次都只覆盖当时那一版。

新建 `tools\test_doc_markers.py`，做两件独立的漂移检查：

1. **日志标记**：验收文档"日志标记对照表"里每一个反引号标记，
   其所有实词 token 都必须能在源码里找到。找不到 = 文档让玩家去等一个永远不会出现的东西。
2. **配置键**：文档默认值表里点名的每个 INI 键必须真的存在于随包 INI 里。

外加一个反向清单：**每一条会到达玩家眼睛的 `log_line` 是否在任意文档里被解释过**
（文档语料 = 验收文档 + `README_CN.md` + `README_EN.md`）。
真正只给内部看的行必须**显式列进 `INTERNAL_ONLY` 白名单**，
所以"新增一条玩家可见的日志却没写文档"会直接让检查失败，而不是悄悄漂移。

### 4.29.2 它当场抓出的东西

初次运行报 13 条"缺失"，逐条分诊后分三类：

| 类型 | 例子 | 处置 |
| --- | --- | --- |
| **真·文档错误** | `GUARD pressed (... mask=0x0100 ...)` —— 把默认值写死进标记，玩家一改掩码就与实际不符 | 改为 `mask=0x....` |
| **真·文档缺失** | 代码能打 **17 种** `SOUND disabled: ...`，文档一种都没解释 | 见 4.29.3 |
| 检查器误报 | `Xms`、`250ms` 这类占位符；INI 键名表被当成日志标记 | 修检查器（占位符过滤、只取标记表那一节） |

现在的结果：

```
log markers claimed by the doc : 22
  verified present in source   : 22
log_line literals in source    : 77
  not referenced in any doc    : 0
```

### 4.29.3 补上的真实文档缺口：音效排查表

代码里有 17 条 `SOUND disabled: ...` 分支（每种失败原因一条），
而**玩家第一次实机游玩最可能的抱怨就是"没有精防音效"** —— 之前文档对此完全沉默。

已在 `README_CN.md` / `README_EN.md` 各加一张对照表，
把每条 `SOUND disabled:` 映射到"原因 + 怎么办"（缺 XAudio2 运行时、
`SoundFile` 名字不对、WAV 不是 16-bit PCM、采样率不是 44.1/48k、
没有输出设备、被独占……），并说明换自定义音效的正确规格。
验收文档也加了一行指引与 `MANUAL EXPORT`（`Ctrl+Shift+F10`）那条日志。

> 顺带确认：`SOUND disabled by configuration` 是 `SoundEnabled=0` 的正常结果，
> 不是故障 —— 这类"看起来像错误其实正常"的行现在都标注清楚了。

---

## 4.30 输入假设被引擎自己的代码证实了（并且发现扳机键匹配不到）

最后一个没有独立佐证的假设是：`mgr+0x49E0C+槽位×20` 到底是不是标准 XInput 按钮字，
位序是否与 XInput 一致。这轮用解密镜像查清了，三条证据互相独立。

### 4.30.1 `+0x49E0C` 在全镜像里只被字面引用一次 —— 它就是那个字段

对整个 `.text` 扫描 4 字节位移：`0x49E0C` 只有 **1** 处（RVA `0xE6B001`），
而 `0x49E04` 有 14 处、`0x49E54` 有 6 处，全部聚在 `0xE6AF34` 附近的输入函数族里。

### 4.30.2 手柄热插拔轮询器：槽位步长与"已连接"字节对上

`0xE6AF34`（257 字节）是**每槽位状态管理器**：

```asm
00E6AF7B  lea rax, [rdi + rdi*4]                  ; rax = slot * 5
00E6AF7F  cmp byte ptr [rbx + rax*4 + 0x49E04], sil  ; ★ +0x49E04 + slot*20 = 已连接
...
00E6AFEA  call 0xFD1D0F                           ; 间接调用：刷新该槽位
00E6AFEF  test eax, eax
00E6AFF1  je next                                 ; eax==0 → 存在
00E6AFF5  mov qword [rbx + rdi*4 + 0x49E04], rax  ; 否则整条记录清零
00E6AFFD  mov qword [rbx + rdi*4 + 0x49E0C], rax
00E6B005  mov dword [rbx + rdi*4 + 0x49E14], eax
...
00E6B00E  add rbp, 0x14                            ; ★ 步长 20
```

`(slot*5)*4 = slot*20`，加上末尾的 `add rbp, 0x14` —— **步长 20 被引擎自己证实**，
`+0x49E04` 是连接标志也证实了。断开时它把整条 20 字节记录清零
（三条重叠的零写入覆盖 `+0x00/+0x08/+0x10`）。

### 4.30.3 按键测试函数：按钮字精确落在 `rdx+4`

`0xE6BBC0` 拿着 `&记录+4`（即 `mgr+0x49E08+slot*20`）循环调用
`0xE6BCF0(index, padstate)`，`index` 从 0 到 **0x17（24）**。而 `0xE6BCF0`：

```asm
00E6BCF7  cmp ecx, 0xd                    ; index <= 13 走按钮位
00E6BCFF  lea rcx, [rip + 0x45398a]       ; 位表 RVA 0x12BF690
00E6BD06  movzx ecx, word [rcx + rax*2]   ; 取该 index 的位值
00E6BD0A  test word ptr [rdx + 4], cx     ; ★ 测 16 位按钮字
...
00E6BD20  cmp byte ptr [rdx + 6], 0       ; index 14 → 扳机 1（字节）
00E6BD32  cmp byte ptr [rdx + 7], 0       ; index 15 → 扳机 2（字节）
00E6BD4E  movzx ecx, word ptr [r10 + 8]   ; index 16 → 摇杆 X（word）
00E6BD79  ...            word ptr [rdx + 0xa]  ; index 17 → 摇杆 Y
```

`padstate = mgr+0x49E08+slot*20`，所以 `rdx+4` = **`mgr+0x49E0C+slot*20`** ——
与 MOD 读的地址**完全一致**，且确实是 **16 位字**（`unsigned short` 正确）。

### 4.30.4 三处偏移同时对上 `XINPUT_GAMEPAD`

| 引擎读法 | 相对 `padstate` | 相对 `XINPUT_GAMEPAD` 基址（= padstate+4） |
| --- | --- | --- |
| `test word [rdx+4]` | +4 | `wButtons` (+0) ✅ |
| `cmp byte [rdx+6]` / `[rdx+7]` | +6/+7 | `bLeftTrigger`/`bRightTrigger` (+2/+3) ✅ |
| `movzx word [rdx+8]` / `[rdx+0xa]` | +8/+0xa | `sThumbLX`/`sThumbLY` (+4/+6) ✅ |

即：**`mgr+0x49E0C+slot*20` 处是一个标准 `XINPUT_GAMEPAD`**。

### 4.30.5 位表：前 12 项与标准 XInput 完全一致

读 RVA `0x12BF690` 的 14 个 word：

| idx | 位 | 含义 |
| --- | --- | --- |
| 0–3 | `0x0001/2/4/8` | 十字键 上/下/左/右 |
| 4 | `0x0010` | START |
| 5 | `0x0020` | BACK |
| 6 | `0x0040` | 左摇杆按下 |
| 7 | `0x0080` | 右摇杆按下 |
| **8** | **`0x0100`** | **L1 / LB** ← MOD 默认 `GuardButtonMask` |
| 9 | `0x0200` | R1 / RB |
| 10–11 | `0x1000` / `0x2000` | X / Y |
| 12–13 | `0x4000` / `0x8000` | 引擎自有（标准 XInput 的 A=0x0400、B=0x0800 **不在表里**） |

**结论：`GuardButtonMask=0x0100` 就是 L1/LB，默认值正确**；
而 `LearnButtons` 打印的 `LEARN pad bit N`（= `1<<N`）直接就是可用掩码，
因为它读的是同一个字、同一种位序。

### 4.30.6 由此发现的真实限制：扳机键永远匹配不到

L2/R2 在 `+0x49E0E`/`+0x49E0F` 两个**独立字节**里（引擎 index 14/15 分别测它们），
**不在 16 位按钮字里**。所以无论 `GuardButtonMask` 写什么值，
**都不可能匹配到扳机键**。

这是一个此前文档完全没提、玩家很容易踩的坑：把游戏里的防御绑成 L2，
然后发现 MOD"完全不工作"，日志里只有一堆"不算精防"。

处置（本轮）：

1. `poll_guard_button` 现在顺带读这两个字节，存进 `g_trig_lt/g_trig_rt`；
2. 一旦"从未检测到防御按下"且扳机被捏到 >128，就打一条明确提示：
   ```
   NOTICE: a trigger is being squeezed (LT=.. RT=..) but GuardButtonMask (0x....)
   only matches the 14 button bits, never the triggers. ...
   ```
3. `STATE input` 行与 `PG_GetInputInfo` 新增 `lt=` / `rt=`，便于事后判读；
4. 中英文 README 都补了"手柄按键与扳机键"一节，写明已验证的布局、
   位表、以及这条限制。

（**没有**去实现"掩码匹配扳机"：需要新增配置项与第二条检测路径，
而当前用户大概率是键鼠（实测四个槽位全是 `conn=[0,0,0,0]`），
先把限制讲清楚更划算。若将来需要，偏移与语义都已确定，实现成本很低。）

---

## 4.31 锚点字节有了机器校验；顺带查清"重新构建为何不是逐字节相同"

### 4.31.1 锚点期望字节是安全机制，也是最危险的沉默失败点

MOD 只在期望字节与实际字节一致时才装断点，所以错的构建永远不会被乱改 ——
代价是：**期望字节里有一个字打错，就会在完全正确的游戏上拒绝安装**，
而日志只显示 `ANCHOR MISS`。这类错误靠人眼核对 4 条 × 5–7 字节并不可靠。

新增 `tools\test_anchors.py`：从 C 源码里解析 `BYTES_*` 数组与 `g_anchor[]` 表，
再从 `_work\nioh1.mem.exe`（解密镜像）读出同一 RVA 的字节逐字节比对。结果：

```
OK   guard_flag_1       rva=0x74DD09   armable=1  C6 81 C8 00 00 00 01
OK   guard_flag_2       rva=0x74DD92   armable=1  C6 81 C8 00 00 00 01
OK   guard_ki_cost      rva=0x74DE51   armable=1  E8 EA 6D 06 00
OK   inputmgr_getter    rva=0xE6BE90   armable=0  48 8B 05 C1 C7 D0 00

armable anchors (need a DR0..DR3 slot): 3 of 4 available
  getter decodes to input-manager slot rva 0x1B78658 (expected 0x1B78658)
OK   BYTES_SUB      rva=0x7B4C40   (spare)
```

它还顺带守三条结构性不变量：

- **可装槽位的锚点数 ≤ 4**（超过就意味着有锚点装不上，而 MOD 不会报错）；
- `inputmgr_getter` **必须是 `armable=0`**（它每帧都被调用，装上就会把异常处理器刷爆）；
- 输入管理器槽必须由 getter 自己的 `disp32` 推出，且等于**独立已知值 `0x1B78658`**
  （这条把"指令解码"和"字段布局"两条独立结论绑在一起互证）。

### 4.31.2 `BYTES_SUB` 是死代码，但它是有价值的死代码

检查器报出 `BYTES_SUB` 未被任何锚点引用。它对应 RE_NOTES 里记的备用锚点
`ki_subtract_prim`（`0x7B4C40`，浮点扣减原语）。**不删**：将来若某次更新移动了扣精点，
它就是现成的备选。改为在测试里**声明为"已固定的备用数组"并照样对镜像校验**，
同时删掉"未使用数组"的噪声告警 —— 知识保留，且从"传说"变成"被验证的资产"。

### 4.31.3 重新构建不是逐字节相同 —— 但代码确实没变

只改了注释后重新构建，DLL 哈希变了。查清楚原因很重要，否则"哈希变了"
既可能是无害的元数据，也可能是代码真的变了。

新增 `tools\compare_builds.py` 逐节比对：

```
  .text      same    old=da07a5f20831f9b8 new=da07a5f20831f9b8
  .rdata     same    old=5fe8710e5143e47b new=5fe8710e5143e47b
  .data      same    old=d273c6b94dd291c4 new=d273c6b94dd291c4
  .buildid   metadata
RESULT: code-identical (only build metadata differs)
```

结论：**Zig 会写入 `.buildid` 节，所以哈希永远会变；除此之外所有节逐字节相同。**
因此以后判断"这次改动是否影响行为"，要看 `.text`/`.rdata`/`.data`，不能看文件哈希。
（`dist` 与已安装副本之间仍然要求哈希完全相同 —— 那是同一份文件的拷贝，与重构建无关。）

> **更正（§4.49，实测后）**：上面"哈希永远会变"说得太满，后来还出现过一次 `.rdata`
> 也不同的比对，于是这句话失去了预测力。实测结论是：
> **哈希随"输出路径"而变** —— 同一路径重建是**逐字节相同**的（3/3）；
> 只要输出**基名**相同，除 `.buildid` 与 PE 头时间戳外所有节相同。
> 那次异常比对的两侧其实是两个**临时名**产物（`pg_a.dll` vs `pg_b.dll`），
> `.rdata` 的那 1 字节差异就是内嵌模块名里的 `a`/`b`。详见 §4.49。

### 4.31.4 交付物新增 `CHANGELOG.md`

把"已验证的部分（含可重跑的命令）"、"修过的问题（按机制/配置/数值/输入分类）"、
"还需实机确认的 4 项"、"三步操作"合成一份交付说明放进包里，
这样回来时不必翻 1900 行笔记。

---

## 4.32 交付彩排：把"文档提到的文件"也变成机器检查

### 4.32.1 问题

包里只有 DLL / INI / 音效 / 文档，但 `CHANGELOG.md` 里有一张"自动化检查"表，
写着 `python test_anchors.py ..` 之类 —— 而 `tools\` **不在包里**。
玩家照着做只会找不到文件。这类"文档指向不存在的东西"只能靠逐条真跑一遍才发现。

### 4.32.2 难点：怎么把路径和普通文字区分开

文档里充满看起来像路径的东西：`SOUND disabled: xaudio2_9.dll / XAudio2Create unavailable`、
`0x0001/0002/0004/0008`、`KIRESTORE a -> b (snapshot ..)`。第一版按"含 `\` 或 `/`
或已知扩展名"筛选，结果 21 条里绝大多数是误报。

改成**自校准**判据：**只有当这个 token 的 basename 在仓库里真的存在某个文件时，
才算作"文件引用"**。日志示例、位列表自然被忽略，因为不存在叫
`xaudio2_9.dll` 或 `0008` 的项目文件。

第一次跑还把 Zig 工具链里的文件算了进来（`thread`、`format`、`expected` 都命中了
`_tools_dl` 里的测试数据），排除 `_tools_dl` 后误报归零。

### 4.32.3 它抓到的三个真缺陷

| 缺陷 | 处置 |
| --- | --- |
| `CHANGELOG.md` 让玩家在 `tools\` 下跑 6 个脚本，但包里没有这些工具 | 重写该节：拆成"包内可自行核对"与"开发目录里重跑（**本包不含**）"，并把命令写成带 `tools\` 前缀的完整形式 |
| `README_EN.md` 写 "Covered by `tools/test_ini_encodings.py` (5/5 cases)"，同样指向包外，且数字已过时（现为 6/6） | 改为明确说明那是开发目录里的检查、且**不随包分发**；数字更正 |
| 验收文档的文件清单里没有源码 | 见 4.32.4 |

检查器还会**自动**判定"文档引用了开发目录却没说明"：它从实际找到的引用推导出
需要披露的文档清单，而不是维护一张硬编码列表 —— 我第一版就是硬编码的，
结果 `README_CN.md` 里的引用早就删了，列表却还在报错。

### 4.32.4 决定随包提供源码

这个 MOD 会**改写游戏内存里的字段**。包里加一个 `source\`（`Nioh1PerfectGuard.c`
约 1900 行 + `pg_logic.h`），玩家可以自己核对到底写了哪些偏移、为什么，
不必相信一个黑盒 DLL。**它不是运行所必需的**，文档也这么说；
单独编译还需要 Zig 工具链，所以包里不含构建脚本。

`test_doc_markers.py` 的语料也把 `CHANGELOG.md` 纳入了 —— 它同样是随包交付的文档，
里面提到的日志行也该被解释。

---

## 4.33 【已修·严重】`Enabled=0`（文档承诺的"回退开关"）会把断点装上却不装处理器

### 4.33.1 怎么发现的

这一轮本来只是去核对 `docs\方案计划.md` 的承诺是否兑现，其中一条是
阶段4的验收标准"**关掉 MOD 后游戏行为与原生完全一致（可回退）**"。
那就得实测 `Enabled=0`。结果日志是：

```
ANCHOR 4/4 verified
STATUS BYPASS: Enabled=0          ← 自称已旁路
ARM pass: 373 threads seen, 373 newly armed      ← ★ 却给 373 条线程装上了断点
ARM purge: dropped 1 stale thread id(s)
ARM rolling re-arm: first full cycle done ...
```

而且**整份日志里没有 `INSTALL step=veh`**。

### 4.33.2 根因

安装块本身是对的 —— `AddVectoredExceptionHandler` 与首次 arm 都在
`if (g_cfg.enabled && g_valid_count >= 2)` 里面。但 worker 主循环里那段：

```c
while (1) {
    if (++pass % 16 == 0) { arm_new_threads(); rearm_batch(); }   // ★ 无条件执行
    ...
}
```

**从不检查是否装过。** 于是只要进程活着，它就会周期性地把 DR0–DR3 写到每条线程上。

**这是最坏的一类失败**：断点装好了，异常处理器却没装。
CPU 在命中地址抛出 `EXCEPTION_SINGLE_STEP`，无人处理 → **进程直接崩**。
受影响的不只是 `Enabled=0`，还有锚点校验失败（`NOT INSTALLED`）的情况 ——
那条路径同样会装断点却没有任何处理器。

自检（`SELFTEST`）也帮不上：它只在安装块里跑，旁路时根本不执行，
所以"机制是否可用"的证明也一并缺失。

### 4.33.3 修法：装断点前必须先有处理器

```c
g_veh = AddVectoredExceptionHandler(1, veh_handler);
if (!g_veh) {
    log_line("WARNING: AddVectoredExceptionHandler failed (err=%lu); refusing to arm "
             "any breakpoint at all", GetLastError());
} else {
    g_installed = 1;        // ★ 先置位，再允许装断点
    arm_new_threads();
    ...
}
```

并在两个装断点的函数入口加守卫（纵深防御，防止将来新增调用点再犯）：

```c
static void arm_new_threads(void) {
    if (!g_installed) return;   // 没有处理器时装断点 = 崩溃，而不是降级
    ...
}
static void rearm_batch(void)    { if (!g_installed) return; ... }
```

同时新增状态 `NOT INSTALLED(no handler)`，避免"自称 ACTIVE 其实没有处理器"。

### 4.33.4 实测确认

| 配置 | 修复前 | 修复后 |
| --- | --- | --- |
| `Enabled=0` | `STATUS BYPASS`，但 **373 线程被装断点**、无处理器 | `STATUS BYPASS: Enabled=0`，**ARM/INSTALL 行数为 0**，游戏存活 |
| `Enabled=1` | 正常 | `ANCHOR 4/4` → `SELFTEST OK: 5/5` → `STATUS ACTIVE`，滚动重装正常 |

### 4.33.5 顺带修掉：运行中改 `Enabled` 会静默无效

安装是启动时的一次性决定，所以运行中把 `Enabled` 从 0 改成 1 不会装任何东西，
而日志什么都不说。现在会打：

```
NOTICE: Enabled changed to 1, but installing or removing the breakpoints happens
only at startup. Restart the game for this key to take effect; every other key
applies immediately.
```

README 也补了「想关掉 MOD / 回退」一节：临时关闭（`Enabled=0` + 重启）、
完全卸载（连同 `dinput8.dll`）、以及为什么"卸载就是删文件夹"。

### 4.33.6 教训

**"关掉它"的路径和"打开它"的路径一样需要测。** 我此前所有实机测试都是
`Enabled=1`；旁路路径从来没跑过，而它恰好藏着最严重的问题。
另外，这也是"回头核对最初承诺"的直接收益 —— 如果只测主路径，
这个 bug 会一直留到你按文档去关 MOD 的那一刻才炸。

---

## 4.34 与最初方案（`docs\方案计划.md`）的逐条对照

已把对照表写进交付包 `CHANGELOG.md` 第五节，要点：

- **按计划交付**：窗口/回精/减免/对敌削精与 HP/提前接续/音效/INI 热更新/F10 导出、
  删除仁王2 专有功能、不改原生判定、失败不安装、SHA256SUMS + 双语文档。
- **有出入**：锚点用**固定 RVA + 期望字节校验**而非 AOB 扫描
  （仁王1 已停更、RVA 不漂移；代价是只适用于 1.24.8，换版本会被拒绝而非勉强适配）；
  **受击反应（轻重冲击）未实现**（方案本身就标"待定"，需实机标定 ID）；
  **视觉特效已砍**（方案标可选，且你选择了只保留音效）。
- **风险清单的实际落地**：加密 `.text` 用运行时镜像导出解决；
  锚点 C（成功格挡事件）**没有降级** —— 定位到扣精点并用资源表归属精确判定格挡方；
  确认仁王1 无原生及时格挡状态位，改为自行判定。

---

## 4.35 逐场景推演验收流程：两处会让"你按文档做却看不到该看到的日志"

这一轮不写新功能，而是把验收文档里要求你做的 6 个场景**逐条对着代码走一遍**，
检查"按文档操作后，那一行日志是否真的会出来"。

### 4.35.1 【已修】`KITRACE` 只看 entry-7，"不防御挨打"时几乎不出行

`KITRACE` 的触发条件是"值与上次采样不同"，而它**只比较 entry-7**（`param+0xDEC`）：

```c
int moved = (g_trace_ecur < 0.0f) || (ecur != g_trace_ecur);
```

但验收场景 1 是"站着不动挨打 5 次（不防御）"。挨打改变的是**可见精力**
`param+0x40`，而 entry-7 是**格挡**资源项 —— 不格挡时它很可能根本不动。
于是场景 1 只能靠 5 秒一次的心跳勉强出行，文档承诺的"`KITRACE` 有输出"变得很弱。

而且即使出了行，**读者也无法判断是哪个字段变了** —— 而"哪个字段会动"
恰恰是 `KIV` 要回答的那个问题的原始数据。

修法：**两个字段各自比较、并标明是谁变了**：

```
KITRACE ki=210.5/250 | entry7 flag=1 cur=1180 cost=6.4  <-- changed: ki
KITRACE ki=204.1/250 | entry7 flag=1 cur=1180 cost=6.4  <-- changed: ki AND entry7
```

这样场景 1 直接看到 `changed: ki`（证明"当前精力"字段活着），
场景 3 则在格挡前后看到是 `ki`、`entry7` 还是两者同时跳 —— 一次游玩就能把
`KIV` 的结论和原始数据对上。

代价：`ki` 在游戏里几乎一直在变（精力回复），所以触发行会变多 ——
但**限流本来就是为此存在的**（1 行/秒）。

### 4.35.2 【已修】`KIV` 的上限恰好等于验收流程的需求量

`KIV` 最多打 10 行（`g_ki_verdicts >= 10` 就停）。而验收流程是：
场景 3 做 5 次精防 + 场景 5 再做 5 次 = **正好 10 次**。

也就是说，**回答最后一个未知问题的那一行，会在场景 5 结束的同一刻用光**。
只要你先随手练几下、或换个顺序做，`KIV` 就会在做关键观察之前静默消失。

既然这些上限只是为了控制文件大小，就整体放宽留余量：

| 日志 | 旧上限 | 新上限 |
| --- | --- | --- |
| `PERFECT GUARD` | 30 | 60 |
| `LAYOUT` | 10 | 20 |
| **`KIV`** | **10** | **40** |
| `KIRESTORE` | 15 | 30 |
| `KITRACE` | 600 | 1200 |
| `ENEMY *` | 15 | 30 |
| `RECOVERY cancel` | 10 | 20 |
| `GUARD cost=` | 20 | 40 |
| `PLAYER BLOCK` | 20 | 40 |
| `GUARD FLAG` | 30 | 60 |
| `GUARD (other)` | 6 | 12 |

合计最坏情况约 350 行 ≈ 32 KB，仍然很小。

### 4.35.3 顺带在文档里说清"详细行停了我还能看什么"

验收文档新增一节：所有逐条事件都有上限；**随时按 `Ctrl+Shift+F10`**
就会打 `MANUAL EXPORT` + 一组 `STATE`，里面有累计的
`blocks= / perfect= / rewards= / freeguards= / rearmed=` —— 总量永远看得到。
另外也写清了场景 1 与场景 3 里 `KITRACE` 各自该看什么。

### 4.35.4 其余 4 个场景的推演结论（无需改动）

| 场景 | 结论 |
| --- | --- |
| 2（按住防御挨打 → `PLAYER BLOCK`） | ✓ 按住不放不产生新按下沿，窗口关闭 → 每次都走"窗口外"分支并计数 |
| 4（敌人格挡我的攻击 → `GUARD (other)`） | ✓ 由资源表归属判定，与奖励路径互斥 |
| 5（对敌效果） | ✓ `g_last_cost` 在门判定之前就记好，顺序正确（这是早期修过的点） |
| 6（后摇取消） | ✓ 有 `0 ≤ frame < 10000` 的范围闸门，不会盲写动画状态 |

---

## 4.36 【已修·严重】`KIV` 在**默认配置下会给出错误结论**

上一轮结尾我提出要检查 `KIV` 的判定阈值。结果发现的问题比阈值更严重：
**在出厂默认值下，这道题在数学上无解，而旧代码会武断地选一个错答案。**

### 4.36.1 三个模型在默认值下退化成两个

`KIV` 的三个候选模型（`r` = `KiDamageReductionPercent`）：

| 模型 | 预测的可见损失 |
| --- | --- |
| 字段没动 | `L = 0` |
| 吃了全额 | `L = D` |
| 吃了缩放后 | `L = D × (1 − r/100)` |

默认 `r = 100`，于是第三个模型 `= D × 0 = 0` —— **和"字段没动"是同一个数字**。
而旧的 if/else 链是**先判断 `L < band`** 的：

```c
if (l < band)            verdict = "...字段根本没动，所以那张表和精力条无关";
else if (l >= d - band)  verdict = "...吃了全额...";
else if (l <= scaled + band) verdict = "...吃了缩放后...";
```

`L = 0` 会命中第一个分支 → 打印出**"字段没动，资源表与精力条无关"** ——
一个语气确定、但完全错误的结论。而"缩放后恰好为 0"才是同样合理的解释。

`r = 0` 时对称地退化：第三个模型 `= D`，和"吃了全额"重合。

**也就是说：拿默认 INI 去打，`KIV` 会给出一条错的判词。** 这比没有判词更糟。

### 4.36.2 修法：换成"最近模型 + 拒绝回答"的分类器，并做成可测试的纯函数

新增 `pg_kiv_classify(D, L, r)`（放在 `pg_logic.h`，与 MOD 共用）：

1. 按**最近模型**分类（不再是 if/else 链的顺序敏感判定）；
2. **可分辨性检查**：任意两个模型若相距不足 `0.20 × D`，直接拒绝回答
   —— 这就把 `r=100`、`r=0`、`r=10` 这类配置判为不可分辨；
3. **领先幅度检查**：最优模型必须比次优模型至少近一倍
   （`bestd ≤ 0.5 × secondd`），否则拒绝 —— 避免把恰好落在中间的损失
   硬分给"稍微近一点"的那个；
4. **拟合质量**：残差 `> 0.25 × D` 也拒绝。

同时新增 `pg_kiv_resolvable(r)`，明确"可分辨区间是 **20–80**"。
日志里也多了 `residual=..%`，让人能自己判断可信度。

### 4.36.3 测试（新增 20 条断言，67/67 全过）

关键几条：

```c
CHECK(pg_kiv_classify(D, 0.0f, 100).model == PG_KIV_INCONCLUSIVE,
      "reduction=100 must not claim 'the field did not move'");   // ★ 就是旧代码的错
CHECK(!pg_kiv_resolvable(100) && !pg_kiv_resolvable(0) && !pg_kiv_resolvable(10));
CHECK(pg_kiv_resolvable(50) && pg_kiv_resolvable(20) && pg_kiv_resolvable(80));
CHECK(pg_kiv_classify(D, 0.0f, 50).model == PG_KIV_NO_MOVE);
CHECK(pg_kiv_classify(D, D, 50).model == PG_KIV_FULL_COST);
CHECK(pg_kiv_classify(D, D*0.5f, 50).model == PG_KIV_SCALED_COST);
CHECK(pg_kiv_classify(D, D*0.30f, 50).model == PG_KIV_INCONCLUSIVE);  // 落在模型之间
```

还加了抗噪断言（`D/2 ± 0.4` 仍判为缩放后）。

> 写测试时我又先写错了一次期望：最初只打算按"残差"判定，
> 于是断言 `L = 0.30D` 会因残差过大而被拒。但算一下几何就知道：
> `r=50` 时相邻模型相距 `0.5D`，任何损失的残差**最多只有 `0.25D`**，
> 残差判据在这种情况下永远不会触发。这才促使我改用"领先幅度"这个真正
> 有区分力的判据 —— 是测试逼出了更好的设计。

### 4.36.4 对验收流程的影响（重要）

既然默认值答不出这道题，**验收步骤必须改**：

- 验收文档场景 3 现在写明：**先把 `KiDamageReductionPercent` 改成 `50`**，
  再做 5 次精防；测完再改回喜欢的数值（热更新）。
- `KIV` 行的说明补上了 `INCONCLUSIVE by configuration` 等三种拒绝情形，
  以及"20–80 可分辨、50 最清晰"。
- README 中英文同步更新，并明确写出：**默认 100% 下这一项答不出来，
  这不是 bug，是问题的固有限制**。

---

## 4.37 【已修】"格挡后立刻反攻"会把攻击的精力消耗也当成格挡耗精补回来

上一轮结尾我提出要检查"引擎把耗精写进可见字段的时刻是否晚于 400ms 窗口"。
查下去发现同一段代码还有两个更要紧的问题。

### 4.37.1 补回的基准是"字段掉了多少"，而不是"格挡该扣多少"

旧逻辑：

```c
float loss = s->snapshot - now;
if (loss > s->peak_loss) s->peak_loss = loss;
float want = s->peak_loss * reduction / 100.0f;   // ★ 按"观察到的掉落"补
```

而**引擎被要求扣多少（D）我们其实是知道的** —— 扣精点那里抓过。

问题在于：精防之后**立刻反攻**是最自然的操作。此时字段的掉落 =
`格挡耗精 + 攻击耗精`，而旧代码会按这个**总和**的比例补回：

> 精防 → 反攻 → 攻击几乎不花精力（甚至被完全补回）。

这是一个**可利用的游戏性 bug**，不是纯粹的诊断问题。

修法：以**已知的 D** 为上限。

```c
static inline float pg_ki_attributed(const PgKiRestore *s) {
    float base = s->attrib_loss;
    if (s->charge > 0.0f && base > s->charge) base = s->charge;   // ★ 封顶
    return base;
}
float want = pg_ki_attributed(s) * reduction / 100.0f;
```

于是：格挡那部分的 `r%` 照补，超出 `D` 的部分（也就是你自己花的）原样保留。

### 4.37.2 归因只看格挡后 **250ms** 内的掉落

紧接上一个问题：即使封了顶，如果**先**反攻再被记录，掉落仍可能被当成格挡耗精。
所以新增归因窗口 `PG_KI_ATTR_MS = 250`：

- **250ms 内**看到的掉落 → 算格挡耗精（`attrib_loss`）；
- **之后**的掉落 → 只用于 `peak` 展示，不参与补回、也不参与判定。

选 250ms 的理由：耗精是引擎在一两帧内写进去的（≤35ms），而"玩家输入一个动作"
现实上需要更久。

### 4.37.3 顺手把"引擎延迟"变成可读数字

这一轮原本要查的问题（窗口够不够长）现在有了直接答案：`PgKiRestore` 记录
**第一次看到掉落的时刻**，`KIV` 行打印出来：

```
KIV block #3 charge D=6.4 visible loss L=6.4 scaled D*(1-r)=3.2 residual=0%
    first loss after 32ms peak=6.4 -> ...吃了全额...
```

- `first loss after 32ms` → 引擎延迟很小，400ms 窗口绰绰有余；
- 若显示 `[loss arrived after the attribution window]` → 说明耗精落得比 250ms 还晚，
  那么补回**没生效**、判定也**不成立**，需要把窗口调长（而不是悄悄给出错误结论）；
- 若显示 `[no loss seen at all]` → 这个字段确实没动。

也就是说：**这条失败路径现在会被明确报出来，而不是伪装成"字段没动"。**

### 4.37.4 测试（新增 14 条断言，81/81 全过）

覆盖：

- 格挡耗精照补，**600ms 后反攻花掉的 50 点精力原样保留**（回归护栏）；
- 归因窗口内掉落大于 `D` 时，**只补 `D` 的那份**；
- 未抓到 `D` 时封顶失效但功能不受影响；
- 记录首次掉落时刻；
- **掉落晚于归因窗口时**：`attrib_loss` 为 0（不算格挡耗精）、`peak` 仍记录、
  延迟仍被记录 —— 供日志明确报出。

> 写测试时我又先算错了一次：断言"反攻后净损失 = 50"时，
> 我给出的当前值把已经补回的格挡耗精又扣了一遍。修正的是**测试的模型**，不是代码。

---

## 4.38 把 `KIV` 的三种结论都变成"你现在就能改配置"的事

### 4.38.1 问题：两种结论可以靠配置修，但其中一种当时没有开关

`KIV` 会给出三种结论，而它们对应**三种不同的实现**：

| 结论 | 正确的实现 |
| --- | --- |
| 掉了**全额 D**（缩放够不到精力条） | 保留"补回"，它才是真正省精力的那一步 |
| 字段**根本没动**（资源表与精力条无关） | 同上，只能靠补回 |
| 掉了 **D×(1-减免%)**（缩放**已经**够到精力条） | **关掉补回**，否则两条机制都减 = 重复计算 |

前两种都能用默认配置，**但第三种没有办法关掉补回** —— 当时只能等我改代码出新版本。
而第三种恰好是"我这边需要一个决策"的那种情况，一来一回就是一轮。

修法：把这件事**提前做成一个配置项**。

### 4.38.2 新键 `KiTopUp`（默认 1）

```c
int effective = g_cfg.ki_topup ? g_cfg.ki_reduction_percent : 0;
float after = pg_ki_step(&g_ki, before, effective, now_ms());
```

- `KiTopUp=1`（默认）：照旧补回，`KIV` 说是前两种世界时正确；
- `KiTopUp=0`：**只采样不写**，完全依赖扣精点的缩放 —— `KIV` 说是第三种世界时正确。

于是无论实机给出哪种结论，**都能靠改一行 INI 得到正确行为**，不需要新构建。

### 4.38.3 顺带修掉：关掉补回会连带失去诊断能力

旧代码在 `ki_reduction_percent <= 0` 时**直接 return**，连采样都不做 ——
也就是说一旦把减免设为 0（或现在把补回关掉），`peak_loss`、`attrib_loss`、
首次掉落延迟全都拿不到，`KIV` 直接失效。而"关掉补回"恰恰是最需要
继续看诊断的那种状态。

所以把 `pg_ki_step` 拆成两件事：**采样无条件进行**（只要有窗口），
**写入才受 reduction 控制**：

```c
    if (s->first_loss_ms == 0) s->first_loss_ms = now_ms_v;
    if (loss > s->peak_loss) s->peak_loss = loss;
    ... 归因 ...
    if (reduction_percent <= 0) return now_value;    // 只观察，不写
```

新增测试断言：`reduction=0` 时**不写入**，但延迟仍被记录、损失仍被归因、`given` 仍为 0。

### 4.38.4 文档：给出"结论 → 该改什么"的对照表

验收文档与中英文 README 都加了这张表，并写清**后果**：

> 若结论是第三种而 `KiTopUp` 仍是 1，那么设 `KiDamageReductionPercent=50`
> 时实际会减掉 **75%**（`1 − 0.5×0.5`）—— 不是按你写的数字生效。

这属于"你拿到结论后想知道下一步"的信息，现在不用再问我。

### 4.38.5 测试

`sanity: 85 passed, 0 failed`（新增 4 条：`reduction=0` 时只观察不写入）。
INI 与内置默认值一致性检查（`test_ini_encodings.py`）自动覆盖了新增的 `KiTopUp` 键。

---

## 4.39 【新增验证】"测试过的算术"是否等于"发货的算术"

### 4.39.1 问题：测试与发货是两个不同的构建产物

`pg_logic.h` 被编译**两次**：

- 进 `tools\test_logic.exe`（单元测试跑的就是它）；
- 进 `Nioh1PerfectGuard.dll`（游戏实际加载的）。

同一份源码，但**不是同一个产物**（exe 与 shared library、可能是不同的编译参数）。
于是"单元测试覆盖了发货代码"这句话里藏着一条从未验证的假设：
两边的**浮点代码生成**必须一致。若编译器做了 FMA 收缩、重结合，
或某个内联决策改变了中间舍入，测试可以全绿而实机行为不同 ——
这是最难查的一类问题。

### 4.39.2 做法：数值指纹

在 `pg_logic.h` 里加 `pg_logic_digest()`：把一大组固定输入
（窗口边界、掩码组合、补回/归因网格、`KIV` 分类网格、限流序列）
依次灌进那些纯函数，用 FNV-1a 折叠**浮点的位模式与整数结果**，返回 32 位指纹。
它只用局部变量、不碰 libc。

- DLL 导出 `PG_GetLogicDigest`；
- `test_logic.exe --digest` 打印自己的值；
- `tools\test_logic_digest.py` 比较两者，不一致就失败。

### 4.39.3 结果 + **反向对照**（否则这个检查等于没做）

```
test_logic.exe          : 6B509D8E
Nioh1PerfectGuard.dll   : 6B509D8E
tested arithmetic == shipped arithmetic: True
```

为了确认这个检查**真的会失败**，做了两个对照构建：

| 构建 | 指纹 | 说明 |
| --- | --- | --- |
| `-O2`（基准） | `6B509D8E` | — |
| `-O2 -ffast-math` | `B6793929` | **不同** → 检查确实能抓到重结合/不安全浮点优化 |
| `-O0` | `6B509D8E` | **相同** → 优化等级本身不影响 IEEE 结果，检查不是"过度敏感" |

这正是想要的灵敏度：**对真正的浮点语义变化敏感，对优化等级不敏感**。
所以"指纹相同"是有信息量的结论，不是同义反复。

### 4.39.4 顺带

导出数从 11 变 12，`CHANGELOG.md` 与笔记里的计数一并更新
（这类小数字最容易过期，正好被这轮的核对逮到）。

---

## 4.40 交接摘要 + 文档里的数字也必须对得上

### 4.40.1 新交付文档 `交接摘要.md`

目标：让任何人**不读 2600 行笔记**也能在 30 秒内知道这个项目处于什么状态。
内容分八节：目标与阶段状态、交付物清单、**已证明的部分（六套检查 + 实机证据）**、
**还差的 4 项（全部需要实机）**、已知限制、恢复上下文所需的关键事实
（游戏路径、锚点地址、字段偏移、工具链、AI 为何进不了关卡）、
一路修掉的严重问题清单、下一步。

### 4.40.2 【已修】文档里的数字过期了 —— 而且不止一处

写摘要时顺手核对数字，发现**六处已过期**：

| 位置 | 写的 | 实际 |
| --- | --- | --- |
| `CHANGELOG.md` | 47 条断言 | **85** |
| `CHANGELOG.md` | 23/23 标记、78 条日志 | **24/24、83** |
| `CHANGELOG.md` | KITRACE 上限 600 行 | **1200**（第 28 轮放宽过） |
| `RE_NOTES.md` 自检表 | 导出表应为 11 个 | **12** |
| `RE_NOTES.md` 状态段 | 47 条断言 | **85** |
| `交接摘要.md` | 同上标记/日志数 | 已按实测写 |

这类错最容易被信任 —— 因为它**看起来非常具体**。之前"11 个导出"也是这样被漏掉的。

（另外注意：`RE_NOTES.md` 里**每轮记录的历史数字**（如 4.25.3 的"30 条断言"、
4.36.3 的"67/67"）是**正确的历史**，不该改。所以下面的检查只针对"当前状态"文档。）

### 4.40.3 新增检查：`tools\test_doc_counts.py`

**实测**这四个数字，再去 `CHANGELOG.md` 与 `交接摘要.md` 里把**文档声明的**数字
用正则抓出来比对：

| 量 | 最新实测 |
| --- | --- |
| 导出数 | 12 |
| 单元测试断言数 | 85 |
| 已文档化的日志标记数 | 24 |
| 源码里的 `log_line` 字符串数 | 83 |

`RE_NOTES.md` 被刻意排除在检查范围外 —— 它是按时间顺序的笔记本，
每轮的计数作为历史是正确的。

### 4.40.4 反向对照（否则等于没做）

把 `CHANGELOG.md` 里的"85 条断言"改回"47 条"，检查立刻变红：

```
stated counts match measured counts: False
  FAIL CHANGELOG.md states 47 assertions, measured 85
exit=1
```

（第一次做这个对照时，我用来注入的命令因为 PowerShell 反引号转义**静默失败**了 ——
"对照通过"其实是"什么都没改"。发现后改用编辑工具重新注入，才真正验证了检查有效。
**验证一个检查器时，必须确认注入真的发生了**，否则会得出"检查有效"的假结论。）

---

## 4.41 把"判定链"本身也变成共享且可测的（而不只是链上的零件）

### 4.41.1 为什么零件级测试不够

前面几轮已经把**零件**都做成了共享纯函数并测过：窗口判定、边沿检测、掩码匹配、
精力补回、`KIV` 分类、限流。但这个项目里最严重的那个 bug（§4.20）
**不是零件错，而是顺序与主语错**：

> 格挡标志位的语义是"**我的攻击被格挡了**"。
> 于是"敌人挡下我的攻击"也走了同一条奖励路径。

每一个判定单独看都是对的 —— 错的是"先问谁在格挡、再问时机"这个**组合与次序**。
而组合当时只存在于 MOD 里，测试里只能手工重搭一遍，**重搭出来的顺序未必和发货一致**。

### 4.41.2 做法：把链本身搬进共享头文件

新增：

```c
typedef struct { unsigned long long press_ms; int down; long presses; } PgGuardInput;
static inline int pg_guard_update(PgGuardInput *g, int down, unsigned long long now);
static inline int pg_guard_gate(const PgGuardInput *g, int gate_timely, ...);

typedef enum { PG_BLOCK_OTHER, PG_BLOCK_LATE, PG_BLOCK_PERFECT } PgBlockOutcome;
static inline PgBlockOutcome pg_classify_block(int entry_is_player,
        const PgGuardInput *g, int gate_timely, unsigned long long now, int window_ms);
```

`pg_classify_block` 把**次序作为契约的一部分**写死在注释与实现里：

```c
    if (!entry_is_player) return PG_BLOCK_OTHER;      // ★ 先问"谁在格挡"
    if (!pg_guard_gate(...)) return PG_BLOCK_LATE;     // 再问时机
    return PG_BLOCK_PERFECT;
```

MOD 侧相应重构：`on_guard_cost` 现在只调用 `pg_classify_block`，
`poll_guard_button` 用 `pg_guard_update`，`perfect_gate_open()` 这个函数被删除
（它被 `pg_guard_gate` 取代）。门状态从三个散落的全局（`g_guard_press_ms`、
`g_guard_down`、`g_guard_presses`）收敛成一个 `PgGuardInput`。

> 附带修掉一处**状态重复**：`g_guard_down`/`g_guard_press_ms` 在重构中一度变成
> "只读不写"的僵尸全局（STATE 行会永远显示 0）。这正是把状态拆成两份的代价 ——
> 现在单一来源，STATE 与 `PG_GetInputInfo` 直接读 `g_guard_in`。

### 4.41.3 场景测试（新增 15 条断言，100/100 全过）

不再只测零件，而是**驱动整条链**：

| 场景 | 断言 |
| --- | --- |
| 按下后 10ms 格挡 | `PERFECT` |
| 按下后 400ms 格挡 | `LATE` |
| **按住不放**（每 8ms 喂一次按下）后格挡 | 只算 1 次按下，且判为 `LATE`（按住不续窗） |
| 从未按下 | `LATE`，永不 `PERFECT` |
| **敌人格挡，时机完美** | 仍是 `OTHER` |
| **敌人格挡后再由玩家格挡** | 玩家的窗口**没有被消耗**，玩家那次仍 `PERFECT` |
| 松开再按 | 窗口重新打开，`presses == 2` |
| 一次窗口内三连击 | 三次都 `PERFECT`，窗口外那次 `LATE`（已文档化的语义） |
| `RequireTimelyGuard=0` | 玩家任何格挡都算，**但别人的格挡仍然忽略** |
| 按键抖动序列 `0,1,1,0,0,1,1,1,0` | 恰好 2 次按下（抖动不能造出假按下） |

### 4.41.4 链也纳入"测试算术 == 发货算术"

`pg_logic_digest()` 新增第 6 组：遍历窗口长度 × `gate_timely` × 一组按下序列 ×
滞后 × 主语，把 `pg_guard_update` / `pg_classify_block` 的结果折叠进指纹。

指纹因此从 `6B509D8E` 变为 **`A47F4E2C`**，而两个构建**仍然一致** ——
说明新增的链逻辑同样在"测试的就是发货的"覆盖之下。

### 4.41.5 顺带：新增的文档数字检查立刻抓到了自己

重构后断言数从 85 变 100，`tools\test_doc_counts.py` 立刻报：

```
FAIL CHANGELOG.md states 85 assertions, measured 100
```

上一轮刚加的检查，这一轮就用上了 —— 改完文档后重新通过。
（**这就是那类"看起来非常具体的过期数字"**，上次靠人眼漏了两次。）

---

## 4.42 长会话审计：找到一条"每秒 2.5 行"的无限日志路径

### 4.42.1 做法：把"每条日志是否有限"变成可机械核对的事

写工具 `tools\audit_log_sites.py`：扫出全部 `log_line(` 调用点，
打印每个调用点**上方 26 行**，并判断那附近有没有 `<\d+` / `% N ==` /
`_log` / `first_` 之类的上限痕迹。它的价值不是"自动判定"（必然会误报），
而是**强迫逐条过一遍**——83 个调用点，人眼扫一遍很快，但很容易漏掉某一条。

### 4.42.2 真找到的两条无上限路径

| 位置 | 触发频率 | 后果 |
| --- | --- | --- |
| `ARM snapshot failed err=` | **每 400ms**（`arm_new_threads` 的失败分支） | 若 `CreateToolhelp32Snapshot` 持续失败 → **2.5 行/秒 = 9000 行/小时**，而且发生在**故障路径**上——正是最需要日志可读的时候 |
| `ARM purge: dropped …` | 每约 25s（有死 id 时） | 线程频繁创建/销毁的会话里每次一行 ≈ 140 行/小时 |

第一条是全项目**唯一**"秒级"泄漏，而且是失败路径 —— 这类问题只在长会话显现，
而玩家的实机游玩很可能就是长会话。

修法：各自加计数上限（快照失败 5 条、purge 20 条），
**累计值改由 `STATE` 行承载**（新增 `purged=` 与已有的 `rearmed=`），
信息不丢、日志不涨。

修完复审：**83 个调用点，0 个看起来无上限**。

### 4.42.3 顺带纠正审计器自己的误报

首轮报 8 条"无上限"，逐条分诊后发现只有 2 条是真的，其余 6 条是：

- `log_line` **自身的定义**里那个时间戳格式串（被当成调用点）；
- `KIV` / `LAYOUT` 的**上限在函数开头**，超出我一开始设的 10 行回看窗口；
- 启动期只打一次的 `=== 版本 ===`、`base(nioh.exe)=…`。

于是把回看窗口放宽到 26 行、排除函数定义、并补充"一次性"关键字。
**先分诊、再据此改工具** —— 而不是为了让输出好看去放宽判据。

### 4.42.4 长会话实测（6 分钟空转，12 次采样）

见本节末表格。要点：日志行数在启动后**基本不再增长**（只有每约 25s 的滚动重装
维护行），证明上限确实生效；游戏全程存活。

（详细采样结果随本轮实机记录附在下方。）

### 4.42.5 计数器溢出复核

所有跨线程计数器都是 `LONG`（32 位有符号）。按最坏速率估算：

| 计数器 | 递增频率 | 溢出所需 |
| --- | --- | --- |
| `g_play_req` / `g_play_done` | 每次精防 | 数十年（且是"差值"语义，回绕无影响） |
| `g_trace_pass` | 每 25ms（40/s） | ≈ 1.7 年 |
| `g_blocks` / `g_perfect` 等 | 每次事件 | 数十年 |

`g_armed_n` 由 `purge_dead_armed` 定期压缩，规模跟随"当前存活线程数"，
不会随时间单调增长。结论：**无需处理**，但已记录以便将来复核。

---

## 4.43 长会话实测撞出一次游戏崩溃 —— 定位到"滚动重装"，并意外发现标题画面会跑战斗演示

这一轮本来只打算审计"计数溢出与日志无限增长"，结果在**长会话实测**里撞出真正的崩溃。

### 4.43.1 崩溃本身

6 分钟空转后游戏消失。查 Windows 事件日志：

```
出错应用程序名称: nioh.exe ... 异常代码: 0xc0000005
Windows Error Reporting: APPCRASH
```

并且留下了 87.7 MB 的 minidump。写 `tools\analyze_dump.py` 解析它（不需要调试器）：

```
nioh.exe: base=0x7FF7AA2D0000
exception code : 0xC0000005        (ACCESS_VIOLATION)
fault address  : nioh.exe+0x52F321
param[0] = 0, param[1] = 0          -> 访问地址 0，空指针解引用
Dr0 = base+0x74DD09   Dr1 = base+0x74DD92   Dr2 = base+0x74DE51   Dr3 = 0
Dr7 = 0x415            (L0/L1/L2 使能)
Dr6 = 0x00000000FFFF0FF0            -> B0..B3 全 0：没有断点条件被命中
Rip = nioh.exe+0x52F321             -> 不是任何锚点
Rcx = 0
```

反汇编故障点：

```asm
0052F316  mov rcx, qword ptr [rsi + 0x48]   ; rcx = this->[0x48]
0052F321  mov r8, qword ptr [rcx]           ; ★ rcx == 0 → 读地址 0 → AV
0052F324  call qword ptr [r8 + 0x60]
```

**结论：崩在游戏自己的空指针上（`this+0x48` 为空），与锚点无关，Dr6 也证明没有断点被命中。**
但 Dr0–Dr2 仍在，说明 MOD 是"装着的"。

### 4.43.2 A/B 定位：不是断点本身，是"滚动重装"

崩溃不可归因，就必须能**逐部件关掉**。于是加了一个诊断位掩码 `DiagDisable`
（1=不装断点 / 2=不启动输入线程 / 4=开滚动重装 / 8=不跑自检），然后逐个跑长会话：

| 运行 | 配置 | 结果 |
| --- | --- | --- |
| A | 装断点 + **滚动重装开** | **5.4 分钟崩溃** |
| B | `Enabled=0`（完全不装） | 8 分钟正常 |
| C | 装 VEH、**不装断点** | 8 分钟正常 |
| D | 装断点、**滚动重装关** | 9 分钟正常 |
| E | 同 D（本想复现 A，但 INI 改写静默失败，实际仍同 D） | 9 分钟正常 |

→ **可归因到"滚动重装"那条路径**（§4.24 我自己加进去的"修复丢失的 DR 状态"）。
它每 400ms 挂起 6 条活着的游戏线程并改写其调试寄存器，5 分钟约 **750 次挂起 + SetThreadContext**，
对象是"游戏认为自己掌控调度"的线程。

**修法：默认关闭它。** 功能上没有任何损失 ——
新线程由 `arm_new_threads()` 在 400ms 内装上，被回收的线程 id 由 `purge_dead_armed()` 处理；
滚动重装想修的"DR 状态丢失"**从未被证实存在**。现在它只能由 `DiagDisable=4` 打开。

> 诚实说明证据强度：只有**一次**崩溃观测（A），而关掉后有 **3 次**干净运行。
> 单一观测不足以证明因果，但这条路径功能上冗余、代价是持续干扰游戏线程，
> 默认关闭是明显正确的取舍。若你在实机长会话里仍遇到崩溃，`DiagDisable` 就是现成的排查工具。

### 4.43.3 意外收获：标题画面会运行战斗演示，锚点会真的触发

同一份 9 分钟日志（D/运行 E）里出现了**没有人操作**的格挡事件：

```
23:06:58 STATE (player object appeared - now in a mission) ... hp=880/880 ki=100/96
23:06:58 STATE input ... pad=0x0000 conn=[1,0,0,0] guard_down=0 presses=0 lt=0 rt=0
23:07:23 GUARD pressed (pad=0x2100 mask=0x0100 key=0x00 key_down=0)
23:08:57 GUARD FLAG #1 set via guard_flag_1 (ctx=0x1F5E6B1B130)
...
23:14:28 GUARD pressed (pad=0x0100 mask=0x0100 key=0x00 key_down=0)
23:14:28 GUARD FLAG #8 set via guard_flag_1 (ctx=0x1F5E6B219B0)
```

统计：`GUARD pressed` **60 次**、`GUARD FLAG` **8 次**（全部 `via guard_flag_1`）、
`PLAYER BLOCK` **0**、`PERFECT GUARD` **0**、`GUARD (other)` **0**、`GUARD cost=` **0**。

也就是说：**标题画面的演示模式（attract mode）会喂入合成手柄输入并跑真正的格挡判定代码。**
这有两个直接后果：

1. **锚点"真的会执行"第一次被实机证实**（不再只有自检证明机制可用）：
   `guard_flag_1`（RVA `0x74DD09`）在真实游戏进程里执行了 8 次，
   其中多次发生在"按下沿之后 63–92ms"——**落在 250ms 窗口之内**。
2. **但权威事件源（扣精点 `0x74DE51`）一次都没触发。**

第 2 点非常重要，而且**正好是原本需要你游玩才能发现的那类信息**。可能的解释：

- 演示模式下的角色**不走扣精分支**（例如该角色的格挡资源未初始化 ——
  `STATE` 里 `guard_res flag=-1 cur=0 cost=100` 看起来正是未初始化的哨兵值）；
- 或者 `0x74DE51` 位于一个**有条件的分支**上，演示场景不满足该条件；
- 或者这条格挡是"攻击方被格挡"（那么扣精应发生在**敌人**身上，
  本该打出 `GUARD (other)` —— 但那条也是 0 次）。

**现在还不能断言锚点错了**，但这条不对称（旗标 8 次 / 扣精 0 次）必须查清，
因为它决定"权威事件源"是否真的权威。

### 4.43.4 这一轮的其他产出

- 新增 `tools\analyze_dump.py`：不依赖调试器，直接从 minidump 读异常码、故障地址、
  **Dr0–Dr7/Rip**，并判断故障地址是否落在锚点上。"游戏崩了，是不是 MOD 干的"
  从此有一条十分钟内能走完的路。
- 新增 `tools\audit_log_sites.py` 并修掉两条无上限日志（详见 §4.42）。
- 新增 `DiagDisable` 诊断位掩码（默认 0），并写进 INI 与验收文档 ——
  下次若再出现崩溃，你不必等我出新版本就能自己缩小范围。

---

## 4.44 扣精点为什么"该响却不响"：它有三个前置条件（并确认玩家确实在命中上下文里）

### 4.44.1 静态：`0x74DE51` 前面有三道门

从 `guard_flag_1`（`0x74DD09`）顺着控制流读到扣精点（`0x74DE51`）：

```asm
0074DDB5  cmp qword ptr [rax + 0x240], 0   ; ① 防守方的 param 必须存在
0074DDBD  je  0x74DEE2                     ;    否则整段跳过
0074DE09  test r12d, r12d                  ; ② [rsp+0x78] = [rsi+0x160]（进入时许存）
0074DE0C  jne 0x74DE56                     ;    必须为 0
0074DE32  mov edx, 7
0074DE37  call 0x7B21F0                    ;    取第 7 项（格挡资源项）
0074DE3C  cmp dword ptr [rax], 1           ; ③ ★ 该项的 state 必须**恰好等于 1**
0074DE3F  jne 0x74DE56                     ;    否则完全不扣精
0074DE41  movss xmm1, [rax + 0x10]
0074DE51  call 0x7B4C40                    ; ← MOD 的锚点
```

**所以"旗标响了但扣精点没响"并不矛盾**：旗标在 `0x74DD09` 是**无条件**写入的，
而扣精点要过三道门。这也说明把扣精点当"权威事件源"在语义上是**对的**
（它精确对应"引擎真的为这次格挡扣了精力"），代价是**条件不满足时什么都看不到**。

### 4.44.2 顺带纠正一处记法错误（结论未变）

笔记里曾把表基址写成 `[[char+0x240]+0xBA8]`（多了一层解引用）。
本轮把访问器反汇编出来，确认是**内联**的：

```asm
007B21F0  movsxd rax, edx
007B21F3  lea rdx, [rax + rax*4]
007B21F7  shl rdx, 4              ; i * 0x50
007B21FB  lea rax, [rcx + 8]      ; ★ 直接用 rcx，没有 mov rax,[rcx]
007B21FF  add rax, rdx
007B2202  ret
```

即 `base = param + 0xBA8` 内联，第 `i` 项在 `param + 0xBA8 + 8 + i*0x50`，
**第 7 项 = `param + 0xDE0`** —— 与 MOD 的 `entry_belongs_to_player`
（范围 `param+0xBB0 .. param+0x10B0`）和 `dump_state` 的读法**一致**。
`0x7B4C70` 的 `+0x08/+0x40/+0x48` 也逐项对上（`+0x00` 状态、`+0x38` 指针、`+0x40` 键）。
**结论：没有 bug，只是笔记的括号写错了。**

### 4.44.3 实机：玩家确实在命中上下文里，而且状态字是 -1

给旗标锚点加了 `FLAGDIAG` 诊断（转储命中上下文里双方 + 攻击方各自的关键字段）。
**第一版诊断自己写错了**：我假设 `mov [rcx+0xC8],1` 里的 `rcx` 是角色指针，
于是去读 `[rcx+0x240]` —— 实际 `rcx` 是经由 `char+0x40 → +0x90` 拿到的**子对象**，
读出来的是一堆垃圾（`param=0x8000000080000000` 之类）。改正后（改为从命中上下文
`ctx+0x100` / `ctx+0xE8` / `ctx+0x40` 取三方）拿到干净数据：

```
FLAGDIAG via guard_flag_1 ctx=0x1DE4E839DB0 ctx160=-1 |
  A=0x1DE3D7C38F0 ply=1 st=-1 cur=0 cost=100 |      ← ★ 玩家！
  B=0x1DE3D6C8D10 ply=0 st=-1 cur=0 cost=100 |
  atk=0x1DE9E6F6F30 ply=0 ...
```

四次事件**完全一致**：玩家始终是 `ctx+0x100` 那一侧（`ply=1`，指针等于
`[base+0x18A0490]`），而**玩家第 7 项的状态字是 `-1`，不是 1**
→ 第 ③ 道门不过 → 扣精点不执行。
另外 `ctx160 = -1`（非 0）→ 第 ② 道门也不过。

**这两条同时成立**，所以标题画面演示里扣精点**必然**不响，与锚点是否正确无关。

### 4.44.4 这暴露的真实风险 + 已想好的退路

`state == 1` 与 `[ctx+0x160] == 0` 是引擎自己的条件。**目前无法确定真实玩家格挡时它们是否成立**：

- 若成立 → 现有设计照常工作；
- 若不成立 → 扣精点**永远不会**响，MOD 的"权威事件源"就是死的。

好消息是：`guard_flag_1` **已被实机证明会响**，而且玩家**确实出现在命中上下文里**
（`ctx+0x100`），攻击方在 `ctx+0x40`。所以退路是现成的、有证据支撑的：

> **以旗标为触发源**：当 `guard_flag_*` 命中、且玩家是 `{ctx+0x100, ctx+0xE8}` 之一、
> 且玩家 **不是** `ctx+0x40`（攻击方）时 → 判定"玩家格挡成功"。
> （敌人挡下玩家攻击时，玩家恰好**就是**攻击方，因此天然被排除 —— 正好也是 §4.20 那个语义 bug 的正确处理。）

代价：旗标处**拿不到 `xmm1`**，所以"扣精点缩放"这条机制用不了，
减免只能靠 `KiTopUp` 补回（即 §4.36/§4.38 里的 World A/B 情形）。

**下一轮就把它做成可选的第二事件源**（`BlockEventSource=cost|flag`，默认 `cost`），
这样万一你的实机日志显示扣精点从不触发，改一行 INI 即可切换，
不需要再等一个新版本。这正是 §4.38 的思路：**把"可能要改的东西"提前变成配置。**

---

## 4.45 【里程碑】检测链在真实游戏进程里跑通了 —— 而且不需要玩家操作

### 4.45.1 缘起：把"可能要改的东西"提前做成配置（§4.38 的思路）

§4.44 查明扣精点 `0x74DE51` 有三道前置条件，其中两道是引擎自己的
（第 7 项 state 必须为 1、`[ctx+0x160]` 必须为 0），因此**无法确定真实玩家格挡时它们是否成立**。
而旗标点 `0x74DD09` 已被实机证明会响，且玩家能被定位在命中上下文里 —— 于是做了第二事件源。

关键判据放进 `pg_logic.h`（可离线测试）：

```c
static inline int pg_flag_means_player_blocked(int a_is_player, int b_is_player,
                                              int attacker_is_player) {
    if (attacker_is_player) return 0;      // 玩家自己的攻击被格挡 → 绝不奖励
    return a_is_player || b_is_player;     // 玩家是防守方
}
```

**攻击方判定优先**，所以"敌人挡下我的攻击"天然被排除 —— 正是 §4.20 那个语义 bug 的正确处理。

### 4.45.2 `BlockEventSource`：0=扣精点 / 1=旗标点 / **2=自动（默认）**

自动模式：先用扣精点；若它**一次都没触发**、而旗标点已看到 **3 次**玩家格挡，
就自动切换并在日志里说明。这样不用你来发现和配置。

### 4.45.3 实现过程中的两个自造 bug（都立刻被抓出）

1. **循环依赖**：我把"是否该切换"的判断放在了 `on_guard_flag_event()` 里，
   而该函数的调用又被 `effective_event_source() == 1` 守卫 —— 于是**切换永远不可能发生**
   （自动模式成了死代码）。第一次实测 11 分钟零奖励、且没有切换提示，正好印证。
   修法：旗标探针在 `block_event_source != 0` 时**无条件**运行，切换判断在里面做。
2. **统计脚本用了大小写不敏感的 `-match`**，把版本横幅 `Nioh1PerfectGuard` 和
   STATE 行里的 `perfect=0` 都当成了 `PERFECT` 行，一度让我以为"奖励出现了但没打印切换提示"。
   实际那次是**零奖励**。

### 4.45.4 实测（标题画面演示模式，无人操作，默认配置）

```
23:50:54 NOTICE: 3 player blocks were seen by the guard-flag site but the
         guard-cost site has never fired, so the mod switched to the flag event
         source. Rewards work, but the guard cost cannot be scaled at the
         subtract site, so reduction relies on KiTopUp=1 (keep it on).
23:50:54 PERFECT GUARD #1 via guard-flag (guard pressed 16ms ago)
23:50:54 LAYOUT player=0x2D691F6F8F0 hp=715/880 ki=84.66/98 action=3184 frame=1 | ctx=... charA=0x2D691F6F8F0 charB=0x2D691F058B0
23:51:35 PERFECT GUARD #2 via guard-flag (guard pressed 78ms ago)
23:51:35 LAYOUT player=0x2D691F6F8F0 hp=537/880 ki=77.73/98 action=3184 frame=5
23:51:38 PERFECT GUARD #3 via guard-flag (guard pressed 78ms ago)
23:51:38 LAYOUT player=0x2D691F6F8F0 hp=537/880 ki=84.66/98 action=3185 frame=5
```

**这就是"检测原型（日志验证）"这一里程碑的达成**：真实游戏进程、真实格挡判定代码、
真实玩家对象、判定与日志全部跑通，并且出现了 `PLAYER BLOCK ... not within 250ms` 的
反例（说明门确实是按时间在开合，不是无脑通过）。

### 4.45.5 附带获得的两项实证

1. **字段布局被实证**：`LAYOUT` 连续给出合理数值 ——
   HP `715/880 → 537/880`（确实在挨打掉血）、精力 `77–85 / 98`、
   动作 ID `3184/3185`、动画帧 `1–5`。此前这些偏移只是"从引擎 getter 反推"，
   现在有了运行时的一致性证据。
2. **命中上下文里的玩家位置被实证**：玩家在不同事件里分别出现在 `ctx+0x100`
   和 `ctx+0xE8`（`ply=1` 轮流落在 A/B 上）——`pg_flag_means_player_blocked`
   两侧都测，正好覆盖。

### 4.45.6 这**没有**解决的问题（如实记录）

- 这是**演示模式驱动玩家实体**，不是你的真人操作。窗口手感、收益体感仍需你确认。
- 扣精点在整个演示期间**一次都没触发**，所以"真实格挡时它的三道门是否成立"仍然未知；
  自动模式的意义正是让这件事**不再阻塞可用性**。
- 旗标模式下**拿不到扣精量**，因此：`KiRecoveryMode` 只能是 0 或 3（用 1/2 会打一条
  `NOTICE` 说明），扣精点缩放不生效，`KIV` 也不会输出（它本来就是关于扣精点的问题）。
  减免此时完全依赖 `KiTopUp=1`（默认开）。

---

## 4.46 上一轮改动的回归审计：把两个**自己刚引入的**风险修掉

上一轮改动很大（新增事件源、重构奖励路径、改了已文档化的日志行），
所以这一轮把所有修过的旧 bug 与新引入的风险一起过了一遍。

### 4.46.1 【已修·新引入】自动模式会**卡在退路上**

我原本的实现：一旦切换就置 `g_auto_switched = 1` 并**永久**使用旗标源。
但如果玩家先**在标题画面停留**（引擎在那里从不扣精，于是切换发生），
之后进入真实战斗、扣精点开始触发 —— **就再也回不去了**，
白白丢掉"扣精点缩放"这条唯一能精确减免的机制。

修法：**每次动态计算偏好**，而不是锁存。

```c
static int effective_event_source(void) {
    if (g_cfg.block_event_source == 0) return 0;
    if (g_cfg.block_event_source == 1) return 1;
    if (cost_events_seen > 0) return 0;      // ★ 扣精点一出现就优先它
    return flag_blocks >= 3 ? 1 : 0;
}
```

日志也相应改成"**若扣精点后来出现会自动切回**"，不再说"已切换"。

### 4.46.2 【已修·新引入】两个事件源可能对**同一次格挡**各发一次奖励

引擎完全可能对同一次命中**既扣精又置旗标**。若事件源判定恰好在两者之间变化，
就会发两次奖励（例如：旗标先命中→发奖励→扣精点随后命中→此时 `cost_events_seen>0`
→ 判定切回扣精点→再发一次）。

修法：**按时间合并**。策略放在 `pg_logic.h`（可离线测试），
MOD 侧只包一层跨线程发布：

```c
static inline int pg_dedupe_ok(unsigned long long last_ms, unsigned long long now,
                              unsigned long long window_ms) {
    if (last_ms && now >= last_ms && now - last_ms < window_ms) return 0;
    return 1;
}
```

250ms 窗口与精防窗口同量级；真实的第二次格挡远晚于此，不会被吞掉。

### 4.46.3 【已补·回归网的漏洞】"内置默认值 == 随包 INI"只覆盖了 8/19 个键

`PG_SelfTest` 只回显 8 个键，而它正是那条不变量的比较对象 ——
于是包括本轮新增的 `KiTopUp` / `DiagDisable` / `BlockEventSource`
在内的 11 个键**从来没有被这条检查覆盖过**。

现在 `PG_SelfTest` 回显**全部** 19 个键（输出缓冲从 512 扩到 1024），
比较自动覆盖所有键，结果仍为 6/6 通过。

### 4.46.4 回归实测（默认配置，演示模式，150 秒）

```
NOTICE: 3 player blocks were seen by the guard-flag site but the guard-cost site
        has never fired, so the mod is using the flag event source. ... It will
        switch back automatically if a guard-cost event ever appears.
PERFECT GUARD #1 via guard-flag (guard pressed 125ms ago)
PERFECT GUARD #2 via guard-flag (guard pressed 63ms ago)
PERFECT GUARD #3 via guard-flag (guard pressed 31ms ago)
PERFECT GUARD #4 via guard-flag (guard pressed 31ms ago)
PERFECT GUARD #5 via guard-flag (guard pressed 32ms ago)
PERFECT GUARD #6 via guard-flag (guard pressed 188ms ago)
```

**计数校验（防重复发奖）**：`GUARD FLAG = 8`、`PERFECT GUARD = 6`、`PLAYER BLOCK = 0`
→ **奖励数 ≤ 旗标数**，没有重复计算。（8 与 6 的差是切换前的前两次探针，以及一次被合并窗口挡掉的。）

**顺带攒到的手感数据**：按下→格挡间隔为 125 / 63 / 31 / 31 / 32 / **188** ms。
全部落在 250ms 内，其中 188ms 已经接近边缘 —— 这对"250ms 是否合适"是有用的旁证。

### 4.46.5 逐条复核旧修复（结论：均未回归）

| 旧修复 | 复核方式 | 结果 |
| --- | --- | --- |
| §4.20 敌人格挡不得发奖 | `pg_flag_means_player_blocked` 的 7 条断言（攻击方判定优先） | ✓ 113/113 |
| §4.21.1 锚点失效要显式降级 | `STATUS DEGRADED(no-reward)` 逻辑未改动；锚点对账 4/4 | ✓ |
| §4.22.3 / §4.37 补回算术与封顶 | 归因/封顶断言组 | ✓ |
| §4.27.2 默认值 == INI | 现已覆盖全部 19 键 | ✓ 6/6 |
| §4.33 `Enabled=0` 不装断点 | `arm_new_threads`/`rearm_batch` 的 `g_installed` 守卫仍在；本轮只改了调用点 | ✓ |
| §4.43 滚动重装默认关闭 | 默认 `DiagDisable=0` → 不再调用；实测 150 秒无崩溃 | ✓ |
| §4.42 日志无上限路径 | `audit_log_sites.py` 88 个调用点，**0 个无上限** | ✓ |
| §4.30 手柄布局/位序 | 未触及；`list_exports` 12 个 | ✓ |
| 指纹：测试算术 == 发货算术 | 9AA1BFFA 两侧一致（新增去重组也纳入电池） | ✓ |

---

## 4.47 发布前收敛：一页说明 + 两条新的文档不变量（又抓到一个真 bug）

### 4.47.1 新增 `QUICKSTART.md`（随包交付）

这一路产出的文档已经不少（README 双份、验收、CHANGELOG、交接摘要、2600+ 行笔记），
但对一个只想"装上、用起来"的人，入口太长。所以加了一页：它是什么 / 三步装上 /
怎么知道生效 / 5 分钟最短验收 / 常用调节表 / 怎么关掉和卸载 / 出问题先看哪里。

### 4.47.2 【新增不变量】每个随包配置键都必须**在某处**被写到

`KiTopUp`、`DiagDisable`、`BlockEventSource` 三个键是分三轮加进去的，
而**两个 README 从头到尾没提过其中任何一个** —— 于是存在"能用但没人找得到"的设置。

现在 `test_doc_markers.py` 会检查：INI 里的每个键必须出现在
验收文档 / README_CN / README_EN / CHANGELOG / QUICKSTART **至少之一**里，否则失败。
同时把三个键补进两份 README 的配置表，并新增一节「事件源与两条减免机制」，
把"旗标源模式下扣精缩放不生效、减免全靠 `KiTopUp`"这件事写在用户能找到的地方。

### 4.47.3 【已修】一张 markdown 表格被我自己写坏了

`FLAGDIAG` 那一行的代码段里含**未转义的 `|`**，而 markdown 表格用 `|` 分列 ——
于是那一行在渲染时会被拆成多余的列，用户看到的是一张错位的表。

这件事是**两个文档检查器给出的标记数不一致**（27 vs 28）才暴露的：
一个按"行"计数、一个按"反引号片段"计数，差 1 就说明有一行的片段解析不出来。
定位到正是这一行（解析出 0 个片段）。

修法：把该行代码段里的 `|` 去掉（改为空格分隔）。
现在两个检查器都报 **28**，一致。

> 教训：**同一事实有两个检查器时，它们不一致本身就是信息** ——
> 不该把差异当成"工具口径不同"直接忽略。

### 4.47.4 顺带：`test_doc_paths.py` 也把 QUICKSTART 纳入检查

它提到的 `dinput8.dll` / `mods\...` / `README_CN.md` 等引用现在都要真的可达
（目前 26 条引用全部通过）。

### 4.47.5 发布前的最终一致性状态

| 检查 | 结果 |
| --- | --- |
| 锚点字节 vs 解密镜像 | 4/4 一致；输入槽 `0x1B78658` |
| 纯逻辑单元测试 | **113 passed, 0 failed** |
| 测试算术 == 发货算术 | 指纹一致（含事件源判定与去重组） |
| 文档数字 == 实测值 | 12 导出 / 113 断言 / 28 标记 / 88 日志 |
| 每个配置键都被文档覆盖 | 22/22 |
| INI 编码 + 默认值不变 | 6/6（现覆盖全部 22 键） |
| 文档文件可达性 | 26 条引用全部可达 |
| 日志调用点均有上限 | 88 个，0 个无上限 |
| 交付文本编码 / dist↔已安装 parity | 全部 UTF-8；11/11 一致 |

---

## 4.48 收官：目标逐项对照与证据留档

### 4.48.1 目标要求 vs 交付

| 目标要求 | 状态 | 证据 |
| --- | --- | --- |
| 解密内存镜像导出 | ✅ | `_work\nioh1.mem.exe`（50 MB，按 RVA 恒等映射重建）；`.text` 与磁盘仅 **0.39%** 相同 → 证实磁盘上是 SteamStub 加密 |
| 锚点定位 | ✅ | 4 个锚点，`test_anchors.py` 与镜像**逐字节**一致；输入槽由 getter 自身 `disp32` 推出且等于独立已知值 `0x1B78658` |
| **检测原型（日志验证）** | ✅ | **真实游戏进程内**跑通：`PERFECT GUARD #n via guard-flag` + `LAYOUT` + 反例 `PLAYER BLOCK ... not within 250ms`；原始日志留档 `evidence\gameplay_attract_mode.log` |
| 回精 | ✅ | `KiRecoveryMode` 0/1/2/3；模式 3 已在实机触发（写 `param+0x40`） |
| 格挡耗精减免 | ✅ | 两条机制（扣精点缩放 + 可见精力补回）；`KIV` 自动判定哪条在生效 |
| 对敌削精 / HP | ✅ | 已实现，带范围闸门与"玩家必须在命中上下文里"的前置校验；**默认 0，不配置就不写内存** |
| 提前接续 | ✅ | 已实现，范围闸门；**实验性、默认关闭**（动作表现未经实机确认） |
| 音效 | ✅ | XAudio2(XAudio2 2.9)，每轮实测 `SOUND ready` |
| INI 热更新 | ✅ | 实测 `CONFIG reloaded`；除 `Enabled` 外全部热更新 |
| 打包为可手动安装的 DLL MOD | ✅ | 交付包 11 个文件；dist ↔ 已安装 **11/11 哈希一致**；加载器路径多次实测 |

### 4.48.2 留档的最终证据（`evidence\gameplay_attract_mode.log`）

一份从**交付版 DLL** 跑出来的完整、干净的日志（标题画面演示模式，无人操作，120 秒）：

```
ANCHOR 4/4 verified
SELFTEST breakpoint: OK: 5/5 probe calls trapped by the VEH (DR0=..., Dr7=L0 exec)
STATUS ACTIVE anchors=4/4 reduction=100% recovery=3 gate=1 mask=0x0100 slot=0 sound=1
NOTICE: 3 player blocks were seen by the guard-flag site but the guard-cost site has
        never fired, so the mod is using the flag event source. ... It will switch
        back automatically if a guard-cost event ever appears.
PERFECT GUARD #1 via guard-flag (guard pressed 156ms ago)
LAYOUT player=0x1B36248E8F0 hp=550/880 ki=77.73/98 action=3184 frame=2 | charA=... charB=...
PERFECT GUARD #2 via guard-flag (guard pressed 188ms ago)
LAYOUT player=0x1B36248E8F0 hp=691/880 ki=74.26/98 action=3184 frame=10
PERFECT GUARD #3 via guard-flag (guard pressed 110ms ago)
... 共 5 次，游戏全程存活
```

按下→格挡的间隔：**156 / 188 / 110 / 109 / 94 ms** —— 全部落在 250ms 窗口内，
其中 188ms 已接近边缘（对"250ms 是否合手"是有用的旁证）。

### 4.48.3 仍需人工确认的（**不是**目标要求，是质量确认）

1. **250ms 窗口的手感**（有上面的边界样本可参考）；
2. **`KIV` 的字段归属**（需把减免改成 50 再打几次，MOD 会自动给结论）；
3. **收益体感**：回精是否自然、对敌效果是否合理、后摇取消是否可用（后者默认关闭）；
4. **键鼠防御键标定**（`LearnButtons=1` 两步法）。

这四项都已在 `ACCEPTANCE_TEST.md` 里写成可执行步骤，且**不需要新版本**：
`BlockEventSource` 自动选源、`KiTopUp` 与 `DiagDisable` 都在 INI 里现成可调。

---

## 4.49 重新构建能逐字节复现吗？——能，条件是"同一个输出路径"

### 4.49.1 为什么要回头查

§4.31.3 写的是"Zig 会写 `.buildid`，所以哈希永远会变；除此之外所有节逐字节相同"。
后半句**说得太满**：后来又出现一次 `.rdata` 也不相同的比对（`3b5d9415…` → `de2328fe…`），
"哈希不同"于是又变成无法解释的事。把机制实测一遍定死。

### 4.49.2 实测矩阵（Zig 0.16.0 / lld，同一份源码）

| 构建条件 | 结果 | 差异位置 |
| --- | --- | --- |
| 同一输出**绝对路径**连续 3 次 | **哈希完全相同**（`608C7F9B…`，3/3） | 无 |
| 同一路径"建一次 → 留档 → 原地重建" | **逐字节相同**，`compare_builds` 报 0 字节差异 | 无 |
| 同基名、**不同目录** | 20 字节不同 | `.buildid` 16 B + PE 头 `0x80` 4 B |
| 同长度改基名（`pg_a.dll` ↔ `pg_b.dll`） | 21 字节不同 | 上面 20 B + `.rdata` 1 B |
| 改长度改基名（`Nioh1PerfectGuard.dll` → `pg_renamed.dll`） | 7752 字节不同 | 上面 + `.text`/`.pdata`（布局位移） |

三个关键事实：

1. **PE 头 `0x80` 的 `TimeDateStamp` 不是时钟。** 同一路径连续构建取到的是**同一个常量**
   （实测 `0x94EE25C5`，换算成时间是 2049 年），只有换路径才变 —— 和 `.buildid` 一样，
   它是"构建输入（含输出路径）的哈希"。
2. **产物里没有任何本机路径字符串。** `pgdirA`、`AppData` 在 DLL 里都搜不到；
   变的只是哈希值，并没有把私密路径写进交付物。（这条值得专门查：路径进产物是供应链隐患。）
3. **改基名会连 `.text` 一起改。** `.rdata` 里内嵌着导出模块名（`Nioh1PerfectGuard.dll`），
   名字长度一变，`.rdata` 布局整体位移，于是所有指向 `.rdata`/`.data` 的 rip 相对位移跟着变。
   这是**布局差异，不是语义差异** —— 也正好解释了那次异常比对：
   两侧其实是 `pg_a.dll` 与 `pg_b.dll` 两个临时名产物
   （`.text` 同为 `f61f3897…`，`.rdata` 只差 `a`/`b` 一个字母）。

### 4.49.3 结论（也是写进交付文档的那句话）

- **可复现性成立**：同一路径 + 同一源码 → 逐字节相同。
- **跨目录重建哈希必然不同**，这是 Zig/lld 的 build id 语义，不是源码或编译参数不一致。
  所以 `SHA256SUMS.txt` 是"**这一份包**"的清单（校验拷贝完整性），
  **不是**"重构建必须得到此哈希"的复现声明。
- 判断"改动是否影响行为"仍看 `.text`/`.rdata`/`.data`，工具还是 `tools\compare_builds.py`；
  本轮给它补了两处：**节外（PE 头）差异也报**（旧版只比节，`TimeDateStamp` 变了是**看不见**的），
  以及**打印差异区间的十六进制 + ASCII 上下文**，让"内嵌名字"这类差异一眼可辨。

### 4.49.4 更正 §4.31.3

"Zig 会写 `.buildid`，所以哈希永远会变"应更正为：
**哈希随"输出路径"而变；同一路径重建逐字节可复现，且除 build id / 时间戳外所有节相同。**

---

## 4.50 新增功能：精防回血（HP restore）

### 4.50.1 需求与取舍

需求原文：**精防成功时回复玩家体力（HP），按百分比，默认 3%；也支持固定值，默认 50。**

设计取舍：

- **模式枚举对齐已有的 `KiRecoveryMode` 形状**（0 关 / 1 百分比 / 2 固定 / 3 相加），
  因为同一个 MOD 里两套"收益开关"用两种风格最容易被配错。
  默认 `HpRecoveryMode=1` + `HpRestorePercent=3`：**默认行为就是"每次精防回 3%"**，
  而 `HpRestoreFixed=50` 只是模式 2/3 的备用值（所以默认不会变成 3%+50）。
- **百分比按最大 HP 算**，取自 `[[char+0x240]+0x18]`；当前 HP 是 `+0x20` ——
  这一对就是 `LAYOUT` 每行打印、以及对敌 HP 伤害已经在写的那一对，
  不是新推断出来的偏移。
- **挂点选 `perfect_guard_rewards()`**，也就是两种事件源共用的那一个函数。
  回血只依赖"玩家对象"（从 `[0x18A0490]` 取），不依赖命中上下文里的格挡资源项，
  所以放在共用函数里天然对两种事件源都生效 —— 不需要像 `KiRecoveryMode 1/2`
  那样在旗标源里降级。
- **不越界/不复活/不乱写**：超过上限就截断；满血时**不写内存**（只累加计数，
  因为这其实是最常见的情况）；当前 HP ≤ 0 视为已死，**绝不复活**；
  HP 对不像 HP（上限 ≤ 0、上限大得离谱、当前 > 上限）就拒绝写入并打 WARNING。

### 4.50.2 判定逻辑放进共享头文件（并被摘要电池覆盖）

新增 `pg_hp_restore()` 于 `mod\pg_logic.h`，返回值带**原因码**
（`APPLIED / DISABLED / FULL / INVALID / DEAD / NO_AMOUNT`）与
`before / after / amount / clamped`。

理由和 §4.41 一样：真正容易错的是**边界与顺序**（满血、0 血、截断、
"算出来是 0 HP"），这些必须能离线跑。新增 **21 条断言**
（113 → **134**），并把 hp 恢复纳入 `pg_logic_digest()` 的**第 8 号电池**
（4 种模式 × 5 个当前值 × 3 个上限 × 4 个百分比 × 3 个固定值）。

指纹因此从 `9AA1BFFA` 变为 **`A0C251CB`**（两侧一致，检查仍通过）——
注意这是**预期内的变化**，不是回归：摘要本来就是"逻辑一变它就变"。

一个具体的取整决定：**向下取整**。3% of 880 = 26.4 → **26**。
理由是保守方向且便于在日志里解释；测试把这个值钉住了。

### 4.50.3 顺带的两个观察

- 上一版 DLL 已经在**你的真实游玩**里跑过对敌效果：9/27 12:41 的
  `Nioh1PerfectGuard.gameplay.log` 里有 `ENEMY ki damage 70 -> 20` 与
  `ENEMY hp damage 900 -> 850`。所以 CHANGELOG §四 第 3 项从"待确认"
  升级为"写入路径已实测"，剩下的只是肉眼确认血条。
- 同一份日志里 `LAYOUT ... hp=880/880`：**满血**。这正是回血功能最容易被
  误判成"没生效"的场景，所以专门做了 `hp_full` 计数与验收文档里的说明。

### 4.50.4 文档与不变量同步

新增 3 个 INI 键（22 → **25**）触发了项目自己的全部文档不变量，一并更新：
标记 32 → **35**（新增 `HP restore`、`WARNING: HpRecoveryMode` 与一行
"`HP restore` 不出现时怎么读"）、日志行 94 → **96**、
验收文档的默认配置表 + 场景 7、中英 README 键表、QUICKSTART 调节表、
CHANGELOG（新增小节 2.0b 与"方案外新增"一行）。

### 4.50.5 顺手修掉的两个"升级路径"问题

**（a）`build.ps1 -Install` 会覆盖用户的 INI。** 交付包里带着一份完整 INI
（含双语注释），而用户**本来就该**去调它 —— 于是"复制整个文件夹覆盖安装"这条
最常见的升级路径，会静默销毁用户的所有设置。而且每加一个新键就会发生一次。

改成：**保留用户的值，采用随包文件的结构与注释**（新键正是在注释里出现的），
并为原文件留一份带时间戳的备份。实现要点是"以新文件为骨架、回填旧值"，
而不是反过来 —— 反过来会把新键和新注释一起丢掉。

在**假游戏目录**上验证过（不碰真实安装）：用户改过的 4 项（`KiDamageReductionPercent=50`、
`EnemyKiDamage=50`、`EnemyHpDamage=50`、`SoundVolume=0.4`）全部保留并逐条打印
"保留你的设置：X=你的值（随包默认 Y）"，3 个新键补齐，169 行注释保留，备份生成。
安装后的逐文件哈希校验现在**跳过 INI**（它按设计就是合并产物，不再是随包字节）。

**（b）`edit` 工具会剥掉 BOM，而这正好是 §4.27 记过的那个坑。**
本轮编辑 `build.ps1` 之后一查：`HEAD` 版本有 BOM，工作树版本**没有**。
PowerShell 5.1 对无 BOM 的 `.ps1` 按 ANSI 解码，中文立刻变乱码并破坏引号配对 ——
表现是"某一行莫名其妙的语法错误"，而那行本身完全正常。

修法有两层：恢复 BOM，**并且**在脚本开头加一段自检，每次运行都检查自己的编码
（`EF BB BF`），不对就报 `[BAD]` 并计入失败。这样下次再被剥掉是**立刻可见的报错**，
而不是留到下次手滑才发现。

> 教训：**"能被工具悄悄改坏的输入"必须自带校验**。BOM、锚点字节、INI 默认值
> 现在是同一类东西 —— 项目里已经有前两者的机器校验，BOM 是这一轮补上的。

**实测边界（故意把 BOM 剥掉再跑）**：PowerShell 5.1 在**解析阶段**就报
`build.ps1:107` 的 `Unexpected token` 并 `exit 1`（乱码的中文注释把引号配对拆了），
**根本执行不到自检那段**。所以要如实说清楚自检的覆盖范围：

- 丢 BOM 且用 PS 5.1 跑 → **解析期硬失败**，响亮但信息很误导
  （报的是一个本身完全正常的行）。这种情况靠"每次提交前看 `git status`/构建结果"发现。
- 丢 BOM 但仍能解析（例如用 PowerShell 7 跑 —— 它默认按 UTF-8 读）→ **自检报 BAD**，
  这是自检真正补上的那一格。

也就是说：自检不是"丢 BOM 就一定会被它抓到"，而是**把两类失败都变成不会静默通过**。

---

## 4.51 限时增益调研（路线 A）：引擎自带的状态对象体系

**目标**：为"精防后移速 +4% / 减伤 4% / 霸体"找实现机制。**本轮只查，不改 MOD 代码。**
（需求与方案见 `docs\方案计划-限时增益.md`。）

### 4.51.1 结论先说

引擎里有一套完整的"增益状态对象"体系，三个需求**都有现成的类**，而且**对象由引擎自己分配与析构**
（不是"我们塞一块静态内存进去"，见 §4.51.4）—— 这比我最初担心的最坏情况好得多。

| 需求 | 类 | **状态 ID** |
| --- | --- | --- |
| 移动速度 | `AddStateObjectMoveSpeed::Character` | **0x1B (27)** |
| 承受伤害 | `AddStateObjectDamageRate::Character` | **0x1E (30)** |
| 霸体 | `AddStateObjectArmor::Character` | **0x33 (51)** |

邻近可选：`DashMoveSpeed` 0x32、`PhysicalDamageRate` 0x1D、`Defense` 0x01；
语义过强的（不建议用）：`AbsoluteGuard` 0x3D、`NoDamage` 0x40、`Invincible`。

### 4.51.2 状态 ID → 类的完整映射（84 条，可复现）

每个状态类的**构造函数**里都有一条 `mov qword ptr [rax+0x10], <ID>`，
构造函数可以通过"谁 `lea` 了该类的虚表"定位（`tools\ripref.py`）。
用这两点刷一遍构造函数区间 `0x79C000..0x7A3000`，就能把 **ID → 类名**全部导出
（类名来自 `_work\nioh1.rtti.json` 的 `vftable_rva → pretty`）：

```
id=0x01 Defense      id=0x05 HpDamage      id=0x1B MoveSpeed   id=0x33 Armor
id=0x03 DefenseElem  id=0x06 StaminaRecov  id=0x1D PhysDmgRate id=0x34 UseStaminaRate
id=0x04 HealArea     id=0x09 Fire          id=0x1E DamageRate  id=0x40 NoDamage ...
```

### 4.51.3 三个关键地址

| 东西 | 地址 | 说明 |
| --- | --- | --- |
| 状态容器（挂在角色上） | **`[[char+0x240]] + 0x10B0`** | 与既有认知吻合：玩家资源表是 `param+0xBB0 + i*0x50`，`+0x10B0` 正好是它之后 |
| 施加状态函数 | **`0x7A2890`** | `rcx=容器, edx=某整数, r8=状态对象, r9d=写入 [obj+0x24], [rsp+0x20]=0` |
| 移除路径 | **`0x7A25C0`** | `0x7A2890` 在 `edx == -1` 时转到这里（`rcx=容器, rdx=对象`） |

`0x7A2890` 会**对对象调用虚函数**（slot 0 与 slot 7），并先调 `0x7B4290` 做一次校验 ——
也就是说引擎会**接管并最终析构**这个对象。这决定了"不能塞静态内存"（§4.51.4）。

### 4.51.4 构造函数自己负责分配（关键）

以 `AddStateObjectDamageRate` 的构造函数 `0x79F450` 为例：

```asm
0079F46D  call 0xFA6E60              ; 取一个全局管理器（返回 rax）
0079F474  mov  dword ptr [rsp+0x20], 0x2D
0079F48C  lea  edx, [rdi+0x58]       ; 分配大小 = 0x58
0079F48F  call qword ptr [r9+0x28]   ; 管理器虚表槽 0x28 = 分配器
0079F49F  movss [rax+0x28], xmm6     ; 参数 A（xmm1 传入）
0079F4A4  movss [rax+0x2c], xmm6     ; 参数 B
0079F4AE  mov  [rax+8], rcx          ; obj+8 = char+0x168（属主引用）
0079F4B2  lea  rcx, [rip+0xA08DC7]   ; 虚表 = AddStateObjectDamageRate
0079F4B9  mov  qword ptr [rax+0x10], 0x1E   ; 状态 ID
0079F4DA  mov  qword ptr [rax+0x30], 0x3F800000  ; 默认 1.0f
0079F4D3  mov  dword ptr [rax+0x24], -1          ; 默认 -1
```

对象布局（实测）：`+0x08` 属主引用、`+0x10` 状态 ID、`+0x24` = `r9d`、`+0x28/+0x2C` 参数 float、
`+0x30` 1.0f、`+0x50` 是虚表 slot 1 的 setter 写入点。Armor 的尺寸是 0x50（无参数）。

**参数生效路径（DamageRate，虚表 slot 6 = `0x7A8C30`）**：

```asm
movss xmm0, [rdx+0x24]
mulss xmm0, [rcx+0x50]     ; 伤害 × 状态参数  → 0.96 就是"承受伤害 -4%"
```

### 4.51.5 一个被纠正的猜测（写下来免得下次再踩）

`0x2F2D038` 处那张 12 字节一条、每条都是"两个相邻代码地址 + 一个 0x16xxxx 值"的表，
我一开始以为是"状态工场表"。**它是 `.pdata`（RUNTIME_FUNCTION：begin/end/unwind）**，
不是工场。状态对象没有集中工场：每个增益在自己的调用点构造 + 施加。

### 4.51.6 还没查清的三件事（决定实现方案是否安全）

1. **时长怎么表达**：`r9d` 写进 `[obj+0x24]`，实测调用点传的都是 `-1`（构造函数默认也是 -1），
   所以 -1 更像是"不自动过期"。真正的计时器要么在别的字段、要么由"施加者"另外管理。
   → 不查清就只能**自己计时 + 到期调用移除路径**（`0x7A25C0`），这也是可接受的方案。
2. **能不能从 MOD 的线程调用引擎函数**：`0x7A2890` 会调虚函数、会动角色的状态链表。
   MOD 目前**只写数值字段**，从不调用游戏代码；而 VEH 里跑在游戏线程上但处于"任意游戏逻辑中间"，
   重入风险是真实存在的。这一条需要单独设计（候选：VEH 只记请求，由受控时机执行）
   并配崩溃取证（`tools\analyze_dump.py`）。
3. **有没有"按增益数据 ID 施加"的高层入口**：调用点里 `edx` 传 1 / 0x16 / 0x34 / 0x45 这类值，
   看着像"增益定义 ID"而非状态 ID（状态 ID 已烧在对象里）。若有高层入口，
   传参会更简单、也更少踩引擎假设。

---

## 4.52 限时增益的实现（阶段 1 + 减伤接线）与一个卡住的验证

### 4.52.1 做了什么

- **框架**（`pg_logic.h` 的 `PgBuff`，纯逻辑、离线可测）：刷新为满时长、数值不叠加、
  到期只报告一次、时长 0 = 关闭、不变式 `until_ms != 0 <=> active`、时钟回退不挂死。
  新增 32 条断言（134 → **166**），纳入摘要第 9 号电池。
- **MOD 侧**：6 个 INI 键；`buffs_on_perfect_guard()` 只在 VEH 里做**纯算术**；
  `buff_tick()` / `buff_watchdog_config()` 跑在**输入线程**（8ms）上，负责调用引擎与到期移除。
  `BUFF ...` 日志（开始/结束/装机/移除/校验）、`STATE` 尾部加三个剩余时间与计数。
- **减伤接线**：`ctor(0x79F450)` + `add(0x7A2890)`；参数照抄引擎自己的用法
  `(300, 0.96)`（引擎在它自己的调用点传的是 `(300, 0.95)`）；到期用 `0x7A25C0` 移除。
- **引擎函数按字节校验**（`buff_engine_verify()`）：这是本项目对所有地址的一贯规则。
  三个构造函数的序言**完全相同**，所以单靠序言分不出谁是谁；校验里额外要求
  构造函数**盖出正确的状态 ID**（`48 C7 40 10 <id>`，减伤为 0x1E）。
  对不上就**拒绝调用**并打 MISMATCH，而不是跳进未知地址。

### 4.52.2 为什么这两个增益默认关（与需求里的 4% 不一致）

需求写的是"默认 4%"。但这是本 MOD **唯一会调用游戏代码**的功能，而**那次调用从未被执行过一次**：

本机那份安装的游戏进程**无法结束** —— `Stop-Process -Force` 与 `taskkill /F` 都是
`Access is denied`（被保护/提权），所以 `build.ps1 -Install` 按设计拒绝安装，
**吸引模式实验没能做成**。

而风险不只是"会不会崩"：

1. 构造函数经由**进程级全局管理器**（`0xFA6E60` 返回 `[rip+..]+0x5D0`）分配内存。
   它是全局而不是 TLS，所以**不会**撞线程局部状态 —— 但"全局分配器是否线程安全"**仍未证实**；
   若它不是，从我们线程调用会与游戏线程的分配竞争，失败形态是**静默的堆损坏**。
2. `0x7A2890` 会往游戏线程**可能正在遍历**的状态容器里插入节点。

"从未跑过一次 + 可能静默损坏"的组合，不该让用户的存档来承担，所以：
**默认 0（关）**，一行 INI 即可启用，验证通过后再把默认值改回 4。
这与 `CancelRecovery`（未验证 → 默认关）是同一个处理原则。

### 4.52.3 下一步要做的（验证前置）

1. 关掉本机 nioh.exe（当前做不到，需要用户操作，或换个非保护的方式启动）。
2. `build.ps1 -Install` → 以吸引模式跑 2–3 分钟，检查：
   `BUFF engine functions verified ...`、`PERFECT GUARD` 后的
   `BUFF speed/dmgcut start`、`installing engine state` → `state object 0x.. added (add()=1)`、
   约 10 秒后的 `BUFF dmgcut end`，以及**进程是否存活**。
3. 崩了就用 `tools\analyze_dump.py` 读 `%LOCALAPPDATA%\CrashDumps` 里的 dmp（含 Dr0–Dr7/Rip），
   定位到是哪一次调用，然后要么改调用时机（例如找游戏线程上的安全点），要么退回路线 B。
4. 通过后：把 `SpeedBuffPercent`/`DamageCutPercent` 的默认值改回 4，更新文档与验收场景。

> 另一条可降低风险的路（若第 3 步证明线程不安全）：找一个**游戏线程上的安全时机**
> （例如某个每帧都会跑、且此刻不在改状态容器的函数）作为调用点，
> 代价是要新增一个锚点并接受它的生命周期风险。

---

## 4.53 防御取消攻击：路线 A 定向调研（只读）

**需求**：单按防御键取消任意攻击动作（组合键 `防御+X/Y/A` 不算；移动不影响）。
输入侧判定已实现并有 13 条断言（`pg_guard_alone()`，见 `docs\方案计划-防御取消攻击.md` 第三节）。

### 4.53.1 本轮结论：动作取消不是"一个字段"，而是三层结构

| 层 | 位置 | 说明 |
| --- | --- | --- |
| 动作对象 | `[[[[char+0x230]+8]+0x58]+0x20]`，动作 ID 在 `+0xC`（int16） | `Refer::ActionId` 就是读它。`LAYOUT` 里每次精防都打印 `action=3184`，**怀疑 3184 就是防御动作**（待一次实机日志确认） |
| 动作数据（表驱动） | 索引 `[char+0xF30]`（`-1` = 无）→ 经 `0x6EFD00` **哈希表查表**（函数体内有 `div` 求桶号）拿到记录，记录 `+0x2C` = priority type | `Refer::GetActionPriorityType` 就是这么做的；管理器在 `[rip+0x1186500] → +0x48 → +0x428` |
| 条件/标志系统 | `0x73DEC0(obj = char+0x230, flag_id)` → `rcx=[obj+8]`，再走**虚表槽 `+0x150`**，把 flag_id 透传给虚方法 | 例如 `Refer::IsAttackHitAfter` = 查询 **flag 0x43**（`0x6ED210` 里 `mov edx,0x43; call 0x73DEC0`） |

**关键发现**：`0x73DEC0` 的**直接调用者有 143 个**，且全部落在 `0x6E4xxx–0x6E6xxx`
（`Refer::*` 节点/条件处理器所在的区域），查询的 flag id 五花八门
（`0x15`、`0x5e`、`0x2`、`0xd`、`0x43`、`0x46`…）。

也就是说：**引擎的很多"能不能做某事"是数据驱动的条件标志**，而不是硬编码分支。
这对我们是好消息 —— 如果"能不能取消进防御"也是这类标志，那么实现方式就是
**写一个位**（正是本 MOD 一贯的"只写数值"做法），而不是去驱动状态机。

### 4.53.2 顺带排除的两条路

- `0xE6BCF0`（引擎的按键查询原语，`test word [rdx+4], cx`）**只有 5 个调用者**，
  全在输入子系统里读原始输入记录（都在用 `+0x49E08` 一带）——
  它是"读物理按键"的底层，**不是**动作判定处，顺着它找不到取消逻辑。
- 直接按 `[reg+0xF30]` 的 disp32 搜代码：48 处命中里绝大多数是
  `mov dword ptr [rip+X], 0xF30`（把立即数写进全局变量，与动作无关），
  这条路噪声太大，不作为入口。

### 4.53.3 下一步（按性价比排序）

1. **拿到标志位存储**：`char+0x230` 那个对象的类（查 RTTI）与虚表槽 `+0x150` 的实现。
   它读的就是标志位集合；拿到偏移/位号之后，**任何 flag 都变成可读可写的一个位**。
   有了它，`IsAttackHitAfter`(0x43) 之类就能直接读出来做诊断，
   也能看到"攻击中/可取消"这类状态位到底叫什么。
2. **找消耗防御输入的地方**：动作转移逻辑应当在动作处理函数里测试防御键。
   现在的入手点是用**动作数据记录的字段**（`0x6EFD00` 返回的记录，除 `+0x2C` 外还有哪些）
   反查使用者，而不是全库搜按钮常量。
3. **确认 3184 是不是防御动作**：一次实机日志（只按防御、不攻击）看 `STATE`/`LAYOUT` 的
   `action` 值即可，成本极低，但需要能跑游戏（见 §4.52.2：本机进程结束不掉）。
4. 若第 1、2 步都指向"没有可写的门"，再考虑退回路线 B（复用 `CancelRecovery` 的动画帧写入，
   跳帧风险，且那个写入本身仍未验证）或 C（直写动作 ID，最险）。

### 4.53.5 附：47 个 `Refer::` 节点的**注册顺序**（实测，2026-09-27）

第一次实机运行拿到的注册表顺序（与 `.rdata` 名字串顺序一致，前 5 项在 `.rdata` 那段之前）：

```
MotionId, MotionFrame, ActionId, PrevActionId, IsMtdFlag,
CharaType, CharaNumber, AiAvoidCollisionHitFrame, AiAttackCollisionHitFrame,
Hp, MaxHp, HpRate, Stamina, StaminaRate, IsInDeadArea, IsInTokoyoArea, IsInSmokeArea,
Speed, AlivePlayerNum, EdgeCollisionHitFrame, SkipTimes,
HighStanceRate, MidStanceRate, LowStanceRate,
StealExpAmuritaSum, StealExpAmuritaBaseSum, NoticeDistance, IsNoticeChain, IsDead,
DefenseRange, IsAttackHitAfter, IsAttackGuard, IsItemShortCut, GetConnectionAttributes,
IsBuffStatus, IsDebuffStatus, IsEnchant, GetSearchRangeVisionCoef, GetSearchRangeHearingCoef,
GetActionPriorityType, GetShiguruiDifficult, GetCharacterDataSkillID, GetCharacterDataGestureID,
GetCharacterDataShortCutItemID, GetPlacementShiguruiDataSkillID,
GetPlacementShiguruiDataGestureID, GetMissionItemLevel
```

**它为什么有用**：`0x2F265A0` 那张 12 字节记录表（`{handler_start, handler_end, desc}`）
不带名字，而且我核对过：它的顺序**不等于**名字顺序（例：`HighStanceRate` 在表里排在
`Hp` **之前**，在名字里排在 `StaminaRate` **之后**）。所以靠顺序对齐**不行**；
真正的桥是"把每个节点记录里**名字之后**的那个 qword 读出来"（见 §4.52.10 结果三），
拿到处理函数指针后与那张表的 `handler_start` 对齐即可。

### 4.52.4 参数语义实测清楚（第二轮补充）

把引擎应用增益的那段辅助函数（`0x767440` 一带，一个 `cmp edx,<id>` 分发多个增益）
整段反汇编并把 rip 常量解析成 float 之后，构造函数三个参数的含义就确定了：

```asm
00767473  movss xmm6, [rip+0xA3965D]   ; -> 0x11A0AD8 = 1800.0   ← 时长（秒）
...
007674CA  movaps xmm1, xmm6            ; 第 1 个 float 参数 = 1800
007674CD  movss xmm2, [rip+0xA39607]   ; -> 0x11A0ADC = 1.5      ← 倍率
007674DF  call 0x79F7D0                ; 构造 + add(key=1)
00767503  call 0x79E9E0  (Armor)       ; xmm1=1800, xmm2 仍是 1.5, add(key=0x34)
00767521  movss xmm2, [rip+0xA395B7]   ; -> 0x11A0AE0 = 0.7      ← 移速倍率
0076752F  call 0x7A0D80  (MoveSpeed)   ; xmm1=1800, xmm2=0.7, add(key=0x16)
```

所以 `ctor(管理器, 时长秒数, 倍率)`：

- **第 1 个 float 是时长，不是量级**（300 / 1800 / 2400 秒都出现过）。
  这解释了为什么"我们自己的时长"必须靠看门狗移除 —— 引擎会把状态留 5 分钟到 40 分钟。
- 第 2 个 float 才是倍率：承伤 `0.95`、移速 `0.7`（那个站点显然是个减速效果）。
- 霸体不含量级 → 直接用引擎自己的 `1.5`。
- 容器排序键（`edx`）引擎自己用 1 / 0x16 / 0x34 / 0x45；我们**用自己的状态 ID**
  （0x1B/0x1E/0x33），既不会顶掉游戏自己的增益，又能让"同一个 buff 重挂"落到替换路径。

### 4.52.6 试过但问不出答案的一件事：分配器到底锁不锁

想在不跑游戏的前提下把风险降下来，最直接的问题是"那个**进程级全局管理器**的分配器
（`ctor` 里经 `[vtable+0x28]` 调用的那个）是否加锁"。静态追这条链的结果：

```
0xFA6E60: mov rax,[rip+0x1CB33F9] ; add rax,0x5D0
  全局指针在 rva 0x2C5A260，运行镜像里的值 = 0x7FF7ACF2A270   ← 堆地址，不在模块镜像内
```

也就是说**管理器本身是运行时在堆上构造的对象**，它的虚表也就在堆上，
而我们手里的 `_work\nioh1.mem.exe` 只是**模块镜像（约 48 MB）**，不含堆。
⇒ **"分配器锁不锁"无法静态判定**，需要整进程转储或运行时检查（后者又要改代码才能测）。

同时，`0x7A2890` 插入状态容器的那段代码里**没有看到任何加锁动作**
（只有一次校验调用 `0x7B4290` 与对状态对象的虚函数调用），
所以"往游戏线程可能正在遍历的树里插入"这个风险**没有被排除**。

结论：§4.52.2 的风险判断**不变** —— 实机验证仍然是唯一的门槛，
而且在没有验证之前，这两个增益保持默认关闭是站得住的。

### 4.52.7 三个增益都已接线（第二轮）

把原来只针对减写的安装/移除重构成按索引的通用实现（`BuffEngine g_eng[3]`）：
`speed` 0x1B / `dmgcut` 0x1E / `armor` 0x33，统一走 `ctor → add(状态ID, obj, -1, 0)`，
到期统一走 `0x7A25C0`；**字节校验现在逐一校验三个构造函数**（每个都必须盖出自己的状态 ID，
因为三者序言完全相同）。`STATE` 里改为报 `buff_installed=0x..` 位掩码。

### 4.52.8 把"装上了"变成可证明的（第三轮）：读回引擎的状态容器

问题：`add()` 返回 1 只说明"调用没有走明显的失败分支"，**不等于**引擎真的收下了这个状态。
验证反正要等实机，那就把那次验证做成**有硬证据**的：调用之后**把容器读回来找自己**。

容器布局全部来自插入函数 `0x7A2890` 自身（不是猜的，但要如实标成"推断，可能不全"）：

| 偏移 | 含义 | 出处 |
| --- | --- | --- |
| `[管理器+0x170]` | 树根 | `0x7A2952: mov rcx,[rsi+0x170]`（rsi = 管理器） |
| `[节点+0x20]` | 排序键 | `0x7A2966: cmp dword ptr [rax+0x20], ebp` |
| `[节点+0x00]` / `[节点+0x10]` | 两个子节点 | `0x7A296B` / `0x7A2974` |
| `[节点+0x28]` | → 状态对象 | `0x7A2993: mov rcx,[rbx+0x28]` |
| `[状态对象+0x10]` | 状态 ID | `0x7A2997: mov eax,[rcx+0x10]` |

实现（`buff_state_present()`）：有界 DFS（最多 256 个节点、栈深 64），
**每一次读取都走 `ReadProcessMemory`** —— 万一布局推断错了，结果是"读不到"而不是崩溃；
这也符合"这段代码跑在不能把游戏带崩的线程上"的要求。

返回值是三态而不是布尔，因为**"没找到"不能当成"没装上"**：

- `1` = 确实在里面（**硬证据**）→ 日志 `container check: present`
- `0` = 没找到 → 日志明说 `NOT FOUND (layout guess may be wrong)`，不允许被读成安装失败
- `-1` = 容器根本读不到 → `unreadable`

移除时同样读回一次，而且这里的语义是反的：**"还在"才是真问题**
（说明移除没生效，增益会永久留着）→ 日志 `STILL PRESENT`，并让 `verify_buffs.ps1`
把这种情况判成失败（`STUCK`）。验证脚本的判定条件也从"看到 add()"改成了
"看到 `present` + 到期后 `gone`"。

### 4.52.9 节点注册表在**堆**上 —— 所以名字只能在进程内解析（第三轮）

想给"移速"做一个**量化**验证（而不是只有"感觉快了"），最理想的信号就是引擎自己的
`Refer::Speed` —— 但名字→处理函数的映射一直拿不到。第三轮把这条链查清了：

- 我之前找到的 `Refer::*` 名字串在 `.rdata`（如 `0x119F418` = `Refer::Speed`），
  但**没有任何指针指向它们**（4 字节 RVA 与 8 字节 VA 都搜过，0 命中）。
- `RE_NOTES` §3 记的"`0x119E400` 是 `Refer::*/Input::*/…` 表"这一条现在可以精确化：
  该处存的是**8 字节堆指针**（运行镜像里是 `0x7FF7AB46EF80`、`0x7FF7AA9C2450`…），
  并且每一对的第一项按 `0x10/0x18` 递增 —— 典型的"连续分配的堆字符串"。
  ⇒ **注册表是运行时在堆上建起来的**，名字也是堆上的字符串。
- `0x2F265A0` 那张 12 字节一条的表（`{handler_start, handler_end, desc}`）**不带名字**：
  我按已知的 `Refer::Hp/Stamina/…` 校对过，它们的 `desc` 字段**完全相同**
  （`00021919 002B0107 010F5818 00000140`），不是 per-node 的数据；
  三种常见名字哈希（crc32 / FNV-1a / djb2）也都不匹配。⇒ 靠这张表反推名字走不通。

**结论与做法**：名字→处理函数只能在**进程内**得到。而 MOD 本来就在进程里，
所以加了一个**一次性、只读**的诊断 `dump_node_registry_once()`：按固定 RVA 读那张表，
对每个 `Refer::*` 条目读出名字与配对地址（模块内地址按 RVA 打印），有界 + 全程
`ReadProcessMemory`（猜错只是打出几行垃圾，不会崩）。

**一条会话能换回什么**：用户跑一次游戏，日志里就有一份 `NODEREG Refer::X pair2=… `
的完整映射。有了它，剩下的两件事都能往下走：

1. **移速的量化验证**：定位 `Refer::Speed` 的处理函数 → 它读的字段 → MOD 直接**打印速度值**
   （带增益 / 不带增益各一份），把"感觉快了"变成数字；
2. **防御取消攻击**（§4.53）：同一张表能给出 `Refer::IsAttackHitAfter` 等条件标志的地址，
   比现在"顺着 143 个调用者猜"要省得多。

### 4.52.10 第一次真正启动游戏的记录（第三轮末，含一次环境事故）

用户授权我自行启动游戏测试。已安装新 DLL（`35F87B81…`，含读回容器与 NODEREG 诊断），
并把 `SpeedBuffPercent`/`DamageCutPercent` 临时打开成 4。

**结果一：新 DLL 装得上、也加载得起来**（`mods` 里的 DLL **没有被锁**，
所以覆盖安装成功；loader 日志显示 `Loading mod: …Nioh1PerfectGuard.dll`）：
`ANCHOR 4/4 verified`、`SELFTEST breakpoint: OK: 5/5`、`STATUS ACTIVE`、
`SOUND ready`，以及 `NODEREG done: 47 Refer:: entries`。

**结果二（重要收获）：47 个 `Refer::` 名字的注册顺序拿到了**（见 §4.53.5），
而且它与 `.rdata` 里名字串的排列**一致**（注册表前面多出 5 项：`MotionId`/`MotionFrame`/
`ActionId`/`PrevActionId`/`IsMtdFlag`）。这 5 项正好是 §4.12 表里靠 getter 反推出来的那几个。

**结果三：我的布局猜测错了一处**（如实记下）：日志里 `pair2=` 打出来的其实是
**名字串自己的后半段**（例如 `Refer::MotionId` → `0x64496E6F69746F` = `"otionId"`）。
说明节点记录是 `{char name[]; …}`（名字**内联**在记录开头），而不是我想的
"名字指针在 +0，处理函数在 +8"。**下次要把名字 NUL 之后的那些 qword 打出来**，
才能拿到处理函数指针 —— 这是个一行改动，但本轮没再改（见"环境事故"）。

**结果四：跑约 3 分钟后崩溃，且与本轮新代码无关**（证据链完整）：

- 异常 `0xC0000005`，`nioh.exe+0x52F321`，`Dr6 = 0xFFFF0FF0` ⇒ **B0–B3 全 0，没有任何断点命中**；
- 日志里 **`PERFECT GUARD` = 0、`BUFF` = 0、`installing/removing engine state` = 0、`KITRACE` = 0**
  ⇒ 这个 session 里 MOD **没有写过任何游戏内存、也没有调用过任何引擎函数**，
  增益代码**根本没被执行到**；
- 本轮唯一相关的变数是**同时有两个游戏实例**在跑（旧实例 pid 21900 结束不掉）。

⚠ **对 §4.43 的更正**：那一节把 `0x52F321` 归因于"滚动重装（rearm_batch）"。
本次崩溃发生在 **`DiagDisable=0`（滚动重装是关的，日志里只有 `ARM purge`、没有 re-arm）**
的情况下，**同一个地址**。所以"这个地址 = 滚动重装"这个单一归因**不成立**：
要么存在两个原因，要么当时就有环境因素。下次必须在**单实例**环境里复现才能定论。

**结果五：一次环境事故，必须如实交代**。为了清掉那个结束不掉的旧实例，我执行了
`steam.exe -shutdown`。后果：

- **旧实例 nioh.exe (21900) 没被带走**（Steam 关了它还在；`taskkill`/`Stop-Process`/WMI
  `Terminate` 全部 `Access is denied`）；
- **Steam 重启后处于未登录状态**（`HKCU\…\ActiveProcess\ActiveUser = 0`），
  于是之后两次启动游戏都在**loader 还没扫 mods 之前**就退出
  （loader 日志只有 `Session started`，没有 `Loading mod`）—— 也就是说
  **现在的环境里游戏起不来了，而且与 MOD 无关**（其中一次还是 `Enabled=0` 完全旁路）。
- 我无法在非提权的情况下恢复它：需要**用户登录 Steam**（并最好结束/重启那个旧实例）。

---

## 5.1 伤害管线节点族（新发现，下一阶段的主要猎场）

`0x120CF90` 表里存在完整的碰撞/伤害管线节点，说明「命中 → 判定是否被格挡 → 扣精 → 反应」这条链在
`0x925900 - 0x926500` 一带有对应实现：

| 节点名 | 处理函数 RVA |
| --- | --- |
| `Collision::Damage::AddSphere/Capsule/Cylinder/Box` | `0x925E60` / `0x925AE0` / `0x925CA0` / `0x925900` |
| `Collision::Damage::SetSide` / `SetSelfObstruction` | `0x926420` / `0x926390` |
| `Collision::Damage::Hit::GetAttackerGameObjectCategory` | `0x926090` |
| `Collision::Damage::Hit::IsAttackBreakTokoyoArea` | `0x926230` |
| `Chara::IsDamagePlayer` | `0x91CCC0` |
| `Check::IsAttackCollisionOn` | `0x6F13A0` |
| `AutoHitCheck::IsHitStage` / `IsHitShot` / `SetHitCheckRange*` | `0x6F0CE0` / `0x6F0C50` / `0x6F0D70` / `0x6F0DE0` |

`Collision::Damage::Hit::*` 这一组是「针对某次命中查询其属性」，与精防判定高度相关。

## 5.2 结构式查询工具（新增能力）

`xref.py` 的交叉引用库现在额外索引了**内存操作数位移**与**每函数的直接调用集合**，支持按"形状"筛函数：

```
python xref.py find <db> --calls 0x755DC0 --disp 0x240   # 调玩家访问器 且 用到 +0x240
python xref.py find <db> --calls 0x755DC0 --calls2 0x7B4C40
```

当前筛选结果（`--calls 0x755DC0` 命中 497 个函数，`--disp 0x240` 命中 1172 个，交集 **205 个**）：

- `0x7B4C40`（精力扣减原语）**只有 2 个直接调用者**：`0x70ECE0`（7,468 字节大分发函数）、`0x749640`（26,632 字节）。
- `0x749640` **同时**出现在「调玩家访问器 + 用 +0x240」的交集里 → 是锚点 B 的首选嫌疑。
- 交集里的小函数（< 300 字节）更适合做干净锚点候选：
  `0x730A10`(192) `0x731573`(126) `0x745AE4`(217) `0x784BB0`(222) `0x8676A0`(68)
  `0x872180`(85) `0x872580`(73) `0x87DB45`(183) `0x87DC55`(183) `0x87DD43`(166) `0x87E0AA`(114)
- `0x7B4C20`（填满精力原语）有 21 个直接调用者，其中 `0x8A5B50` 即 `Player::RecoverStamina`（验证了方法正确）。

> ⚠️ 注意：`.pdata` 边界会把被 `/OPT:ICF` 折叠的函数合并进更大的范围，所以「函数大小」有时偏大；
> 反汇编时应用 `xref.py dumpfn --exact` 从精确地址开始，避免被误导。

## 5.3 运行时插桩管线（已建成并验证）✅

这是为了让「锚点定位」从纯静态推理转到**经验闭环**而建的工具，全部通过验证：

### 探针 DLL —— 硬件断点，零代码修改

`_work\probe.c` → `probe.dll`（Zig 构建）。用 **VEH（向量化异常处理）+ x64 调试寄存器 DR0–DR3**
在最多 4 个目标函数上装执行断点：

- **完全不修改游戏代码字节** —— 只读代码、写自己的日志文件。
- 命中时记录：目标编号、RVA、线程 ID、`rcx/rdx/r8/r9` 四个参数寄存器、
  **栈回溯**（扫描栈上落在 `.text` 范围内的返回地址，最多 6 层链）。
- DR 寄存器是**每线程**状态，所以探针每 700ms 轮询一次线程列表、只给新线程装断点
  （已装过的记在表里，避免反复 suspend 造成卡顿），每 20 轮做一次全量重装作为安全网。
- 配置来自 DLL 同目录的 `probe.ini`：`Module` / `Target1..4` / `MaxHits` / `PerTargetCap` /
  `Log`。**每目标独立上限**，避免高频目标淹没信号。
- 每 40 轮（≈28 秒）输出一行 `PROBE totals total=... t0=... t1=...` 累计计数，
  即使没导出函数也能看到总量。

### 注入器 —— 不改游戏目录、不需加载器

`tools\inject_dll.py`：纯 ctypes 实现 `OpenProcess` + `VirtualAllocEx` + `WriteProcessMemory`
+ `CreateRemoteThread(LoadLibraryW)`。

> 踩过的坑：`LoadLibraryW` 要 **UTF-16LE** 路径，早期用 ANSI 编码导致宽字符串乱码、
> LoadLibrary 静默返回 NULL。回读远程缓冲区才定位到。修复后一次成功。

### 为什么不用 dinput8 加载器

`Nioh2ModLoader` 的 dinput8 代理虽然理论可用于 `nioh.exe`，但：
(a) 需要往游戏目录写文件；(b) 递归扫描子目录未验证；(c) 与 Steam DRM 的交互未验证。
外部注入完全绕开这三点，且**可以在游戏已经运行、已经进关卡之后再注入**——这对
「进关卡才有运行态单例」这个约束是决定性的。

### 验证证据（全部本地复现，不依赖游戏）

| 测试 | 方法 | 结果 |
| --- | --- | --- |
| 断点引擎正确性 | `selftest.exe` 自加载探针并调用 `dummy_target(i,i+1,i+2,i+3)` 10 次 | 命中 **10** 次，`rcx=0..9 / rdx=1..10 / r8=2..11 / r9=3..12` 逐一吻合；未调用的 `never_called` **0** 命中 |
| 栈回溯 | 同上 | `chain=[0x122C]` 正确指向调用方 `main` 内的返回地址 |
| 外部注入 | `inject_target.exe` 运行中，从外部注入探针 | 注入成功，**42** 次命中，参数为 `tick(i)` 的 seq 值 |
| 日志汇总 | `summarize_probe.py` | 按目标分组命中数、线程分布、**直接调用方 TOP-N**、浅调用链、参数样本 |

### 一次性会话脚本

`tools\probe_session.ps1`：启动游戏（或复用已运行的）→ 写 `probe.ini` → 注入探针 →
（可选 `-Dump`）在关卡内同时抓取解密镜像 → 打印游戏内操作清单。

首轮配置的 4 个目标：

| 目标 | RVA | 选它的理由 |
| --- | --- | --- |
| t1 | `0x7B4C40` | 精力扣减原语，**只有 2 个直接调用者**，信号极干净 |
| t2 | `0x749640` | 首选嫌疑：同时"调玩家访问器 + 用 +0x240 + 调 t1" |
| t3 | `0x70ECE0` | t1 的另一个调用者 |
| t4 | `0x755DC0` | 玩家槽位访问器，**必然高频**，用来证明管线在游戏里是活的 |

---

### 实机验证（已完成，2026-09-26 20:34）

在真实的 `nioh.exe` 上跑通，证据：

```
PROBE begin pid=30580 module=nioh.exe targets=4 maxhits=20000 cap=400
PROBE base(0xnioh.exe)=0x7FF7AA2D0000
PROBE armed target0 rva=0x7B4C40 va=0x7FF7AAA84C40
PROBE armed target1 rva=0x749640 va=0x7FF7AAA19640
PROBE armed target2 rva=0x70ECE0 va=0x7FF7AA9DECE0
PROBE armed target3 rva=0x755DC0 va=0x7FF7AAA25DC0
PROBE text range 0x7FF7AA2D1000 - 0x7FF7AB46A498
HIT#1 target=3 rva=0x755DC0 tid=28964 ... chain=[0xE0CA9A]
```

结论：

1. **外部注入 `nioh.exe` 成功**（HMODULE=0x27B50000）。
2. **Steam DRM 对硬件调试寄存器无反应** —— 注入后游戏稳定运行 30 秒以上，
   工作集正常增长（1.4GB → 2.0GB），无崩溃、无退出。这是本阶段最大的未知项，现已排除。
3. 4 个断点全部装在正确 VA（基址 + RVA 吻合），`text` 范围正确解析。
4. **管线在游戏里是活的**：立刻命中玩家槽位访问器 `0x755DC0`。
5. 新发现：加载/标题画面阶段就有 **3 个不同的调用方**在调用玩家访问器：
   `0xE0CA9A`(134 次)、`0xDEDB00`(133)、`0x98468A`(133) —— 属于 UI/加载/系统路径。

> 操作教训：`0x755DC0` 在加载画面就以约 400 次/秒触发，**不能当"活性哨兵"**，
> 而且高频陷入异常会造成卡顿、干扰毫秒级精防时序。首轮配置已把它换成
> `Refer::Stamina`(`0x6F65D0`) 作为哨兵。

### 预算耗尽自动卸载（实测通过）

高频目标超过 `PerTargetCap` 后**直接从所有线程上摘掉该断点**，而不是继续陷入异常：

```
PROBE begin pid=40684 module=inject_target.exe targets=1 maxhits=20000 cap=6
PROBE target0 budget exhausted (6 hits); unarming
```

命中数恰好 6 条，随后不再产生任何条目。被抑制的命中**不消耗全局预算**，
所以冷门目标不会被高频目标"饿死"。

---

## 5.4 架构决策：用「硬件断点 + 上下文改写」替代 inline hook ✅（已验证）

**这是本项目最重要的架构判断，且已用实验证实。**

传统做法是在目标函数头部写 `jmp` 跳到自己的代码（inline hook）。但那需要：
指令长度解码器、被搬运指令的重定位、±2GB 内的 trampoline 分配、改代码字节（可能触发完整性校验）。

**换个思路**：硬件执行断点触发 `EXCEPTION_SINGLE_STEP`，在 VEH 里改**被保存的线程上下文**，
然后 `EXCEPTION_CONTINUE_EXECUTION` 放行。线程恢复执行时看到的就是被改过的世界 ——
**游戏代码字节一个都没动**。

已实现并验证三种改写：

| 动作 | ini 语法 | 用途 |
| --- | --- | --- |
| 改整数参数 | `ActionN=rcx=0x64` / `rdx` / `r8` / `r9` | 改伤害值、改精力扣除量、改状态标志 |
| 改 SSE 寄存器 | `ActionN=xmm1=0.0` | 精力/伤害在浮点寄存器里传参，这一条最关键 |
| 跳步（跳过指令） | `ActionN=skip=5` | 跳过整个 `call`，等价于"取消后摇" |

### 验证证据

自测程序里 `compute(5)` 正常返回 10。装断点 + `Action2=rcx=100` 后：

```
compute(5) = 200   (expect 200: the probe forces rcx=100 at entry)
  OK: behaviour changed without patching a single code byte
```

日志同时显示原始参数被正确捕获（`target=1 rva=0x1040 ... rcx=0000000000000005`），
随后在放行前被改写。

### 与仁王2 MOD 的对比

| | 仁王2 MOD | 本项目（仁王1） |
| --- | --- | --- |
| 挂钩方式 | inline hook（写 `jmp`）+ trampoline | **硬件断点 + 上下文改写** |
| 改代码字节 | 是 | **否** |
| 断点数量上限 | 无（可任意多） | **4（DR0–DR3）** |
| 完整性校验风险 | 有 | 无 |
| 需要指令解码器 | 是 | **否** |

**4 个断点的限制可以接受**，因为实际只有 3 类事件需要挂钩：

1. 格挡时的精力结算（读/改 `xmm1` → 免耗精、回精）
2. 命中结算（读攻击来源 → 对敌削精/HP）
3. 后摇取消（`skip`）

而**格挡输入边沿不需要断点** —— 用内存轮询（见 5.5）读取输入状态即可，零成本。

## 5.5 内存监视器（已实现，配置解析已验证）

按指针链周期采样并在**数值变化时**记一行，用来在一次会话里同时看到精力曲线：

```ini
Watch1=player_slot0;0x18A0490;+0x0;24
Watch2=player_stamina;0x18A0490;+0x240;+0x40;16
```

语义：`label;基址RVA;+偏移(解引用);...;末尾字节数`。
末尾那段会同时以 hex 和 **float 数组**两种形式打印（因为精力/伤害都是 float）。

**已实测**：在加载画面注入后，`0x18A0490`（玩家槽位表）始终为 NULL，
**因此没有任何 WATCH 记录** —— 这从运行时角度再次确认了「必须进关卡才有玩家对象」，
也验证了监视器"只记变化"的行为是对的（NULL 不刷屏）。

---

## 6. 工具链与环境（已就绪）

- **Zig 0.16.0 已安装并验证**：`_tools_dl\zig-x86_64-windows-0.16.0\zig.exe`
  构建命令：`zig cc -target x86_64-windows-gnu -shared -O2 -o X.dll X.c`
  冒烟测试通过：产出合法 x64 PE（`machine=8664`）、导出符号可用、`DllMain` 正常执行并写出日志。
  免安装、免管理员，整个工具链约 97 MB。
- **加载器可直接复用** `Nioh2ModLoader - dinput8 loader` 包里的 `dinput8.dll`：内部名
  `NiohNativeModLoader`（作者 balfa），扫描游戏目录下 `mods\` 并 `LoadLibraryW` 每个 DLL，
  **不校验 exe 名**，是通用 x64 dinput8 代理；`nioh.exe` 确实导入 `DINPUT8.dll!DirectInput8Create`，
  代理链成立。加载器日志写 `loader.log`，并有 `NiohModLoaderOverlay` 覆盖窗。
- **离线自检（不需要游戏）**：

  | 命令 | 验证什么 |
  | --- | --- |
  | `zig cc -target x86_64-windows-gnu -O2 -o test_logic.exe test_logic.c`（在 `tools\`） | 精防窗口 / 按键边沿 / 掩码 / 精力补回算术（30 条断言） |
  | `python tools\test_ini_encodings.py mod` | INI 编码与取值边界（5 组用例）+ **内置默认值必须等于随包 INI** |
  | `python tools\test_doc_markers.py .` | 文档声明的日志标记/配置键必须真实存在；每条玩家可见日志都必须被解释过 |
  | `python tools\test_doc_paths.py .` | 随包文档提到的每个文件都必须真的可达；引用开发目录时必须写明 |
  | `python tools\test_logic_digest.py .` | **测试过的算术必须等于发货的算术**（比较两个构建的数值指纹） |
  | `python tools\test_doc_counts.py .` | 文档里写的数字（导出数、断言数、标记数、日志行数）必须等于实测值 |
  | `python tools\test_anchors.py .` | 锚点期望字节必须与解密镜像逐字节一致（含备用锚点与输入槽推导） |
  | `python tools\compare_builds.py A.dll B.dll` | 两次构建的差异是"代码"还是"仅构建元数据"（含 PE 头时间戳与内嵌输出名；见 §4.49） |
  | `python tools\list_exports.py mod\Nioh1PerfectGuard.dll` | 导出表（应为 12 个 `PG_*`） |
  | `_work\dr7test.c`（zig cc 编译运行） | DR0–DR3 执行断点 + VEH 是否会触发（含 0 命中的对照组） |

  纯逻辑单一来源：`mod\pg_logic.h` 同时被 MOD 本体和 `tools\test_logic.c` 包含。

## 7. 当前状态与剩余待办

上面 1–4 项**均已完成**：探针管线已建成并实机跑通（§5.3），输入系统已定位到
`wButtons`/`QueryButton` 与手柄绑定对象（§4.13），及时格挡改为由
"格挡键按下沿 + 250ms 窗口"自行判定（引擎无原生该状态位），对敌削精/伤害
已按 `Character` 同族访问器实现（§4.2）。

**MOD 本体已完工**（`mod\Nioh1PerfectGuard.c` + `mod\pg_logic.h`，DLL 226,304 字节）：
硬件断点 + VEH 改上下文，不改一个字节游戏代码；4 个锚点按期望指令字节校验，
失败即拒绝安装；`STATUS ACTIVE anchors=4/4` 已在加载器路径下实测；
**核心陷阱机制每次启动自动自检**（§4.24），实机输出 `SELFTEST breakpoint: OK: 5/5`；
**纯逻辑有离线单元测试**（§4.25/§4.28/§4.36/§4.37，113 条断言全过，与本体共享同一头文件）；
**配置默认值与随包 INI 由测试钉住一致**（§4.27.2，6/6 用例）。

共 12 个导出：`PG_GetVersion`、`PG_GetStatus`、`PG_GetBlocks`、`PG_GetPerfect`、
`PG_GetRewards`、`PG_GetFreeGuards`、`PG_GetInputInfo`、`PG_GetSelfTest`、
`PG_GetLogicDigest`、`PG_ReloadConfig`、`PG_SelfTest`、`PG_DumpState`。

### 7.1 剩下的全部是不可静态化的实机验证

| # | 待验证 | 怎么看日志 |
| --- | --- | --- |
| 0 | **断点机制是否真的会触发** | `SELFTEST breakpoint: OK: 5/5 ...` —— **已在本机 nioh.exe 内证实**（§4.24） |
| 1 | **扣精表项是否驱动可见精力条** | `KIV block #n charge D=.. visible loss L=.. -> 结论`（自动判定，不需要算） |
| 2 | `CancelRecovery`（后摇取消）是否生效 | `RECOVERY cancel: motion frame X -> Y` |
| 3 | 对敌削精/HP（需先把 INI 值调 >0） | `ENEMY ki/hp damage` |
| 4 | 键鼠玩家的 `GuardButtonMask`/`GuardKeyVK` 实际值 | `LEARN key VK=0x..`（`LearnButtons=1` 标定） |

### 7.2 卡在哪：AI 无法自主进关卡

工作站处于锁屏状态（`GetForegroundWindow -> NULL`，`PrintWindow` 约 95.9% 全黑），
**截屏与合成输入都不可用**；且无存档（`Savedata\1149138589\` 只有 53 字节的
`steam_autocloud.vdf`），标题画面**不构造玩家对象**（实测 `[0x18A0490]` 为 NULL）。
所以 7.1 的四项只能由人操作一局。

**给玩家的一行操作**：Steam 启动 → 新游戏 → 进关卡打一场 →
把 `<游戏目录>\mods\Nioh1PerfectGuard\Nioh1PerfectGuard.gameplay.log` 发回。

> Nioh 1 有在线存档校验，建议**离线模式**游玩以免 MOD 影响云端存档。

---

## 4.54 【已修·真 bug】旗标事件源下"精力补回"从来不可能生效（外部用户实测定位）

来源不是本机实验，而是 Nexus 上一位用户的反馈：**其它奖励全部正常，只有格挡不返还精力**。
他附了日志，那份日志里有两行足够定案：

```
KITRACE ki=105/105 | entry7 flag=-1 cur=0 cost=100
PERFECT GUARD #1 via guard-flag (guard pressed 328ms ago)
LAYOUT ... ki=71.03/105 action=3184 frame=49
KITRACE ki=97.59/105 (changed: ki)
```

### 4.54.1 为什么这一定不是用户配错

- `entry7 flag=-1`：格挡资源表项**从未**进入"已计费"状态 → 扣精点的三道前置条件
  （§4.44）自始至终没满足 → 模块跑在**旗标事件源**上。这条日志与"使用旗标源"的
  `NOTICE` 一致。
- `LAYOUT` 与 `PERFECT GUARD` **同一毫秒**（05.241），里面 `ki=71.03/105`，而同一份
  日志前面是满的 `105`。也就是说：**旗标被写下时，引擎已经扣完精了**。
- 整份日志**没有一条 `KIRESTORE`** → 补回路径一次都没有写过内存。

### 4.54.2 根因（以及我此前文档里的错误结论）

旧实现的 `ki_snapshot()` 在**事件时刻现读**可见精力当基准：

```c
float ki = *(float *)((char *)param + 0x40);   // 事件已经发生，这里已是扣完后
pg_ki_begin(&g_ki, ki, ...);                   // => 基准含这次扣除
```

`pg_ki_step()` 是按 `snapshot - now_value` 算损失的。基准里已经含了这次扣除，
后续精力只会上涨，损失恒为 0 → `want = 0` → **一分钱都不还**。

关键区别在于两个事件源的时间顺序：

| 事件源 | 触发时刻 | 现读基准是否正确 |
| --- | --- | --- |
| 扣精点 `0x74DE51` | 引擎减法**之前**（断点就在调用指令上） | ✅ 正确 |
| 旗标点 `0x74DD09/0x74DD92` | 引擎减法**之后** | ❌ 基准已含扣除 |

所以此前写在 INI / README / 验收文档里的"旗标源下减免靠 `KiTopUp=1`"是**错话**：
`KiTopUp` 在旗标源下等于死设置。这也解释了为什么本机（`state` 里记着
`costsite=never`）从来没有观察到补回生效 —— 不是没测，是根本不可能发生。

### 4.54.3 修法：滚动"事件前"样本

- 8ms 输入线程每 tick 把可见精力压进一个 64 槽环形缓冲（写者是输入线程、读者是
  VEH 里的游戏线程，因此用 **seqlock**：写前写后各动一次序号，读者只在序号未变
  且为偶数时采信，撕裂就退回现读值 —— 异常处理器里绝不自旋）。
- 事件发生时，基准 = **事件前 `KiTopUpPreEventMs`（默认 100ms）窗口内的最高值**，
  且不低于现读值（`pg_ki_ref()`，纯函数、可离线单测）。
- 该规则**只用于旗标源**：扣精点源的现读值本来就是对的，若也在那里回溯窗口，
  会把"这一刀没造成、但恰好在窗口内的消耗"重复补回。
- 新增两条日志：`KIREF`（基准取到了多少、现读多少）与 `KIV-FLAG`（这次补回了多少）。
  以前旗标源下 `KIV` 直接静默返回，用户看不到任何结论 —— 这也是"看起来像坏了"的一半原因。

代价如实记录：落在窗口内的**无关**消耗会被一并补回；窗口越短越保守，0 = 关闭。
默认 100ms 的依据：引擎扣精与旗标写在同一帧内（本次实测同毫秒），而"先出一刀再格挡"
的那一刀的精耗通常在前面 250ms 以外。

### 4.54.4 同一份日志的附带结论

- `KiRecoveryMode=3` 的算术完全吻合：`71.03 + 105/6 = 88.53`，638ms 后 `97.59`
  （差 +9.06 是游戏自身回复）。补回没生效时，回精模式是唯一在工作的那条。
- 该用户是**键鼠**玩家（`GUARD pressed ... key=0x02` 即鼠标右键）：`AttackButtonMask`
  是手柄位掩码，对键鼠的"防御+攻击"组合键保护无效 —— 已知限制，本轮未修，
  需要新增键盘攻击键位（`AttackKeyVK`）才能覆盖。
- 那 34 点精力也可能**部分来自他自己的攻击**（日志里他先出了 action 3181 才按防御）。
  因此给他的验证建议是："站着不动、不先攻击，单按防御挨一刀" ——
  若精力根本不掉，说明他这套 build 的格挡本来就不扣精。

### 4.54.5 回归

新增 13 条断言（200 → **213**），覆盖：窗口内取最高值、窗口外不取、空环/未写槽/
超界值/时钟回退一律忽略、关闭（0）退回现读值、以及"基准 105 + 现值 71.03 + 减免 100%
→ 首次 tick 就回到 105（正好还回 33.97）"和"减免 50% → 只回到 88.02"两条端到端用例。
`KIREF` / `KIV-FLAG` 两条新日志同时进了验收文档的标记表与文档一致性检查。

---

## 4.55 九十九槽（精华量表 / 守护灵槽）：找到引擎自己的"加量表"状态对象

需求是两个新功能：① 精防成功积累九十九槽（默认关，默认 10%）；② 九十九状态下精防
续烧条（默认关，默认 10%）。两者共用一个写入，区别只在**什么时候**写。

### 4.55.1 先确定它叫什么（名字对了，代码就浮出来了）

- 引擎内部把九十九叫 **Tsukumo**：符号表里有 `Player::SetTsukumoWeaponActiveFlag`、
  `Tutorial::GetCountTsukumoUse`，字符串 `TSUKUMO_WEAPON`、`Living Weapon Activated`。
- 把"守护灵 / 精华量表"叫 **Amrita / Amurita**：`Gadget::GetAbosrbAmuritaRate`、
  `Gadget::UseAbosrbAmurita`、`Character::ForceDropAmurita` ……
- 真正的突破口是 **RTTI 类名**（`analyze.py rtti --grep Amrita`）：

```
Character::AddStateObjectAmritaGaugeUp             vft=0x11A8B90
Character::AddStateObjectAmritaGaugeRecover        vft=0x11A95E0
Character::AddStateObjectAttackHitRecoverAmrita    vft=0x11A8D20
Character::AddStateObjectAmritaUpPlus              vft=0x11A8DC0
Character::AddStateObjectDyingAmritaUp             vft=0x11A9400
```

即"给精华量表加量"是**引擎自己的一个状态对象**（与 §4.51/4.52 那套 buff 状态对象同一
体系），而不是某个可以直接写的字段。

### 4.55.2 量表是 0～1 归一化的：从 apply 的两行反汇编直接读出

取 `AmritaGaugeUp` vtable，逐个方法反汇编，slot 8（`0x7A8120`）就是 apply：

```asm
007A8120  movss xmm2, [rcx + 0x50]      ; 构造函数第 2 个 float = 本次增量
007A8128  addss xmm2, [rdx + 0x15C]     ; += 量表当前值（rdx = 目标对象）
007A8130  movss xmm3, [rip+0xde0800]    ; -> 0x1588938，float = 1.0
007A8138  movaps xmm1, xmm2
007A813B  subss xmm1, xmm3
007A813F  comiss xmm1, xmm0             ; xmm0 = 0
007A8142  jb   0x7A814D
007A8144  movss [rdx + 0x15C], xmm3     ; 超过 1.0 → 夹到 1.0
007A814D  movss [rdx + 0x15C], xmm2     ; 否则写回新值
```

三条结论一次到手：

1. 量表是**归一化 float（0～1）**，上限常量就是 `1.0`；
2. `"10% 槽" ＝ 构造函数第二个参数 0.10` —— 百分比不需要任何换算或猜测；
3. **不需要知道量表挂在哪个对象上**：写 `[rdx+0x15C]` 的 rdx 由引擎的状态系统传进来。

（附带：`AmritaUpPlus` 的 apply 用的是 `[rbx+0x100]`，与量表不是同一个字段 —— 可见这一族
状态对象各自改各自的字段，"哪个类改哪个字段"必须一个个看，不能类推。）

### 4.55.3 构造函数：`0x79E870`，状态 id `0x20`，与 buff ctor 同簇

vtable 指针的写引用（`ripref.py` refs 0x11A8B90）把 ctor 钉在这一带；**真起点是
`0x79E870`**：

```asm
0079E870  mov qword ptr [rsp + 8], rbx  ; ★ 前 16 字节：保存寄存器 + 分配栈帧
0079E875  push rdi
0079E876  sub rsp, 0x50
0079E87A  movaps [rsp + 0x40], xmm6
0079E87F  mov rbx, rcx                  ; arg1（管理器）
0079E887  movaps xmm6, xmm1             ; arg2 = 时长（秒）
0079E88A  movaps xmm7, xmm2             ; arg3 = 增量
0079E8AF  call [r9+0x28]                ; 分配（0x58 字节）
0079E8CE  mov [rax+8], rcx              ; rcx = [arg1+0x168] ← 宿主角色指针
0079E8D9  mov qword [rax+0x10], 0x20    ; ★ 状态 id = 0x20
0079E90C  movss [rax+0x50], xmm7        ; 增量存 +0x50（apply 读的就是它）
0079E916  mov qword [rax], rcx(=vtable)
```

> **踩过的坑（务必记住）**：第一版把起点写成 `0x79E880` —— 那是 `sub rsp,0x50` **之后**的
> 地址，跳进去会少分配 0x50 字节栈、还会跳过 `mov [rsp+8],rbx` 的保存，等于在游戏线程里
> 破坏栈帧。之所以没被"必须盖章自己 state id"的校验拦住，是因为 id 存点（`+0x69`）无论从
> `0x79E870` 还是 `0x79E880` 起算都落在 0x80 的扫描窗口内 —— **校验能证明"这是对的函数"，
> 不能证明"这是函数的开头"**。两条独立线索定案：① `.pdata` 里这个函数从 `0x79E870` 开始
> （175 字节）；② 只有从 `0x79E870` 起反汇编才有完整序言（保存 rbx/rdi + `sub rsp,0x50`），
> 从 `0x79E880` 起的第一条指令就是往 `[rsp+0x30]` 写 xmm —— 一个没有栈帧的写入。
> 并行的静态复核（见 §4.55.7）独立得出同一个起点。

- 状态 id **0x20** 落在 MOD 现有校验的扫描窗口（`mov qword [rax+0x10], imm32`，见 §4.52 的
  `ctor_stamps_state_id`）内 → 可以直接复用"**必须盖章自己的 state id** 才允许跳进去"这条规则。
- `[arg1+0x168]` 是宿主角色：这一点与 §4.51 的观察一致 —— 状态对象的方法都用
  `[[obj+8]+0x240]`（= 角色 → param），所以 obj+8 是角色，ctor 的 arg1 就是"持有角色指针的
  那个对象"，也就是 MOD 现有的 `buff_manager()`（`[[char+0x240]]+0x10B0`）。
- 引擎自己的加状态调用点（`ripref.py callers 0x7A2890`，43 处）显示它**同一个指针**同时传
  ctor 与 add：`mov rcx, rbx; call ctor` → `mov rcx, rbx; call add`。这在 §4.52.6 曾是一个
  疑点（"我们是不是把容器当成了管理器"），现在可以排除：形状与引擎自己一致。

### 4.55.4 "是否在九十九状态"：容器的 `CallSpirit`(0x22) 为主，激活标志字节为辅

第一版只用引擎那个激活标志：`Player::SetTsukumoWeaponActiveFlag`（符号表 `0x8A6AA0`）把
布尔写到"进程全局 `0x18715E0` 指向的对象 +0x104"：

```asm
008A6ADD  mov rcx, [rip+0xfcaafc]   ; -> 0x18715E0，进程全局里的对象指针
008A6AE4  mov byte [rcx+0x104], al  ; ★ 九十九激活标志 = 该对象 +0x104
```

该字节的语义是**静态推断**（函数名 + 写入点一致），不足以单独当判据。并行的静态复核
（§4.55.7）给出了更好的判据：**九十九状态期间，容器 `[[char+0x240]]+0x10B0` 里存在
`CallSpirit` 状态对象（状态 id `0x22`，ctor `0x79F1B0`）** —— 这不需要任何新偏移，
MOD 已有的 `buff_state_present()` 遍历（§4.52.8）本来就能按状态 id 找节点。

所以实现取**两路相或**：

| 来源 | 读法 | 角色 |
| --- | --- | --- |
| 容器状态 `0x22` | `buff_state_present(mgr, 0x22, NULL) == 1` | 主判据 |
| 激活标志字节 | `*(BYTE*)(*(void**)(base+0x18715E0) + 0x104)` | 只允许"补充"证据 |

任一路说"在"就算在；一路可读且说"不在"、另一路读不到就算不在；两路都读不到 → 未知（-1，
`pg_lw_plan` 当"不在"处理）。偏向是刻意的：**误判为"在"只会用错百分比（无害），误判为
"不在"会让"攒槽"功能永远不触发**。判定在输入线程上每 8 个 tick（约 64ms）刷新一次并缓存，
VEH 里只读缓存 —— 异常处理器里不该走 256 个节点的树。

### 4.55.5 落地的实现与它为什么这样写

- 新增纯逻辑 `pg_lw_plan(gauge_on, gauge_pct, extend_on, extend_pct, in_lw)`：九十九状态中
  且"续烧条"开着 → 用续的百分比；否则用攒槽的百分比。**顺序是刻意的**：续烧条没开时标志
  根本不参与判断，所以标志读错无法把攒槽功能关掉。
- 写入沿用限时增益那套（`ctor(mgr, 0.05s, pct/100)` → `add(mgr, 0x20, obj, -1, 0)`），
  **永不调用移除**（§4.52 的崩溃），时长交给引擎过期；**引擎调用只在输入线程**，
  VEH 里只把百分比 parking 到一个 volatile 变量。
- 新增 14 条断言（213 → **227**）与两条日志（`LW engine:` 一次性校验、`LW gauge:` 每次调用
  写明加了多少 / 是否在九十九状态 / `add()` 返回值 / 容器回读结果）。
- 未实机验证：与限时增益一样，默认关闭，且只有实机日志能证明"引擎真的把幅度加上去了"。

### 4.55.6 顺带得到的两件工具性收获

1. **引擎自带符号表**：`.rdata` 里有一张 `{name_ptr, func_ptr}` 的 16 字节记录表（1466 条），
   `analyze.py namerefs` 已经能查其中一条；本轮把它整张导出成 `_work\symtab.txt`
   （`Player::*` / `Gadget::*` / `Character::*` / `Refer::*` 等），以后按名字找函数不再靠猜。
2. **RTTI → vtable → 方法**这条链（`analyze.py rtti`）可以直接把"某个效果是哪个类、哪个
   方法在改哪个字段"读出来，本轮就是靠它一步跨过"量表字段在哪"这个原本最难的问题。

### 4.55.7 并行静态复核的结论（独立一轮，报告在 `_work\re_lw_report.md`）

为了不让一个判断独自承担风险，本轮同一问题开了第二条独立线。它带回了三件事：

**① 它确认了实现路线**（"LW/amrita 由施加到容器 `[[char+0x240]]+0x10B0` 的**状态对象**驱动，
不是直接写量表字段"），并重建了完整的状态 id → 类映射（用 RTTI vftable 偏移对 6 个已知
ctor 校准为 0；"ctor = 唯一 `lea` 该 vftable 的函数"；id 存 `[obj+0x10]`）：

| 类 | id | ctor |
| --- | --- | --- |
| `AmritaGaugeUp` | `0x20` | `0x79E870` |
| `AmritaGaugeRecover` | `0x42` | `0x79E7B0` |
| `AttackHitRecoverAmrita` | `0x25` | `0x79ED80` |
| `AmritaUpPlus` | `0x27` | `0x79E920` |
| `DyingAmritaUp` | `0x38` | `0x79FB40` |
| **`CallSpirit`** | **`0x22`** | `0x79F1B0` |
| `TransformSpirit` | `0x26` | `0x7A1EE0` |

并且所有 amrita / 守护灵相关的施加点都落在**同一个 32,603 字节的大函数 `0xA83F97`** 里
（例：`0xA8667C lea rcx,[r15+0x10b0]` → `0xA86690 call 0x79F1B0`）。
`AmritaGaugeUp` 的 ctor 起点**独立得出 `0x79E870`**，与本轮 §4.55.3 的更正一致。

**② 它给出了更好的"是否在九十九状态"判据**：容器里存在 **`CallSpirit`(0x22)** ——
已按上面的办法实现（§4.55.4）。

**③ 量表的宿主对象仍未唯一确定，它拒绝编一个偏移**：确认了表基址
`[[char+0x240]]+0xBB0`、16 项 × 0x50、字段 +0x00 状态 / +0x0C 当前 / +0x40 状态 id，
也确认 `0x7B4C20`（当前=上限，即"填满"）的 22 个调用点**全部**指向 record 0
（`param+0x40`），且 `param` 构造函数会把 `[param+0x44]` 硬写成 `100.0f` 再填满 record 0。
但这与本项目自己用引擎 getter 证明过的"`param+0x40/+0x44` = 精力"（§4.17）冲突 ——
于是把这个冲突**如实留着**，没有硬凑结论。

> 这个"未确定"**不影响本轮实现**：本功能走的是状态对象那条路（引擎自己往正确的对象上写），
> 不需要知道量表字段在哪。它影响的是"能不能在游戏外自证效果"，因此验收仍以游戏内看量表为准。
>
> 它同时给出了一次就能定案 ① 的只读测量（下一轮若要做）：在进入九十九前后各 dump 一遍
> `[[char+0x240]]+0x40/+0x44/+0x48/+0x4C` 四个 dword —— 如果 `+0x44` 从 100.0f 上移、
> 或 `+0x40` 在九十九期间单调下降，那么 record 0 就是九十九槽。



