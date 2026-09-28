# Where the amrita / 99 ("Tsukumo" / Living Weapon) gauge is stored, and what writes it

**Target**: `nioh.exe` 1.24.8 (Nioh 1 Complete Edition) — static analysis only, **no game session**.
**Image under analysis**: `D:\AIWorkspace\DSHWorkSpcae\Nioh1PerfectGuardMod\_work\nioh1.mem.exe`
(50,782,208 B, decrypted runtime image; **file offset == RVA**; image base **0x7FF7AA2D0000**;
all addresses below are **RVAs** unless they are raw runtime pointers from the dump).

**Method**: Capstone linear disassembly driven by the exact function boundaries in
`_work\nioh1.mem.funcs.txt` (from `.pdata`), plus a whole-image one-pass scan
(3,899,266 instructions / 51,747 indirect-call sites) that I cached, plus the pre-existing
`_work\nioh1.xref.pkl` displacement index, plus the RTTI (`_work\nioh1.mem.rtti.json`) and the
engine's own 1466-entry symbol table (`_work\symtab.txt`).

**Confidence vocabulary**: *confirmed* = direct code/byte evidence, reproducible;
*probable* = strong but indirect; *guess* = explicitly labelled.

**Files written by this task**: only this file. Scratch scripts live outside the project in
`%TEMP%\gauge_re\` (`h.py`, `s1..s43*.py`); nothing in the project tree was modified.

---

## 0. Answers in one table

| # | Question | Answer | Confidence |
|---|---|---|---|
| 1 | Caller of the "apply" `0x7A8120` | Exactly **one** reference in the whole image (`0x11A97F0`). It is invoked **only** virtually, from the generic per-state tick **`0x7ACCA0`**, at **`0x7ACD1A`**: `mov rax,[rbx]; movaps xmm2,xmm7; mov rdx,rdi; mov rcx,rbx; call qword ptr [rax+0x30]` — i.e. **vtable slot 6 (`+0x30`), not slot 8 (`+0x40`)**. | **confirmed** |
| 1b | What the apply receives in `rdx` | **The state-manager object** at every one of the 8 call sites of `0x7ACCA0`: `rdx = rsi/rbp/rdi` = the object that owns `+0x168` (char), `+0x170` (state store), `+0x180/+0x188/+0x198/+0x1A8` (state lists) = **`[[char+0x240]] + 0x10B0`** = `param + 0x10B0`. | **confirmed** |
| 2 | Chain from the player character to that object; DLL expression | `param = *(void**)(*(void**)0x18A0490 + 0x240)`; target = `(char*)param + 0x10B0`; the field written by `0x7A8120` = **`(char*)param + 0x120C`** | **confirmed** |
| 3 | The generic per-frame state tick | **`0x7ACCA0`** (271 B, `0x7ACCA0..0x7ACDAF`). Full listing in §3. It handles duration book-keeping, calls slot 5 (`+0x28`) once on activation, then slot 6 (`+0x30`) with `(rcx=state object, rdx=manager, xmm2=dt)` every frame, and slot 7 (`+0x38`) on deactivation. | **confirmed** |
| 4 | **Correction**: is the premise right? | **No.** `0x7A8120` is **not** `AmritaGaugeUp`'s apply and it is **not slot 8**. It is slot **6** of `Character::AddStateGuardPenetration` (vtable `0x11A97C0`, state id **0x48**, ctor `0x79FED0`). So `[rdx+0x15C]` = a **guard-penetration ratio (0…1)** in the manager, not the 99 gauge. | **confirmed** |
| 5 | What `AmritaGaugeUp` (id 0x20) really does | Its apply is slot 6 = **`0x7A8280`**: `[manager+0x64] *= (magnitude+1)`. It is a **rate multiplier**, it never adds to any gauge. (Explains why the mod's out-of-band `AmritaGaugeUp` insertion produced no visible gauge change.) | **confirmed** |
| 6 | Where the 99 / amrita bar really lives | A **sub-object embedded in `param` at `+0xB0`** (`[[char+0x240]]+0xB0`): `+0x00` = int mode (1 / 2 / -1 = invalid, `0x7B4220` = `mode==1`), `+0x08` = int LW mode 0…7, `+0x10` = int counter with max `+0x18`, `+0x1C` = float (clamped by `+0x20`) — **this is the measured "burning bar" `param+0xCC`**, `+0x4C` = int accumulator, **`+0x50` = float gauge** that is only ever *decremented*, and reaching 0 calls `0x7C7EB0(param)`; `+0x58` = owner `char*`. | structure **confirmed**; which of `+0x50` / `+0x1C` the HUD shows as "the 99 gauge": **probable** |
| 7 | Bonus: how the engine drains the gauge | **`0x7B08C0(sub, xmm1)`** (float) and **`0x7B0860(sub, edx)`** (`= (float)edx * [0x11A055C]`): both `[sub+0x50] -= x; if (x < 0) [sub+0x50] = 0; if (hit 0) call 0x7C7EB0(param)`, where `sub = param+0xB0` and `param = [[sub+0x58]+0x240]`. | **confirmed** |
| 8 | Single expression a DLL should write | If the goal is "make the 99 gauge": **`*(float*)((char*)param + 0x100)`** (= `sub+0x50`) — but see §7 for the sign caveat and for the *recommended* alternative (drive the engine's own `AmritaGaugeRecover` state instead of writing the field). | **probable** |

---

## 1. Correction #1 — what `0x7A8120` actually is

### 1.1 The function itself (raw bytes, as given in the brief, confirmed)

```
007A8120  f30f105150               movss xmm2, dword ptr [rcx + 0x50]
007A8125  0f57c0                   xorps xmm0, xmm0
007A8128  f30f58925c010000         addss xmm2, dword ptr [rdx + 0x15c]
007A8130  f30f101d0008de00         movss xmm3, dword ptr [rip + 0xde0800]   ; -> 0x1588938 = 1.0f
007A8138  0f28ca                   movaps xmm1, xmm2
007A813B  f30f5ccb                 subss xmm1, xmm3
007A813F  0f2fc8                   comiss xmm1, xmm0
007A8142  7209                     jb 0x7A814D
007A8144  f30f119a5c010000         movss dword ptr [rdx + 0x15c], xmm3
007A814C  c3                       ret
007A814D  f30f11925c010000         movss dword ptr [rdx + 0x15c], xmm2
007A8155  c3                       ret
```
raw: `f30f1051500f57c0f30f58925c010000f30f101d0008de00…`
Constant at `0x1588938` reads **1.0f** (`h.f32(0x1588938)`), so this is
`target->f15C = min(target->f15C + this->f50, 1.0f)` — a saturating accumulator into a
normalised 0…1 field. That part of the brief is right.

### 1.2 It is at slot 6 of a *different* class's vtable

I searched the entire image for the 8-byte pointer `0x7FF7AAA78120` (= image base + `0x7A8120`):

```
targets: apply 0x7A8120 : 1 hit(s)
    file 0x11A97F0   .rdata   rva 0x11A97F0
```

Exactly one occurrence. The 9-entry vtable block it belongs to starts at **`0x11A97C0`**
(blocks are `0x50` apart: `0x11A9770`, `0x11A97C0`, `0x11A9810`, each = 9 method slots at
`+0x00…+0x40`); `0x11A97F0 − 0x11A97C0 = 0x30` ⇒ **slot 6**:

```
0x11A97C0 : 0x7FF7AAA6C2C0 -> rva 0x79C2C0   slot 0
0x11A97C8 : 0x7FF7AAA7C530 -> rva 0x7AC530   slot 1
0x11A97D0 : 0x7FF7AAA73DA0 -> rva 0x7A3DA0   slot 2
0x11A97D8 : 0x7FF7AAA74A00 -> rva 0x7A4A00   slot 3
0x11A97E0 : 0x7FF7AAA75720 -> rva 0x7A5720   slot 4
0x11A97E8 : 0x7FF7AAA75900 -> rva 0x7A5900   slot 5
0x11A97F0 : 0x7FF7AAA78120 -> rva 0x7A8120   slot 6   ★ the brief's "apply"
0x11A97F8 : 0x7FF7AAA77590 -> rva 0x7A7590   slot 7
0x11A9800 : 0x7FF7AAA78110 -> rva 0x7A8110   slot 8   (3-byte `c2 00 00` = `ret 0`)
```

I resolved the class name **independently of the pre-existing `rtti.json`** by walking the RTTI
CompleteObjectLocator at `vtable−8`:

```
vftable 0x11A97C0   COL rva 0x1593B10
   signature=1 offset=0 cdOffset=0 pTypeDescriptor=0x17FE248 pClassDescriptor=0x1593B38
   name = ".?AVAddStateGuardPenetration@Character@@"
