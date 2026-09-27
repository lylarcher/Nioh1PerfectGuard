# One-page guide — everything you need on a single page

A **perfect guard** mod for *Nioh 1 Complete Edition* (1.24.8): press guard at the
moment you are hit, and the block becomes a perfect guard with rewards. It **changes
no game file** — not one byte of `.text`: it uses CPU hardware execution breakpoints
plus an exception handler, and only writes a few numeric fields when a breakpoint hits.

Chinese version: `QUICKSTART.md` ｜ Engineering notes: `README.md`

## What is on by default

| Feature | Default |
| --- | --- |
| Perfect-guard detection (guard press edge + 250 ms window) | **on** |
| Guard Ki cost reduction, 100% (blocking costs no Ki) | **on** |
| Ki recovery (one sixth of maximum Ki) | **on** |
| HP restore on a perfect guard (3% of maximum HP) | **on** |
| **A pure guard press cancels the action you are performing** (attacks/skills, drinking, talismans, ninjutsu, thrown items) | **on** |
| Parry sound | **on** |
| Enemy Ki / HP damage | **off** (`EnemyKiDamage` / `EnemyHpDamage`; nothing is written unless you set them) |
| Timed buffs (move speed / damage taken / armour) | **dropped** (all off; see `docs/RE_NOTES.md` §4.52) |

## Install in three steps

1. **Close the game.**
2. Put `dinput8.dll` (a generic mod loader) in the game root
   (`...\steamapps\common\Nioh\`), then put the whole `Nioh1PerfectGuard\` folder
   into `mods\`.
3. Start the game **through Steam** (launching the exe directly makes it exit after
   ~28 s). **Offline mode is recommended**: Nioh 1 validates saves online.

```
Nioh\
  nioh.exe
  dinput8.dll            <- loader
  mods\
    Nioh1PerfectGuard\
      Nioh1PerfectGuard.dll
      Nioh1PerfectGuard.ini
      Sounds\parry.wav
```

## How to tell it is working

Open `mods\Nioh1PerfectGuard\Nioh1PerfectGuard.gameplay.log` after starting the game.
These three lines mean it is healthy:

```
ANCHOR 4/4 verified
SELFTEST breakpoint: OK: 5/5 probe calls trapped by the VEH
STATUS ACTIVE anchors=4/4 ...
```

- A failing `ANCHOR` means the game is not 1.24.8 — the mod then **refuses to install**
  on purpose.
- `SELFTEST` is the mod verifying *itself* on every start (that hardware breakpoints
  really trap on your machine). If it says `FAILED`, stop and send the log.
- For cumulative state at any time, press **`Ctrl+Shift+F10`** with the game focused;
  it writes a block of `STATE` lines.

## Shortest acceptance run (~5 minutes)

1. **Enter a mission** (the title screen does not really start one, though its demo
   loop does produce real block events).
2. Get hit and press guard **with the right timing**, five times → expect `PERFECT GUARD`.
3. **Lose some HP first** → expect `HP restore 550 -> 576 (+26) max=880 mode=1`.
4. **Cancel an attack with guard**: start an attack (X/Y), then while it is still
   running press **guard alone** (do not press an attack button with it) → expect
   `ACTION CANCEL: action=..` followed by `ACTION CANCEL follow-up: action .. -> ..`.
   Pressing **guard+X** (the shape of a martial-skill input) must **not** produce
   `ACTION CANCEL` — only `ACTION CANCEL skipped: guard+attack combination ...`.
5. The full checklist is in `ACCEPTANCE_TEST.md` (Chinese).

## Common adjustments (`Nioh1PerfectGuard.ini`; everything except `Enabled` is hot-reloaded, ~1 s)

| I want to… | Change |
| --- | --- |
| widen / narrow the perfect-guard window | `WindowMs` (250) |
| keyboard player: guard key does nothing | `LearnButtons=1` → press your guard key → copy `LEARN key VK=0x..` into `GuardKeyVK` → set it back to 0 |
| change the Ki cost reduction | `KiDamageReductionPercent` (100 = free blocking) |
| change how Ki comes back | `KiRecoveryMode` (0 none / 1 refund / 2 fixed / **3 one sixth of max**) |
| change HP restore | `HpRecoveryMode` (**1 percent** / 2 fixed / 3 both / 0 off), `HpRestorePercent` (3), `HpRestoreFixed` (50) |
| turn the attack cancel off | `CancelActionOnGuard=0` |
| stop a martial skill being read as "guard alone" | raise `ComboGuardWindowMs` from 100 to **150–250** (safer for skills, slightly later cancel) |
| make the cancel crisper | `CancelActionFrames` (30; higher skips more animation) |
| enable the enemy effects | set `EnemyKiDamage` / `EnemyHpDamage` non-zero (off by default, writes nothing) |
| the sound | `SoundEnabled` / `SoundVolume` / `SoundFile` (your own 16-bit PCM 44.1/48 kHz WAV) |
| quieter logs | `KiTrace=0` (worth doing once you are done testing) |

## Disabling / uninstalling

- **Temporarily off**: set `Enabled=0` and **restart the game** → the log shows
  `STATUS BYPASS`, **not a single breakpoint is armed** and no memory is written.
- **Full uninstall**: delete `mods\Nioh1PerfectGuard\` (and `dinput8.dll` if you do not
  need the loader any more).

## If something is wrong, look here first

| Symptom | Check |
| --- | --- |
| no `ANCHOR` line at all | does `mods\loader.log` show the mod being loaded? is the DLL in the right place? |
| lots of "not a perfect guard", never `PERFECT GUARD` | the guard button is not bound/configured (see `LearnButtons` above) |
| guard does not cancel the attack | the reason is printed after `ACTION CANCEL skipped:` (combination? no attack started recently?) |
| no parry sound | the `SOUND ...` lines, against the sound troubleshooting table in `README_CN.md` |
| the game crashes | send `%LOCALAPPDATA%\CrashDumps\nioh.exe.*.dmp` plus the log; use `DiagDisable` to bisect |
| cannot install / DLL in use | the game must be closed first; a protected leftover `nioh.exe` needs an **elevated** Task Manager (or a reboot) |

> Documents inside the package: `README_CN.md` / `README_EN.md` (full manual),
> `ACCEPTANCE_TEST.md` (in-game acceptance, Chinese), `CHANGELOG.md` (what is verified,
> what was fixed), `source\` (the source, so you can check every offset it writes).
