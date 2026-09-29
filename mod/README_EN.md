# Nioh 1 · Perfect Guard — v0.1.1

Timed-guard rewards for *Nioh: Complete Edition* (`nioh.exe` 1.24.8), in the spirit
of Nioh 3's Guard Parry.

> **Status: all four code anchors verified in the live game (4/4) and the mod
> installs and runs.**
> Implemented: guard window detection (real button edge), guard Ki cost
> reduction, Ki recovery, enemy Ki/HP damage, sound, live INI reload, logging.
> Experimental and off by default: cancel recovery.
> Not implemented: visual effects (deliberately dropped).

---

## Install

Requires an x64 DLL loader that scans a `mods\` folder (e.g. the
`NiohNativeModLoader` / `dinput8.dll` proxy).

1. Close the game.
2. Copy the whole `Nioh1PerfectGuard` folder into the game's `mods\` directory:
   ```
   nioh.exe
   dinput8.dll            <- loader
   mods\
     Nioh1PerfectGuard\
       Nioh1PerfectGuard.dll
       Nioh1PerfectGuard.ini
       Sounds\parry.wav
   ```
3. Start the game.

**Uninstall**: close the game and delete the `mods\Nioh1PerfectGuard` folder. No
game file is ever modified. If you no longer want the loader either, also delete
`dinput8.dll` from the game root.

### Turning the mod off / rolling back

| Goal | How | Effect |
| --- | --- | --- |
| **Temporarily disable** (keep the files) | set `Enabled=0` in the INI, **then restart the game** | the log shows `STATUS BYPASS: Enabled=0`, **no breakpoint is armed at all**, nothing is written |
| **Full uninstall** | delete `mods\Nioh1PerfectGuard\` (and `dinput8.dll` if unwanted) | the game is back to stock |
| **Disable just sound / one reward** | change that INI key | takes effect immediately |

> `Enabled` is the **only** key that needs a restart: installing or removing the
> breakpoints is a one-shot startup decision, and changing it at run time does
> nothing (the mod logs `NOTICE: Enabled changed to ...` to tell you).
> Every other key applies live, within about a second.

No game file is modified, and not one byte of `.text` is changed: the mod uses the
CPU's hardware execution breakpoints (DR0–DR3) plus an exception handler, and only
edits a few numeric fields in memory when a breakpoint hits. That is why uninstalling
is just deleting a folder.

---

## How it works (and why it is safe)

The mod **never writes a single byte of game code**. It arms x64 hardware
execution breakpoints (DR0–DR3) on the guard-resolution instructions, and edits
the *saved thread context* inside a vectored exception handler. When the thread
resumes it sees the modified world.

Consequences:

- the code section stays pristine — there is no integrity-check surface;
- no instruction-length decoding and no trampoline relocation is needed;
- at load time every anchor's **expected instruction bytes are verified**, and a
  mismatch makes the mod refuse to install rather than corrupt anything.

Verified startup log:

```
ANCHOR ok   guard_flag_1       rva=0x74DD09
ANCHOR ok   guard_flag_2       rva=0x74DD92
ANCHOR ok   guard_ki_cost      rva=0x74DE51
ANCHOR ok   inputmgr_getter    rva=0xE6BE90
INPUT manager slot = 0x7FF7ABE48658 (from getter 0xE6BE90 + disp 0xD0C7C1)
ANCHOR 4/4 verified
ARM pass: 372 threads seen, 372 newly armed
STATUS ACTIVE anchors=4/4 reduction=100% recovery=3 gate=1 mask=0x0100 slot=0 sound=1
```

Note that the input-manager address is **derived from the getter's own
instruction bytes** rather than hardcoded, so a different build fails the check
instead of reading the wrong memory.

---

## Configuration (`Nioh1PerfectGuard.ini`)

Every key has bilingual comments with allowed ranges and defaults. Most settings
apply **within about a second** while the game is running.

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | 1 | Master switch. 0 = no breakpoints are armed |
| `WindowMs` | **450** | Guard window in ms, from the fresh press (widened from 250 on 2026-09-28) |
| `CancelRecovery` | 0 | **Experimental** cancel of guard recovery (see below) |
| `CancelRecoveryFrames` | 30 | Frames advanced when cancelling |
| `RequireTimelyGuard` | 1 | 1 = only a fresh press within `WindowMs` counts |
| `GuardButtonMask` | 0x0100 | Pad guard button (standard XInput bits; `0x0100` = LB/L1) |
| `PadSlot` | 0 | Which controller slot to read, 0–3 |
| `GuardKeyVK` | 0 | Optional keyboard virtual-key code, OR'd with the pad mask |
| `LearnButtons` | 0 | Log newly seen pad bits and key codes (calibration) |
| `KiTrace` | 1 | Sample both candidate Ki fields (temporary diagnostic) |
| `KiDamageReductionPercent` | 100 | Guard Ki cost reduction, 0–100. 100 = free blocking |
| `KiTopUp` | 1 | Also top the visible Ki field back up — see "two reduction mechanisms" below. **Leave at 1** |
| `KiTopUpPreEventMs` | 100 | How far back (ms) the top-up's reference Ki value is taken from (0 = disabled). The flag source fires *after* the charge, so a wrong reference refunds nothing — see below |
| `KiRecoveryMode` | 3 | 0 none / 1 refund cost / 2 fixed / **3 one sixth of max Ki** |
| `FixedRecovery` | 50 | Used by mode 2 |
| `HpRecoveryMode` | 1 | **HP restore on a perfect guard**: 0 off / **1 percent of max HP (default)** / 2 fixed / 3 both |
| `HpRestorePercent` | 3 | Percent of maximum HP per perfect guard (truncated: 3% of 880 = 26) |
| `HpRestoreFixed` | 50 | Flat HP per perfect guard, used by mode 2 or 3 |
| `SpeedBuffPercent` / `SpeedBuffMs` | **0** / 10000 | **Move-speed buff after a perfect guard** (0 = off). Set to 4 to enable. ⚠ Off this round: it is the only feature that calls game code and it is not yet verified in game |
| `DamageCutPercent` / `DamageCutMs` | **0** / 10000 | **Damage-taken reduction after a perfect guard** (0 = off). Set to 4 to enable |
| `ArmorBuff` / `ArmorBuffMs` | 0 / 5000 | **Armour** (dropped by decision, see below) |
| `LivingWeaponGaugeOnGuard` / `LivingWeaponGaugePercent` / `LivingWeaponGaugeMax` / `LivingWeaponGaugeOffset` | **1** / 10 / **515** / **0x100** | **99 gauge (amrita / guardian-spirit gauge) accumulation**: +N% per perfect guard; **on by default**, 10% |
| `LivingWeaponExtendOnGuard` / `LivingWeaponExtendPercent` | **1** / 10 | **Extend the burning gauge while the 99 state is active**: +N% per perfect guard; **on by default**, 10% |
| `CancelActionOnGuard` | **1** | **A pure guard press cancels the current action** (0 = off): attacks/skills, drinking and using items, onmyo talismans, ninjutsu, throwing items. Guard+X/Y/A is a combination and does not cancel; movement is irrelevant |
| `AttackButtonMask` / `ComboGuardWindowMs` | 0xF000 / 100 | Which buttons count as attacks / how close a press counts as a combination |
| `CancelActionStrictHold` / `CancelActionFrames` / `CancelActionRecentMs` | 0 / 30 / **0** | Strict “held blocks” mode / motion frames advanced / **0 = any guard press cancels** (perfect and normal blocks alike) |
| `BlockEventSource` | 2 | **Which event means the player blocked**: `2` auto (recommended) / `0` guard-cost site only / `1` guard-flag site only |
| `DiagDisable` | 0 | Diagnostic bitmask, keep at 0: `1` no breakpoints / `2` no input thread / `4` enable the rolling re-arm (**known to crash the game**) / `8` skip the self-test |
| `EnemyKiDamage` | 0 | Ki damage to the attacker (`[[char+0x240]+0x40]`) |
| `EnemyHpDamage` | 0 | HP damage to the attacker (`[[char+0x240]+0x20]`, can kill) |
| `SoundEnabled` / `SoundVolume` / `SoundFile` | 1 / 1 / parry.wav | Guard sound |
| `DiagnosticHotkey` | 1 | `Ctrl+Shift+F10` in game writes a stats line |

Invalid values are rejected and the previous valid configuration is kept, with
the reason written to the log:

```
CONFIG reject KiDamageReductionPercent=150 (allowed 0..100); keeping 100
CONFIG gameplay group rejected; previous settings kept
```

`KiDamageReductionPercent` is enforced by **two** parallel mechanisms, because
static analysis cannot prove the entry the guard-cost anchor writes is the field
the Ki bar displays:

1. scale `xmm1` at the subtract site (`0x74DE51`);
2. top the confirmed Ki field `[[char+0x240]+0x40]` back up by `reduction%` of the
   loss, within a 400ms window — only ever raising, only up to the snapshot.

> The first release of (2) re-added `loss * reduction%` on *every* worker tick.
> The worker ticks every 8ms, so a 400ms window is ~50 iterations and even
> reduction=20% left only `0.8^50 ~= 1e-5` of the loss: **any non-zero setting
> behaved like 100%**. It now latches the peak loss and adds only the shortfall,
> so the percentage is honoured. `KIV` reports which mechanism is doing the work.

### No guard sound? Read the `SOUND` line

The audio path never guesses: any problem prints an explicit `SOUND disabled: ...`
line and stays silent rather than pretending to work or crashing the game.

| Log | Cause / fix |
| --- | --- |
| `SOUND ready (XAudio2 engine=xaudio2_9.dll)` | normal, sound is available |
| `SOUND disabled by configuration` | `SoundEnabled=0` in the INI — normal |
| `SOUND disabled: xaudio2_9.dll / XAudio2Create unavailable` | XAudio2 runtime missing (install the latest DirectX end-user runtime) |
| `SOUND disabled: cannot open <path>` | `Sounds\parry.wav` is not next to the DLL, or `SoundFile` names the wrong file |
| `SOUND disabled: expected RIFF WAVE` | that file is not a WAV |
| `SOUND disabled: only 16-bit PCM supported` / `use 16-bit integer PCM WAV (format 1)` | not 16-bit PCM — re-encode your own sound |
| `SOUND disabled: need 44100 or 48000 Hz (got N)` | sample rate is neither 44100 nor 48000 |
| `SOUND disabled: WAV missing fmt or data chunk` / `cannot read complete WAV` / `WAV size N out of range` | the WAV is truncated or corrupt |
| `SOUND disabled: CreateMasteringVoice hr=0x...` | no usable audio output device |
| `SOUND disabled: CoInitializeEx hr=0x...` / `XAudio2Create hr=0x...` | COM or XAudio2 init failed |
| `SOUND submit failed` | buffer submission failed (usually an exclusive-mode device) |

To use your own sound, drop a 16-bit PCM 44.1/48kHz WAV into `Sounds\` and set
`SoundFile` to its name (applies live, no restart).

### The event-source decision is remembered (`Nioh1PerfectGuard.state`)

On the first automatic run, if three player blocks go by without the guard-cost site
ever firing, the mod switches to the flag source and **writes that conclusion to a
one-line file next to the DLL** (`Nioh1PerfectGuard.state`). The next launch then
starts on the right source immediately, instead of waiting for three blocks again.

Delete that file to re-decide (for example after changing character, level, or game
version). The reverse works too: if a guard-cost event ever does fire, the mod
rewrites the file (`costsite=fires`) and returns to the precise source.

### The guard event source, and the two reduction mechanisms

Two different moments can mean "the player blocked". `BlockEventSource` picks one:

| Value | Event | Upside | Cost |
| --- | --- | --- | --- |
| `0` | the guard Ki cost site | most precise; **the only place the engine's actual charge can be scaled** | the engine gates that whole block behind three conditions of its own, and when they do not hold nothing is observable at all |
| `1` | the "attack was guarded" flag site | fires on a far broader condition, and is confirmed to fire in this game | no charge is visible: `KiRecoveryMode` can only be 0 or 3, the subtract-site scaling does not apply, and `KIV` is not emitted |
| `2` (default) | **auto** | tries the cost site; if it never fires while the flag site has seen 3 player blocks, it switches and **says so in the log**, and switches back if a cost event ever appears | none |

**Two reduction mechanisms** (`KiDamageReductionPercent`):

1. **scale** the amount the engine is about to subtract at the cost site — only
   available when the source is `0` (or auto decided the cost site works);
2. **top up** the visible Ki by `reduction%` of the loss — controlled by `KiTopUp`.

**Both depend on the order of events.** The cost site fires *before* the engine
subtracts, so there the live value is the correct reference. The flag site fires
*after*: in a real session the log showed `ki=71.03/105` in the very same
millisecond as the perfect guard, down from a full 105. A reference read at event
time therefore already contains the charge, the measured loss is zero, and the
top-up silently does nothing — which is exactly what a user reported ("everything
works except the Ki refund").

So under the flag source the reference comes from **`KiTopUpPreEventMs`** (100ms by
default): the 8ms input tick keeps a ring of samples and the reference is the
highest value seen inside that window, i.e. from before the charge. In the log:

- `KIREF pre-event reference 105 vs live 71.03` → the reference is right;
- `KIV-FLAG ... handed back 33.97/100%` → that 33.97 was actually given back.

Trade-off: an unrelated spend that lands inside the window is refunded too — and the
likeliest case is this mod's own cancel playstyle (attack, then press guard to cancel
it; the cost is charged the moment the swing starts). If the two are closer together
than the window, that swing's Ki is refunded as well. `KIREF` prints the reference it
adopted: if it is clearly above your real Ki before the block, that is what happened —
lower `KiTopUpPreEventMs` (e.g. 50) or set it to `0`.

### The 99 gauge (amrita / guardian-spirit gauge): two switches

| Switch | When it applies | Default |
| --- | --- | --- |
| `LivingWeaponGaugeOnGuard` (+ `LivingWeaponGaugePercent`) | while **not** in the 99 state: +N% gauge per perfect guard | **on** / 10 |
| `LivingWeaponExtendOnGuard` (+ `LivingWeaponExtendPercent`) | while **in** the 99 state: +N% per perfect guard (extends the burning gauge) | **on** / 10 |

Mechanically these do not write a field: they call the engine's own state object
(`Character::AddStateObjectAmritaGaugeUp`, constructor `0x79E870`, state id `0x20`),
whose apply is `gauge = min(gauge + magnitude, 1.0)` on a gauge normalised to 0..1. So
"10% of the gauge" is the magnitude `0.10` and the engine does the clamping. Whether the
99 state is active is decided by **two sources OR-ed**: ① the engine's state container
(`[[char+0x240]]+0x10B0`) holding the `CallSpirit` state object (state id `0x22`) — the
primary judge; ② the engine's activation flag byte (the one
`Player::SetTsukumoWeaponActiveFlag` writes). Either one counting as "active" is enough;
if neither can be read the mod treats it as inactive (accumulate), so a wrong reading can
never silently disable accumulation.

Every call is logged:

```
LW engine: the 99-gauge constructor at 0x... stamps state id 0x20 in its first 128 bytes
LW gauge: +10% via AmritaGaugeUp state 0x... (in 99 state=0 [container 0x22=0, flag=-1], add()=1, container check: present)
```

`add()=1` and `present` mean the engine accepted the state object. Like the timed buffs
this path calls game code (switched off together by `DiagDisable` bit 16) and it
**never calls the removal path** (that one crashes), leaving the tiny duration to expire
on its own.

### Encoding

The config reader detects the encoding itself, so UTF-8 (with or without BOM),
UTF-16 LE (with BOM) and ANSI all parse correctly. Keys outside `[PerfectGuard]`
are ignored, and `;` / `#` comments are skipped. An automated check in the
development source tree covers these cases (`tools\test_ini_encodings.py`, 6/6);
that tool does not ship in the release package.