vftable 0x11A8B90   COL rva 0x1592790   name = ".?AVAddStateObjectAmritaGaugeUp@Character@@"
```

So:

* `0x11A97C0` = `Character::AddStateGuardPenetration`, and `0x7A8120` = its **apply at slot 6**;
* `0x11A8B90` = `Character::AddStateObjectAmritaGaugeUp` (**the class the brief names**), and its
  slot 6 is **`0x7A8280`**, its slot 8 is `0x7A8110` (`ret 0`).

### 1.3 Why the mis-attribution happened (and how to avoid it)

Every ctor in this family has the identical prologue and the identical tail:
`movss [rax+0x50], xmm7` (magnitude) + `mov qword [rax+0x30], 0x3f800000` +
`mov qword [rax+0x10], <state id>` + `mov qword [rax], <vtable>`. So "reads `[rcx+0x50]`"
and "the ctor stores the magnitude at `+0x50`" are true of **many** classes, and the address
`0x7A8120` sits only 0x10 bytes after `0x7A8110` (the shared no-op that *is* at slot 8 of
`AmritaGaugeUp`). Reading "the qword at `vtable + 8*8`" from the *wrong* vtable base (or
assuming the first `movss [reg+0x50]` apply found by a pattern scan belongs to the class of
interest) produces exactly this off-by-one-class error. The reliable discriminator is the
**slot index inside the vtable**, plus the RTTI COL immediately at `vtable−8` — not the shape
of the method body.

### 1.4 `AmritaGaugeUp`'s real apply, for the record

```
############ AmritaGaugeUp vt=0x11A8B90 (COL -> AddStateObjectAmritaGaugeUp) ############
slot 0 (+0x00) = 0x79C350   ; destructor (sets vptr to base 0x11A80F0)
slot 1 (+0x08) = 0x7AC4E0   ; movss [rcx+0x50], xmm1        -> set magnitude
slot 2 (+0x10) = 0x7A3CF0   ; clamp([rcx+0x50]) -> returns mag+1 (0 if mag<0 -> 1.0)
slot 3 (+0x18) = 0x7A4A30   ; setae al  ([rcx+0x50] >= 0)   -> "is active"
slot 4 (+0x20) = 0x7A5720   ; cmp [rdx+0x10],[rcx+0x10]     -> "same state id"
slot 5 (+0x28) = 0x7A5990   ; onActivate (calls 0x7AC190, tail-jmp 0x7AC2B0)
slot 6 (+0x30) = 0x7A8280   ; ★ APPLY
slot 7 (+0x38) = 0x7A7980   ; ret 0
slot 8 (+0x40) = 0x7A8110   ; ret 0
```

```
007A8280  4053                     push rbx
007A8282  4883ec20                 sub rsp, 0x20
007A8286  488b01                   mov rax, qword ptr [rcx]        ; vtable of the state object
007A8289  488bda                   mov rbx, rdx                    ; rbx = TARGET (the manager)
007A828C  ff5010                   call qword ptr [rax + 0x10]     ; slot 2 -> mag+1
007A828F  f30f594364               mulss xmm0, dword ptr [rbx + 0x64]
007A8294  f30f114364               movss dword ptr [rbx + 0x64], xmm0
007A8299  4883c420                 add rsp, 0x20
007A829D  5b                       pop rbx
007A829E  c3                       ret
```

`0x7A3CF0` (slot 2) returns `x+1` for `x = [rcx+0x50] >= 0`, else `1.0f`:

```
007A3CF0  movss xmm2,[rcx+0x50]        ; x
007A3CF8  movss xmm3,[0x1588938]       ; 1.0f
007A3D00  addss xmm2,xmm3              ; x+1
007A3D07  subss xmm1,xmm3              ; x
007A3D0B  comiss xmm1,0
007A3D10  movaps xmm0,xmm2 ; ret       ; x+1 if x>=0
007A3D14  movaps xmm0,xmm3 ; ret       ; else 1.0
```

⇒ `AmritaGaugeUp(mgr, duration, mag)` multiplies **`manager->f64` by `(1+mag)`** every frame it
is ticked (the base is recomputed each frame from stats, so in practice this is a `+mag` bonus to
whatever `manager+0x64` is used for — *not* an increment of a gauge). The AMRITA gauge itself is
**not** touched by this class.
**This alone explains the mod's symptom** ("`add()` returns success but the gauge never changes"):
the engine's `AmritaGaugeUp` is a rate multiplier, so a `+10 %` expectation cannot be met by it.

---

## 2. Correction #2 — what vtable slot 8 (`+0x40`) really is

The brief assumed slot 8 was the apply. The only place in the whole state framework that calls
slot 8 is `add()` itself, `0x7A2890` (the routine the notes already identified), at
`0x7A29C2` and `0x7A29F9`:

```
007A2952  488b8e70010000           mov rcx, qword ptr [rsi + 0x170]   ; rsi = mgr, tree/list head
007A2959  488bd9                   mov rbx, rcx
...                      (search for key == ebp in the store)
007A2993  488b4b28                 mov rcx, qword ptr [rbx + 0x28]    ; existing node -> state object
007A2997  8b4110                   mov eax, dword ptr [rcx + 0x10]    ; its state id
007A299A  448b4710                 mov r8d, dword ptr [rdi + 0x10]    ; new object's state id
007A299E  413bc0                   cmp eax, r8d
007A29A1  410f94c6                 sete r14b                          ; same id?
007A29A5  80791c00                 cmp byte ptr [rcx + 0x1c], 0
007A29A9  7435                     je 0x7A29E0
007A29AB  80792000                 cmp byte ptr [rcx + 0x20], 0
007A29AF  742f                     je 0x7A29E0
007A29B1  80792100                 cmp byte ptr [rcx + 0x21], 0
007A29B5  740e                     je 0x7A29C5
007A29B7  413bc0                   cmp eax, r8d
007A29BA  7509                     jne 0x7A29C5
007A29BC  488b01                   mov rax, qword ptr [rcx]           ; vtable of the EXISTING object
007A29BF  488bd7                   mov rdx, rdi                       ; rdx = the NEW object
007A29C2  ff5040                   call qword ptr [rax + 0x40]        ; ★ slot 8
...
007A29E0  488bd1                   mov rdx, rcx                       ; rdx = existing object
007A29E3  80792100                 cmp byte ptr [rcx + 0x21], 0
007A29E7  7417                     je 0x7A2A00
007A29E9  413bc0                   cmp eax, r8d
007A29EC  7512                     jne 0x7A2A00
007A29EE  4885c9                   test rcx, rcx
007A29F1  740d                     je 0x7A2A00
007A29F3  488b07                   mov rax, qword ptr [rdi]           ; vtable of the NEW object
007A29F6  488bcb                   mov rcx, rdi                       ; rcx = the NEW object
007A29F9  ff5040                   call qword ptr [rax + 0x40]        ; ★ slot 8
```

So slot 8 = **"merge/replace against an already-present state of the same id"**, `arg2` is
*another state object*, not a gauge holder. It is `ret 0` for most classes precisely because
most classes have no merge logic (`0x7A8110` is shared by 87 vtable entries —
`0x11A8130, 0x11A8180, …, 0x11A8D60`, stride `0x50`). This is a coherent, minimal reading of the
framework and it is what *kills* the original hypothesis that following slot 8 leads to the gauge.

---

## 3. Sub-question 3 — ANSWERED: the generic per-frame state tick is `0x7ACCA0`

Discovery route: I enumerated every `call qword ptr [reg+disp]` site in the image and intersected
with functions touching the state store. In the whole framework region `0x790000..0x7B6000` there
is **exactly one** `call [reg+0x30]` site and **exactly one** `call [reg+0x40]` pair:

```
=== call [reg+0x30] sites whose containing fn is in 0x790000..0x7B6000 ===
fn 0x7ACCA0   size=271    : 0x7ACD1A qword ptr [rax + 0x30]

