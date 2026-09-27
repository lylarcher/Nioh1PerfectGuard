# Nioh 1 Perfect Guard — a reverse-engineered gameplay mod

By **lylarcher** — source: <https://github.com/lylarcher/Nioh1PerfectGuard>

A mod for **Nioh 1 Complete Edition** (`nioh.exe` 1.24.8) that adds a *perfect guard*
— block at the moment you are hit and you get rewarded — plus a set of related
player-side rewards. Chinese version: [`README-CN.md`](README-CN.md).

> **User-facing manual** (installs with the package): `mod/README_CN.md`,
> `mod/README_CN.md` / `mod/README_EN.md` (full manual), `QUICKSTART.md` (Chinese one-pager, shipped) / `QUICKSTART-EN.md` (English), `docs/验收测试说明.md` (acceptance test, shipped as `ACCEPTANCE_TEST.md`).
> This file is the **project/engineering** readme.

---

## What it does

| Feature | Shipped state | Notes |
| --- | --- | --- |
| Perfect-guard detection | on | guard **press** rising edge within `WindowMs` (250) of the block |
| Ki cost reduction on block | on | `KiDamageReductionPercent=100` |
| Ki recovery | on | `KiRecoveryMode=3` (a sixth of maximum Ki) |
| Enemy Ki / HP damage | off | `EnemyKiDamage` / `EnemyHpDamage`, range-gated, never guesses a target |
| HP restore on perfect guard | **on** | 3% of maximum HP by default, or a flat value |
| **Guard cancels your attack** | **on** | a *pure* guard press cancels the current action; guard + X/Y/A (a martial-skill input) is preserved |
| Timed buffs (move speed / damage taken / armour) | **off, parked** | implemented against the engine's own state objects, dropped by decision (see `CHANGELOG.md` 2.0e) |
| Parry sound | on | XAudio2, played from a dedicated thread |

## How it works (and why it is built this way)

* **`.text` on disk is encrypted** by SteamStub (only ~0.39% matches the running
  image), so all reverse engineering is done on a **runtime memory image** dumped
  through `ReadProcessMemory` and rebuilt with an identity RVA mapping
  (`tools/dump_module.py` → `_work/nioh1.mem.exe`).
* The mod **modifies zero bytes of game code**. It installs x64 **hardware execution
  breakpoints (DR0–DR3)** on a handful of anchor sites and edits the saved thread
  context in a **vectored exception handler**.
* Every address it depends on is **byte-verified at load**; on a mismatch it refuses
  to install or degrade instead of writing to an address that a different build may
  have moved (`tools/test_anchors.py`).
* Rewards that touch the engine's own systems (the timed buffs) are **never called
  from the exception handler** — the handler only arms timers, and a watchdog thread
  performs engine calls. That path found a real crash and is documented in
  `docs/RE_NOTES.md` §4.52.2 / §4.54.

## Layout

```
mod/            MOD source (Nioh1PerfectGuard.c, pg_logic.h) + the shipped user docs
tools/          analysis, verification and test scripts (24 python tools + Zig tests)
docs/           plan documents (方案计划*.md)
docs/RE_NOTES.md     the reverse-engineering notebook — every address, dead end and correction
CHANGELOG.md    delivery notes: what is verified, what was fixed, what is pending
docs/交接摘要.md      handover summary (state, limitations, next steps)
docs/验收测试说明.md   in-game acceptance test (the user-facing checklist)
docs/探针会话说明.md   probe-session write-up
build.ps1/.cmd  build → run all checks → produce dist\  (dist is generated, not tracked)
dist/           the generated package (do not edit; run build.ps1)
```

## Build and verify

```powershell
.\build.ps1              # compile, run all nine checks, generate dist\
.\build.ps1 -Install     # also copy into the game's mods\ directory (game must be closed)
build.cmd                # double-click equivalent
```

The nine checks include: anchor bytes against the decrypted image, 200 pure-logic
assertions, "tested arithmetic == shipped arithmetic" (a digest compared between the
test binary and the shipped DLL), INI defaults vs. compiled defaults, documented log
markers/keys, documentation file reachability, a log-call-site cap audit, and the
export table. **Any failure aborts the build and produces no package.**

> `build.ps1` must stay **UTF-8 with BOM** — Windows PowerShell 5.1 reads a BOM-less
> `.ps1` as ANSI, and the Chinese text then breaks quote pairing. The script checks
> its own encoding on every run.

`tools/verify_buffs.ps1` is a one-command in-game experiment runner (installs, turns a
feature on temporarily, launches the game, watches the log and crash dumps, restores
the INI).

## Status

* Implementation of every shipped feature is complete; `build.ps1` is green.
* Verified **in game**: perfect-guard detection and rewards, HP restore, and
  guard-cancels-attack (confirmed by the author, `ACTION CANCEL` +
  `follow-up: action N -> M` in the log).
* Pending in game: the martial-skill (guard + X/Y/A) cases end-to-end, which need the
  skills to be unlocked; the cancel gate already preserves the input by design
  (`docs/RE_NOTES.md` §4.53, `CHANGELOG.md` 2.0d).
* Parked by decision: the timed buffs (their *install* path was proven in game; their
  *removal* path crashed and was removed — `docs/RE_NOTES.md` §4.54).

## Known environment caveats

* Installing requires the game to be **closed** (the DLL is held while it runs).
* The DLL hash depends on the **absolute output path** (Zig/lld build id), so a
  rebuild elsewhere legitimately differs; `SHA256SUMS.txt` verifies the shipped copy,
  not a rebuild (`docs/RE_NOTES.md` §4.49).
* A protected/leftover `nioh.exe` cannot be terminated from an unelevated shell and
  will block new instances — end it with an elevated Task Manager, or reboot.

## License

**PolyForm Noncommercial License 1.0.0** — see [`LICENSE`](LICENSE).
Copyright (c) 2026 lylarcher.
Free for **any noncommercial purpose**: personal use, study, hobby projects,
research, and use by charities, schools, public research bodies and government
institutions. **Commercial use is not permitted** — you may not sell it, bundle it
into a paid product, or distribute it as part of a monetised service.

Two honest notes about what this license is and is not:

* It is **source-available, not OSI "open source"**. No OSI-approved license can
  forbid commercial use, so a "noncommercial open-source license" does not exist;
  this is the standard way to get the restriction you asked for.
* A short summary is not the license: the English text in `LICENSE` is what governs,
  and it must travel with any copy you pass on (including the `Required Notice` line).

Separately from the license: the mod contains **no game code, assets or data** from
Nioh. Reverse engineering was done for personal, single-player use on a legally owned
copy of the game. *Nioh* and all related marks belong to Koei Tecmo; this project is
not affiliated with or endorsed by them.