### Keyboard and mouse players

`GuardButtonMask` describes a controller. For keyboard, use the calibration
built into the mod:

1. Set `LearnButtons=1` and save (applies live, no restart).
2. In game, press your guard key a few times, then read the log:

```
LEARN pad bit 8 pressed -> GuardButtonMask=0x0100 (raw word 0x0100)
LEARN key VK=0xA0 pressed -> GuardKeyVK=160
```

3. Put those numbers in the INI and set `LearnButtons` back to 0.
   Common VK codes: left Shift = 160 (0xA0), Ctrl = 17, Space = 32, LMB = 1.

### Controller buttons and triggers (a layout that is now proven)

The mod reads the input manager's per-slot record (20 bytes per slot), which
embeds a standard Windows `XINPUT_GAMEPAD`:

| Offset (+ slot*20) | Field |
| --- | --- |
| `+0x49E04` | slot is connected (byte) |
| `+0x49E0C` | **`wButtons` (16-bit button mask)** |
| `+0x49E0E` / `+0x49E0F` | `bLeftTrigger` / `bRightTrigger` (1 byte each) |
| `+0x49E10` … | thumbsticks (four 16-bit signed values) |

This is not a guess. The engine's own button test at `0xE6BCF0` is
`test word ptr [padstate+4], cx` with `padstate = manager+0x49E08+slot*20`, which
puts the button word exactly at `+0x49E0C`; indices 14 and 15 test
`byte [padstate+6]` and `byte [padstate+7]`, and the sticks are read at `+8`/`+0xa`.
Three offsets agree with `XINPUT_GAMEPAD` independently.