=== call [reg+0x40] sites whose containing fn is in 0x790000..0x7B6000 ===
fn 0x7A2890   size=567    : 0x7A29C2 | 0x7A29F9   (the two merge calls of §2)
```

### 3.1 The tick, annotated (`.pdata` `0x7ACCA0..0x7ACDAF`, 271 bytes, complete)

```
007ACCA0  48895c2408               mov qword ptr [rsp + 8], rbx
007ACCA5  57                       push rdi
007ACCA6  4883ec40                 sub rsp, 0x40
007ACCAA  80791c00                 cmp byte ptr [rcx + 0x1c], 0     ; obj+0x1C = "is active"
007ACCAE  488bfa                   mov rdi, rdx                     ; ★ rdi = TARGET
007ACCB1  0fb6411d                 movzx eax, byte ptr [rcx + 0x1d] ; obj+0x1D = "was activated"
007ACCB5  488bd9                   mov rbx, rcx                     ; rbx = the state object (this)
007ACCB8  0f29742430               movaps [rsp+0x30], xmm6
007ACCBD  0f57f6                   xorps xmm6, xmm6                 ; 0.0f
007ACCC0  0f297c2420               movaps [rsp+0x20], xmm7
007ACCC5  0f28fa                   movaps xmm7, xmm2                ; ★ xmm7 = dt (from xmm2)
007ACCC8  0f849d000000             je 0x7ACD6B                      ; not active -> deactivate path
007ACCCE  84c0                     test al, al
007ACCD0  750e                     jne 0x7ACCE0
007ACCD2  488b01                   mov rax, qword ptr [rcx]
007ACCD5  c6411e01                 mov byte ptr [rcx + 0x1e], 1
007ACCD9  ff5028                   call qword ptr [rax + 0x28]      ; slot 5 = onActivate (once)
007ACCDC  c6431d01                 mov byte ptr [rbx + 0x1d], 1
007ACCE0  f30f104b38               movss xmm1, dword ptr [rbx + 0x38]   ; obj+0x38 = period
007ACCE5  0f2fce                   comiss xmm1, xmm6
007ACCE8  c6431f00                 mov byte ptr [rbx + 0x1f], 0
007ACCEC  7620                     jbe 0x7ACD0E
007ACCEE  f30f104334               movss xmm0, dword ptr [rbx + 0x34]   ; obj+0x34 = phase
007ACCF3  f30f5cc7                 subss xmm0, xmm7                     ; -= dt
007ACCF7  0f2ff0                   comiss xmm6, xmm0
007ACCFA  f30f114334               movss dword ptr [rbx + 0x34], xmm0
007ACCFF  720d                     jb 0x7ACD0E
007ACD01  f30f58c1                 addss xmm0, xmm1
007ACD05  c6431f01                 mov byte ptr [rbx + 0x1f], 1         ; obj+0x1F = "ticked this frame"
007ACD09  f30f114334               movss dword ptr [rbx + 0x34], xmm0
007ACD0E  488b03                   mov rax, qword ptr [rbx]
007ACD11  0f28d7                   movaps xmm2, xmm7                ; xmm2 = dt
007ACD14  488bd7                   mov rdx, rdi                     ; ★ rdx = TARGET
007ACD17  488bcb                   mov rcx, rbx                     ; rcx = this
007ACD1A  ff5030                   call qword ptr [rax + 0x30]      ; ★★★ slot 6 = per-frame APPLY
007ACD1D  0f2f732c                 comiss xmm6, dword ptr [rbx + 0x2c]
007ACD21  7758                     ja 0x7ACD7B
007ACD23  f30f104330               movss xmm0, dword ptr [rbx + 0x30]
007ACD28  0f2fc6                   comiss xmm0, xmm6
007ACD2B  764e                     jbe 0x7ACD7B
007ACD2D  488bcb                   mov rcx, rbx
007ACD30  e82b72ffff               call 0x7A3F60
007ACD35  f30f594330               mulss xmm0, dword ptr [rbx + 0x30]
007ACD3A  f30f104b28               movss xmm1, dword ptr [rbx + 0x28]   ; obj+0x28 = duration left
007ACD3F  f30f59c7                 mulss xmm0, xmm7
007ACD43  f30f5cc8                 subss xmm1, xmm0                     ; duration -= decay*dt
007ACD47  f30f10432c               movss xmm0, dword ptr [rbx + 0x2c]
007ACD4C  0f2fc8                   comiss xmm1, xmm0
007ACD4F  f30f114b28               movss dword ptr [rbx + 0x28], xmm1
007ACD54  7607                     jbe 0x7ACD5D
007ACD56  f30f114328               movss dword ptr [rbx + 0x28], xmm0
007ACD5B  eb1e                     jmp 0x7ACD7B
007ACD5D  0f2ff1                   comiss xmm6, xmm1
007ACD60  7619                     jbe 0x7ACD7B
007ACD62  c7432800000000           mov dword ptr [rbx + 0x28], 0
007ACD69  eb10                     jmp 0x7ACD7B
007ACD6B  84c0                     test al, al                      ; deactivate path
007ACD6D  740c                     je 0x7ACD7B
007ACD6F  488b01                   mov rax, qword ptr [rcx]
007ACD72  33d2                     xor edx, edx                     ; rdx = 0
007ACD74  ff5038                   call qword ptr [rax + 0x38]      ; slot 7 = onDeactivate
007ACD77  c6431d00                 mov byte ptr [rbx + 0x1d], 0
007ACD7B  0f2f732c                 comiss xmm6, dword ptr [rbx + 0x2c]
007ACD7F  f30f587b18               addss xmm7, dword ptr [rbx + 0x18]
007ACD84  f30f117b18               movss dword ptr [rbx + 0x18], xmm7   ; accumulate elapsed
007ACD89  7604                     jbe 0x7ACD8F
007ACD8B  b001                     mov al, 1
007ACD8D  eb0b                     jmp 0x7ACD9A
007ACD8F  f30f104328               movss xmm0, dword ptr [rbx + 0x28]
007ACD94  0f2fc6                   comiss xmm0, xmm6
007ACD97  0f97c0                   seta al                          ; return (duration left > 0)
007ACD9A  488b5c2450               mov rbx, qword ptr [rsp + 0x50]
007ACD9F  0f28742430               movaps xmm6, [rsp+0x30]
007ACDA4  0f287c2420               movaps xmm7, [rsp+0x20]
007ACDA9  4883c440                 add rsp, 0x40
007ACDAD  5f                       pop rdi
007ACDAE  c3                       ret
```

raw bytes at the dispatch (15 B): `0x7ACD0E: 488b03 0f28d7 488bd7 488bcb ff5030`
= `mov rax,[rbx]; movaps xmm2,xmm7; mov rdx,rdi; mov rcx,rbx; call qword ptr [rax+0x30]`.

**Method-slot contract of these `AddStateObject*@Character` classes** (derived from this tick +
`add()` + the per-class overrides; consistent across all 179 ctors):

| slot | off | meaning | evidence |
|---|---|---|---|
| 0 | `+0x00` | destructor (sets vptr to base `0x11A80F0`, optional free) | `0x79C2C0/0x79C350` |
| 1 | `+0x08` | `setMag(float)` = `movss [rcx+0x50], xmm1` | `0x7AC4E0` |
| 2 | `+0x10` | `getMag()` (clamped) | `0x7A3CF0` |
| 3 | `+0x18` | `isActive()` = `[rcx+0x50] >= 0` | `0x7A4A30` |
| 4 | `+0x20` | `isSameType(o)` = `[rdx+0x10]==[rcx+0x10]` | `0x7A5720` |
| 5 | `+0x28` | `onActivate()` — called **once**, from the tick | `0x7ACCD9` |
| **6** | **`+0x30`** | **`apply(this, target, dt)` — called every frame** | **`0x7ACD1A`** |
| 7 | `+0x38` | `onDeactivate(0)` | `0x7ACD74`, `0x7A28E9` |
| 8 | `+0x40` | `merge(this, otherStateObject)` — only from `add()` | `0x7A29C2`, `0x7A29F9` |

### 3.2 Who calls the tick — and the container layout it walks

`ripref.py callers 0x7ACCA0` → 8 direct call sites in 6 functions:

```
0x7AC866 in fn 0x7AC6E0   (main per-frame driver)
0x7AC90F in fn 0x7AC6E0
0x7AC9CF in fn 0x7AC6E0
0x7AD1B1 in fn 0x7ACDB0
0x7AD393 in fn 0x7AD260   (expiry/tick pass over the store; also frees expired nodes)
0x7AD4F4 in fn 0x7AD470
0x7AD60F in fn 0x7AD540
0x7AD70F in fn 0x7AD640
```

`0x7AC6E0` is the driver: it copies the container's cached floats
(`[rsi+0x124..+0x14c]`), reads `[[rsi+0x168]+0x240]` as the param, calls `0x79E320` /
`0x7B51B0` (per-frame container maintenance), calls the store pass **`0x7AD260`**, then ticks the
single object at `[rsi+0x180]`, then walks the intrusive lists rooted at
`[rsi+0x188]`, `[rsi+0x198]`, `[rsi+0x1a8]` (nodes: `+0x00` next, `+0x08` prev, `+0x10` or `+0x28`
= state object, with `dec qword [rsi+0x190/0x1a0]` when unlinking), and finally checks the byte
`[rsi+0x1bc]` (`!= 0` ⇒ call `0x7A2EF0`, the "clear all states" walk shown in §6.3). The store at
`[rsi+0x170]` (the one `add()` inserts into) is the map/rbtree the notes already documented, and
it is re-walked by `0x7AD260`.

**The container object itself is `param + 0x10B0`** — proof: the tick driver and every list pass
reach the character as `mov rax,[rsi+0x168]; mov rcx,[rax+0x240]` (`[manager+0x168]` = `char`,
`[char+0x240]` = `param`), and the engine's own ctor calls do `lea rcx,[r15+0x10B0]` before
`call <state ctor>` (e.g. `0xA8667C` → `0x79F1B0`). So `[manager+0x168]` = char and
`manager = param+0x10B0`.

### 3.3 Two distinct registration APIs (relevant to why an out-of-band insert can be inert)

| API | shape | where the object ends up | used by |
|---|---|---|---|
| `0x7A2890` | `(mgr, int key, obj, int r9d, byte stack0)` — the `add()` the notes already documented | the **store** at `[mgr+0x170]` (insert + optional slot-8 merge against a same-id object) | engine's amrita helper (`0x7AA459`, `0x7AA554`, `0x7AA651`) with keys `0x39/0x3a/0x3b`; the mod's current code |
| `0x7A2C80` | `(mgr, obj, byte replaceSameType)` | the **active list** at `[mgr+0x198]` (`call 0x7AAB30` to link), optionally destroying a same-type predecessor | engine's `GuardPenetration` application (`0x76D055`, `0x76D02F` queries presence via `0x7A56A0` first) |
| `0x7A2AD0` | wrapper: `(mgr, key, obj, stack0)` → `add()` with `r9d = -1`, then post-processing | store | per-object variants |

Both routes are reachable from the tick (`0x7AD260` re-walks the store at `[mgr+0x170]`; the
driver walks `[mgr+0x180/0x188/0x198/0x1a8]`), so an object inserted through either API *should* be
ticked. This is why I flag the "mod inserted a node but nothing happened" symptom as most likely a
**class/semantics** error (`AmritaGaugeUp` is a rate multiplier, §1.4) rather than a
registration-path error — but a DLL that wants an effect identical to an engine call should copy
the engine's exact call pair (`0x7A56A0` presence check → ctor → `0x7A2C80`), not just `add()`.

---

## 4. Sub-questions 1 & 2 — ANSWERED: `rdx` is the state **manager** (`[[char+0x240]]+0x10B0`)

All 8 tick call sites, with the 14 instructions preceding each (only the `rdx` setup shown):

| site | function | code setting `rdx` | `rdx` value |
|---|---|---|---|
| `0x7AC866` | `0x7AC6E0` | `0f28d7 488bd6 488bcb` = `movaps xmm2,xmm7; mov rdx,rsi; mov rcx,rbx` | **`rsi` = manager** |
| `0x7AC90F` | `0x7AC6E0` | `movaps xmm2,xmm7; mov rdx,rsi; mov rcx,rbx` | **`rsi` = manager** |
| `0x7AC9CF` | `0x7AC6E0` | `movaps xmm2,xmm7; mov rdx,rsi; mov rcx,rbx` | **`rsi` = manager** |
| `0x7AD1B1` | `0x7ACDB0` | `movaps xmm2,xmm7; mov rdx,rsi; …` (fn uses `[rsi+0x1b8]`, `lea r14,[rsi+0x1a8]`) | **`rsi` = manager** |
| `0x7AD393` | `0x7AD260` | `movaps xmm2,xmm8; mov rdx,rbp` (fn uses `lea rcx,[rbp+0x170]`) | **`rbp` = manager** |
| `0x7AD4F4` | `0x7AD470` | `movaps xmm2,xmm1; mov rdx,rdi` (fn writes `[rdi+0x180]`) | **`rdi` = manager** |
| `0x7AD60F` | `0x7AD540` | `movaps xmm2,xmm7; mov rdx,rsi; mov rcx,rbx` | **`rsi` = manager** |
| `0x7AD70F` | `0x7AD640` | `movaps xmm2,xmm7; mov rdx,rsi; mov rcx,rbx` | **`rsi` = manager** |

Every one of them passes **the container**, never the character and never `param`. Therefore, for
`0x7A8120`:

```
target            = state manager            = [[char+0x240]] + 0x10B0
field written     = target + 0x15C           = [[char+0x240]] + 0x120C
```

**DLL expression (as asked), confidence *confirmed***:

```c
char *base = (char *)GetModuleHandleW(NULL);                 // image base 0x140000000
void *chr  = *(void **)(base + 0x18A0490);                   // player character
void *param = chr ? *(void **)((char *)chr + 0x240) : NULL;   // param / "PlayerParam"
void *mgr  = (char *)param + 0x10B0;                          // state manager (container)
float *f   = (float *)((char *)mgr + 0x15C);                  // <- what 0x7A8120 accumulates into
```

### 4.1 …but that field is *guard penetration*, not the 99 gauge (confirmed)

`Character::AddStateGuardPenetration`: ctor **`0x79FED0`** (found as the unique
`lea reg,[rip+d]` → `0x11A97C0`), state id **0x48**:

```
0079FED0  48895c2408               mov qword ptr [rsp + 8], rbx
0079FED5  57                       push rdi
0079FED6  4883ec50                 sub rsp, 0x50
0079FEDA  0f29742440               movaps [rsp + 0x40], xmm6
0079FEDF  488bd9                   mov rbx, rcx                  ; arg1 = manager
0079FEE2  0f297c2430               movaps [rsp + 0x30], xmm7
0079FEE7  0f28f1                   movaps xmm6, xmm1             ; arg2 = duration
0079FEEA  0f28fa                   movaps xmm7, xmm2             ; arg3 = magnitude
0079FEED  e86e6f8000               call 0xFA6E60                 ; allocator accessor
0079FF0C  8d5758                   lea edx, [rdi + 0x58]         ; 0x58 bytes
0079FF0F  41ff5128                 call qword ptr [r9 + 0x28]    ; allocate
0079FF13  488b8b68010000           mov rcx, qword ptr [rbx + 0x168]; ★ [mgr+0x168] = char
0079FF1F  f30f117028               movss [rax + 0x28], xmm6
0079FF24  f30f11702c               movss [rax + 0x2c], xmm6
0079FF2E  48894808                 mov qword ptr [rax + 8], rcx  ; obj+8 = char
0079FF32  488d0d8798a000           lea rcx, [rip + 0xa09887]     ; -> 0x11A97C0 (GuardPenetration vtable)
0079FF39  48c7401048000000         mov qword ptr [rax + 0x10], 0x48 ; ★ state id = 0x48
0079FF6C  f30f117850               movss dword ptr [rax + 0x50], xmm7 ; magnitude (0.2f at the only call site)
0079FF76  488908                   mov qword ptr [rax], rcx
```

It is applied from exactly **one** place in the image (`ripref.py callers 0x79FED0` → `0x76D046`),
inside `fn 0x76CDE0` (contiguous `.pdata` run `0x76CDE0..0x76D86A`; `rdi` = the character,
`[rdi+0x240]` = param):

```
0076CFE7  e8447c1600               call 0x8D4C30                  ; LW-module query: movzx eax,[0x191DE38]
0076CFEC  84c0                     test al, al
0076CFEE  746a                     je 0x76D05A
0076CFF0  488b87a8000000           mov rax, qword ptr [rdi + 0xa8]
0076CFF7  44397810                 cmp dword ptr [rax + 0x10], r15d
0076CFFB  755d                     jne 0x76D05A
0076CFFD  488b0ddc451001           mov rcx, qword ptr [rip + 0x11045dc]  ; -> 0x18715E0
0076D004  ba15000000               mov edx, 0x15
0076D009  488b9f40020000           mov rbx, qword ptr [rdi + 0x240]      ; param
0076D010  4881c1c8070000           add rcx, 0x7c8
0076D017  4881c3b0100000           add rbx, 0x10b0                        ; ★ manager
0076D01E  e88dbd1400               call 0x8B8DB0
0076D023  84c0                     test al, al
0076D025  7433                     je 0x76D05A
0076D027  ba48000000               mov edx, 0x48                          ; is state 0x48 present?
0076D02C  488bcb                   mov rcx, rbx
0076D02F  e86c860300               call 0x7A56A0
0076D034  84c0                     test al, al
0076D036  7522                     jne 0x76D05A
0076D038  f30f1015dc3ca300         movss xmm2, dword ptr [rip + 0xa33cdc] ; -> 0x11A0D1C = 0.2f
0076D040  0f28ce                   movaps xmm1, xmm6
0076D043  488bcb                   mov rcx, rbx
0076D046  e8852e0300               call 0x79FED0                          ; ★ ctor(mgr, xmm1, 0.2f)
0076D04B  488bd0                   mov rdx, rax
0076D04E  450fb6c7                 movzx r8d, r15b
0076D052  488bcb                   mov rcx, rbx
0076D055  e8265c0300               call 0x7A2C80                          ; ★ ACTIVE-LIST registration
```

`0x8D4C30` is one of the LW/achievement module accessors from the existing notes:
`movzx eax, byte ptr [rip + 0x1049201]` → **`*(BYTE*)0x191DE38`**. So
`manager+0x15C` is set to `min(0.2 + current, 1.0)` while the LW module's `0x191DE38` byte is
non-zero, and its class name (`AddStateGuardPenetration`) plus the `0.2f` constant identify it as
a **guard-penetration ratio**, i.e. an LW-time buff aggregate, **not** the 99 gauge bar.

Note the registration call: `0x7A2C80(mgr, obj, flag)` is **not** `add()` (`0x7A2890`). Its body
(`0x7A2C80..0x7A2D8A`) walks the manager's list at `[mgr+0x198]`; when `flag != 0` it looks for a
node whose object reports "same type" (`call [rax+0x20]`, slot 4) and, on a match, deactivates
(slot 7 with `rdx=0`), destructs (slot 0) and frees that old object and unlinks its node
(`dec qword [rsi+0x1a0]`); then it always links the new object into the list:
`007A2D63  lea rcx,[rsi+0x198]; lea rdx,[rsp+0x48]; call 0x7AAB30`. So there are **two**
registration APIs — see §3.3.

**Consequence for the brief**: the literal answer to questions 1–3 is
`*(float*)(*(void**)(*(void**)(base+0x18A0490)+0x240) + 0x10B0 + 0x15C)`, *confirmed* — but it is
not the field the brief actually wants. Writing it changes guard penetration, not the 99 gauge.

---

## 5. Where the amrita / 99 gauge actually is: the sub-object embedded at `param + 0xB0`

### 5.1 How it was found

Route: the class names (`AddStateObjectAmritaGaugeUp` / `AmritaGaugeRecover` / `AmritaUpPlus` /
`DyingAmritaUp` / `CallSpirit`) did not lead to a gauge field, so I followed what their **slot-6
applies actually do with `rdx`/`[obj+8]`**:

* `CallSpirit` slot 6 = `0x7A8960`: reads/writes `[rdx+0x10C]` (manager field);
* `AmritaGaugeUp` slot 6 = `0x7A8280`: multiplies `[rdx+0x64]`;
* `AmritaGaugeRecover` slot 6 = `0x7A8170` (real body `0x7A8170..0x7A8274`):
  ```
  007A818B  488b7908                 mov rdi, qword ptr [rcx + 8]     ; obj+8 = char
  007A8198  488bbf40020000           mov rdi, qword ptr [rdi + 0x240] ; param
  007A819F  4881c7b0000000           add rdi, 0xb0                    ; ★ param + 0xB0
  007A81A9  e872c00000               call 0x7B4220                    ; is mode == 1 ?
  007A81FE  8b5350                   mov edx, dword ptr [rbx + 0x50]  ; ★ magnitude read as INT
  007A8204  e867710000               call 0x7AF370                    ; sub+0x10 += edx  (clamp sub+0x18)
  007A8209  f30f104f50               movss xmm1, dword ptr [rdi + 0x50] ; ★ reads the float gauge
  007A8211  0f2fc8                   comiss xmm1, xmm0
  007A8214  7618                     jbe 0x7A822E
  007A8216  660f6e4b50               movd xmm1, dword ptr [rbx + 0x50]
  007A821E  0f5bc9                   cvtdq2ps xmm1, xmm1
  007A8221  f30f590db3879f00         mulss xmm1, dword ptr [rip + 0x9f87b3] ; -> 0x11A09DC = 3.0f
  007A8229  e892860000               call 0x7B08C0                    ; ★ sub+0x50 -= 3*mag
  007A824D  660f6e4350               movd xmm0, dword ptr [rbx + 0x50]
  007A8255  f30f590583879f00         mulss xmm0, dword ptr [rip + 0x9f8783] ; -> 0x11A09E0 = 0.2f
  007A825D  f30f2cd0                 cvttss2si edx, xmm0
  007A8261  e83a770000               call 0x7AF9A0                    ; accumulator sub+0x4C += ...
  ```
* `DyingAmritaUp` slot 6 = `0x7A8DE0`: `rcx = param+0xB0; movd xmm0,[rcx+0x14]; cvtdq2ps;
  mulss xmm0,[rbx+0x50]; cvttss2si edx; call 0x7AF370` (`0x7A8E62..0x7A8E7A`).

So a whole family of amrita state objects operates on **one sub-object at `param+0xB0`**, via 5
tiny primitives. That sub-object is the amrita/LW gauge holder.

### 5.2 Layout (all offsets relative to `sub = [[char+0x240]] + 0xB0`)

| off | param off | type | role | evidence |
|---|---|---|---|---|
| `+0x00` | `+0xB0` | int | mode; `1` / `2` / `-1` = invalid | `0x7B4220  cmp dword [rcx],1; sete al; ret`; `0x7B4230 cmp [rcx],2`; `0x7AF370  cmp dword [rcx],-1; je → false`; **independent**: `0x7B4390` does `cmp dword ptr [param+0xb0], 1` (`0x7B43B9`) |
| `+0x04` | `+0xB4` | float | current of a (max,cur) pair; max in `+0x00`-as-float? | `0x7AF3C0`: `[rcx+4] += xmm1`, clamp `[rcx]` — **no callers found**, so listed as *guess* |
| `+0x08` | `+0xB8` | int | **LW level/mode 0…7**; `7` = off | `0x7AFE20`: `cmp edx,[rcx+8]; je ret; … [rdi+8] = ebx`; `0x7B5FC0  cmp dword [rbx+8],7`; `0x7B66E0  cmp dword [rcx+8],7` |
| `+0x10` | `+0xC0` | int | amrita counter (current) | `0x7AF370`: `[rcx+0x10] += edx`, clamp `[rcx+0x18]` |
| `+0x14` | `+0xC4` | int | coefficient used by `DyingAmritaUp` | `0x7A8E69  movd xmm0,[rcx+0x14]` |
| `+0x18` | `+0xC8` | int | counter max (clamp) | `0x7AF370` (`cmp eax,r8d; cmovg eax,r8d`) |
| `+0x1C` | **`+0xCC`** | float | **the measured "burning bar"** — increased by `0x7AF970`, clamped by `+0x20` | `0x7AF970`: `[rcx+0x1c] += xmm1; if > [rcx+0x20] → [rcx+0x20]`; callers `0x7AA6E4/0x7AA6F9` with `0.12f`(`0x11A0A04`) / `0.10f`(`0x11A0A08`) |
| `+0x20` | `+0xD0` | float | clamp/max of `+0x1C`; also used by `0x7AF9A0` as `[rcx+0x20]-[rcx+0x1c]` | `0x7AF994 movss [rcx+0x1c], xmm2` (xmm2 = `[rcx+0x20]`); `0x7AF9D1` |
| `+0x40` | `+0xF0` | float | scale used by `0x7AF9A0` (`xmm4 = (float)n * [rcx+0x40]`) | `0x7AF9C7` |
| `+0x4C` | `+0xFC` | int | accumulator with fractional carry | `0x7AF9A0`: `[rcx+0x4c]` read/written, `cvttss2si r8d, xmm6; mov [rcx+0x4c], r8d` |
| **`+0x50`** | **`+0x100`** | **float** | **the gauge: only ever decremented; 0 ⇒ `call 0x7C7EB0(param)`** | `0x7B08C0`, `0x7B0860` (see §5.4) |
| `+0x55` | `+0x105` | byte | flag tested before LW start/stop work | `0x7B5FC0  cmp byte [rcx+0x55],0`; `0x7B66E0  cmp byte [rcx+0x55],0` |
| `+0x58` | `+0x108` | `char*` | owner character | `0x7B08C0  mov rcx,[rcx+0x58]; mov rcx,[rcx+0x240]; call 0x7C7EB0`; `0x7B5FC0/0x7B66E0` |
| `+0x60` | `+0x110` | int | LW-use counter (flushed with an event id) | `0x7B5FC0  8b5360 / c7436000000000` |
| `+0x68` | `+0x118` | ptr | cleared on LW start/stop | `0x7B5FC0  mov qword [rbx+0x68],0` |

> **Cross-check with the in-game measurement given in the brief**: `param+0xCC` = the burning bar
> 0…100. Statically `param+0xCC` = `sub+0x1C`, and it is a float clamped by `sub+0x20`, written
> only by `0x7AF970`. That is an exact structural match: the measured field is a field of *this*
> sub-object. This is what makes me confident the sub-object at `param+0xB0` **is** the
> amrita / Living-Weapon gauge structure (rather than another coincidental struct).

### 5.3 The primitives (complete listings for the ones that matter)

```
; mode test
007B4220  833901                   cmp dword ptr [rcx], 1
007B4223  0f94c0                   sete al
007B4226  c3                       ret