Its bit table (RVA `0x12BF690`, 14 entries) matches standard XInput for the first
twelve: `0x0001/0002/0004/0008` dpad, `0x0010` START, `0x0020` BACK,
`0x0040/0080` stick clicks, **`0x0100` = L1/LB (the default guard button)**,
`0x0200` = R1/RB, `0x1000` = X, `0x2000` = Y.

> ⚠️ **`GuardButtonMask` can never match a trigger.** L2/R2 live in those two
> separate bytes, not in the 16-bit button word, so no mask value can hit them.
> If the game has guard bound to L2/R2, rebind it in game or use `GuardKeyVK`.
> When the mod sees a trigger squeezed while no guard press has ever been
> detected, it prints `NOTICE: a trigger is being squeezed (LT=.. RT=..) ...`.

### Cancel recovery (experimental)

Recovery is cancelled by **advancing the character's motion frame**
(`[[char+0x38]+0x60]`, the field the engine's `Refer::MotionFrame` reads). This
is **not yet verified in game** and may conflict with some actions, so it is off
by default. It only ever *adds* to a value, and only when the current value
already looks like a motion frame (`0 ≤ v < 10000`); otherwise it does nothing.
Enable with `CancelRecovery=1` and tune `CancelRecoveryFrames`.

---

## Field layout used

All of these were read out of the engine's own getters, not guessed:

| Node | Path | Type |
| --- | --- | --- |
| `Refer::Hp` | `[[char+0x240]] + 0x20` | int32 |
| `Refer::MaxHp` | `[[char+0x240]] + 0x18` | int32 |
| `Refer::Stamina` | `[[char+0x240]] + 0x40` | float |
| max Ki | `[[char+0x240]] + 0x44` | float |
| `Refer::MotionFrame` | `[[char+0x38]] + 0x60` | float |
| `Refer::ActionId` | `[[[[char+0x230]+8]+0x58]+0x20] + 0xC` | int16 |
| guard resource entry | `[[char+0x240]+0xBA8] + 8 + i*0x50` (i = 7) | 0x50 per entry |

The hit context carries both combatants (`[ctx+0x100]` and `[ctx+0xE8]`); the one
that is not the player is treated as the attacker. Both use the same `Character`
layout the player does.

---

## Troubleshooting

The log is `Nioh1PerfectGuard.gameplay.log` next to the DLL.

| Symptom | Meaning |
| --- | --- |
| No `PROBE`/`ANCHOR` lines at all | the DLL was not loaded by the game |
| `ANCHOR MISS ...` lines | wrong game build; the mod refuses to install |
| `SELFTEST breakpoint: OK: 5/5 ...` | the DR0–DR3 + VEH trap mechanism provably fires in this process |
| `SELFTEST breakpoint: FAILED ...` | execution breakpoints do not work here; no reward can ever fire |
| `STATUS DEGRADED(no-reward)` | the guard-cost anchor failed to verify, so no reward can ever fire |
| `GUARD pressed (pad=0x0000 ...)` never appears | wrong button mask — see LearnButtons |
| No `KITRACE` lines | still in the title screen; the player object does not exist yet |
| `LAYOUT` shows odd numbers | send me the line; the offsets need adjusting |

### Why there is a `SELFTEST` line

The mod runs on execution breakpoints in DR0–DR3 dispatched through a vectored
exception handler. If that mechanism did not work on your machine, the symptom
would be **zero events, zero crashes, a completely silent log** — indistinguishable
from "the player has not reached combat yet". So the mod no longer assumes it: on
every start it arms a breakpoint on **its own** probe function, calls it five
times, and counts how many of those calls the handler trapped.

```
SELFTEST breakpoint: OK: 5/5 probe calls trapped by the VEH (DR0=..., Dr7=L0 exec)
```

`OK` means the trap mechanism is confirmed working in `nioh.exe`. `FAILED` means
nothing further can work, and the line says so explicitly.

`LAYOUT` is written on each of the first ten perfect guards and reports the
player pointer, HP/max HP, Ki/max Ki, action id, motion frame and both combatant
pointers.

### `KIV`: which field a guard cost actually moves

One question could not be closed statically. The guard-cost anchor writes the
resource-table entry at `[[char+0x240]+0xBA8]+8+7*0x50`, `+0x0C`, but the Ki bar's
own accessor `Refer::StaminaRate` (`0x6EC800`) reads `[[char+0x240]]+0x40` and
`+0x44` and returns current/max as a 0..1 ratio. So the mod prints both numbers
side by side and states the conclusion for you:

```
KIV block #1 charge D=6.4 visible loss L=6.4 scaled D*(1-r)=3.2 residual=0% -> the
confirmed Ki field took the FULL cost
```

The `residual` is how far the observed loss sits from the chosen model, as a
percentage of `D` — smaller is more trustworthy.

| Verdict | Meaning |
| --- | --- |
| the field did **not** move | the resource table is unrelated to the bar |
| it took the **full** `D` | the subtract-site scaling does not reach the bar; the top-up is what spares Ki |
| it took the **scaled** `D*(1-r)` | the subtract-site scaling already reaches the bar, so the top-up double-counts |
| `INCONCLUSIVE by configuration` | at the current reduction two models coincide numerically, so the question cannot be answered |
| `INCONCLUSIVE: ... margin` / `no model fits` | the loss sits between models, so no conclusion is drawn |

> **Set `KiDamageReductionPercent=50` to actually get an answer here.** At the
> default of 100 the scaled cost is `D*(1-100%) = 0`, which is the same number as
> "the field never moved" — mathematically indistinguishable. In that case the mod
> now reports `INCONCLUSIVE by configuration` rather than guessing; an earlier
> version guessed, and guessed wrong. Anything in 20–80 separates them, 50 is
> cleanest. Change it back afterwards if you like — it applies live.

**What to change once you have the verdict** — two of the three outcomes you can
act on yourself, with no new build:

| `KIV` verdict | What to set |
| --- | --- |
| it took the **full** `D`, or the field **never moved** | keep the default `KiTopUp=1`; the top-up is what spares Ki |
| it took the **scaled** `D*(1-r)` (scaling already reaches the bar) | set **`KiTopUp=0`** — otherwise both mechanisms subtract, and `KiDamageReductionPercent=50` actually removes 75% |

Switching the top-up off does not cost you the diagnosis: the mod keeps sampling
and keeps printing `KIV`.

> **`KIV` does not apply under the flag source** (no charge is visible there) — look
> at `KIV-FLAG` instead: `reference` is the pre-charge value and `handed back` is
> what was actually refunded. If a block produces neither `KIREF` nor `KIV-FLAG`,
> the charge landed outside the `KiTopUpPreEventMs` window; raise it (see the
> trade-off above).

The first ten perfect guards each produce one line — no arithmetic required.

Anchors used (RVAs, `nioh.exe` 1.24.8):

| Name | RVA | Meaning |
| --- | --- | --- |
| `guard_flag_1` | `0x74DD09` | sets "this attack was guarded" (branch 1) |
| `guard_flag_2` | `0x74DD92` | the other guard branch in the same function |
| `guard_ki_cost` | `0x74DE51` | guard Ki cost subtraction; `xmm1` = cost |
| `inputmgr_getter` | `0xE6BE90` | input manager singleton getter (validated, never armed) |

---

## Requirements and caveats

- Windows Steam *Nioh: Complete Edition*, `nioh.exe` 1.24.8.
- Recommended **offline**: Nioh 1 has online save validation, and community
  experience is that anomalous values can flag a save.
- `guard_ki_cost` reduces the guard cost at the point the engine applies it. The
  engine's `Refer::Stamina` getter reads a *different* field, so whether that is
  exactly the Ki bar the player sees is being confirmed with `KiTrace` in game.