; int counter: sub+0x10 += edx, clamp at sub+0x18 ; false if sub+0 == -1
007AF370  8339ff                   cmp dword ptr [rcx], -1
007AF373  7447                     je 0x7AF3BC                 ; -> xor eax,eax; ret
007AF375  8b4110                   mov eax, dword ptr [rcx + 0x10]
007AF378  448b4118                 mov r8d, dword ptr [rcx + 0x18]
007AF37C  413bc0                   cmp eax, r8d
007AF37F  743b                     je 0x7AF3BC
007AF381  03c2                     add eax, edx
007AF383  413bc0                   cmp eax, r8d
007AF386  410f4fc0                 cmovg eax, r8d
007AF38A  894110                   mov dword ptr [rcx + 0x10], eax
007AF38D  488b4158                 mov rax, qword ptr [rcx + 0x58]
007AF391  4885c0                   test rax, rax
007AF394  7423                     je 0x7AF3B9
007AF396  488b8030020000           mov rax, qword ptr [rax + 0x230]
007AF3A2  488b4008                 mov rax, qword ptr [rax + 8]
007AF3AB  48b90000000000000004     movabs rcx, 0x400000000000000
007AF3B5  48094840                 or qword ptr [rax + 0x40], rcx   ; dirty-flag on the owner
007AF3B9  b001                     mov al, 1
007AF3BB  c3                       ret

; burn bar: sub+0x1C += xmm1, clamp at sub+0x20 ; false if sub+0 == -1
007AF970  8339ff                   cmp dword ptr [rcx], -1
007AF973  7427                     je 0x7AF99C
007AF975  f30f10411c               movss xmm0, dword ptr [rcx + 0x1c]
007AF97A  f30f105120               movss xmm2, dword ptr [rcx + 0x20]
007AF97F  0f2ec2                   ucomiss xmm0, xmm2
007AF982  7a02                     jp 0x7AF986
007AF984  7416                     je 0x7AF99C
007AF986  f30f58c1                 addss xmm0, xmm1
007AF98A  0f2fc2                   comiss xmm0, xmm2
007AF98D  f30f11411c               movss dword ptr [rcx + 0x1c], xmm0
007AF992  7605                     jbe 0x7AF999
007AF994  f30f11511c               movss dword ptr [rcx + 0x1c], xmm2
007AF999  b001                     mov al, 1
007AF99B  c3                       ret
007AF99C  32c0                     xor al, al
007AF99E  c3                       ret

; "counter has reached its max?"  (sub+0 == 0 and sub+0x10 >= sub+0x14)
007AFDA0  833900                   cmp dword ptr [rcx], 0
007AFDA3  750b                     jne 0x7AFDB0
007AFDA5  8b4114                   mov eax, dword ptr [rcx + 0x14]
007AFDA8  394110                   cmp dword ptr [rcx + 0x10], eax
007AFDAB  7c03                     jl 0x7AFDB0
007AFDAD  b001                     mov al, 1
007AFDAF  c3                       ret
007AFDB0  32c0                     xor al, al
007AFDB2  c3                       ret
```

### 5.4 The drain (bonus question 4) — same field, confirmed

```
; 0x7B08C0(sub, xmm1): float drain, clamp 0, "empty" callback
007B08C0  4883ec28                 sub rsp, 0x28
007B08C4  f30f104150               movss xmm0, dword ptr [rcx + 0x50]   ; ★ the gauge
007B08C9  0f57d2                   xorps xmm2, xmm2
007B08CC  0f2fc2                   comiss xmm0, xmm2
007B08CF  762c                     jbe 0x7B08FD                          ; <= 0 -> return 0
007B08D1  f30f5cc1                 subss xmm0, xmm1                      ; -= amount
007B08D5  0f2fd0                   comiss xmm2, xmm0
007B08D8  f30f114150               movss dword ptr [rcx + 0x50], xmm0
007B08DD  7217                     jb 0x7B08F6                          ; still > 0 -> done
007B08DF  c7415000000000           mov dword ptr [rcx + 0x50], 0
007B08E6  488b4958                 mov rcx, qword ptr [rcx + 0x58]      ; owner char
007B08EA  488b8940020000           mov rcx, qword ptr [rcx + 0x240]     ; param
007B08F1  e8ba750100               call 0x7C7EB0                        ; ★ "gauge reached 0" handler
007B08F6  b001                     mov al, 1
007B08FC  c3                       ret
007B08FD  32c0                     xor eax, eax
007B08FF  4883c428                 add rsp, 0x28
007B0903  c3                       ret

; 0x7B0860(sub, edx): the same drain driven by an int: amount = (float)max(edx,0) * [0x11A055C]
007B0864  f30f104950               movss xmm1, dword ptr [rcx + 0x50]
007B086F  7642                     jbe 0x7B08B3
007B0871  33c0                     xor eax, eax
007B0873  85d2                     test edx, edx
007B0875  0f49c2                   cmovns eax, edx
007B0878  660f6ec0                 movd xmm0, eax
007B087C  0f5bc0                   cvtdq2ps xmm0, xmm0
007B087F  f30f5905d5fc9e00         mulss xmm0, dword ptr [rip + 0x9efcd5] ; -> 0x11A055C
007B0887  f30f5cc8                 subss xmm1, xmm0
007B088B  0f2fd1                   comiss xmm2, xmm1
007B088E  f30f114950               movss dword ptr [rcx + 0x50], xmm1
007B0893  7217                     jb 0x7B08AC
007B0895  c7415000000000           mov dword ptr [rcx + 0x50], 0
007B089C  488b4958                 mov rcx, qword ptr [rcx + 0x58]
007B08A0  488b8940020000           mov rcx, qword ptr [rcx + 0x240]
007B08A7  e804760100               call 0x7C7EB0
```

Callers of the two drains (raw `E8` scan, whole image):

```
0x7B08C0 : 0x7A8229 (in AmritaGaugeRecover::apply)  |  0x7AA6B5, 0x7AA6CA (in fn 0x7AA67C)
0x7B0860 : (not reached through the paths I traced; the caller set is disjoint from the above)
```

`0x7AF370` (the int add) has 9 callers, all in amrita-related code:
`0x745B99`, `0x7A8204` (AmritaGaugeRecover), `0x7A8E7A` (DyingAmritaUp), `0x7AA68F` (fn `0x7AA67C`),
`0x7C98DF`, `0x86D0F4`, `0x86D472`, `0x86D69A`, `0x86D72B`.
`0x7AF970` (the burn-bar add) has exactly **two** callers, both inside fn `0x7AA67C`
(`0x7AA6E4` with `0.12f`, `0x7AA6F9` with `0.10f`).
`0x7C7EB0` (the "gauge empty" handler, 5858 B) is called from **152** sites — it is the
`param`-level LW/amrita "state changed" notifier, not a private helper.

### 5.5 The absorb / mode-switch function (`0x7AA67C` region) — how the bar is fed

The tail of the function that contains `0x7AA395..0x7AA67C` (the engine's own "apply the
amrita/LW state objects" routine; it builds `AmritaGaugeUp`(id `0x20`, vtable `0x11A8B90`,
key `0x39`), another state (id `6`, vtable `0x11A8410`, key `0x3a`) and a third (id `0x14`,
vtable `0x11A8820`, key `0x3b`) — each time by allocating 0x58 bytes, stamping
`[obj+8] = [mgr+0x168]` (char), `[obj+0x10] = id`, `[obj+0x28]=[obj+0x2c]=duration`,
`[obj+0x50]=magnitude`, then `call 0x7A2890` (`add`):

```
007AA656  488b4608                 mov rax, qword ptr [rsi + 8]        ; rsi: object with +8 = char
007AA65A  488b9840020000           mov rbx, qword ptr [rax + 0x240]    ; param
007AA661  4881c3b0000000           add rbx, 0xb0                       ; ★ sub = param+0xB0
007AA668  488bcb                   mov rcx, rbx
007AA66B  e8b09b0000               call 0x7B4220                       ; sub->mode == 1 ?
007AA678  84c0                     test al, al
007AA67A  7558                     jne 0x7AA6D4
007AA67C  8b157a639f00             mov edx, dword ptr [rip + 0x9f637a] ; -> 0x11A09FC = 1  (int 1)
007AA682  4084ed                   test bpl, bpl
007AA685  488bcb                   mov rcx, rbx
007AA688  0f451571639f00           cmovne edx, dword ptr [rip + 0x9f6371] ; -> 0x11A0A00 = 1
007AA68F  e8dc4c0000               call 0x7AF370                       ; sub+0x10 += 1   (one amrita)
007AA694  f30f104b50               movss xmm1, dword ptr [rbx + 0x50]  ; ★ gauge
007AA699  0f57c0                   xorps xmm0, xmm0
007AA69C  0f2fc8                   comiss xmm1, xmm0
007AA69F  0f8609010000             jbe 0x7AA7AE
007AA6A5  4084ed                   test bpl, bpl
007AA6A8  7415                     je 0x7AA6BF
007AA6AA  f30f100d5e639f00         movss xmm1, dword ptr [rip + 0x9f635e] ; -> 0x11A0A10 = 1.5f
007AA6B5  e806620000               call 0x7B08C0                       ; sub+0x50 -= 1.5f
007AA6BF  f30f100d45639f00         movss xmm1, dword ptr [rip + 0x9f6345] ; -> 0x11A0A0C = 2.0f
007AA6CA  e8f1610000               call 0x7B08C0                       ; sub+0x50 -= 2.0f
...
007AA6D4  4084ed                   test bpl, bpl                       ; mode==1 branch
007AA6D9  f30f100d27639f00         movss xmm1, dword ptr [rip + 0x9f6327] ; -> 0x11A0A08 = 0.10f
007AA6E4  e887520000               call 0x7AF970                       ; sub+0x1C += 0.10f
007AA6EE  f30f100d0e639f00         movss xmm1, dword ptr [rip + 0x9f630e] ; -> 0x11A0A04 = 0.12f
007AA6F9  e872520000               call 0x7AF970                       ; sub+0x1C += 0.12f
```

Constants (measured from `.rdata`):
`0x11A09DC = 3.0f`, `0x11A09E0 = 0.2f`, `0x11A0A04 = 0.12f`, `0x11A0A08 = 0.10f`,
`0x11A0A0C = 2.0f`, `0x11A0A10 = 1.5f`, `0x11A0D1C = 0.2f`, `0x1588938 = 1.0f`,
`0x11A04FC = 4.0f`, `0x11A059C = 180.0f`, **`0x11A055C = 15.0f`** (the int→gauge drain scale of
`0x7B0860`, i.e. one unit of `edx` removes `15.0` from `sub+0x50`), and the ints
`0x11A04BC = 30`, `0x11A0540 = 300`, `0x11A0544 = 1000` (all three read as ints by the amrita
code).

Mode machine: `0x7AFE20(sub, edx)` sets `[sub+8] = edx` and then calls
`0x79E1C0([[sub+0x58]+0x240]+0x10B0, n)` for `n = 0..5` / `-1`. **Correction on what that helper
does** (it is not "apply state n"): `0x79E1C0(mgr, edx)` walks the manager's list at `[mgr+0x188]`
and, for every state object whose **state id `[obj+0x10] == 2`**, writes the magnitude:
`0079E1F0  cmp dword ptr [r8+0x10], 2 / 0079E1F7  mov dword ptr [r8+0x50], edx`. So it is a
"retune the level-2 state object's magnitude" call driven by the amrita mode.
`0x7B5FC0` / `0x7B66E0` are the LW start/stop routines (VFX/SE/UI, both
`cmp byte [sub+0x55],0` and `cmp dword [sub+8],7`).

### 5.6 What I could **not** determine statically (and will not invent)

The **scale and sign** of `sub+0x50`:

* it is *only ever decremented* by the code I found, and `0 ⇒ 0x7C7EB0(param)`. That is consistent
  with two opposite readings:
  * **Reading A (probable)**: `sub+0x50` is "amrita still needed"; absorbing amrita and the
    `AmritaGaugeRecover` state *reduce* it, so **0 = gauge full** and the HUD draws
    `max − sub+0x50`. This reading is supported by the class semantics: the class literally named
    `AmritaGaugeRecover` is the one that decrements it.
  * **Reading B**: `sub+0x50` is the gauge itself, full at 1.0 (or 100, or 1000), and 0 = empty;
    then the name "Recover" is a misnomer and 0 is the "LW ended" event.
* the maximum of `sub+0x50` was not found as a static immediate: no writer of `sub+0x50` with a
  positive value exists in the traced code (all 3 known call sites are the two drains plus the
  guard read at `0x7A8209`), so its initial/refill value must come from either the C++ ctor of the
  sub-object (which I did not locate — its fields are set through `0x7AFE20`/`0x7B5FC0`, not
  through an obvious `mov dword [param+0x100], imm`) or from a script/database value.

I therefore do **not** claim an offset for "the 99 bar" with *confirmed* status; I give the two
candidates in §7 with the evidence for each and the one measurement that settles it in §8.

---

## 6. Hypotheses examined and rejected

### 6.1 (rejected, **confirmed** to be wrong) "`0x7A8120` is `AmritaGaugeUp`'s apply, at slot 8"
The pointer occurs exactly once in the image (`0x11A97F0`), which is `+0x30` from the vtable base
`0x11A97C0`; that vtable's COL resolves to `.?AVAddStateGuardPenetration@Character@@`;
`AmritaGaugeUp`'s own vtable `0x11A8B90` has `ret 0` at slot 8 (`0x7A8110`, 87 vtable entries share
that slot) and `0x7A8280` at slot 6.

### 6.2 (rejected) "slot 8 is the per-frame apply"
Slot 8 is reached only from `add()` (`0x7A2890`) with `arg2` = the *other* state object of the same
id — a merge hook (§2). The per-frame apply is slot 6, proven by the tick's own
`call [rax+0x30]` (§3).

### 6.3 (rejected) "`rdx` for the apply is the character or `param`"
All 8 tick call sites pass the container. Also, the container's own field layout is what the
applies write: `manager+0x64` (AmritaGaugeUp), `manager+0x10C` (CallSpirit),
`manager+0x15C` (GuardPenetration) — three different aggregate fields of the same 0x170-byte+
status block, consistent with a "status aggregate" object rather than a character.

### 6.4 (rejected) "`param+0x40/+0x44` is the 99 gauge"
The existing parallel report (`_work\re_lw_report.md` §3.1) left this open because
`RecoverStamina` fills it. I resolved it independently: `Refer::Stamina` (`0x6F65D0`) calls
`0x6EC7C0`, which is

```
006EC7CE  488b8040020000           mov rax, qword ptr [rax + 0x240]   ; param
006EC7D8  f30f104840               movss xmm1, dword ptr [rax + 0x40]  ; ★ param+0x40
```
and `Refer::StaminaRate` (`0x6F6650`) → `0x6EC800` reads the same `+0x40`/`+0x44` pair.
So `param+0x40/+0x44` is **stamina (Ki)**, and the LW gauge is not there.

### 6.5 (rejected) "`manager+0x15C` is a normalized 0…1 gauge, therefore the 99 gauge"
It *is* clamped to 1.0, but its only writer is `AddStateGuardPenetration` with a hard-coded
`0.2f`, applied only while the LW-module byte `0x191DE38` is set (§4.1). It is a **ratio
aggregate**, not a bar — no code drains it, and it saturates after 5 frames.

### 6.6 (rejected) "`[[char+0x240]]+0xBB0` record table contains the 99 gauge"
Not re-derived here; the previous report found no `0x7B4C20`/`0x7B4C40` caller pointing at a
record ≠ 0, and my scan of accesses through registers known to hold `param+0xB0` produced only the
`+0x50`/`+0x14`/`+0x04` accesses of §5 — i.e. the amrita gauge is the `+0xB0` sub-object, not a
`+0xBB0` record.

### 6.7 (tooling caveat that produced false negatives)
`_work\nioh1.mem.funcs.txt` is **not** a function list: `.pdata` contains several entries per
large function (region-split unwind info). Example: the real `AmritaGaugeRecover::apply` is
`0x7A8170..0x7A8274`, but `funcs.txt` reports three chunks (`0x7A8170..0x7A8183`,
`0x7A8183..0x7A824D`, `0x7A824D..0x7A826E`). Consequences: `xref.py callers` returns *chunk*
addresses (that is why the parallel report's "callers of `0x79E870`" style queries look odd), and
any per-chunk displacement census (e.g. "who uses disp `0xB0`") fragments one function into
several rows. I always reassembled contiguous chunks before concluding anything.

---

## 7. What a DLL should write

### 7.1 The literal answer to the brief (confirmed, but **not** the 99 gauge)

```c
char  *base  = (char *)GetModuleHandleW(NULL);
void  *chr   = *(void **)(base + 0x18A0490);        // null outside a level
void  *param = chr ? *(void **)((char *)chr + 0x240) : NULL;
float *f15C  = (float *)((char *)param + 0x10B0 + 0x15C);   // == param + 0x120C
```
`*f15C` is the guard-penetration ratio (0…1) that `0x7A8120` accumulates into. It is **rewritten
every frame** by the engine's own tick while a `GuardPenetration` (id `0x48`) state exists, so an
out-of-band write persists only as long as no such state is active — the same "read back a
different value" symptom the brief reports for `param+0x48`.

### 7.2 The amrita / 99 gauge: two candidates in the `param+0xB0` sub-object

```c
char  *sub = (char *)param + 0xB0;      // amrita / LW sub-object ("AmritaGauge" state machine)

// (B) the only continuously drained field of the cluster  -> best candidate for the 99 bar
float *gauge   = (float *)(sub + 0x50);   // == param + 0x100   [probable]
// (C) the field the brief measured in game as the "burning bar" (0..100)
float *burn    = (float *)(sub + 0x1C);   // == param + 0xCC    [confirmed as a field of sub]
float *burnMax = (float *)(sub + 0x20);   // == param + 0xD0    (clamp for (C))
// integer amrita counter pair, clamped by 0x7AF370
int   *icur    = (int   *)(sub + 0x10);   // == param + 0xC0
int   *imax    = (int   *)(sub + 0x18);   // == param + 0xC8
```

**Recommended single write, if a raw write is what is wanted** — and with the sign caveat:

```c
// "fill" the 99 gauge: drive sub+0x50 to its empty/complete end (Reading A in §5.6)
*(float *)((char *)param + 0xB0 + 0x50) = 0.0f;      // param + 0x100
```
…but because the sign is *probable* and not *confirmed*, the **safer and mechanism-correct**
action is to let the engine do it:

### 7.3 Preferred: drive the engine's own gauge primitives (no sign guessing)

Option 1 — reuse the primitive directly (fastcall, no stack args, no allocation):

```c
typedef unsigned char (__fastcall *amrita_add_t)(void *sub, int amount);      // 0x7AF370
typedef unsigned char (__fastcall *amrita_drain_t)(void *sub, float amount);  // 0x7B08C0
amrita_add_t   amrita_add   = (amrita_add_t)(base + 0x7AF370);
amrita_drain_t amrita_drain = (amrita_drain_t)(base + 0x7B08C0);
amrita_add((char *)param + 0xB0, n);      // sub+0x10 += n  (clamped by sub+0x18)
```
Caution: `0x7B08C0` calls `0x7C7EB0(param)` when the gauge reaches 0, i.e. it can trigger the
engine's own "gauge empty / LW changed" notification — call it only from the game thread.

Option 2 — **the important correction for the existing mod**: the state class whose apply actually
*refills* the amrita gauge is **`Character::AddStateObjectAmritaGaugeRecover`, state id `0x42`,
ctor `0x79E7B0`** (its apply is `0x7A8170` → `0x7AF370` + `0x7B08C0`, §5.1), **not**
`AmritaGaugeUp` (id `0x20`), which only multiplies `manager+0x64` by `1+mag` (§1.4). So the mod's
"add `pct` % to the 99 gauge" call should target id `0x42`, and — this is an easy-to-miss ABI
detail — **its magnitude is read as an `int`** (`0x7A81FE  mov edx, dword ptr [rbx+0x50]`), so the
value must be passed in the low 32 bits of `xmm2` (e.g. `_mm_cvtsi32_si128(n)`), not as a float;
`AmritaGaugeUp`'s ctor, by contrast, wants a genuine float. The ctor signature is
`ctor(manager, float seconds, xmm_magnitude)` and the object must then be inserted with
`add(manager, key, obj, -1, 0)`; the engine's own calls use **key = the object's own id**
(`0x38`/`0x39`/`0x3a`/`0x3b` were the keys used by the engine's amrita helper, §5.5), and it
re-applies only if the key is absent (`0x7A56A0`), otherwise it just refreshes
`[obj+0x28] = [obj+0x2c]`.

---

## 8. Best next step (one runtime measurement) and remaining static steps

**One measurement settles §5.6 (and therefore the exact offset).** With the mod's existing
read-only probe (hardware breakpoint / `ReadProcessMemory`), resolve `param` as above and sample
**six dwords** at three moments each — *before* LW, at the frame LW is activated, and every second
during LW:

```
param+0xC0  (sub+0x10, int)      param+0xC8  (sub+0x18, int)
param+0xCC  (sub+0x1C, float)    param+0xD0  (sub+0x20, float)
param+0x100 (sub+0x50, float)    param+0x108 (sub+0x58, char* -> must equal chr)
```

* whichever of `sub+0x50` / `sub+0x1C` moves **monotonically** toward its clamp while LW is active
  is the burning bar; the other one is a secondary timer/ratio;
* if `sub+0x20` reads `1.0` the bar is normalised; if `100.0` it is the 0…100 bar the brief
  measured; that number is the scale to write;
* if `sub+0x50` runs *down* from a positive max to 0 exactly when LW ends, Reading A/B in §5.6 is
  decided by whether LW ends at `0` or at `max`.

If a static route is preferred instead, the two remaining steps are:

1. **Find the ctor of the `param+0xB0` sub-object** (the function that stores `chr` into
   `[sub+0x58]` and initialises `+0x00/+0x08/+0x10/+0x18/+0x1C/+0x20/+0x50`). Search for
   `mov [reg+0x58], <reg holding char>` together with `mov dword [reg], -1` in the `param`
   constructor `0x7698E0` and its callees; that ctor gives the *initial* (full or empty) values and
   therefore the scale of `sub+0x50`, plus the meaning of `sub+0x00`.
2. **Read the HUD binding**: `Menu::PlayerGauge` (vtable `0x1277E28`, COL-verified) slot 3
   `0xC59D60` reads `param+0x74/+0x7C` and `lea rdi,[param+0x40]` and calls `0x7B2290(param+0x40)`
   — i.e. it is the *Ki* gauge, not the LW bar. Finding the sibling menu widget that reads
   `param+0xB0+0x1C/+0x50` (cross-reference `disps[0x100]`/`disps[0xCC]` inside the `Menu` region
   `0xC5xxxx..0xC6xxxx`) would name the bar and give its display scale directly.

**Which sub-questions I did answer** (per the brief's menu):

* **1 — yes, fully**: the caller is the tick `0x7ACCA0` at `0x7ACD1A`; the apply is
  **slot 6 (+0x30)**, `arg2 (rdx)` = the state manager = `[[char+0x240]]+0x10B0`
  (all 8 tick call sites verified), so `[rdx+0x15C]` = `param+0x120C`; plus the correction that
  `0x7A8120` is `AddStateGuardPenetration`'s apply, not `AmritaGaugeUp`'s.
* **2 — yes**: exact DLL expression given, `*(float*)(*(void**)(*(void**)(base+0x18A0490)+0x240) + 0x10B0 + 0x15C)`.
  (Note it identifies a guard-penetration aggregate, not the 99 gauge.)
* **3 — yes, fully**: the generic per-frame tick is `0x7ACCA0`, full listing and its 8 call sites /
  6 driving functions given.
* **4 (bonus) — yes**: the drain is `0x7B08C0` / `0x7B0860`, both on `sub+0x50` = `param+0x100`,
  with the "reached 0" notification `0x7C7EB0(param)`.
* **Not fully answered**: the *exact offset of the displayed 99 bar* — because the parent premise's
  offset (`manager+0x15C`) belongs to a different class and the two remaining candidates live in a
  sub-object whose drain direction/scale is only resolvable at runtime. I did **not** invent an
  offset for it; §7.2/§8 give the candidates and the measurement.

---

## 9. Appendix — RVA quick reference (this report)

| RVA | what |
|---|---|
| `0x7A8120` | apply of `AddStateGuardPenetration` (slot 6 of `0x11A97C0`); `[rdx+0x15C] += [rcx+0x50]`, clamp 1.0 |
| `0x11A97F0` | the **only** qword in the image equal to `base+0x7A8120` |
| `0x11A97C0` | vtable `.?AVAddStateGuardPenetration@Character@@` (COL `0x1593B10`) |
| `0x11A8B90` | vtable `.?AVAddStateObjectAmritaGaugeUp@Character@@` (COL `0x1592790`) |
| `0x79E870` | ctor `AmritaGaugeUp` (id `0x20`, mag at `+0x50`, vtable `0x11A8B90`) |
| `0x7A8280` | **real** `AmritaGaugeUp` apply (slot 6): `[rdx+0x64] *= slot2()` |
| `0x7A3CF0` | `AmritaGaugeUp` slot 2: returns `mag+1` |
| `0x7A8110` | shared `ret 0` (slot 8 of 87 state vtables) |
| `0x7A2890` | `add(mgr, key, obj, r9d, stack0)`; calls slot 8 at `0x7A29C2` / `0x7A29F9`; inserts into the store `[mgr+0x170]` |
| `0x7A2C80` | `(mgr, obj, replaceSameType)`: links into the **active list** `[mgr+0x198]` (link via `0x7AAB30`) |
| `0x7A2AD0` | `add()` wrapper (`r9d = -1`) + post-processing |
| `0x79E1C0` | `(mgr, n)`: for objects with `[obj+0x10]==2` in list `[mgr+0x188]`, `[obj+0x50] = n` |
| `0x7AAB30` | intrusive-list link helper used by `0x7A2C80` |
| `0x7A56A0` | "is a state with this id already in the manager?" presence check |
| `0x7A2EF0` | clear-all state walk (slot 7 + slot 0 + free) |
| `0x7ACCA0` | **generic per-frame state tick** (`rcx`=state obj, `rdx`=target, `xmm2`=dt); `call [rax+0x30]` at `0x7ACD1A` |
| `0x7AC6E0` | per-frame driver; 3 tick sites; walks `[mgr+0x180/0x188/0x198/0x1a8]` |
| `0x7AD260` | tick/expiry pass over the store (`[mgr+0x170]`) |
| `0x79FED0` | ctor `AddStateGuardPenetration` (id `0x48`); 1 caller |
| `0x76D046` | the only application site of `GuardPenetration` (fn `0x76CDE0`), gated on `0x8D4C30` |
| `0x8D4C30` | `movzx eax,[0x191DE38]` (LW-module flag) |
| `0x7A8170` | apply `AmritaGaugeRecover` (id `0x42`); body `0x7A8170..0x7A8274`; ctor `0x79E7B0` |
| `0x7A8DE0` | apply `DyingAmritaUp` (id `0x38`) |
| `0x7A8960` | apply `CallSpirit` (id `0x22`); touches `manager+0x10C` |
| `0x7AF370` | `sub+0x10 += edx`, clamp `sub+0x18` (+ owner dirty flag) |
| `0x7AF970` | `sub+0x1C += xmm1`, clamp `sub+0x20` |
| `0x7AF9A0` | accumulator `sub+0x4C += (float)n * sub+0x40 * (1 - over/…)` |
| `0x7AFDA0` | `sub+0x00 == 0 && sub+0x10 >= sub+0x14` |
| `0x7AFE20` | sets `sub+0x08 = mode` and applies state to `[[sub+0x58]+0x240]+0x10B0` |
| `0x7B0860` | drain `sub+0x50 -= (float)max(edx,0) * [0x11A055C]`; 0 ⇒ `0x7C7EB0(param)` |
| `0x7B08C0` | drain `sub+0x50 -= xmm1`; 0 ⇒ `0x7C7EB0(param)` |
| `0x7B4220` | `sub+0x00 == 1` |
| `0x7B4390` | `(cur*100/max) <= [0x11A04BC] && [param+0xB0] != 1` |
| `0x7C7EB0` | "gauge empty / amrita state changed" handler (152 callers) |
| `0x7AA67C` | absorb handler tail: +1 counter, `sub+0x50 -= 1.5/2.0`, or `sub+0x1C += 0.10/0.12` if `sub+0x00==1` |
| `0x6EC7C0` / `0x6EC800` | `Refer::Stamina` / `StaminaRate` → `param+0x40` / `+0x44` |
| `0x6EC970` / `0x6EC9A0` | `Refer::StealExpAmuritaBaseSum` / `Sum` → `param+0x1278` / `+0x1270` |
| `0x11A055C` | `15.0f` — int→gauge drain scale of `0x7B0860` |
| `0x11A09DC/9E0/A04/A08/A0C/A10/A0D1C` | `3.0f / 0.2f / 0.12f / 0.10f / 2.0f / 1.5f / 0.2f` |
| `0x11A04BC/540/544` | ints `30 / 300 / 1000` read by the amrita code |
| `0x11A04FC / 0x11A059C` | `4.0f` / `180.0f` |
| `0x1588938` | `1.0f` |
