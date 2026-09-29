// Nioh1PerfectGuard.c -- perfect guard mod for Nioh: Complete Edition (nioh.exe 1.24.8)
//
// Design notes
// ------------
// This mod never writes a single byte of game code. It arms x64 hardware
// execution breakpoints (DR0..DR3) on the guard-resolution instructions found by
// reverse engineering, and edits the *saved thread context* in a vectored
// exception handler. That gives full read/inspect/modify power with none of the
// risks of inline hooking (no instruction relocation, no integrity-check surface,
// and the game's own code stays pristine).
//
// Anchors (verified against the decrypted image, RVAs relative to nioh.exe):
//   0x74DD09  [[char+0x40]+0x90]+0xC8 = 1   -> "this attack was guarded" (branch 1)
//   0x74DD92  same flag, the other guard branch inside the same resolver
//   0x74DE51  call 0x7B4C40 with xmm1 = guard Ki cost
//             entry = [[char+0x240]+0xBA8] + 8 + 7*0x50
//             cost  = [entry+0x10] * 0.2f  (0.2f is a real constant at 0x1588828)
//   0x7B4C40  float subtract primitive: [rcx+0x0C] -= xmm1, clamped at 0
//   0x755DC0  player slot accessor: get_player(idx), idx 0..3
//
// Everything is validated at load time against expected instruction bytes, so a
// mismatched build refuses to install rather than corrupting anything.
//
// Build:
//   zig cc -target x86_64-windows-gnu -shared -O2 -o Nioh1PerfectGuard.dll Nioh1PerfectGuard.c -lole32

#include <windows.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Pure decision logic, shared verbatim with tools/test_logic.c so the offline
// tests cannot drift away from what ships.
#include "pg_logic.h"

#define MOD_VERSION "0.1.1-nioh1"
#define MAX_TARGETS 4
#define MAX_ARMED 8192
#define LOG_CAP (MAX_PATH * 2)

// ------------------------------------------------------------------ state ---

static char g_dir[MAX_PATH] = {0};
static char g_ini_path[MAX_PATH * 2] = {0};
static char g_log_path[MAX_PATH * 2] = {0};
static unsigned long long g_base = 0;
static volatile LONG g_started = 0;
static HANDLE g_log_mutex = NULL;
static PVOID g_veh = NULL;
static DWORD g_armed[MAX_ARMED];
static volatile LONG g_armed_n = 0;
static int g_installed = 0;
static int g_debug_arm = 0;   // VerboseArm: log every arm pass
static int g_first_arm_done = 0;
static char g_status[256] = "not started";

static void log_line(const char *fmt, ...);

// counters are touched both by the exception handler and by the worker thread
static volatile LONG g_blocks;
static volatile LONG g_perfect;
static volatile LONG g_rewards;
static volatile LONG g_free;
static volatile LONG g_guard_presses;
static volatile LONG g_other_blocks;
static volatile LONG g_gate_closed;
// Event-source selection state (see on_guard_flag_event / effective_event_source).
static volatile LONG g_cost_events_seen;   // player-side cost events, gate aside
static volatile LONG g_flag_blocks;        // flag-side player blocks, gate aside
static int g_enemy_log = 0;
static int g_recovery_log = 0;
static unsigned long long g_last_ctx = 0;
static void apply_enemy_effects(void *ctx);
static void apply_cancel_recovery(void *player);
static void ki_snapshot(int from_flag);
static void ki_restore_tick(void);
static void ki_verdict(void);

// Pre-scale guard cost recorded at the subtract site, so the restore path can
// compare "what the engine was told to charge" against "what the visible field
// actually lost". Emitting that comparison is what settles, from one session,
// whether Refer::StaminaRate's field is the one a guard cost moves.
static float g_last_cost = -1.0f;
static long g_last_cost_block = 0;
static void on_guard_flag(int which, CONTEXT *c);
static void apply_ki_recovery(unsigned long long entry, float charge);
static void on_guard_flag_event(CONTEXT *c);
static void report_late_block(void);
static int effective_event_source(void);
static int reward_slot_free(void);
static void state_save(const char *what);
static void state_load(void);
static int g_costsite_dead;          // defined with the event-source state
static void perfect_guard_rewards(int from_flag);
static int entry_belongs_to_player(unsigned long long entry);
static HANDLE g_sound_event = NULL; // VEH signals this; the audio thread plays
static HANDLE g_cfg_mutex = NULL;   // serialises config_load (shared INI buffer)
static int g_no_arm = 0;            // DiagDisable=1: installed but deliberately unarmed
static unsigned long long now_ms(void);

// ----------------------------------------------------------------- config ---

typedef struct {
    int enabled;
    int window_ms;
    int cancel_recovery;
    float cancel_recovery_frames;
    // rewards
    int ki_reduction_percent;      // 0..100 scale applied to xmm1 at the cost site
    int ki_topup;                  // 1 = also top the visible Ki field back up
    // How far back the pre-event Ki reference looks (ms). The flag event source
    // fires *after* the engine charged the Ki, so a reference sampled at event time
    // already contains the charge and the top-up has nothing to hand back -- the
    // defect a user reported. With this window the reference comes from a rolling
    // 8ms sample ring instead (pg_ki_ref). 0 disables it (old behaviour).
    int ki_topup_preevent_ms;
    int ki_recovery_mode;          // 0 none, 1 refund the guard cost, 2 fixed
    float fixed_recovery;
    // Restoring the player's own HP (the "体力" reward). Modes mirror the Ki ones
    // but the amounts are integral HP, so the pure decision lives in pg_logic.h
    // (pg_hp_restore) where the clamping and refusal rules are unit tested.
    int hp_recovery_mode;          // 0 off, 1 percent of max, 2 fixed, 3 both
    float hp_restore_percent;      // percent of maximum HP
    float hp_restore_fixed;        // flat HP
    // Timed buffs granted by a perfect guard. These are the mod's first
    // *stateful* rewards: they must be applied to the engine, then removed when
    // the window closes (see the timed-buff section below).
    //   0 in the Percent/Ms field disables that buff; ArmorBuff is 0/1 because
    //   its effect ("no hit stun") has no magnitude.
    float speed_buff_percent;      // move speed +N%
    int speed_buff_ms;
    float damage_cut_percent;      // damage taken -N%
    int damage_cut_ms;
    int armor_buff;                // 0/1
    int armor_buff_ms;
    // The 99 gauge (精华量表 / 守护灵槽). Two independent switches sharing one write:
    // while the 99 state is inactive the gauge accumulates, while it is active the
    // same gauge is the burning timer, so adding to it extends the state. Both are
    // off by default; each has its own percentage so "10% per guard" can mean two
    // different things in the two phases.
    int lw_gauge_on;
    int lw_gauge_percent;
    int lw_gauge_max;              // full value of the 99 gauge (measured ~636)
    int lw_gauge_offset;           // byte offset of the real gauge store, 0 = not identified yet
    int lw_extend_on;
    int lw_extend_percent;
    // Guard cancels the player's attack action -- but only a *pure* guard press.
    // `attack_mask` says which buttons count as attacks (A/B/X/Y by default), and
    // the two windows encode the two ways a press can be a combination instead:
    //   `combo_ms`  -- an attack button going down this close to the guard press is
    //                  a guard+attack combination, not a cancel;
    //   `recent_ms` -- an attack must have been started this recently, otherwise we
    //                  do not touch the animation at all (so idle guarding and
    //                  holding guard are never disturbed).
    int cancel_action_on_guard;
    int attack_button_mask;
    int combo_guard_ms;
    int cancel_action_strict_hold;
    float cancel_action_frames;
    int cancel_action_recent_ms;
    // gate
    int gate_timely;               // 0 = every block counts, 1 = require a fresh press
    int guard_button_mask;         // XInput button bit; LB/L1 = 0x0100
    int pad_slot;                  // which controller slot to read
    int guard_key_vk;              // optional keyboard virtual-key code (0 = off)
    int learn_buttons;             // 1 = log newly seen pad bits and key codes
    int ki_trace;                  // 1 = log both Ki fields whenever either moves
    // Which event means "the player blocked".
    //   0 = the guard-cost site only: precise, and the only place the Ki cost can
    //       be scaled, but the engine skips that whole block unless the guard
    //       resource entry's state dword is 1 and [ctx+0x160] is 0.
    //   1 = the guard-flag site only: fires on a much broader condition and the
    //       player can be identified from the hit context, but the cost is not
    //       available there (so KiRecoveryMode 1/2 and the subtract-site scaling do
    //       not apply).
    //   2 = auto (default): start with the cost site, and if a few player blocks
    //       have been seen by the flag site while the cost site has never once
    //       fired, switch to the flag site and say so. In the attract-mode demo the
    //       cost site never fires at all, and that is not something the user should
    //       have to discover and configure by hand.
    int block_event_source;
    // Diagnostic switch, default 0 (everything on). A crashed session with no
    // breakpoint involvement still needs to be attributable, and the only way to
    // attribute it is to be able to switch the risky parts off one at a time:
    //   1 = do not arm any breakpoint at all
    //   2 = do not start the input polling thread
    //   4 = do not periodically re-arm already-armed threads
    //   8 = do not run the hardware-breakpoint self-test
    int diag_disable;
    // enemy effects (anchors pending)
    float enemy_ki_damage;
    float enemy_hp_damage;
    // feedback
    int sound_enabled;
    float sound_volume;
    char sound_file[128];
    int diagnostic_hotkey;
} Config;

static Config g_cfg;
static int g_cfg_loaded = 0;
static FILETIME g_ini_mtime = {0, 0};

static void config_defaults(Config *c) {
    memset(c, 0, sizeof(*c));
    c->enabled = 1;
    c->window_ms = 450;
    c->cancel_recovery = 0;        // unverified: off unless the player opts in
    c->cancel_recovery_frames = 30.0f;
    c->ki_reduction_percent = 100;
    c->ki_topup = 1;               // also write the visible Ki field (see the INI)
    // 100ms: the charge and the guard flag land in the same frame, while a spend
    // from an earlier attack is 250ms+ away in practice (see pg_ki_ref).
    c->ki_topup_preevent_ms = 100;
    c->ki_recovery_mode = 3;       // Balanced: one sixth of maximum Ki
    c->fixed_recovery = 50.0f;
    // HP restore is on by default because it was asked for as a default: 3% of
    // maximum HP per perfect guard. HpRestoreFixed is the flat alternative (50)
    // and is only used by mode 2 or 3, so the shipped default heals 3%, not 53.
    c->hp_recovery_mode = 1;       // 1 = percent of maximum
    c->hp_restore_percent = 3.0f;
    c->hp_restore_fixed = 50.0f;
    // The buffs the user asked for: speed +4% for 10s and damage taken -4% for
    // 10s, armour 5s but off.
    //
    // The magnitudes are shipped as 0 -- i.e. OFF -- until this build has been
    // seen to work at least once, because these two are the only rewards that make
    // the mod *call game code*, and that call has never been executed anywhere yet:
    //  * the constructors allocate through a process-global manager and insert
    //    into a container the game thread is walking; whether that manager's
    //    allocator is safe to touch from a second thread is NOT established, and
    //    the failure mode is silent heap corruption, not a clean crash;
    //  * the local test instance on this machine could not be closed (Access
    //    denied), so the attract-mode experiment has not run.
    // Setting the two numbers to 4 in the INI is the whole opt-in. This is the same
    // treatment CancelRecovery got: unverified => off until proven.
    c->speed_buff_percent = 0.0f;
    c->speed_buff_ms = 10000;
    c->damage_cut_percent = 0.0f;
    c->damage_cut_ms = 10000;
    c->armor_buff = 0;

    c->armor_buff_ms = 5000;
    // The 99 gauge: ON by default, 10% per perfect guard in each phase. This is now a
    // direct write to the measured field ([param+0x48], maximum shared with Ki at
    // [param+0x44]) -- no game code is called at all, so there is nothing left to verify
    // in the engine. See CHANGELOG 2.0j for the measurement that identified the field.
    c->lw_gauge_on = 1;
    c->lw_gauge_percent = 10;
    // Measured: one small spirit stone adds 212 and three fill the bar, so the full
    // value is ~636 -- NOT the Ki maximum at param+0x44, which is what an earlier
    // version clamped to (that is why every write was a no-op).
    c->lw_gauge_max = 0;          // 0 = take the maximum from the field next to it
    // [param+0x48] turned out to be a value the engine rewrites every frame (LWD2 proved
    // it: wrote 260.6, read back 197), so the mod must not write it. Until the real
    // store is identified this is 0 = disabled; the offset can then be set from the INI
    // without a new build.
    c->lw_gauge_offset = 0xC0;    // int counter inside the amrita/99 cluster
    c->lw_extend_on = 1;
    c->lw_extend_percent = 10;
    // Guard-cancels-attack is ON by default: it was asked for as a default feature,
    // and unlike the timed buffs it needs no engine calls -- it only advances the
    // current action's motion frame, the same kind of write this mod already makes.
    c->cancel_action_on_guard = 1;
    c->attack_button_mask = 0xF000;   // standard XInput A(0x1000) B(0x2000) X(0x4000) Y(0x8000)
    c->combo_guard_ms = 100;          // guard+attack within this window = combination
    c->cancel_action_strict_hold = 0; // 1 = any held attack button blocks the cancel
    c->cancel_action_frames = 30.0f;  // frames to advance past the rest of the action
    c->cancel_action_recent_ms = 0; // 0 = no gate: ANY guard press cancels
    // These must match the values shipped in Nioh1PerfectGuard.ini: if the INI is
    // missing or unreadable the mod runs on these, and a gate_timely of 0 would
    // silently turn *every* block into a perfect guard.
    c->gate_timely = 1;
    c->guard_button_mask = 0x0100;   // XINPUT_GAMEPAD_LEFT_SHOULDER (L1 / LB)
    c->pad_slot = 0;
    c->guard_key_vk = 0;
    c->learn_buttons = 0;
    c->ki_trace = 1;               // temporary: still resolving which field is Ki
    c->diag_disable = 0;           // 0 = everything on (see the struct comment)
    c->block_event_source = 2;     // 2 = auto (see the struct comment)
    c->enemy_ki_damage = 0.0f;
    c->enemy_hp_damage = 0.0f;
    c->sound_enabled = 1;
    c->sound_volume = 1.0f;
    lstrcpyA(c->sound_file, "parry.wav");
    c->diagnostic_hotkey = 1;
}

// --------------------------------------------------------------- ini text ---
// Read and normalise the INI ourselves rather than using GetPrivateProfile*.
// Those APIs only understand ANSI or UTF-16LE-with-BOM; a file saved as UTF-8
// (with or without BOM) - which any modern editor will do, and which the
// bilingual comments invite - would be read incorrectly or silently ignored.
// Handling the encodings explicitly makes the config robust.
static char *g_ini_text = NULL;

static int ini_load_text(void) {
    if (g_ini_text) { free(g_ini_text); g_ini_text = NULL; }
    HANDLE f = CreateFileA(g_ini_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD size = GetFileSize(f, NULL);
    if (size == 0 || size > 4 * 1024 * 1024) { CloseHandle(f); return 0; }
    char *raw = (char *)malloc(size + 2);
    DWORD got = 0;
    if (!raw || !ReadFile(f, raw, size, &got, NULL) || got == 0) {
        if (raw) free(raw);
        CloseHandle(f);
        return 0;
    }
    CloseHandle(f);
    raw[got] = 0;
    raw[got + 1] = 0;

    const unsigned char *u = (const unsigned char *)raw;
    if (got >= 2 && u[0] == 0xFF && u[1] == 0xFE) {
        // UTF-16LE -> ANSI
        int wlen = (int)((got - 2) / 2);
        int need = WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)(raw + 2), wlen,
                                       NULL, 0, NULL, NULL);
        char *conv = (char *)malloc((size_t)need + 2);
        if (!conv) { free(raw); return 0; }
        WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)(raw + 2), wlen, conv, need, NULL, NULL);
        conv[need] = 0;
        free(raw);
        raw = conv;
    } else if (got >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) {
        memmove(raw, raw + 3, (size_t)got - 3);
        raw[got - 3] = 0;
    }
    g_ini_text = raw;
    return 1;
}

// Look up `key` inside the [PerfectGuard] section, ignoring comments.
static int ini_get(const char *key, char *out, size_t cap) {
    out[0] = 0;
    if (!g_ini_text) return 0;
    size_t klen = strlen(key);
    int in_section = 0;
    const char *p = g_ini_text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (!*p) break;
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        while (len && (p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t'))
            --len;
        if (*p == ';' || *p == '#') {
            /* comment */
        } else if (*p == '[') {
            in_section = (len >= 13 && _strnicmp(p, "[PerfectGuard]", 13) == 0);
        } else if (in_section && len > klen && _strnicmp(p, key, klen) == 0 &&
                   p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t vlen = len - klen - 1;
            while (vlen && (*v == ' ' || *v == '\t')) { ++v; --vlen; }
            while (vlen && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t')) --vlen;
            if (vlen >= cap) vlen = cap - 1;
            memcpy(out, v, vlen);
            out[vlen] = 0;
            return 1;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}

static int ini_int(const char *key, int def, int lo, int hi, int *ok) {
    char buf[64];
    if (!ini_get(key, buf, sizeof(buf))) return def;
    int v = (int)strtol(buf, NULL, 0);
    if (v < lo || v > hi) {
        log_line("CONFIG reject %s=%s (allowed %d..%d); keeping %d", key, buf, lo, hi, def);
        *ok = 0;
        return def;
    }
    return v;
}

static float ini_float(const char *key, float def, float lo, float hi, int *ok) {
    char buf[64];
    if (!ini_get(key, buf, sizeof(buf)) || !buf[0]) return def;
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf || v < lo || v > hi) {
        log_line("CONFIG reject %s=%s (allowed %g..%g); keeping %g", key, buf, lo, hi, def);
        *ok = 0;
        return def;
    }
    return (float)v;
}

// NOTE: `def` may point at `out` (that is how the only caller uses it). The
// lookup must therefore go through a local buffer -- writing straight into `out`
// destroys the default before it can be copied, which silently turned an absent
// SoundFile key into an empty filename and disabled the guard sound.
static void ini_str(const char *key, char *out, size_t cap, const char *def) {
    char tmp[512];
    if (!ini_get(key, tmp, sizeof(tmp)) || !tmp[0]) {
        lstrcpynA(out, def, (int)cap);
        return;
    }
    lstrcpynA(out, tmp, (int)cap);
}

// Returns 0 on success. Gameplay values are group-validated: if any of the core
// gameplay keys is invalid the whole group rolls back to the previous config,
// matching how the Nioh 2 mod behaved. The rollback falls out of never assigning
// `g_cfg` on rejection, so the baseline here is the *current* config: keys absent
// from the INI keep their current values.
//
// (An earlier version baselined on g_cfg_prev, i.e. the generation *before* the
// live one. A missing or half-written INI -- editors truncate before they write --
// then silently rolled the config back two generations instead of leaving it
// alone.)
static int config_load_inner(int first_time) {
    Config c = g_cfg;
    if (!g_cfg_loaded) config_defaults(&c);

    int loaded = ini_load_text();
    if (!loaded && !g_cfg_loaded) {
        log_line("CONFIG: INI unavailable (%s); using defaults", g_ini_path);
    }

    int ok = 1;
    c.enabled = ini_int("Enabled", c.enabled, 0, 1, &ok);
    c.window_ms = ini_int("WindowMs", c.window_ms, 1, 1000, &ok);
    c.cancel_recovery = ini_int("CancelRecovery", c.cancel_recovery, 0, 1, &ok);
    c.cancel_recovery_frames = ini_float("CancelRecoveryFrames",
                                         c.cancel_recovery_frames, 0.0f, 1000.0f, &ok);
    c.ki_reduction_percent = ini_int("KiDamageReductionPercent",
                                     c.ki_reduction_percent, 0, 100, &ok);
    c.ki_topup = ini_int("KiTopUp", c.ki_topup, 0, 1, &ok);
    c.ki_topup_preevent_ms = ini_int("KiTopUpPreEventMs", c.ki_topup_preevent_ms,
                                     0, PG_KI_REF_MAX_SAMPLES * 8 - 100, &ok);
    c.ki_recovery_mode = ini_int("KiRecoveryMode", c.ki_recovery_mode, 0, 3, &ok);
    c.fixed_recovery = ini_float("FixedRecovery", c.fixed_recovery, 0.0f, 100000.0f, &ok);
    c.hp_recovery_mode = ini_int("HpRecoveryMode", c.hp_recovery_mode, 0, 3, &ok);
    c.hp_restore_percent = ini_float("HpRestorePercent", c.hp_restore_percent,
                                     0.0f, 1000.0f, &ok);
    c.hp_restore_fixed = ini_float("HpRestoreFixed", c.hp_restore_fixed,
                                   0.0f, 100000.0f, &ok);
    c.speed_buff_percent = ini_float("SpeedBuffPercent", c.speed_buff_percent,
                                     0.0f, 1000.0f, &ok);
    c.speed_buff_ms = ini_int("SpeedBuffMs", c.speed_buff_ms, 0, 600000, &ok);
    c.damage_cut_percent = ini_float("DamageCutPercent", c.damage_cut_percent,
                                     0.0f, 100.0f, &ok);
    c.damage_cut_ms = ini_int("DamageCutMs", c.damage_cut_ms, 0, 600000, &ok);
    c.armor_buff = ini_int("ArmorBuff", c.armor_buff, 0, 1, &ok);
    c.armor_buff_ms = ini_int("ArmorBuffMs", c.armor_buff_ms, 0, 600000, &ok);
    c.lw_gauge_on = ini_int("LivingWeaponGaugeOnGuard", c.lw_gauge_on, 0, 1, &ok);
    c.lw_gauge_percent = ini_int("LivingWeaponGaugePercent", c.lw_gauge_percent,
                                 0, 100, &ok);
    c.lw_gauge_max = ini_int("LivingWeaponGaugeMax", c.lw_gauge_max, 0, 100000, &ok);
    c.lw_gauge_offset = ini_int("LivingWeaponGaugeOffset", c.lw_gauge_offset, 0, 0x4000, &ok);
    c.lw_extend_on = ini_int("LivingWeaponExtendOnGuard", c.lw_extend_on, 0, 1, &ok);
    c.lw_extend_percent = ini_int("LivingWeaponExtendPercent", c.lw_extend_percent,
                                  0, 100, &ok);
    c.cancel_action_on_guard = ini_int("CancelActionOnGuard", c.cancel_action_on_guard, 0, 1, &ok);
    c.attack_button_mask = ini_int("AttackButtonMask", c.attack_button_mask, 1, 0xFFFF, &ok);
    c.combo_guard_ms = ini_int("ComboGuardWindowMs", c.combo_guard_ms, 0, 1000, &ok);
    c.cancel_action_strict_hold =
        ini_int("CancelActionStrictHold", c.cancel_action_strict_hold, 0, 1, &ok);
    c.cancel_action_frames =
        ini_float("CancelActionFrames", c.cancel_action_frames, 0.0f, 1000.0f, &ok);
    c.cancel_action_recent_ms =
        ini_int("CancelActionRecentMs", c.cancel_action_recent_ms, 0, 10000, &ok);
    c.gate_timely = ini_int("RequireTimelyGuard", c.gate_timely, 0, 1, &ok);
    c.guard_button_mask = ini_int("GuardButtonMask", c.guard_button_mask, 1, 0xFFFF, &ok);
    c.pad_slot = ini_int("PadSlot", c.pad_slot, 0, 3, &ok);
    c.guard_key_vk = ini_int("GuardKeyVK", c.guard_key_vk, 0, 255, &ok);
    c.learn_buttons = ini_int("LearnButtons", c.learn_buttons, 0, 1, &ok);
    c.ki_trace = ini_int("KiTrace", c.ki_trace, 0, 1, &ok);
    c.diag_disable = ini_int("DiagDisable", c.diag_disable, 0, 15, &ok);
    c.block_event_source = ini_int("BlockEventSource", c.block_event_source, 0, 2, &ok);
    c.enemy_ki_damage = ini_float("EnemyKiDamage", c.enemy_ki_damage, 0.0f, 100000.0f, &ok);
    c.enemy_hp_damage = ini_float("EnemyHpDamage", c.enemy_hp_damage, 0.0f, 100000.0f, &ok);

    int sok = 1;
    c.sound_enabled = ini_int("SoundEnabled", c.sound_enabled, 0, 1, &sok);
    c.sound_volume = ini_float("SoundVolume", c.sound_volume, 0.0f, 1.0f, &sok);
    ini_str("SoundFile", c.sound_file, sizeof(c.sound_file), c.sound_file);
    c.diagnostic_hotkey = ini_int("DiagnosticHotkey", c.diagnostic_hotkey, 0, 1, &sok);

    if (!ok) {
        log_line("CONFIG gameplay group rejected; previous settings kept");
        return 1;
    }
    // Installation is a one-shot decision made at startup, so flipping Enabled at
    // run time cannot take effect. Say so instead of silently doing nothing.
    if (g_cfg_loaded && c.enabled != g_cfg.enabled && !first_time) {
        log_line("NOTICE: Enabled changed to %d, but installing or removing the "
                 "breakpoints happens only at startup. Restart the game for this key "
                 "to take effect; every other key applies immediately.", c.enabled);
    }
    g_cfg = c;
    if (!first_time) {
        log_line("CONFIG reloaded: enabled=%d window=%dms reduction=%d%% topup=%d "
                 "recovery=%d gate_timely=%d sound=%d vol=%.2f file=%s "
                 "hpmode=%d hppct=%g hpfixed=%g",
                 c.enabled, c.window_ms, c.ki_reduction_percent, c.ki_topup,
                 c.ki_recovery_mode,
                 c.gate_timely, c.sound_enabled, c.sound_volume, c.sound_file,
                 c.hp_recovery_mode, (double)c.hp_restore_percent,
                 (double)c.hp_restore_fixed);
    }
    g_cfg_loaded = 1;
    return 0;
}

// ini_load_text parses into one shared heap buffer, so two concurrent loads would
// free each other's memory. The input thread reloads on a file change and
// PG_ReloadConfig can be called by a harness at any moment, so serialize.
static int config_load(int first_time) {
    if (g_cfg_mutex) WaitForSingleObject(g_cfg_mutex, 2000);
    int rc = config_load_inner(first_time);
    if (g_cfg_mutex) ReleaseMutex(g_cfg_mutex);
    return rc;
}

static void ini_poll(void) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(g_ini_path, GetFileExInfoStandard, &fad)) return;
    if (fad.ftLastWriteTime.dwLowDateTime == g_ini_mtime.dwLowDateTime &&
        fad.ftLastWriteTime.dwHighDateTime == g_ini_mtime.dwHighDateTime) {
        return;
    }
    g_ini_mtime = fad.ftLastWriteTime;
    if (!g_cfg_loaded) return;
    config_load(0);
}
// ------------------------------------------------------------------- log ----

static void log_line(const char *fmt, ...) {
    if (!g_log_path[0]) return;
    if (g_log_mutex) WaitForSingleObject(g_log_mutex, 2000);
    HANDLE f = CreateFileA(g_log_path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char line[1200];
        int n = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u ",
                         st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        int m = vsnprintf(line + n, sizeof(line) - n, fmt, ap);
        va_end(ap);
        if (m < 0) m = 0;
        n += m;
        if (n > (int)sizeof(line) - 3) n = (int)sizeof(line) - 3;
        line[n++] = '\r';
        line[n++] = '\n';
        DWORD w = 0;
        WriteFile(f, line, (DWORD)n, &w, NULL);
        CloseHandle(f);
    }
    if (g_log_mutex) ReleaseMutex(g_log_mutex);
}

// ----------------------------------------------------------------- audio ----
// XAudio2 2.9 via its exported factory (xaudio2_9.dll) and hand-declared COM
// vtables, so no import library and no C++ runtime is required.

typedef struct IUnknownVt { void *qi, *addref, *release; } IUnknownVt;

// IXAudio2 vtbl (verified against mingw's xaudio2.h):
//   0-2 IUnknown, 3 RegisterForCallbacks, 4 UnregisterForCallbacks,
//   5 CreateSourceVoice, 6 CreateSubmixVoice, 7 CreateMasteringVoice,
//   8 StartEngine, 9 StopEngine, 10 CommitChanges, 11 GetPerformanceData,
//   12 SetDebugConfiguration
typedef struct {
    void *qi, *addref, *release;
    void *RegisterForCallbacks, *UnregisterForCallbacks;
    HRESULT (WINAPI *CreateSourceVoice)(void *self, void **ppVoice,
                                       const WAVEFORMATEX *fmt, UINT32 flags,
                                       float maxFreq, void *cb, const void *sends,
                                       const void *chain);
    void *CreateSubmixVoice;
    HRESULT (WINAPI *CreateMasteringVoice)(void *self, void **ppVoice,
                                          UINT32 inChannels, UINT32 inRate,
                                          UINT32 flags, LPCWSTR deviceId,
                                          const void *chain, UINT32 category);
    HRESULT (WINAPI *StartEngine)(void *self);
    void (WINAPI *StopEngine)(void *self);
    void *CommitChanges, *GetPerformanceData, *SetDebugConfiguration;
} IXAudio2Vt;

// IXAudio2Voice base: 0 GetVoiceDetails ... 12 SetVolume ... 18 DestroyVoice
// IXAudio2SourceVoice adds: 19 Start, 20 Stop, 21 SubmitSourceBuffer, ...

typedef struct {
    // voice base slots 0..18
    void *GetVoiceDetails, *SetOutputVoices, *SetEffectChain;
    void *EnableEffect, *DisableEffect, *GetEffectState;
    void *SetEffectParameters, *GetEffectParameters;
    void *SetFilterParameters, *GetFilterParameters;
    void *SetOutputFilterParameters, *GetOutputFilterParameters;
    HRESULT (WINAPI *SetVolume)(void *self, float volume, UINT32 opSet);
    void *GetVolume, *SetChannelVolumes, *GetChannelVolumes;
    void *SetOutputMatrix, *GetOutputMatrix;
    void (WINAPI *DestroyVoice)(void *self);
    // source voice slots 19..
    HRESULT (WINAPI *Start)(void *self, UINT32 flags, UINT32 opSet);
    HRESULT (WINAPI *Stop)(void *self, UINT32 flags, UINT32 opSet);
    HRESULT (WINAPI *SubmitSourceBuffer)(void *self, const void *buf,
                                         const void *wma);
    HRESULT (WINAPI *FlushSourceBuffers)(void *self);
} SourceVoiceVt;

typedef struct {
    UINT32 Flags, AudioBytes;
    const BYTE *pAudioData;
    UINT32 PlayBegin, PlayLength, LoopBegin, LoopLength, LoopCount;
    void *pContext;
} XAUDIO2_BUFFER_LOCAL;

static HMODULE g_xaudio = NULL;
static void *g_xa = NULL;             // IXAudio2*
static void *g_master = NULL;         // mastering voice
static void *g_voice = NULL;          // source voice
static BYTE *g_wav = NULL;
static DWORD g_wav_len = 0;
static WAVEFORMATEX g_wav_fmt;
static int g_audio_ok = 0;

static int wav_load(const char *path) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        log_line("SOUND disabled: cannot open %s", path);
        return 0;
    }
    DWORD size = GetFileSize(f, NULL);
    if (size < 44 || size > 8 * 1024 * 1024) {
        log_line("SOUND disabled: WAV size %lu out of range", size);
        CloseHandle(f);
        return 0;
    }
    BYTE *buf = (BYTE *)malloc(size);
    DWORD got = 0;
    if (!buf || !ReadFile(f, buf, size, &got, NULL) || got != size) {
        log_line("SOUND disabled: cannot read complete WAV");
        if (buf) free(buf);
        CloseHandle(f);
        return 0;
    }
    CloseHandle(f);

    if (memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        log_line("SOUND disabled: expected RIFF WAVE");
        free(buf);
        return 0;
    }
    // walk chunks for fmt and data
    DWORD off = 12;
    int have_fmt = 0;
    BYTE *data = NULL;
    DWORD data_len = 0;
    while (off + 8 <= size) {
        const BYTE *id = buf + off;
        DWORD len = *(DWORD *)(buf + off + 4);
        if (off + 8 + len > size) break;
        if (!memcmp(id, "fmt ", 4) && len >= 16) {
            memcpy(&g_wav_fmt, buf + off + 8, 16);
            if (g_wav_fmt.wFormatTag != 1) {
                log_line("SOUND disabled: use 16-bit integer PCM WAV (format 1)");
                free(buf);
                return 0;
            }
            have_fmt = 1;
        } else if (!memcmp(id, "data", 4)) {
            data = buf + off + 8;
            data_len = len;
        }
        off += 8 + len + (len & 1);
    }
    if (!have_fmt || !data || !data_len) {
        log_line("SOUND disabled: WAV missing fmt or data chunk");
        free(buf);
        return 0;
    }
    if (g_wav_fmt.wBitsPerSample != 16) {
        log_line("SOUND disabled: only 16-bit PCM supported (got %u)",
                 g_wav_fmt.wBitsPerSample);
        free(buf);
        return 0;
    }
    if (g_wav_fmt.nSamplesPerSec != 44100 && g_wav_fmt.nSamplesPerSec != 48000) {
        log_line("SOUND disabled: need 44100 or 48000 Hz (got %lu)",
                 (unsigned long)g_wav_fmt.nSamplesPerSec);
        free(buf);
        return 0;
    }
    if (g_wav) free(g_wav);
    g_wav = buf;
    g_wav_len = data_len;
    g_wav_fmt.cbSize = 0;
    memmove(g_wav, data, data_len);   // compact so pAudioData = g_wav
    log_line("SOUND loaded: %lu bytes, %lu Hz, %u channels, 16-bit",
             (unsigned long)data_len, (unsigned long)g_wav_fmt.nSamplesPerSec,
             g_wav_fmt.nChannels);
    return 1;
}

typedef HRESULT (WINAPI *pfnXAudio2Create)(void **ppXAudio2, UINT32 flags,
                                           UINT32 processor);

static int audio_init(void) {
    if (g_audio_ok) return 1;     // already ready; never rebuild the engine
    // XAudio2Create needs COM initialised on the calling thread. The dedicated
    // audio thread does that, so the game's apartment model stays untouched.
    HRESULT ci = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (ci != S_OK && ci != S_FALSE && ci != RPC_E_CHANGED_MODE) {
        log_line("SOUND disabled: CoInitializeEx hr=0x%08lX", (unsigned long)ci);
        return 0;
    }
    log_line("SOUND CoInitializeEx hr=0x%08lX", (unsigned long)ci);

    const char *names[] = {"xaudio2_9.dll", "xaudio2_9redist.dll"};
    pfnXAudio2Create create = NULL;
    for (int i = 0; i < 2 && !create; ++i) {
        g_xaudio = LoadLibraryA(names[i]);
        if (g_xaudio) {
            create = (pfnXAudio2Create)(void *)GetProcAddress(g_xaudio, "XAudio2Create");
            if (!create) { FreeLibrary(g_xaudio); g_xaudio = NULL; }
        }
    }
    if (!create) {
        log_line("SOUND disabled: xaudio2_9.dll / XAudio2Create unavailable");
        return 0;
    }
    HRESULT hr = create(&g_xa, 0, 0 /* XAUDIO2_DEFAULT_PROCESSOR */);
    if (FAILED(hr) || !g_xa) {
        log_line("SOUND disabled: XAudio2Create hr=0x%08lX", (unsigned long)hr);
        return 0;
    }
    IXAudio2Vt *vt = *(IXAudio2Vt **)g_xa;
    hr = vt->CreateMasteringVoice(g_xa, &g_master, 2, 48000, 0, NULL, NULL, 0);
    if (FAILED(hr) || !g_master) {
        log_line("SOUND disabled: CreateMasteringVoice hr=0x%08lX", (unsigned long)hr);
        return 0;
    }
    hr = vt->StartEngine(g_xa);
    if (FAILED(hr)) {
        log_line("SOUND disabled: StartEngine hr=0x%08lX", (unsigned long)hr);
        return 0;
    }
    hr = vt->CreateSourceVoice(g_xa, &g_voice, &g_wav_fmt, 0, 1.0f, NULL, NULL, NULL);
    if (FAILED(hr) || !g_voice) {
        log_line("SOUND disabled: CreateSourceVoice hr=0x%08lX", (unsigned long)hr);
        return 0;
    }
    SourceVoiceVt *sv = *(SourceVoiceVt **)g_voice;
    sv->SetVolume(g_voice, g_cfg.sound_volume, 0);
    g_audio_ok = 1;
    log_line("SOUND ready (XAudio2 engine=%s)", names[0]);
    return 1;
}

static void audio_play(void) {
    if (!g_audio_ok || !g_voice || !g_wav) return;
    SourceVoiceVt *sv = *(SourceVoiceVt **)g_voice;
    sv->Stop(g_voice, 0, 0);
    sv->FlushSourceBuffers(g_voice);
    sv->SetVolume(g_voice, g_cfg.sound_volume, 0);
    XAUDIO2_BUFFER_LOCAL b;
    memset(&b, 0, sizeof(b));
    b.AudioBytes = g_wav_len;
    b.pAudioData = g_wav;
    if (FAILED(sv->SubmitSourceBuffer(g_voice, &b, NULL))) {
        log_line("SOUND submit failed");
        return;
    }
    sv->Start(g_voice, 0, 0);
}

// ------------------------------------------------------- breakpoint engine --

typedef struct {
    const char *name;
    unsigned long long rva;
    const unsigned char *expect;
    int expect_len;
    int enabled;
    int armable;      // 0 = validate the bytes only, never take a DR slot
} Anchor;

static const unsigned char BYTES_FLAG1[7] = {0xC6,0x81,0xC8,0x00,0x00,0x00,0x01};
static const unsigned char BYTES_FLAG2[7] = {0xC6,0x81,0xC8,0x00,0x00,0x00,0x01};
static const unsigned char BYTES_COST[5]  = {0xE8,0xEA,0x6D,0x06,0x00};
// Verified spare, deliberately not wired to an anchor: the float subtract
// primitive the guard cost flows into (`movss xmm2,[rcx+0xC]`). The cost anchor
// covers the same logic at an earlier point, so this is only the fallback if a
// future build moves the cost site. tools/test_anchors.py still pins its bytes
// against the image, so the address stays verified instead of becoming folklore.
static const unsigned char BYTES_SUB[6]   = {0xF3,0x0F,0x10,0x51,0x0C,0xF3};
// mov rax, qword ptr [rip + disp32] ; ret   -- the input-manager singleton getter.
// The disp32 encodes which global holds the pointer, so the mod derives the
// address from the instruction instead of hardcoding it.
static const unsigned char BYTES_IMGET[7] = {0x48,0x8B,0x05,0xC1,0xC7,0xD0,0x00};

static Anchor g_anchor[MAX_TARGETS] = {
    {"guard_flag_1",     0x74DD09, BYTES_FLAG1, 7, 0, 1},
    {"guard_flag_2",     0x74DD92, BYTES_FLAG2, 7, 0, 1},
    {"guard_ki_cost",    0x74DE51, BYTES_COST,  5, 0, 1},
    // The input-manager getter is called on almost every frame. It only needs its
    // bytes checked once so we can derive the singleton address from them;
    // trapping it would flood the exception handler and stall the game.
    {"inputmgr_getter",  0xE6BE90, BYTES_IMGET, 7, 0, 0},
};
static int g_anchor_count = 4;
static int g_valid_count = 0;

// Address of the pointer slot that holds the input manager, derived from the
// getter's own instruction bytes.
static unsigned long long g_input_mgr_slot = 0;
static unsigned long long g_input_mgr_va = 0;
static unsigned short g_last_buttons = 0;
static PgGuardInput g_guard_in;                    // the gate state (shared logic)
static unsigned short g_seen_pad_bits = 0;
static unsigned char g_seen_vk[256] = {0};
static int g_vk_sweep = 0;
// Trigger bytes of the same per-pad record. They sit *outside* the 16-bit button
// word (proven: engine index 14 tests byte[padstate+6], index 15 tests
// byte[padstate+7]), so GuardButtonMask can never match them. Reading them is
// therefore purely diagnostic -- it explains "I am pressing my guard button and
// nothing happens" instead of leaving the user to guess.
static unsigned char g_trig_lt = 0;
static unsigned char g_trig_rt = 0;
static int g_trigger_notice = 0;

static int anchor_verify(void) {
    int valid = 0;
    for (int i = 0; i < g_anchor_count; ++i) {
        Anchor *a = &g_anchor[i];
        const unsigned char *p = (const unsigned char *)(ULONG_PTR)(g_base + a->rva);
        if (memcmp(p, a->expect, a->expect_len) == 0) {
            a->enabled = 1;
            valid++;
            log_line("ANCHOR ok   %-18s rva=0x%llX", a->name, a->rva);
        } else {
            a->enabled = 0;
            char got[3 * 16 + 1];
            char want[3 * 16 + 1];
            int n = 0, m = 0;
            for (int k = 0; k < a->expect_len && k < 16; ++k) {
                n += snprintf(got + n, sizeof(got) - n, "%02X", p[k]);
                m += snprintf(want + m, sizeof(want) - m, "%02X", a->expect[k]);
            }
            log_line("ANCHOR MISS %-18s rva=0x%llX want=[%s] got=[%s]",
                     a->name, a->rva, want, got);
        }
    }
    // Derive the input-manager pointer slot from the getter's own instruction
    // bytes: `mov rax, [rip + disp32]` -> target = rva + 7 + disp32.
    //
    // Looked up by name rather than by index: a hardcoded g_anchor[3] would keep
    // compiling if the table were ever reordered, and would then silently derive
    // the slot from the wrong anchor, leaving every press undetected.
    const Anchor *getter = NULL;
    for (int i = 0; i < g_anchor_count; ++i)
        if (g_anchor[i].enabled && strcmp(g_anchor[i].name, "inputmgr_getter") == 0)
            getter = &g_anchor[i];
    if (!getter) {
        log_line("INPUT manager getter unavailable; guard presses cannot be detected");
    } else if (getter->expect_len >= 7) {
        int disp = (int)((unsigned int)getter->expect[3] |
                         ((unsigned int)getter->expect[4] << 8) |
                         ((unsigned int)getter->expect[5] << 16) |
                         ((unsigned int)getter->expect[6] << 24));
        unsigned long long next = getter->rva + 7;
        g_input_mgr_slot = g_base + next + disp;
        log_line("INPUT manager slot = 0x%llX (from getter 0x%llX + disp 0x%X)",
                 g_input_mgr_slot, getter->rva, disp);
    } else {
        log_line("INPUT manager getter has only %d expected bytes; need 7 to decode "
                 "the displacement", getter->expect_len);
    }
    log_line("ANCHOR %d/%d verified", valid, g_anchor_count);
    return valid;
}

// Poll the raw pad button mask, plus an optional keyboard key.
//
// Everything here is a pure read (ReadProcessMemory / GetAsyncKeyState): no game
// code is called, so nothing can fault or disturb the game. This matters because
// we do not know in advance whether the player is on a controller or on
// keyboard+mouse, and the guard binding differs per device.
// Attack-button edge tracking and the guard-cancel counters. These live here (with
// the input polling) because poll_guard_button needs them; the cancel itself -- which
// writes the motion frame -- sits further down with the rest of the reward code.
static PgGuardInput g_attack_in;
static LONG g_cancel_log = 0;
static LONG g_cancel_count = 0, g_cancel_skipped = 0;
static void apply_action_cancel(void);
static int read_action_id(void *player);
static void report_cancel_skipped(const char *why, unsigned short pad,
                                 int attack_down, unsigned long long at_press);
static PgCancelGate g_cancel_gate;
// Follow-up: what the action became shortly after a cancel. Advancing the motion
// frame only *ends* the action; whether the character then actually guards is the
// engine's decision (it depends on whether the engine buffered the press), and that
// is the difference between "the mod did something" and "the feature works". So the
// action id is sampled for a moment afterwards and the transition is logged.
static int g_cancel_action_before = -1;
static unsigned long long g_cancel_followup_until = 0;
static int g_cancel_followup_done = 0;
static int g_cancel_followup_before = -1;

static void poll_guard_button(void) {
    if (!g_input_mgr_slot) return;
    void *mgr = *(void **)(ULONG_PTR)g_input_mgr_slot;
    if (!mgr) return;
    g_input_mgr_va = (unsigned long long)(ULONG_PTR)mgr;
    unsigned short buttons = 0;
    SIZE_T got = 0;
    unsigned long long at = (unsigned long long)(ULONG_PTR)mgr + 0x49E0C +
                            (unsigned long long)g_cfg.pad_slot * 20;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(ULONG_PTR)at, &buttons,
                           2, &got) || got != 2) {
        return;
    }
    unsigned short mask = (unsigned short)g_cfg.guard_button_mask;
    int key_down = 0;
    if (g_cfg.guard_key_vk > 0) {
        key_down = (GetAsyncKeyState(g_cfg.guard_key_vk) & 0x8000) != 0;
    }
    int down = pg_down(buttons, mask, key_down);

    // Triggers live in the two bytes just above the button word (the engine's own
    // index-14/15 tests read byte[padstate+6] and byte[padstate+7], and
    // padstate+4 is the word we just read).
    unsigned char trig[2] = {0, 0};
    ReadProcessMemory(GetCurrentProcess(),
                      (LPCVOID)(ULONG_PTR)(at + 2), trig, 2, &got);
    g_trig_lt = trig[0];
    g_trig_rt = trig[1];
    if (!g_trigger_notice && g_guard_presses == 0 && (trig[0] > 128 || trig[1] > 128)) {
        log_line("NOTICE: a trigger is being squeezed (LT=%u RT=%u) but GuardButtonMask "
                 "(0x%04X) only matches the 14 button bits, never the triggers. If that "
                 "is your guard button, bind something else to guard in game, or use "
                 "GuardKeyVK for the keyboard.", trig[0], trig[1],
                 g_cfg.guard_button_mask);
        g_trigger_notice = 1;
    }

    unsigned long long at_press = now_ms();
    // Track the attack buttons' own edges: the cancel test needs to know whether an
    // attack press happened *together* with the guard press (a combination), and it
    // also needs to notice one that arrives a few ms *after* it -- that is how a
    // martial skill (guard+attack, or an attack derived into one) is usually entered.
    int attack_down = ((buttons & (unsigned short)g_cfg.attack_button_mask) != 0);
    int attack_fresh = pg_guard_update(&g_attack_in, attack_down, at_press);
    if (pg_guard_update(&g_guard_in, down, at_press)) {
        if (g_blocks < 60) {
            log_line("GUARD pressed (pad=0x%04X mask=0x%04X key=0x%02X key_down=%d)",
                     buttons, mask, g_cfg.guard_key_vk, key_down);
        }
        InterlockedIncrement(&g_guard_presses);   // display/diagnostic mirror only
        // A guard press, with no attack button seen in the window *before* it. It is
        // not a cancel yet: the gate waits a moment to see whether an attack press
        // follows (a skill input), and only then writes anything.
        if (g_cfg.cancel_action_on_guard) {
            int alone = pg_guard_alone(1, &g_attack_in, at_press, g_cfg.combo_guard_ms,
                                       g_cfg.cancel_action_strict_hold);
            int recent = 1;
            if (g_cfg.cancel_action_recent_ms > 0) {
                recent = (g_attack_in.press_ms != 0 && at_press >= g_attack_in.press_ms &&
                          (at_press - g_attack_in.press_ms) <=
                              (unsigned long long)g_cfg.cancel_action_recent_ms);
            }
            if (alone && recent) {
                pg_cancel_gate_arm(&g_cancel_gate, at_press);
            } else {
                report_cancel_skipped(alone ? "no attack started recently"
                                            : "guard+attack combination",
                                      buttons, attack_down, at_press);
            }
        }
    }
    // Resolve a pending cancel: an attack press inside the window means the engine
    // keeps the input (skill / derivation preserved) and we write nothing.
    if (g_cfg.cancel_action_on_guard) {
        int res = pg_cancel_gate_step(&g_cancel_gate, attack_fresh, now_ms(),
                                      g_cfg.combo_guard_ms);
        if (res == PG_CANCEL_FIRE) {
            apply_action_cancel();
        } else if (res == PG_CANCEL_DROP) {
            report_cancel_skipped("guard+attack combination", buttons, attack_down,
                                  at_press);
        }
    } else if (g_cancel_gate.pending) {
        pg_cancel_gate_reset(&g_cancel_gate);
    }
    g_last_buttons = buttons;

    // Learn mode: reveal the real bindings on whatever device is in use, so the
    // INI can be corrected without a second round of guessing.
    if (g_cfg.learn_buttons) {
        unsigned short fresh = (unsigned short)(buttons & ~g_seen_pad_bits);
        if (fresh) {
            for (int b = 0; b < 16; ++b) {
                if (fresh & (1 << b)) {
                    log_line("LEARN pad bit %d pressed -> GuardButtonMask=0x%04X "
                             "(raw word 0x%04X)", b, 1 << b, buttons);
                }
            }
            g_seen_pad_bits |= buttons;
        }
        // sweep the keyboard once in a while; logging only newly-down keys keeps
        // this cheap enough to run alongside the game
        if (++g_vk_sweep >= 6) {
            g_vk_sweep = 0;
            for (int vk = 0x08; vk <= 0xFE; ++vk) {
                if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;
                if (GetAsyncKeyState(vk) & 0x8000) {
                    if (!g_seen_vk[vk]) {
                        g_seen_vk[vk] = 1;
                        log_line("LEARN key VK=0x%02X pressed -> GuardKeyVK=%d",
                                 vk, vk);
                    }
                }
            }
        }
    }
}

static int is_armed(DWORD tid) {
    LONG n = g_armed_n;
    for (LONG i = 0; i < n; ++i) if (g_armed[i] == tid) return 1;
    return 0;
}

static void remember_armed(DWORD tid) {
    LONG n = InterlockedIncrement(&g_armed_n) - 1;
    if (n >= 0 && n < MAX_ARMED) g_armed[n] = tid;
    else InterlockedDecrement(&g_armed_n);
}

// Windows recycles thread ids. A recorded id whose thread has since died can be
// handed to a brand-new thread, which `is_armed` would then skip forever -- and
// that thread would silently lose every guard event, with no error anywhere.
//
// Once a thread has exited and its handles are closed its object is freed, so
// OpenThread on that id fails. Dropping those ids lets a recycled id be armed
// again. The residual window is one purge interval (a reuse that happens inside
// that window is still missed), which is why this runs periodically rather than
// once.
static int g_purge_log = 0;                    // cap on the maintenance line
static volatile LONG g_purged_total = 0;       // reported in the STATE dump

static void purge_dead_armed(void) {
    LONG n = g_armed_n;
    if (n <= 0) return;
    if (n > MAX_ARMED) n = MAX_ARMED;
    LONG keep = 0;
    for (LONG i = 0; i < n; ++i) {
        HANDLE th = OpenThread(SYNCHRONIZE, FALSE, g_armed[i]);
        int alive = 0;
        if (th) {
            alive = (WaitForSingleObject(th, 0) == WAIT_TIMEOUT);
            CloseHandle(th);
        }
        if (alive) g_armed[keep++] = g_armed[i];
    }
    if (keep != n) {
        InterlockedExchange(&g_armed_n, keep);
        InterlockedExchangeAdd(&g_purged_total, n - keep);
        // Capped: this runs every ~25s, and a session with thread churn would
        // otherwise write a line every time. The running total stays visible in the
        // STATE dump as purged=.
        if (g_purge_log < 20) {
            log_line("ARM purge: dropped %ld stale thread id(s), %ld still tracked",
                     n - keep, keep);
            g_purge_log++;
        }
    }
}

// Repair threads whose debug registers were silently lost, a few per pass.
//
// *** OFF BY DEFAULT -- this destabilises the game. ***
//
// It was written to repair DR state that is assumed to go missing, but that
// assumption was never demonstrated, and an A/B on this machine says the cost is
// real: with it running the game faulted with 0xC0000005 (a null dereference deep
// in nioh.exe) after about 5.4 minutes; with it switched off, two runs of 8 and 9
// minutes were clean, and so was the run with nothing armed at all.
//
// The plausible mechanism is the sheer amount of interference: every 400ms it
// suspends six live game threads and rewrites their debug registers, i.e. ~750
// suspensions and SetThreadContext calls in five minutes, against threads the game
// assumes it controls the scheduling of.
//
// Nothing functional is lost by leaving it off: new threads are armed by
// arm_new_threads() as soon as they appear, and recycled thread ids are handled by
// purge_dead_armed(). Enabled only via DiagDisable=4, for investigation.
#define REARM_BATCH 6
static int g_rearm_cursor = 0;
static volatile LONG g_rearm_writes = 0;   // successful rolling re-arms
static int g_rearm_cycle_logged = 0;

static BOOL arm_thread(HANDLE th);   // forward: defined just below

static void rearm_batch(void) {
    if (!g_installed) return;        // never touch a DR without a handler in place
    LONG n = g_armed_n;
    if (n <= 0) return;
    if (n > MAX_ARMED) n = MAX_ARMED;
    // The wrap has to be remembered explicitly: the loop below always leaves the
    // cursor non-zero, so testing `cursor == 0` afterwards never fires.
    int wrapped = 0;
    if (g_rearm_cursor >= n || g_rearm_cursor < 0) {
        g_rearm_cursor = 0;
        wrapped = 1;
    }
    int did = 0;
    for (int k = 0; k < REARM_BATCH && g_rearm_cursor < n; ++k) {
        DWORD tid = g_armed[g_rearm_cursor++];
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                               THREAD_SET_CONTEXT, FALSE, tid);
        if (!th) continue;               // died since: purge_dead_armed will drop it
        if (arm_thread(th)) {
            InterlockedIncrement(&g_rearm_writes);
            did++;
        }
        CloseHandle(th);
    }
    // Once the cursor has walked the whole list, say so: this is the evidence that
    // the rolling repair is running without the old whole-process sweep.
    if (wrapped && did && !g_rearm_cycle_logged) {
        log_line("ARM rolling re-arm: first full cycle done (%ld writes over %ld "
                 "tracked ids, no whole-process suspend)",
                 InterlockedCompareExchange(&g_rearm_writes, 0, 0), n);
        g_rearm_cycle_logged = 1;
    }
}

static BOOL arm_thread(HANDLE th) {
    static LONG fail_logged = 0;
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    DWORD susp = SuspendThread(th);
    if (susp == (DWORD)-1) {
        if (g_debug_arm && InterlockedIncrement(&fail_logged) <= 8)
            log_line("ARM suspend failed tid=%lu err=%lu", GetThreadId(th), GetLastError());
        return FALSE;
    }
    BOOL ok = FALSE;
    if (GetThreadContext(th, &ctx)) {
        ctx.Dr0 = ctx.Dr1 = ctx.Dr2 = ctx.Dr3 = 0;
        DWORD64 dr7 = 0;
        int slot = 0;
        for (int i = 0; i < g_anchor_count && slot < 4; ++i) {
            if (!g_anchor[i].enabled || !g_anchor[i].armable) continue;
            (&ctx.Dr0)[slot] = g_base + g_anchor[i].rva;
            dr7 |= (DWORD64)1 << (slot * 2);
            slot++;
        }
        // Keep whatever reserved bits the OS put in DR7; only own L0..G3 and the
        // RW/LEN fields. Wiping the whole register (the original behaviour) still
        // traps 5/5 on this machine, but it needlessly discards information the
        // kernel considers part of the debug state.
        {
            DWORD64 keep = ctx.Dr7;
            keep &= ~(DWORD64)0xFF;
            keep &= ~(DWORD64)0xFFFF0000;
            ctx.Dr7 = keep | dr7;
        }
        ok = SetThreadContext(th, &ctx);
        if (!ok && g_debug_arm && InterlockedIncrement(&fail_logged) <= 8)
            log_line("ARM setctx failed tid=%lu err=%lu", GetThreadId(th), GetLastError());
    } else if (g_debug_arm && InterlockedIncrement(&fail_logged) <= 8) {
        log_line("ARM getctx failed tid=%lu err=%lu", GetThreadId(th), GetLastError());
    }
    ResumeThread(th);
    return ok;
}

static void arm_new_threads(void) {
    // Arming a debug register is only safe once a vectored handler exists to
    // consume the resulting single-step. Without this guard the worker loop armed
    // every thread even when the mod reported BYPASS (Enabled=0) or NOT INSTALLED,
    // which would have crashed the game on the first guard the moment it ran.
    if (!g_installed) return;
    DWORD pid = GetCurrentProcessId();
    DWORD self = GetCurrentThreadId();
    static LONG arm_pass = 0;
    static int g_snapfail_log = 0;
    // Called every 32 ms, so this is roughly every two seconds.
    if ((InterlockedIncrement(&arm_pass) % 64) == 0) purge_dead_armed();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        // Capped: this is reached every ~400ms, so a persistent failure would
        // otherwise write 2.5 lines/second for the whole session -- the worst
        // unbounded path in the mod, and it is a failure path at that, where the
        // user most needs the log to stay readable.
        if (g_snapfail_log < 5) {
            log_line("ARM snapshot failed err=%lu", GetLastError());
            g_snapfail_log++;
        }
        return;
    }
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    int seen = 0, armed_now = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (te.th32ThreadID == self) continue;   // never suspend ourselves
            seen++;
            if (is_armed(te.th32ThreadID)) continue;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
            if (!th) continue;
            if (arm_thread(th)) {
                remember_armed(te.th32ThreadID);
                armed_now++;
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    if (armed_now && (g_debug_arm || !g_first_arm_done)) {
        log_line("ARM pass: %d threads seen, %d newly armed", seen, armed_now);
        g_first_arm_done = 1;
    }
}

// ---- guard logic state -----------------------------------------------------

static unsigned long long now_ms(void) {
    return (unsigned long long)GetTickCount64();
}

// Guard flag sites: the flag is written on the character whose *attack* was
// guarded, so this fires both when the player blocks and when an enemy blocks
// the player. Logging only - the authoritative player-block event is the cost
// site, which can be attributed by pointer.
//
// The flag fires on a much broader condition than the cost site, which is worth
// knowing: the charge at 0x74DE51 is skipped unless `[defender+0x240] != 0`,
// `[ctx+0x160] == 0` AND the guard resource entry's state dword is exactly 1
// (0x74DE3C: cmp dword [rax], 1 / jne). So a flag with no cost event is not
// necessarily a missed detection -- it can be a guard the engine did not charge
// for. FLAGDIAG prints the state the decision was made on.
static int g_flag_diag = 0;

static void on_guard_flag(int which, CONTEXT *c) {
    if (!g_cfg.enabled) return;
    long n = InterlockedIncrement(&g_blocks);
    if (n <= 20) {
        log_line("GUARD FLAG #%ld set via %s (ctx=0x%llX)", n,
                 g_anchor[which].name, g_last_ctx);
    }
    // Wait: g_blocks is also incremented by the log-only paths, so keep the
    // original 20-line cap semantics and print the diagnostic separately.
    if (g_cfg.ki_trace && g_flag_diag < 20) {
        // NOTE: `c->Rcx` here is NOT the character. The store is
        // `mov byte [rcx+0xC8], 1` on a sub-object reached via char+0x40 -> +0x90,
        // so probing [rcx+0x240] yields garbage (the first version of this
        // diagnostic did exactly that and printed nonsense).
        //
        // What matters is the pair of combatants in the hit context: the cost site
        // charges the DEFENDER, and only if that side's entry-7 state dword is 1.
        // So dump both sides' entry-7 and the state the decision hinges on.
        unsigned long long ctx = c ? (unsigned long long)c->Rsi : 0;
        void *player = g_base ? *(void **)(ULONG_PTR)(g_base + 0x18A0490) : NULL;
        void *sides[3] = {NULL, NULL, NULL};   // 0/1 = the two combatants, 2 = attacker
        int ctx160 = -1;
        if (ctx) {
            SIZE_T got = 0;
            ReadProcessMemory(GetCurrentProcess(), (void *)(ULONG_PTR)(ctx + 0x100),
                              &sides[0], sizeof(void *), &got);
            ReadProcessMemory(GetCurrentProcess(), (void *)(ULONG_PTR)(ctx + 0xE8),
                              &sides[1], sizeof(void *), &got);
            ReadProcessMemory(GetCurrentProcess(), (void *)(ULONG_PTR)(ctx + 0x40),
                              &sides[2], sizeof(void *), &got);
            ReadProcessMemory(GetCurrentProcess(), (void *)(ULONG_PTR)(ctx + 0x160),
                              &ctx160, 4, &got);
        }
        char detail[340];
        int dlen = 0;
        for (int i = 0; i < 3 && dlen < (int)sizeof(detail) - 90; ++i) {
            int state = -12345;
            float cur = -1.0f, cost = -1.0f;
            void *param = NULL;
            if (sides[i]) {
                SIZE_T got = 0;
                ReadProcessMemory(GetCurrentProcess(), (char *)sides[i] + 0x240,
                                  &param, sizeof(param), &got);
                if (param) {
                    ReadProcessMemory(GetCurrentProcess(), (char *)param + 0xDE0,
                                      &state, 4, &got);
                    ReadProcessMemory(GetCurrentProcess(), (char *)param + 0xDEC,
                                      &cur, 4, &got);
                    ReadProcessMemory(GetCurrentProcess(), (char *)param + 0xDF0,
                                      &cost, 4, &got);
                }
            }
            const char *tag = (i == 0) ? "A" : (i == 1) ? "B" : "atk";
            dlen += snprintf(detail + dlen, sizeof(detail) - dlen,
                             "%s=0x%llX ply=%d st=%d cur=%.4g cost=%.4g | ",
                             tag, (unsigned long long)(ULONG_PTR)sides[i],
                             sides[i] == player ? 1 : 0, state, cur, cost);
        }
        log_line("FLAGDIAG via %s ctx=0x%llX ctx160=%d | %s",
                 g_anchor[which].name, ctx, ctx160, detail);
        g_flag_diag++;
    }

    // Run the probe for flag and auto modes. It must be reached even before the
    // switch has happened -- the switch decision is made *inside* it, so gating
    // this call on `effective_event_source() == 1` would make auto mode dead code
    // (it would never observe the blocks that trigger the switch).
    if (g_cfg.block_event_source != 0) on_guard_flag_event(c);
}

// The HP half of the reward.
//
// It reads the player object from the game's own global rather than from the hit
// context, so it cannot accidentally heal whoever else is in the context -- and
// it therefore works identically from both event sources, which is why the call
// site is perfect_guard_rewards() instead of either handler.
//
// The decision (clamping, the refusals, the "0 HP means dead") is in
// pg_hp_restore(); this wrapper only does the memory access, the counters and the
// logging.
static volatile LONG g_hp_restores = 0;   // times HP was actually written
static volatile LONG g_hp_restored = 0;   // total HP handed back
static volatile LONG g_hp_skipped = 0;    // full HP: normal, not reported per event
static LONG g_hp_log = 0;
static int g_hp_bad_notice = 0;

static void apply_hp_restore(void) {
    if (g_cfg.hp_recovery_mode == PG_HP_OFF) return;
    void *player = g_base ? *(void **)(ULONG_PTR)(g_base + 0x18A0490) : NULL;
    if (!player) return;
    void *param = *(void **)((char *)player + 0x240);
    if (!param) return;

    int *cur = (int *)((char *)param + 0x20);
    int *max = (int *)((char *)param + 0x18);
    PgHpResult r = pg_hp_restore(*cur, *max, g_cfg.hp_recovery_mode,
                                 g_cfg.hp_restore_percent, g_cfg.hp_restore_fixed);
    if (r.outcome == PG_HP_FULL) {
        // The common case by far. Counting it is what makes "the setting does
        // nothing" distinguishable from "HP happened to be full every time".
        InterlockedIncrement(&g_hp_skipped);
        return;
    }
    if (r.outcome != PG_HP_APPLIED) {
        // Either the offsets moved or the configured amount is 0 HP. Both look
        // exactly like a broken setting from the player's side, so say which.
        if (g_hp_bad_notice < 3) {
            log_line("WARNING: HpRecoveryMode=%d applied no HP restore (%s). "
                     "Current/max HP read %d/%d; check HpRestorePercent=%g and "
                     "HpRestoreFixed=%g in the INI.",
                     g_cfg.hp_recovery_mode,
                     r.outcome == PG_HP_INVALID ? "the HP fields do not look like HP"
                                                : "the configured amount is 0 HP",
                     r.before, *max, (double)g_cfg.hp_restore_percent,
                     (double)g_cfg.hp_restore_fixed);
            g_hp_bad_notice++;
        }
        return;
    }

    *cur = r.after;
    InterlockedIncrement(&g_hp_restores);
    InterlockedExchangeAdd(&g_hp_restored, r.amount);
    if (g_hp_log < 30) {
        log_line("HP restore %d -> %d (+%d%s) max=%d mode=%d",
                 r.before, r.after, r.amount, r.clamped ? ", at max" : "",
                 *max, g_cfg.hp_recovery_mode);
        g_hp_log++;
    }
}

// ------------------------------------------------- node registry dump (once) --
//
// The engine registers its data-driven nodes ("Refer::Hp", "Refer::Speed", ...) in
// a table whose *entries* live on the heap: at a fixed RVA the module holds pairs
// of absolute heap pointers, so the names are heap strings and cannot be resolved
// from a module dump (that is why hunting "Refer::Speed" from disk failed). The
// mod, however, runs inside the game.
//
// So: read the table once and log the Refer:: names with whatever the paired word
// points at. This is *read-only*, every access goes through ReadProcessMemory, and
// the whole thing is bounded -- a wrong guess about the layout prints a short block
// of nonsense instead of touching anything. Its purpose is to let one session hand
// back the name -> handler map, which is what the remaining reverse engineering
// (the movement-speed value, and the attack-cancel gate) needs.
#define PG_RVA_NODE_REGISTRY 0x119E400
#define PG_NODE_REG_MAX 512
#define PG_NODE_LOG_MAX 60

static void dump_node_registry_once(void) {
    if (!g_base) return;
    HANDLE self = GetCurrentProcess();
    const unsigned long long table = (unsigned long long)(ULONG_PTR)g_base + PG_RVA_NODE_REGISTRY;
    int logged = 0;
    for (int i = 0; i < PG_NODE_REG_MAX && logged < PG_NODE_LOG_MAX; ++i) {
        void *node = NULL;
        if (!ReadProcessMemory(self, (void *)(table + (unsigned long long)i * 16), &node,
                               sizeof(node), NULL)) break;
        if (!node) break;
        char name[64] = {0};
        if (!ReadProcessMemory(self, node, name, sizeof(name) - 1, NULL)) continue;
        if (!(name[0] >= 32 && name[0] < 127)) continue;
        if (strncmp(name, "Refer::", 7) != 0) continue;
        unsigned long long second = 0;
        if (!ReadProcessMemory(self, (char *)node + 8, &second, sizeof(second), NULL)) continue;
        // Report the paired word as an RVA when it is inside this module (that is
        // how a handler would look) and as a raw value otherwise.
        unsigned long long mod = (unsigned long long)(ULONG_PTR)g_base;
        if (second >= mod && second < mod + 0x4000000) {
            log_line("NODEREG %s pair2=0x%llX (module rva 0x%llX)", name, second,
                     second - mod);
        } else {
            log_line("NODEREG %s pair2=0x%llX (not this module)", name, second);
        }
        logged++;
    }
    log_line("NODEREG done: %d Refer:: entries read from the node registry", logged);
}

// ------------------------------------------------------------- timed buffs ---
//
// A perfect guard grants up to three temporary effects: move speed, damage taken,
// and armour. Unlike every other reward in this mod, these are *stateful*: the
// effect has to exist in the engine for several seconds and then be taken away
// again. So the mod has to do something it never did before -- call game code.
//
// The engine's own buff machinery was located in RE_NOTES 4.51 and is used
// verbatim rather than faked by writing fields:
//
//   * each effect is a "state object" the engine allocates and later destroys;
//   * its class stamps the state id (MoveSpeed 0x1B, DamageRate 0x1E, Armor 0x33);
//   * DamageRate's effect is literally `incoming damage * [obj+0x50]`, so -4% is
//     the parameter 0.96 -- no guessing about "which field holds the multiplier";
//   * add/replace is one call (0x7A2890), and it already implements "same state id
//     => replace", which is exactly the refresh semantics we want;
//   * the manager the constructors use is a process global (0xFA6E60), not TLS,
//     so calling from our own thread does not hit thread-local state.
//
// THE HARD RULE: none of this runs in the VEH. The exception handler runs on a
// game thread in the middle of arbitrary game logic, and re-entering the engine
// from there is not something we can reason about. The handler only arms the
// timers (pure arithmetic) and sets a request; the input thread performs the
// calls, and nothing else touches the container.
//
// It is also the one path in this mod that can plausibly crash the game, so it
// has its own DiagDisable bit (16) and logs every call it makes.

#define PG_STATE_ID_MOVE_SPEED 0x1B
#define PG_STATE_ID_DAMAGE_RATE 0x1E
#define PG_STATE_ID_ARMOR 0x33

#define PG_RVA_STATE_CTOR_DAMAGE_RATE 0x79F450
#define PG_RVA_STATE_CTOR_MOVE_SPEED 0x7A0D80
#define PG_RVA_STATE_CTOR_ARMOR 0x79E9E0
#define PG_RVA_STATE_ADD 0x7A2890
#define PG_RVA_STATE_REMOVE 0x7A25C0

// The constructors take (manager, duration_seconds, multiplier). Both were read
// off the engine's own call sites: it passes 300 / 1800 / 2400 seconds and
// multipliers like 0.95 (damage) and 0.7 (speed). So the first parameter is a
// *duration*, not a magnitude -- which is exactly why this mod still needs its own
// watchdog: the engine would keep our state for 300 seconds, we want 10.
//
// Armour is a flag-like state: it has no magnitude to scale, so it is created with
// the very multiplier the engine itself uses (1.5) rather than a number we invented.
#define PG_STATE_DURATION_S 300.0f
#define PG_ARMOR_PARAM_B 1.5f

static PgBuff g_buff_speed, g_buff_dmgcut, g_buff_armor;
static volatile LONG g_buff_started, g_buff_ended, g_buff_failed;
static LONG g_buff_log = 0, g_buff_log_cap = 0;

// The engine state object currently installed for each buff (NULL when none), and
// whether it is in place. Only ever touched by the thread that makes the calls.
typedef struct {
    const char *name;              // for the logs
    int state_id;
    unsigned long long ctor_rva;
    PgBuff *timer;
    void *obj;
    int installed;
} BuffEngine;

static BuffEngine g_eng[3];

static void buff_engine_init(void) {
    if (g_eng[0].name) return;
    g_eng[0].name = "speed";
    g_eng[0].state_id = PG_STATE_ID_MOVE_SPEED;
    g_eng[0].ctor_rva = PG_RVA_STATE_CTOR_MOVE_SPEED;
    g_eng[0].timer = &g_buff_speed;
    g_eng[1].name = "dmgcut";
    g_eng[1].state_id = PG_STATE_ID_DAMAGE_RATE;
    g_eng[1].ctor_rva = PG_RVA_STATE_CTOR_DAMAGE_RATE;
    g_eng[1].timer = &g_buff_dmgcut;
    g_eng[2].name = "armor";
    g_eng[2].state_id = PG_STATE_ID_ARMOR;
    g_eng[2].ctor_rva = PG_RVA_STATE_CTOR_ARMOR;
    g_eng[2].timer = &g_buff_armor;
}

// The multiplier each buff asks for: 1.04 for +4% speed, 0.96 for -4% damage, and
// the engine's own value for armour (nothing to scale).
static float buff_rate(int i) {
    if (i == 0) return 1.0f + g_cfg.speed_buff_percent / 100.0f;
    if (i == 1) return 1.0f - g_cfg.damage_cut_percent / 100.0f;
    return PG_ARMOR_PARAM_B;
}

// Whether this buff is configured to exist at all.
static int buff_enabled(int i) {
    if (i == 0) return g_cfg.speed_buff_percent > 0.0f && g_cfg.speed_buff_ms > 0;
    if (i == 1) return g_cfg.damage_cut_percent > 0.0f && g_cfg.damage_cut_ms > 0;
    return g_cfg.armor_buff && g_cfg.armor_buff_ms > 0;
}

static int buff_installed_mask(void) {
    int m = 0;
    for (int i = 0; i < 3; ++i) if (g_eng[i].installed) m |= (1 << i);
    return m;
}

// Read the engine's state container back and look for our state.
//
// `add()` returning 1 only means the call did not take an obvious failure path --
// it is not proof that the engine kept the state. The container is a tree hanging
// off [manager+0x170] whose nodes carry a key at +0x20, child links at +0x00/+0x10,
// and a pointer to the state object at +0x28 whose state id is at +0x10 (all read
// off the insert function 0x7A2890). Walking it turns "we called it" into "the
// engine really has it".
//
// Every read goes through ReadProcessMemory on our own process: a wrong guess about
// the layout then returns false instead of faulting, which matters because this
// code runs on a thread that must not take the game down. Returns 1 = found,
// 0 = not found, -1 = the container could not be read at all (so nothing is proven
// either way -- the layout is inferred, and absence must not be reported as proof
// that the install failed).
#define PG_STATE_TREE_MAX 256

static int buff_state_present(void *mgr, int state_id, void *want_obj) {
    if (!mgr) return -1;
    HANDLE self = GetCurrentProcess();
    void *root = NULL;
    SIZE_T got = 0;
    if (!ReadProcessMemory(self, (char *)mgr + 0x170, &root, sizeof(root), &got)) return -1;
    if (!root) return 0;

    void *stack[64];
    int sp = 0, visited = 0;
    stack[sp++] = root;
    while (sp > 0 && visited < PG_STATE_TREE_MAX) {
        void *n = stack[--sp];
        if (!n) continue;
        visited++;
        unsigned char marker = 0;
        void *l = NULL, *r = NULL, *so = NULL;
        // `marker` is read for the record but must NOT prune the walk: on this build it
        // looks like a colour/flag bit, and treating a set bit as "leaf, nothing below"
        // made this check report NOT FOUND even for a node add() had just inserted (a
        // gameplay log shows exactly that). Reads of a bogus pointer fail safely and the
        // walk is bounded, so exploring both children is cheap.
        ReadProcessMemory(self, (char *)n + 0x19, &marker, 1, &got);
        ReadProcessMemory(self, (char *)n + 0x00, &l, sizeof(l), &got);
        ReadProcessMemory(self, (char *)n + 0x10, &r, sizeof(r), &got);
        if (!ReadProcessMemory(self, (char *)n + 0x28, &so, sizeof(so), &got)) continue;
        if (so) {
            int sid = -1;
            if (ReadProcessMemory(self, (char *)so + 0x10, &sid, sizeof(sid), &got) &&
                sid == state_id) {
                if (!want_obj || so == want_obj) return 1;
            }
        }
        if (sp < 62) { stack[sp++] = l; stack[sp++] = r; }
    }
    return 0;
}

typedef void *(*pg_state_ctor_fn)(void *mgr, float a, float b);
typedef unsigned char (*pg_state_add_fn)(void *mgr, int key, void *obj, int flags,
                                         unsigned char stack0);
typedef unsigned char (*pg_state_remove_fn)(void *mgr, void *obj);

static int buff_engine_calls_enabled(void) {
    return g_base && !(g_cfg.diag_disable & 16);
}

// Byte-verify the engine functions before ever calling them.
//
// This is the same rule the mod applies to its own anchors (verify, or refuse and
// say so): jumping into an address that a different build happens to use for
// something else is the one failure mode that cannot be recovered from, and it is
// exactly what a game update would cause. The three constructors share an
// identical prologue, so the prologue alone cannot tell them apart -- the
// distinguishing feature is the state id each one stamps into the new object
// (`mov qword ptr [rax+0x10], imm32`), which is why the check requires both.
static int bytes_match(const unsigned char *p, const unsigned char *want, int n) {
    for (int i = 0; i < n; ++i) if (p[i] != want[i]) return 0;
    return 1;
}

// Returns the offset of the state-id store, or -1.
//
// The window has to be big enough: measured on this build the three constructors
// stamp their id at +0x61 (armour) and +0x69 (speed / damage rate), i.e. just past
// a 0x60-byte window -- the first version used 0x60 and therefore refused to call
// anything ("does not stamp state id"), which is safe but useless. It must not be
// *too* big either, because these functions contain other state ids further in
// (+0x101, +0x119, +0x12A, +0x1C1, +0x1D9), so a wide scan could match the wrong
// one. 0x80 covers all three and stops before the next id.
#define PG_CTOR_ID_SCAN 0x80

static int ctor_stamps_state_id(const unsigned char *p, int scan, int state_id) {
    for (int i = 0; i + 8 <= scan; ++i) {
        if (p[i] == 0x48 && p[i + 1] == 0xC7 && p[i + 2] == 0x40 && p[i + 3] == 0x10) {
            int v = 0;
            memcpy(&v, p + i + 4, 4);
            if (v == state_id) return i;
        }
    }
    return -1;
}

static int g_buff_engine_ok = -1;   // -1 = not checked yet, 0 = mismatch, 1 = ok

// Every constructor is checked, not just the one we happen to install first: all
// three share a prologue, so each must prove it stamps its own state id.
static int buff_engine_verify(void) {
    if (g_buff_engine_ok >= 0) return g_buff_engine_ok;
    g_buff_engine_ok = 0;
    if (!g_base) return 0;
    buff_engine_init();
    const unsigned char *p_add =
        (const unsigned char *)(ULONG_PTR)(g_base + PG_RVA_STATE_ADD);
    const unsigned char *p_rem =
        (const unsigned char *)(ULONG_PTR)(g_base + PG_RVA_STATE_REMOVE);
    static const unsigned char want_add[10] = {0x48, 0x8B, 0xC4, 0x57, 0x41,
                                               0x56, 0x41, 0x57, 0x48, 0x83};
    static const unsigned char want_rem[13] = {0x48, 0x85, 0xD2, 0x74, 0x44, 0x53, 0x48,
                                               0x83, 0xEC, 0x20, 0x80, 0x7A, 0x1D};
    int ok = bytes_match(p_add, want_add, sizeof(want_add)) &&
             bytes_match(p_rem, want_rem, sizeof(want_rem));
    for (int i = 0; i < 3 && ok; ++i) {
        const unsigned char *p =
            (const unsigned char *)(ULONG_PTR)(g_base + g_eng[i].ctor_rva);
        int at = ctor_stamps_state_id(p, PG_CTOR_ID_SCAN, g_eng[i].state_id);
        if (at < 0) {
            ok = 0;
            log_line("BUFF engine MISMATCH: the constructor at 0x%llX does not stamp "
                     "state id 0x%X (%s) in its first %d bytes -- engine calls disabled",
                     (unsigned long long)(ULONG_PTR)p, g_eng[i].state_id, g_eng[i].name,
                     PG_CTOR_ID_SCAN);
        }
    }
    log_line("BUFF engine functions %s (add=0x%llX remove=0x%llX)",
             ok ? "verified against this build" : "MISMATCH -- engine calls disabled",
             (unsigned long long)(ULONG_PTR)p_add,
             (unsigned long long)(ULONG_PTR)p_rem);
    g_buff_engine_ok = ok;
    return ok;
}

// The container the state objects live in: [[char+0x240]] + 0x10B0.
static void *buff_manager(void) {
    if (!g_base) return NULL;
    void *player = *(void **)(ULONG_PTR)(g_base + 0x18A0490);
    if (!player) return NULL;
    void *param = *(void **)((char *)player + 0x240);
    if (!param) return NULL;
    return (void *)((char *)param + 0x10B0);
}

// Install one buff's state object. Returns 1 when a state object is (or already
// was) in place.
//
// The key passed to add() is the engine's ordering key for that container
// (measured: it compares against the node's +0x20, then checks the node's state id
// for a same-state replacement). We key by state id: the engine's own applications
// of these classes use other keys (0x45 / 0x16 / 0x34), so ours cannot displace a
// game buff, and re-installing our own replaces our own -- the refresh we want.
static int buff_engine_install(int i) {
    buff_engine_init();
    BuffEngine *e = &g_eng[i];
    if (e->installed) return 1;
    if (!buff_engine_calls_enabled()) return 0;
    if (!buff_engine_verify()) return 0;
    void *mgr = buff_manager();
    if (!mgr) return 0;

    float rate = buff_rate(i);
    if (rate < 0.0f) rate = 0.0f;

    pg_state_ctor_fn ctor = (pg_state_ctor_fn)(ULONG_PTR)(g_base + e->ctor_rva);
    pg_state_add_fn add = (pg_state_add_fn)(ULONG_PTR)(g_base + PG_RVA_STATE_ADD);

    if (g_buff_log < 40) {
        log_line("BUFF %s: installing engine state 0x%X rate=%.4f dur=%.0fs "
                 "mgr=0x%llX ctor=0x%llX", e->name, e->state_id, rate,
                 (double)PG_STATE_DURATION_S, (unsigned long long)mgr,
                 (unsigned long long)(ULONG_PTR)ctor);
        g_buff_log++;
    }
    void *obj = ctor(mgr, PG_STATE_DURATION_S, rate);
    if (!obj) {
        if (g_buff_log < 40) { log_line("BUFF %s: constructor returned NULL", e->name); g_buff_log++; }
        InterlockedIncrement(&g_buff_failed);
        return 0;
    }
    unsigned char ok = add(mgr, e->state_id, obj, -1, 0);
    e->obj = obj;
    e->installed = 1;
    if (g_buff_log < 40) {
        int seen = buff_state_present(mgr, e->state_id, obj);
        log_line("BUFF %s: state object 0x%llX added (add()=%u) container check: %s",
                 e->name, (unsigned long long)obj, (unsigned)ok,
                 seen == 1 ? "present"
                           : (seen == 0 ? "NOT FOUND (layout guess may be wrong)"
                                        : "unreadable"));
        g_buff_log++;
    }
    return 1;
}

// Take one buff's state away again.
static void buff_engine_remove(int i) {
    buff_engine_init();
    BuffEngine *e = &g_eng[i];
    if (!e->installed) return;
    void *mgr = buff_manager();
    void *obj = e->obj;
    e->obj = NULL;
    e->installed = 0;
    if (!mgr || !obj || !buff_engine_calls_enabled()) return;
    pg_state_remove_fn rem = (pg_state_remove_fn)(ULONG_PTR)(g_base + PG_RVA_STATE_REMOVE);
    if (g_buff_log < 40) {
        log_line("BUFF %s: removing state object 0x%llX", e->name,
                 (unsigned long long)obj);
        g_buff_log++;
    }
    // NOTE: this deliberately does *not* call the engine's teardown (0x7A25C0).
    // Measured on 2026-09-27: that call faults inside itself (Rip = nioh.exe+0x7A25DD,
    // the virtual call at the object's vtable slot +0x38) because the state object is
    // engine-managed and is being used by the game thread -- pulling it out from our
    // own thread crashes the game. The engine's own buff applications pass a duration
    // (300/1800/2400 seconds) and let the engine expire the state, so the mod does the
    // same: it hands over its duration when constructing and then only *observes*.
    // The timed buffs are parked anyway (the user dropped them), but leaving a
    // known-crashing call in the build would be indefensible.
    (void)rem;
}

// Arm the timers. Called from the VEH -- pure arithmetic only, no engine calls.
static void buffs_on_perfect_guard(unsigned long long now) {
    buff_engine_init();
    if (buff_enabled(0) &&
        pg_buff_start(&g_buff_speed, now, g_cfg.speed_buff_ms)) {
        InterlockedIncrement(&g_buff_started);
        if (g_buff_log < 40) {
            log_line("BUFF speed start +%g%% for %dms (proc #%ld)",
                     (double)g_cfg.speed_buff_percent, g_cfg.speed_buff_ms,
                     g_buff_speed.procs);
            g_buff_log++;
        }
    }
    if (buff_enabled(1) &&
        pg_buff_start(&g_buff_dmgcut, now, g_cfg.damage_cut_ms)) {
        InterlockedIncrement(&g_buff_started);
        if (g_buff_log < 40) {
            log_line("BUFF dmgcut start -%g%% for %dms (proc #%ld)",
                     (double)g_cfg.damage_cut_percent, g_cfg.damage_cut_ms,
                     g_buff_dmgcut.procs);
            g_buff_log++;
        }
    }
    if (buff_enabled(2) &&
        pg_buff_start(&g_buff_armor, now, g_cfg.armor_buff_ms)) {
        InterlockedIncrement(&g_buff_started);
        if (g_buff_log < 40) {
            log_line("BUFF armor start for %dms (proc #%ld)", g_cfg.armor_buff_ms,
                     g_buff_armor.procs);
            g_buff_log++;
        }
    }
}

// The watchdog: runs on our own thread. Applies each engine state shortly after
// the guard, keeps it applied while its window is open, and takes it away when the
// window closes -- including the case where the window closed while the game was
// busy, because removal is driven by the timer, not by an event.
static void buff_tick(void) {
    buff_engine_init();
    unsigned long long now = now_ms();
    for (int i = 0; i < 3; ++i) {
        PgBuff *t = g_eng[i].timer;
        int expired = pg_buff_step(t, now);
        if (t->active) {
            if (!g_eng[i].installed) buff_engine_install(i);
        } else if (g_eng[i].installed) {
            buff_engine_remove(i);
        }
        if (expired) {
            InterlockedIncrement(&g_buff_ended);
            if (g_buff_log_cap < 40) {
                log_line("BUFF %s end (held %dms, %ld procs total)", g_eng[i].name,
                         i == 0 ? g_cfg.speed_buff_ms
                                : (i == 1 ? g_cfg.damage_cut_ms : g_cfg.armor_buff_ms),
                         t->procs);
                g_buff_log_cap++;
            }
        }
    }
}

// Nothing may stay applied once the mod stops caring: if a buff is switched off in
// the configuration while its state object is still installed (the operator edited
// the INI mid-window), take it away on the next tick.
static void buff_watchdog_config(void) {
    buff_engine_init();
    for (int i = 0; i < 3; ++i) {
        if (g_eng[i].installed && (!buff_enabled(i) || !g_cfg.enabled)) {
            buff_engine_remove(i);
            pg_buff_init(g_eng[i].timer);
        }
    }
}

// ------------------------------------------- the 99 gauge (精华量表 / 守护灵槽) --
// A perfect guard can add to the 99 gauge in both of its phases: while the 99 state
// is inactive the gauge accumulates towards activation, and while it is active that
// same gauge is the burning timer, so adding extends it.
//
// HOW, and why not by writing a field: the engine already has a state object for
// exactly this. RTTI names it Character::AddStateObjectAmritaGaugeUp; its constructor
// is 0x79E870 and stamps state id 0x20, and its apply method is literally
//
//     007A8120  movss xmm2, [rcx+0x50]      ; the constructor's magnitude
//     007A8128  addss xmm2, [rdx+0x15C]     ; + the gauge's current value
//     007A8130  movss xmm3, [rip+...] = 1.0 ; the gauge's maximum is 1.0 (normalised)
//               ... clamp, then store back to [rdx+0x15C]
//
// So the gauge is a 0..1 float, "10% of the gauge" is exactly the magnitude 0.10, and
// which object actually carries the gauge does not have to be guessed: the engine
// applies it. The call shape is the one this mod already uses for the timed buffs
// (ctor(mgr, duration_seconds, magnitude) then add(mgr, key, obj, -1, 0)) and, like
// the engine's own call sites, the same manager pointer goes to both.
//
// Two hard rules, both inherited from the timed-buff work:
//   * NEVER call the removal path (0x7A25C0). It crashes the game (RE_NOTES 4.52.5);
//     the duration handed to the constructor is what takes the node away, so it is
//     deliberately tiny -- the effect is applied when the node goes in, and a stale
//     node must not linger.
//   * the engine call happens on the input thread, never in the VEH. The handler only
//     computes a percentage and parks it.
//
// Which of the two switches applies is decided by pg_lw_plan() in pg_logic.h, and the
// "is the 99 state active" input to it comes from **two** sources, OR-ed together:
//
//   * the engine's own state container ([[char+0x240]]+0x10B0): while the 99 state is
//     up, the container holds the CallSpirit state object, state id 0x22 (ctor
//     0x79F1B0). The mod already walks that container (buff_state_present), so this
//     needs no new offset at all. This is the primary judge.
//   * the flag the engine's Player::SetTsukumoWeaponActiveFlag (0x8A6AA0) writes: a
//     byte at +0x104 of the object held by the process global at 0x18715E0. Its
//     semantics are inferred rather than proven, so it is only ever allowed to *add*
//     evidence of the 99 state, never to talk the container out of it.
//
// Any evidence => "active"; one readable source saying no while the other is
// unreadable => "not active"; neither readable => unknown (-1), which pg_lw_plan
// treats as "not active". The bias is deliberate: a false "active" merely uses the
// extend percentage (harmless), while a false "not active" would silently stop the
// accumulate feature from ever firing.
#define PG_STATE_ID_AMRITA_UP 0x20             // stamped by the ctor at +0x10
#define PG_STATE_ID_CALL_SPIRIT 0x22           // present in the container while in the 99 state
#define PG_RVA_STATE_CTOR_AMRITA_UP 0x79E870
#define PG_LW_STATE_DURATION_S 0.05f           // seconds; the engine expires the node
#define PG_RVA_LW_FLAG_HOLDER 0x18715E0
#define PG_LW_FLAG_OFFSET 0x104
// The amrita/99 cluster is a sub-object at param+0xB0; its gauge is an INT counter pair
// (the engine's own "recover the amrita gauge" primitive 0x7AF370 does sub+0x10 += n,
// clamped by sub+0x18). A float-based scan cannot see an int, which is why this took so
// many rounds to find.
#define PG_LW_INT_OFF 0xC0
#define PG_LW_INT_MAX_OFF 0xC8
#define PG_LW_REFRESH_TICKS 8                  // ~64ms on the 8ms input tick

static volatile LONG g_lw_seen_in_state = -1;  // -1 unknown, 0/1 resolved by the thread
static volatile LONG g_lw_seen_present = -1;   // container judge: 1/0, -1 unreadable (log only)
static volatile LONG g_lw_seen_flag = -1;      // flag byte: 1/0, -1 unreadable (log only)
static LONG g_lw_log = 0;
// Set by the write below, checked by the input thread ~64ms later: if the engine has
// already put its own value back, the field is a mirror and writing it can never work.
static volatile LONG g_lw_wrote = 0;
static float g_lw_wrote_val = -1.0f;
static float g_lw_wrote_max = -1.0f;
static volatile LONG g_lw_back = 0;
static volatile LONG g_lw_verify_off = 0;   // offset the write went to
static volatile LONG g_lw_verify_int = 0;   // 1 = int counter, 0 = float
static float g_lw_back_val = -1.0f;

// 1 = the engine's flag byte says the 99 state is active, 0 = it says it is not,
// -1 = cannot tell. ReadProcessMemory on our own process, so a wrong or not-yet
// initialised pointer fails the call instead of faulting.
static int lw_flag_byte(void) {
    if (!g_base) return -1;
    void *holder = NULL;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           (void *)(ULONG_PTR)(g_base + PG_RVA_LW_FLAG_HOLDER),
                           &holder, sizeof(holder), NULL) || !holder)
        return -1;
    unsigned char flag = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           (char *)holder + PG_LW_FLAG_OFFSET, &flag, 1, NULL))
        return -1;
    return flag ? 1 : 0;
}

// Pick between the two sources. Called from the input thread (it walks a tree), never
// from the exception handler.
//
// Precedence matters, and the first version got it wrong in a way a gameplay log
// caught: it OR-ed the two, and on the machine that was tested the flag byte read 1
// *permanently* (the container said 0x22 absent for the whole session). Every guard was
// therefore classified as "in the 99 state". The container -- the engine's own state
// list -- is the authoritative judge whenever it can be read at all; the flag byte is
// only a fallback for when it cannot.
static int lw_resolve(void) {
    int flag = lw_flag_byte();
    void *mgr = buff_manager();
    int present = mgr ? buff_state_present(mgr, PG_STATE_ID_CALL_SPIRIT, NULL) : -1;
    // The field that was 1 for the whole burning window in the second gameplay log: while
    // the 99 state is up the gauge drained monotonically (178.5 -> 110.03 in seven
    // seconds) and [param+0x4C] was 1 for exactly that window, then went back to 0. It is
    // the most direct evidence available -- the container id 0x22 never appeared at all
    // in that session, and the inferred global flag byte reads 1 permanently.
    int burning = 0;
    if (g_base) {
        void *player = *(void **)(ULONG_PTR)(g_base + 0x18A0490);
        void *param = player ? *(void **)((char *)player + 0x240) : NULL;
        if (param) burning = *(int *)((char *)param + 0x4C) != 0;
    }
    InterlockedExchange(&g_lw_seen_present, present);
    InterlockedExchange(&g_lw_seen_flag, flag);
    if (burning) return 1;              // burning the gauge: unambiguously in the 99 state
    if (present >= 0) return present;
    return flag;
}

// Cheap enough for the input tick, and it must not run in the VEH: an exception
// handler on a game thread has no business walking a 256-node tree.
static void lw_refresh_state(void) {
    static int tick = 0;
    if (++tick % PG_LW_REFRESH_TICKS) return;
    InterlockedExchange(&g_lw_seen_in_state, lw_resolve());
    // Did our write survive? Only the input thread checks, and only once per write.
    if (InterlockedCompareExchange(&g_lw_wrote, 0, 0) && !g_lw_back) {
        if (g_base) {
            void *pl = *(void **)(ULONG_PTR)(g_base + 0x18A0490);
            void *pa = pl ? *(void **)((char *)pl + 0x240) : NULL;
            if (pa) {
                int voff = (int)InterlockedCompareExchange(&g_lw_verify_off, 0, 0);
                if (voff > 0 && InterlockedCompareExchange(&g_lw_verify_int, 0, 0)) {
                    int iv = 0;
                    ReadProcessMemory(GetCurrentProcess(), (char *)pa + voff, &iv, 4, NULL);
                    g_lw_back_val = (float)iv;
                } else if (voff > 0) {
                    g_lw_back_val = *(float *)((char *)pa + voff);
                }
                InterlockedExchange(&g_lw_back, 1);
            }
        }
    }
}

// Read-only measurement of every candidate field, once per second (max 120 lines).
//
// Why it exists: a gameplay log proved that adding an AmritaGaugeUp state object has no
// visible effect (add() reported success, the gauge never moved), so the mod cannot keep
// guessing about which object carries the 99 gauge. This line prints, side by side:
//   * the four floats around the visible Ki pair (param+0x40/+0x44/+0x48/+0x4C) -- the
//     pair the engine's own one-shot "refill" path touches;
//   * the +0x15C float of both candidates (char+0x15C and param+0x15C) -- the offset the
//     AmritaGaugeUp apply writes to;
//   * the flag byte, whether the container holds CallSpirit (0x22), and whether the
//     container holds *our* AmritaGaugeUp node (0x20).
// Watching that line while the 99 gauge fills and drains identifies the real field in one
// session, and then the feature can write it directly instead of calling game code.
static void lw_diag(void) {
    static unsigned long long last = 0;
    static int lines = 0;
    if (lines >= 1200) return;
    unsigned long long now = now_ms();
    if (now - last < 1000) return;
    last = now;
    if (!g_base) return;
    void *player = *(void **)(ULONG_PTR)(g_base + 0x18A0490);
    if (!player) return;
    void *param = *(void **)((char *)player + 0x240);
    if (!param) return;

    HANDLE self = GetCurrentProcess();
    SIZE_T got = 0;
    float p40 = -1, p44 = -1, p48 = -1, p4c = -1, p15c = -1, c15c = -1;
    ReadProcessMemory(self, (char *)param + 0x40, &p40, 4, &got);
    ReadProcessMemory(self, (char *)param + 0x44, &p44, 4, &got);
    ReadProcessMemory(self, (char *)param + 0x48, &p48, 4, &got);
    ReadProcessMemory(self, (char *)param + 0x4C, &p4c, 4, &got);
    ReadProcessMemory(self, (char *)param + 0x15C, &p15c, 4, &got);
    ReadProcessMemory(self, (char *)player + 0x15C, &c15c, 4, &got);

    void *mgr = buff_manager();
    int c22 = mgr ? buff_state_present(mgr, PG_STATE_ID_CALL_SPIRIT, NULL) : -1;
    int n20 = mgr ? buff_state_present(mgr, PG_STATE_ID_AMRITA_UP, NULL) : -1;

    float rec[16];
    for (int i = 0; i < 16; ++i)
        ReadProcessMemory(self, (char *)param + 0xBB0 + i * 0x50 + 0x0C, &rec[i], 4, &got);
    int iC0 = 0, iC8 = 0;
    ReadProcessMemory(self, (char *)param + PG_LW_INT_OFF, &iC0, 4, &got);
    ReadProcessMemory(self, (char *)param + PG_LW_INT_MAX_OFF, &iC8, 4, &got);
    float p100 = -1.0f;
    ReadProcessMemory(self, (char *)param + 0x100, &p100, 4, &got);
    log_line("LWD iC0=%d iC8=%d p100=%.5g param40=%.5g param44=%.5g param48=%.5g param4c=%.5g p15c=%.5g "
             "c15c=%.5g flag=%ld c22=%d n20=%d",
             p40, p44, p48, p4c, p15c, c15c,
             (long)InterlockedCompareExchange(&g_lw_seen_flag, 0, 0), c22, n20);
    log_line("LWD2 wrote=%.5g/%.5g back=%.5g | rec0..7 %.4g %.4g %.4g %.4g %.4g %.4g %.4g %.4g "
             "| rec8..15 %.4g %.4g %.4g %.4g %.4g %.4g %.4g %.4g",
             g_lw_wrote_val, g_lw_wrote_max, g_lw_back_val,
             rec[0], rec[1], rec[2], rec[3], rec[4], rec[5], rec[6], rec[7],
             rec[8], rec[9], rec[10], rec[11], rec[12], rec[13], rec[14], rec[15]);
    lines++;
}
// ---------------------------------------------------------------- change scanner --
// The one measurement that cannot be fooled by a wrong guess.
//
// Four rounds guessed which field is the 99 gauge: the engine-call route did nothing,
// [param+0x48] read "full" while the visible gauge was one third full, and the record
// table was all zeros. What the operator *does* know is an action that moves the gauge:
// eating one small spirit stone. So instead of guessing, snapshot two float windows once
// a second and print only the offsets whose value actually moved by a meaningful amount.
// One stone then prints the gauge's offset and the size of the step directly; burning it
// (99 state) prints the drain; and nothing else has to be assumed.
//
// Windows: the param object ([[char+0x240]]) 0x00..0x1400 -- it holds the Ki pair at
// +0x40, the resource table at +0xBB0 and the state container at +0x10B0 -- and the
// character object's low 0x600 bytes. Per-offset throttle (5s) keeps a continuously
// draining field from eating the whole log.
#define PG_LW_SCAN_ZERO 0x00
#define PG_LW_SCAN_PAR_LEN 0x1400
#define PG_LW_SCAN_CHR_LEN 0x600
#define PG_LW_SCAN_MAX (PG_LW_SCAN_PAR_LEN / 4)
#define PG_LW_SCAN_MIN_DELTA 20.0f

static float g_lw_scan_prev[4][PG_LW_SCAN_MAX];
static unsigned long long g_lw_scan_last[4][PG_LW_SCAN_MAX];
static int g_lw_scan_valid[4];
static LONG g_lw_scan_log = 0;

static void lw_scan_window(int w, void *base, int len, const char *tag, float min_delta) {
    if (!base || len / 4 > PG_LW_SCAN_MAX) return;
    float cur[PG_LW_SCAN_MAX];
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (char *)base + PG_LW_SCAN_ZERO, cur,
                           (SIZE_T)len, &got) || got != (SIZE_T)len)
        return;
    if (!g_lw_scan_valid[w]) {
        memcpy(g_lw_scan_prev[w], cur, (size_t)len);
        g_lw_scan_valid[w] = 1;
        return;
    }
    unsigned long long now = now_ms();
    for (int i = 0; i < len / 4; ++i) {
        float a = g_lw_scan_prev[w][i], b = cur[i];
        if (!(a == a) || !(b == b)) continue;                  // NaN
        // Transform/position floats live in the tens of thousands and would drown the
        // log; a gauge is small (0..1 normalised, or 0..max a few hundred).
        if (a > 20000.0f || a < -20000.0f || b > 20000.0f || b < -20000.0f) continue;
        float d = b - a;
        if (d < 0) d = -d;
        if (d < min_delta) continue;
        if (now - g_lw_scan_last[w][i] < 2000) continue;        // per-offset throttle
        g_lw_scan_last[w][i] = now;
        if (g_lw_scan_log < 300) {
            log_line("LWC %s+0x%X %g -> %g", tag, PG_LW_SCAN_ZERO + i * 4, a, b);
            g_lw_scan_log++;
        }
    }
    memcpy(g_lw_scan_prev[w], cur, (size_t)len);
}

static void lw_scan(void) {
    static unsigned long long last = 0;
    if (g_lw_scan_log >= 300 || !g_base) return;
    unsigned long long now = now_ms();
    
    last = now;
    void *player = *(void **)(ULONG_PTR)(g_base + 0x18A0490);
    if (!player) return;
    void *param = *(void **)((char *)player + 0x240);
    lw_scan_window(0, param, PG_LW_SCAN_PAR_LEN, "param", PG_LW_SCAN_MIN_DELTA);
    lw_scan_window(1, player, 0x1000, "char", PG_LW_SCAN_MIN_DELTA);
    // The state manager ([[char+0x240]]+0x10B0) area, low threshold: a normalised gauge
    // would live in something like this and is invisible to the >=20 jump test.
    if (param) lw_scan_window(2, (char *)param + 0x10B0, 0x200, "smgr", 0.02f);
    // The amrita/99 cluster the static pass found: a sub-object embedded at param+0xB0.
    // param+0xCC = sub+0x1C is the burning bar measured in game; sub+0x10/sub+0x18 are
    // the only add-with-clamp pair (primitive 0x7AF370) = the gauge and its maximum, so
    // the step per spirit stone is probably small and needs a low threshold.
    if (param) lw_scan_window(3, (char *)param + 0xB0, 0x80, "lw", 0.01f);
}
// Called from the VEH: reads the cached judgement, then writes the gauge field.
//
// The engine-call route is dead (see the block comment above), so this is a plain field
// write. Which field was *measured*, not guessed -- a gameplay log with the LWD probe
// printed, once a second for two minutes:
//
//     param40=197    param44=197 param48=0      Ki full, gauge empty (start of session)
//     param40=197    param44=197 param48=197    the gauge jumped to full in under a second
//     param40=109.84 param44=197 param48=110.03 both drain in combat; the gauge refills
//
// So [param+0x48] is the 99/amrita gauge and it shares the maximum at [param+0x44] (the
// Ki maximum): it never exceeded it in 120 samples, and it sat at 0 while Ki was full.
// "+10% of the gauge" is therefore `cur += 0.10f * max`, clamped at max.
//
// Guard rails, because this writes a game field from an exception handler: the maximum
// must look like a maximum, the current value must be inside [0, max], and the write only
// ever *raises* the value, never above the maximum.
#define PG_LW_BURN_OFF 0xCC
#define PG_LW_BURN_MAX 100.0f
#define PG_LW_CUR_MAX 100000.0f

static void lw_plan_on_guard(void) {
    int in_lw = (int)InterlockedCompareExchange(&g_lw_seen_in_state, 0, 0);
    int pct = pg_lw_plan(g_cfg.lw_gauge_on, g_cfg.lw_gauge_percent,
                         g_cfg.lw_extend_on, g_cfg.lw_extend_percent, in_lw);
    if (pct <= 0 || !g_base) return;
    void *player = *(void **)(ULONG_PTR)(g_base + 0x18A0490);
    if (!player) return;
    void *param = *(void **)((char *)player + 0x240);
    if (!param) return;

    // Two different bars, measured on a real session:
    //   [param+0xCC] is the burning bar: 0 -> 100 the moment the 99 state starts and
    //                then draining to 0 over ~33s. "Extend the burn" = add percentage
    //                points to it (clamped at 100).
    //   [param+0x48] is the essence gauge: one small spirit stone adds ~212 and three
    //                fill it, so its maximum is ~636 -- NOT [param+0x44] (that is the Ki
    //                maximum, 212, and clamping to it is why the earlier version never
    //                changed anything). "Accumulate" = add pct% of the configured 636.
    float burn = *(float *)((char *)param + PG_LW_BURN_OFF);
    if (burn > 0.0f) {
        float nv = burn + (float)pct;
        if (nv > PG_LW_BURN_MAX) nv = PG_LW_BURN_MAX;
        if (nv > burn) {
            *(float *)((char *)param + PG_LW_BURN_OFF) = nv;
            InterlockedExchange(&g_lw_wrote, 1);
            g_lw_wrote_val = nv; g_lw_wrote_max = PG_LW_BURN_MAX; g_lw_back = 0;
            InterlockedExchange(&g_lw_verify_off, PG_LW_BURN_OFF);
            InterlockedExchange(&g_lw_verify_int, 0);
        }
        if (g_lw_log < 30) {
            log_line("LW burn: +%ld%% %g -> %g (max %g, in 99 state=1)", pct, burn, nv,
                     PG_LW_BURN_MAX);
            g_lw_log++;
        }
        return;
    }

    // The real store is an INT counter pair inside the amrita/99 cluster (param+0xB0):
    //   [param+0xC0] = counter, [param+0xC8] = its maximum, written by the engine's own
    //   "recover the amrita gauge" primitive 0x7AF370 (`sub+0x10 += n`, clamped by
    //   `sub+0x18`). A float-based scan could never see it: an int read as a float is a
    //   denormal (~1e-43), far below any threshold -- which is why ten rounds of float
    //   scanning found nothing.
    if (g_cfg.lw_gauge_offset <= 0) return;
    int *icur = (int *)((char *)param + g_cfg.lw_gauge_offset);
    int *imax = (int *)((char *)param + g_cfg.lw_gauge_offset + 8);
    int c = *icur, mx = *imax;
    if (g_cfg.lw_gauge_max > 0) mx = g_cfg.lw_gauge_max;   // 0 = use the runtime maximum
    if (mx <= 0 || mx > 1000000) return;               // not a counter/max pair
    if (c < 0 || c > mx) return;
    int nv = c + (int)((long long)pct * mx / 100);
    if (nv > mx) nv = mx;
    if (nv <= c) return;                               // already full
    *icur = nv;
    g_lw_wrote_val = (float)nv; g_lw_wrote_max = (float)mx; g_lw_back = 0;
    InterlockedExchange(&g_lw_verify_off, g_cfg.lw_gauge_offset);
    InterlockedExchange(&g_lw_verify_int, 1);
    InterlockedExchange(&g_lw_wrote, 1);
    if (g_lw_log < 30) {
        log_line("LW gauge: +%ld%% %d -> %d (max %d, in 99 state=%ld)", pct, c, nv, mx,
                 (long)in_lw);
        g_lw_log++;
    }
    return;
}

// (The engine-call route and its byte check were removed here: it was proven to have no
// effect in game -- see the LWD measurement and CHANGELOG 2.0j.)

// Guard cancels the player's attack action.
//
// Mechanism: advance the current action's motion frame -- the exact write the older
// CancelRecovery option makes (Refer::MotionFrame = [[char+0x38]+0x60]), now driven
// by a *guard press* instead of by a successful perfect guard. Skipping the rest of
// the animation is what lets the character leave the attack; the engine's own input
// buffering then takes over the guard.
//
// Why not the engine-native route (a cancel flag / the state machine): that needs
// more reverse engineering than has been done (RE_NOTES 4.53), and this write is
// already in the mod, bounded, and easy to reason about: it only ever *adds* to a
// value that already looks like a motion frame, and it refuses to write anything
// implausible. It is off unless the operator asks for it.
//
// Two windows keep a combination or an idle press from touching the animation:
//
//   * a *fresh* guard press (a held guard never re-triggers);
//   * no attack button went down within ComboGuardWindowMs of it (guard + X/Y/A is
//     a deliberate combination, not a cancel);
//   * an attack button must have been pressed within CancelActionRecentMs, i.e. the
//     player plausibly just started an attack. Without this, pressing guard while
//     idle would advance the *guard/idle* animation, which is exactly the kind of
//     unrequested interference that makes a mod feel broken.
//
// Movement is deliberately not consulted: guard + walking must still cancel.
static void apply_action_cancel(void) {
    if (!g_cfg.cancel_action_on_guard || !g_base) return;
    void *player = *(void **)(ULONG_PTR)(g_base + 0x18A0490);
    if (!player) return;

    // Refer::ActionId -> [[[[char+0x230]+8]+0x58]+0x20]+0xC  (int16)
    int action = -1;
    unsigned long long p1 = 0, p2 = 0, p3 = 0;
    SIZE_T got = 0;
    if (ReadProcessMemory(GetCurrentProcess(), (char *)player + 0x230, &p1, 8, &got) && p1)
        if (ReadProcessMemory(GetCurrentProcess(), (void *)(p1 + 8), &p2, 8, &got) && p2)
            if (ReadProcessMemory(GetCurrentProcess(), (void *)(p2 + 0x58), &p3, 8, &got) && p3)
                if (ReadProcessMemory(GetCurrentProcess(), (void *)(p3 + 0x20), &p2, 8, &got) && p2) {
                    short v = 0;
                    if (ReadProcessMemory(GetCurrentProcess(), (void *)(p2 + 0xC), &v, 2, &got))
                        action = v;
                }

    void *mobj = *(void **)((char *)player + 0x38);
    if (!mobj) return;
    float *frame = (float *)((char *)mobj + 0x60);
    float v = *frame;
    if (!(v >= 0.0f && v < 10000.0f)) return;      // not a motion frame: do nothing

    *frame = v + g_cfg.cancel_action_frames;
    InterlockedIncrement(&g_cancel_count);
    // Arm the follow-up sample (the action id right after the cancel).
    g_cancel_action_before = action;
    g_cancel_followup_before = action;
    g_cancel_followup_until = now_ms() + 400;
    g_cancel_followup_done = 0;
    if (g_cancel_log < 40) {
        log_line("ACTION CANCEL: action=%d motion frame %.3g -> %.3g (guard pressed "
                 "alone, attack within %dms)", action, v, *frame,
                 g_cfg.cancel_action_recent_ms);
        g_cancel_log++;
    }
}

// Log one refusal, with the evidence needed to tell the two cases apart later: which
// action was running, what the pad word was, and how long ago the attack button went
// down. The action id matters most -- it is how "the skill input did not disturb the
// attack in progress" becomes checkable from a log instead of an opinion.
static void report_cancel_skipped(const char *why, unsigned short pad, int attack_down,
                                 unsigned long long at_press) {
    InterlockedIncrement(&g_cancel_skipped);
    if (g_cancel_log >= 40) return;
    void *pl = g_base ? *(void **)(ULONG_PTR)(g_base + 0x18A0490) : NULL;
    log_line("ACTION CANCEL skipped: %s (action=%d pad=0x%04X attack_down=%d "
             "last attack press %llums ago)",
             why, pl ? read_action_id(pl) : -1, pad, attack_down,
             g_attack_in.press_ms && at_press >= g_attack_in.press_ms
                 ? at_press - g_attack_in.press_ms : 0);
    g_cancel_log++;
}

// Reads the current action id (Refer::ActionId), or -1.
static int read_action_id(void *player) {    int action = -1;
    unsigned long long p1 = 0, p2 = 0, p3 = 0;
    SIZE_T got = 0;
    if (ReadProcessMemory(GetCurrentProcess(), (char *)player + 0x230, &p1, 8, &got) && p1)
        if (ReadProcessMemory(GetCurrentProcess(), (void *)(p1 + 8), &p2, 8, &got) && p2)
            if (ReadProcessMemory(GetCurrentProcess(), (void *)(p2 + 0x58), &p3, 8, &got) && p3)
                if (ReadProcessMemory(GetCurrentProcess(), (void *)(p3 + 0x20), &p2, 8, &got) && p2) {
                    short v = 0;
                    if (ReadProcessMemory(GetCurrentProcess(), (void *)(p2 + 0xC), &v, 2, &got))
                        action = v;
                }
    return action;
}

// Called from the input thread: if a cancel just happened, log what the action
// became. `A -> B` with B being the guard/idle action is the proof the feature works.
static void action_cancel_followup(void) {
    if (g_cancel_followup_done || !g_cancel_followup_until) return;
    unsigned long long now = now_ms();
    if (now > g_cancel_followup_until) { g_cancel_followup_until = 0; return; }
    if (!g_base) return;
    void *player = *(void **)(ULONG_PTR)(g_base + 0x18A0490);
    if (!player) return;
    int after = read_action_id(player);
    if (after < 0 || after == g_cancel_followup_before) return;   // not changed yet
    g_cancel_followup_done = 1;
    g_cancel_followup_until = 0;
    if (g_cancel_log < 60) {
        log_line("ACTION CANCEL follow-up: action %d -> %d after the cancel",
                 g_cancel_followup_before, after);
        g_cancel_log++;
    }
}

static void perfect_guard_rewards(int from_flag) {
    InterlockedIncrement(&g_perfect);
    if (g_perfect <= 60) {
        log_line("PERFECT GUARD #%ld via %s (guard pressed %llums ago)",
                 g_perfect, g_cfg.block_event_source ? "guard-flag" : "guard-cost",
                 now_ms() - g_guard_in.press_ms);
    }
    // Never call into XAudio2 from an exception handler on a game thread: just
    // raise a request and let the worker (which owns the COM apartment) play it.
    if (g_cfg.sound_enabled && g_sound_event) SetEvent(g_sound_event);

    void *player_now = g_base ? *(void **)(ULONG_PTR)(g_base + 0x18A0490) : NULL;
    ki_snapshot(from_flag);
    apply_hp_restore();
    // Timers only: the engine calls happen on our own thread (see the timed-buff
    // section). Never call into the engine from this exception handler.
    buffs_on_perfect_guard(now_ms());
    // The 99 gauge: also just a parked percentage here (pg_lw_plan decides which
    // switch applies); the engine call happens on the input thread.
    lw_plan_on_guard();
    apply_cancel_recovery(player_now);

    // One-shot layout diagnostic: the engine's Refer::* getters read these exact
    // fields, so printing them at block time shows whether the offsets are right
    // and lets a single session settle which field a guard cost actually moves.
    if (g_perfect <= 20) {
        void *player = player_now;
        void *ctx = (void *)(ULONG_PTR)g_last_ctx;
        void *a = ctx ? *(void **)((char *)ctx + 0x100) : NULL;
        void *b = ctx ? *(void **)((char *)ctx + 0xE8) : NULL;
        float ki = -1.0f, maxki = -1.0f, mframe = -1.0f;
        int hp = -1, maxhp = -1, action = -1;
        if (player) {
            void *param = *(void **)((char *)player + 0x240);
            if (param) {
                ki = *(float *)((char *)param + 0x40);
                maxki = *(float *)((char *)param + 0x44);
                hp = *(int *)((char *)param + 0x20);
                maxhp = *(int *)((char *)param + 0x18);
            }
            // Refer::MotionFrame -> [[char+0x38]+0x60]
            void *mobj = *(void **)((char *)player + 0x38);
            if (mobj) mframe = *(float *)((char *)mobj + 0x60);
            // Refer::ActionId -> [[[[char+0x230]+8]+0x58]+0x20]+0xC  (int16)
            unsigned long long p1 = 0, p2 = 0, p3 = 0;
            SIZE_T got = 0;
            if (ReadProcessMemory(GetCurrentProcess(), (char *)player + 0x230, &p1, 8, &got) && p1)
                if (ReadProcessMemory(GetCurrentProcess(), (void *)(p1 + 8), &p2, 8, &got) && p2)
                    if (ReadProcessMemory(GetCurrentProcess(), (void *)(p2 + 0x58), &p3, 8, &got) && p3)
                        if (ReadProcessMemory(GetCurrentProcess(), (void *)(p3 + 0x20), &p2, 8, &got) && p2) {
                            short v = 0;
                            if (ReadProcessMemory(GetCurrentProcess(), (void *)(p2 + 0xC), &v, 2, &got))
                                action = v;
                        }
        }
        log_line("LAYOUT player=0x%llX hp=%d/%d ki=%.4g/%.4g action=%d frame=%.4g | "
                 "ctx=0x%llX charA=0x%llX charB=0x%llX",
                 (unsigned long long)player, hp, maxhp, ki, maxki, action, mframe,
                 (unsigned long long)ctx, (unsigned long long)a, (unsigned long long)b);
    }
}

static void on_guard_cost(CONTEXT *c) {
    if (!g_cfg.enabled) return;

    // Who is blocking is decided before any timing question, and that ordering is
    // part of the shared contract (pg_classify_block): an enemy blocking the
    // player's attack is not "the player blocked too late", it is not the player
    // at all, and it must not consume the player's window or produce a reward.
    unsigned long long at = now_ms();
    PgBlockOutcome outcome =
        pg_classify_block(entry_belongs_to_player(c->Rcx), &g_guard_in,
                          g_cfg.gate_timely, at, g_cfg.window_ms);

    if (outcome == PG_BLOCK_OTHER) {
        if (g_other_blocks < 12) {
            log_line("GUARD (other) entry=0x%llX is outside the player's resource "
                     "table; ignored", (unsigned long long)c->Rcx);
            g_other_blocks++;
        }
        return;
    }
    // A player-side cost event exists, so auto mode must keep using this source --
    // and if a previous session had concluded otherwise, correct the record.
    if (InterlockedIncrement(&g_cost_events_seen) == 1 && g_costsite_dead) {
        g_costsite_dead = 0;
        state_save("costsite=fires\n");
        log_line("NOTICE: the guard-cost event fires after all, so the mod is back on "
                 "the precise event source (the subtract-site cost scaling works "
                 "again).");
    }

    // If the flag source has been chosen, this site is only an observer: rewarding
    // here as well would count the same block twice.
    if (effective_event_source() == 1) return;

    // Record the un-scaled cost before any reward work, so the snapshot taken by
    // perfect_guard_rewards() below can pair this block's charge with what the
    // visible Ki field actually loses.
    g_last_cost = *(float *)&c->Xmm1;
    g_last_cost_block = g_rewards + 1;

    if (outcome == PG_BLOCK_LATE) {
        report_late_block();
        return;
    }
    if (!reward_slot_free()) return;   // this block was already rewarded elsewhere

    perfect_guard_rewards(0);
    InterlockedIncrement(&g_rewards);

    // xmm1 holds the guard Ki cost; scale it by the configured reduction.
    float *cost = (float *)&c->Xmm1;
    float original = *cost;
    if (g_cfg.ki_reduction_percent >= 100) {
        *cost = 0.0f;
        InterlockedIncrement(&g_free);
    } else if (g_cfg.ki_reduction_percent > 0) {
        *cost = original * (float)(100 - g_cfg.ki_reduction_percent) / 100.0f;
    }

    // nioh's own code then executes  [rcx+0x0C] -= xmm1  with a clamp at 0, so a
    // refund is just pre-adding to the same field. Both need the entry, which only
    // this event source has.
    apply_ki_recovery(c->Rcx, original);

    if (g_rewards <= 40) {
        log_line("GUARD cost=%.5g -> %.5g (reduction=%d%% recovery=%d entry=0x%llX)",
                 original, *cost, g_cfg.ki_reduction_percent,
                 g_cfg.ki_recovery_mode, (unsigned long long)c->Rcx);
    }
    apply_enemy_effects((void *)(ULONG_PTR)c->Rsi);
}

// The recovery half of the reward, shared by both event sources.
//
// `entry` is the charged resource-table entry and `charge` its un-scaled cost;
// the flag-based source has neither, so the modes that write the entry are
// unavailable there. Skipping them silently would look like a broken setting, so
// say so once.
static int g_mode_notice = 0;

static void apply_ki_recovery(unsigned long long entry, float charge) {
    if (g_cfg.ki_recovery_mode == 3) {
        // Balanced: restore a sixth of maximum Ki, the Nioh 2 mod's default.
        // The engine's own Refer::Stamina getter reads [param+0x40], and
        // Refer::StaminaRate divides it by [param+0x44], so those two fields are
        // current and maximum Ki. This works from either event source because it
        // needs only the player object, not the guard entry.
        void *player = g_base ? *(void **)(ULONG_PTR)(g_base + 0x18A0490) : NULL;
        if (player) {
            void *param = *(void **)((char *)player + 0x240);
            if (param) {
                float *cur = (float *)((char *)param + 0x40);
                float *max = (float *)((char *)param + 0x44);
                if (*max > 0.0f && *max < 100000.0f && *cur >= 0.0f && *cur <= *max) {
                    *cur += *max / 6.0f;
                    if (*cur > *max) *cur = *max;
                }
            }
        }
        return;
    }
    if (g_cfg.ki_recovery_mode == 0) return;
    if (!entry) {
        if (g_mode_notice < 3) {
            log_line("NOTICE: KiRecoveryMode=%d refunds the guard cost, which only the "
                     "guard-cost event source can see; with BlockEventSource=1 use "
                     "KiRecoveryMode=3 instead. No recovery was applied.",
                     g_cfg.ki_recovery_mode);
            g_mode_notice++;
        }
        return;
    }
    float *cur = (float *)(ULONG_PTR)(entry + 0x0C);
    if (g_cfg.ki_recovery_mode == 1) *cur += charge;   // exactly what this cost
    else if (g_cfg.ki_recovery_mode == 2) *cur += g_cfg.fixed_recovery;
}

// A block that did not land inside the window. Shared so both event sources report
// it identically (the log line text is part of the acceptance checklist).
static void report_late_block(void) {
    long n = InterlockedIncrement(&g_gate_closed);
    // A keyboard player whose guard key is not configured would see nothing but
    // "not a perfect guard" forever. Say so explicitly instead.
    if (n == 3 && g_guard_presses == 0) {
        log_line("WARNING: %ld player blocks seen but NO guard press was ever "
                 "detected, so the perfect-guard gate can never open. "
                 "GuardButtonMask is currently 0x%04X and GuardKeyVK is %d. Set "
                 "LearnButtons=1, press your guard key in game, and copy the "
                 "LEARN line's value here.",
                 n, g_cfg.guard_button_mask, g_cfg.guard_key_vk);
    }
    if (n <= 40) {
        log_line("PLAYER BLOCK #%ld -- not within %dms of a guard press; no reward",
                 n, g_cfg.window_ms);
    }
}

// The flag-based event source.
//
// Why it exists: the cost site is skipped unless the engine's own conditions hold
// (the defender's guard resource entry state must be 1, and [ctx+0x160] must be
// 0 -- see RE_NOTES 4.44). Those conditions are the engine's, and whether a real
// player guard satisfies them is not knowable without a session. The flag fires on
// a far broader condition and has been observed firing in-process, and the player
// can be located in the hit context, so this is a workable second source. In the
// attract-mode demo the cost site never fires and this one produces correct rewards.
// Auto mode must not *stick* with the fallback. If the mod switches while sitting
// at the title screen (where the engine never charges the guard cost) and the cost
// site later starts firing in real combat, a latch would permanently give up the
// precise path -- including the subtract-site cost scaling. So the preference is
// computed fresh every time: the cost site wins the moment it proves it fires.
static int g_auto_notice = 0;

// Whether this game build ever charges the guard cost is a property of the build
// and the running state, NOT of one session -- so it is remembered on disk.
//
// Without this, every restart had to re-learn it, which took three player blocks
// of nothing happening. That reads as "the mod worked, then I restarted and it
// stopped working", and it was reported exactly that way.
static char g_state_path[LOG_CAP] = {0};
static int g_costsite_dead = 0;      // loaded from disk / concluded this session

static void state_load(void) {
    if (!g_state_path[0]) {
        snprintf(g_state_path, sizeof(g_state_path), "%sNioh1PerfectGuard.state", g_dir);
    }
    HANDLE f = CreateFileA(g_state_path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[128] = {0};
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(f);
    if (strstr(buf, "costsite=fires")) {
        g_costsite_dead = 0;
    } else if (strstr(buf, "costsite=never")) {
        g_costsite_dead = 1;
        log_line("NOTICE: a previous session established that the guard-cost event "
                 "never fires on this build, so the flag event source is used from "
                 "the start. Delete Nioh1PerfectGuard.state to re-decide.");
    }
}

static void state_save(const char *what) {
    if (!g_state_path[0]) return;
    HANDLE f = CreateFileA(g_state_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, what, (DWORD)strlen(what), &w, NULL);
    CloseHandle(f);
}

static int effective_event_source(void) {
    if (g_cfg.block_event_source == 0) return 0;
    if (g_cfg.block_event_source == 1) return 1;
    // auto
    if (InterlockedCompareExchange(&g_cost_events_seen, 0, 0) > 0) return 0;
    if (!g_costsite_dead &&
        InterlockedCompareExchange(&g_flag_blocks, 0, 0) < 3) {
        return 0;                    // still giving the cost site its chance
    }
    if (!g_auto_notice) {
        g_auto_notice = 1;
        log_line("NOTICE: %s so the mod is using the flag event source. Rewards work, "
                 "but the guard cost cannot be scaled at the subtract site -- and "
                 "because this source fires *after* the engine charged the Ki, the "
                 "Ki reduction relies on the top-up measuring against a value from "
                 "before the charge (KiTopUp=1, KiTopUpPreEventMs>0). A KIV-FLAG "
                 "line reports what it found. It will switch back automatically if "
                 "a guard-cost event ever appears.",
                 g_costsite_dead
                     ? "a previous session established that the guard-cost event never "
                       "fires on this build,"
                     : "3 player blocks were seen by the guard-flag site but the "
                       "guard-cost site has never fired,");
    }
    return 1;
}

// One physical block can be visible to both sites (the engine may charge the Ki
// and set the flag for the same hit). Whichever site is authoritative rewards it;
// this stops the other from rewarding the same block if the decision changes
// between the two events. The policy lives in pg_logic.h (pg_dedupe_ok) so it can
// be unit tested; this wrapper only adds the cross-thread publication.
#define PG_REWARD_DEDUPE_MS 120
static volatile LONG g_last_reward_ms = 0;

static int reward_slot_free(void) {
    unsigned long long now = now_ms();
    for (;;) {
        LONG last = InterlockedCompareExchange(&g_last_reward_ms, 0, 0);
        if (!pg_dedupe_ok((unsigned long long)last, now, PG_REWARD_DEDUPE_MS))
            return 0;
        if (InterlockedCompareExchange(&g_last_reward_ms, (LONG)now, last) == last)
            return 1;
        // another thread published in between; re-evaluate against its value
    }
}

// Auto mode: count the blocks the flag site sees. The actual source decision is
// made in effective_event_source(), which prefers the cost site whenever it fires.
// Reaching the threshold also records the conclusion on disk, so the next launch
// starts in the right mode instead of re-learning it.
static void switch_event_source_if_needed(void) {
    if (g_cfg.block_event_source != 2) return;
    if (InterlockedCompareExchange(&g_cost_events_seen, 0, 0) > 0) return;
    LONG n = InterlockedIncrement(&g_flag_blocks);
    if (n == 3 && !g_costsite_dead) {
        g_costsite_dead = 1;
        state_save("costsite=never\n");
    }
}

static void on_guard_flag_event(CONTEXT *c) {
    unsigned long long ctx = c ? (unsigned long long)c->Rsi : 0;
    if (!ctx || !g_base) return;
    void *player = *(void **)(ULONG_PTR)(g_base + 0x18A0490);
    if (!player) return;

    void *a = NULL, *b = NULL, *atk = NULL;
    SIZE_T got = 0;
    ReadProcessMemory(GetCurrentProcess(), (void *)(ULONG_PTR)(ctx + 0x100), &a,
                      sizeof(a), &got);
    ReadProcessMemory(GetCurrentProcess(), (void *)(ULONG_PTR)(ctx + 0xE8), &b,
                      sizeof(b), &got);
    ReadProcessMemory(GetCurrentProcess(), (void *)(ULONG_PTR)(ctx + 0x40), &atk,
                      sizeof(atk), &got);

    if (!pg_flag_means_player_blocked(a == player, b == player, atk == player))
        return;

    switch_event_source_if_needed();
    if (effective_event_source() != 1) return;   // still deferring to the cost site

    if (!pg_guard_gate(&g_guard_in, g_cfg.gate_timely, now_ms(), g_cfg.window_ms)) {
        report_late_block();
        return;
    }
    if (!reward_slot_free()) return;   // this block was already rewarded elsewhere

    // No entry and no cost are visible here, so the charge is recorded as unknown:
    // that disables the refund cap (harmless) and makes the KIV verdict skip the
    // cost-site models, which is correct -- KIV is a question about the cost site.
    // The Ki reference, however, must come from *before* the charge this event
    // reports, so the snapshot uses the rolling ring (see pg_ki_ref).
    g_last_cost = 0.0f;
    g_last_cost_block = g_rewards + 1;
    perfect_guard_rewards(1);
    InterlockedIncrement(&g_rewards);
    apply_ki_recovery(0, 0.0f);
    apply_enemy_effects((void *)(ULONG_PTR)ctx);
}

// Full state snapshot. Triggered by the diagnostic hotkey, and once
// automatically the first time a player object appears (i.e. the moment the
// player enters a mission). That automatic dump means a single session produces
// useful data even if nothing else is pressed.
static int g_dumped_layout = 0;

static void dump_state(const char *why) {
    if (!g_base) {
        log_line("STATE (%s): no module base yet", why);
        return;
    }
    int anchors_ok = 0;
    for (int i = 0; i < g_anchor_count; ++i)
        if (g_anchor[i].enabled) anchors_ok++;

    void *mgr = g_input_mgr_slot ? *(void **)(ULONG_PTR)g_input_mgr_slot : NULL;
    unsigned short padword = 0;
    unsigned char flags[4] = {0, 0, 0, 0};
    if (mgr) {
        SIZE_T got = 0;
        ReadProcessMemory(GetCurrentProcess(), (char *)mgr + 0x49E0C, &padword, 2, &got);
        for (int i = 0; i < 4; ++i)
            ReadProcessMemory(GetCurrentProcess(),
                              (char *)mgr + 0x49E04 + i * 20, &flags[i], 1, &got);
    }

    void *player = *(void **)(ULONG_PTR)(g_base + 0x18A0490);
    void *param = player ? *(void **)((char *)player + 0x240) : NULL;
    int hp = -1, maxhp = -1, action = -1;
    float ki = -1.0f, maxki = -1.0f, mframe = -1.0f, ecur = -1.0f, ecost = -1.0f;
    int eflag = -1;
    if (param) {
        SIZE_T got = 0;
        ReadProcessMemory(GetCurrentProcess(), (char *)param + 0x20, &hp, 4, &got);
        ReadProcessMemory(GetCurrentProcess(), (char *)param + 0x18, &maxhp, 4, &got);
        ReadProcessMemory(GetCurrentProcess(), (char *)param + 0x40, &ki, 4, &got);
        ReadProcessMemory(GetCurrentProcess(), (char *)param + 0x44, &maxki, 4, &got);
        // guard resource entry = base(0xBA8) + 8 + 7*0x50
        ReadProcessMemory(GetCurrentProcess(), (char *)param + 0xDE0, &eflag, 4, &got);
        ReadProcessMemory(GetCurrentProcess(), (char *)param + 0xDEC, &ecur, 4, &got);
        ReadProcessMemory(GetCurrentProcess(), (char *)param + 0xDF0, &ecost, 4, &got);
    }
    if (player) {
        void *mobj = *(void **)((char *)player + 0x38);
        if (mobj)
            ReadProcessMemory(GetCurrentProcess(), (char *)mobj + 0x60, &mframe, 4, NULL);
        unsigned long long p1 = 0, p2 = 0, p3 = 0;
        SIZE_T got = 0;
        if (ReadProcessMemory(GetCurrentProcess(), (char *)player + 0x230, &p1, 8, &got) && p1)
            if (ReadProcessMemory(GetCurrentProcess(), (void *)(p1 + 8), &p2, 8, &got) && p2)
                if (ReadProcessMemory(GetCurrentProcess(), (void *)(p2 + 0x58), &p3, 8, &got) && p3)
                    if (ReadProcessMemory(GetCurrentProcess(), (void *)(p3 + 0x20), &p2, 8, &got) && p2) {
                        short v = 0;
                        if (ReadProcessMemory(GetCurrentProcess(), (void *)(p2 + 0xC), &v, 2, &got))
                            action = v;
                    }
    }
    void *ctx = (void *)(ULONG_PTR)g_last_ctx;
    void *ca = ctx ? *(void **)((char *)ctx + 0x100) : NULL;
    void *cb = ctx ? *(void **)((char *)ctx + 0xE8) : NULL;

    log_line("STATE (%s) version=%s", why, MOD_VERSION);
    log_line("STATE anchors=%d/%d installed=%d mask=0x%04X slot=%d vk=%d "
             "gate=%d window=%dms reduction=%d%% recovery=%d hpmode=%d hppct=%g "
             "hpfixed=%g spd=%g/%dms dmgcut=%g/%dms armor=%d/%dms",
             anchors_ok, g_anchor_count, g_installed, g_cfg.guard_button_mask,
             g_cfg.pad_slot, g_cfg.guard_key_vk, g_cfg.gate_timely,
             g_cfg.window_ms, g_cfg.ki_reduction_percent, g_cfg.ki_recovery_mode,
             g_cfg.hp_recovery_mode, (double)g_cfg.hp_restore_percent,
             (double)g_cfg.hp_restore_fixed,
             (double)g_cfg.speed_buff_percent, g_cfg.speed_buff_ms,
             (double)g_cfg.damage_cut_percent, g_cfg.damage_cut_ms,
             g_cfg.armor_buff, g_cfg.armor_buff_ms);
    log_line("STATE input slot=0x%llX mgr=0x%llX pad=0x%04X conn=[%u,%u,%u,%u] "
             "guard_down=%d presses=%ld lt=%u rt=%u",
             g_input_mgr_slot, (unsigned long long)mgr, padword, flags[0], flags[1],
             flags[2], flags[3], g_guard_in.down, g_guard_presses,
             g_trig_lt, g_trig_rt);
    log_line("STATE player=0x%llX param=0x%llX hp=%d/%d ki=%.4g/%.4g "
             "guard_res flag=%d cur=%.4g cost=%.4g frame=%.4g action=%d",
             (unsigned long long)player, (unsigned long long)param, hp, maxhp,
             ki, maxki, eflag, ecur, ecost, mframe, action);
    log_line("STATE ctx=0x%llX charA=0x%llX charB=0x%llX | blocks=%ld perfect=%ld "
             "rewards=%ld freeguards=%ld rearmed=%ld purged=%ld | hp_restores=%ld "
             "hp_total=%ld hp_full=%ld | buff_started=%ld buff_ended=%ld "
             "buff_failed=%ld buff_installed=0x%X left spd=%lldms dmgcut=%lldms "
             "armor=%lldms",
             (unsigned long long)ctx, (unsigned long long)ca, (unsigned long long)cb,
             g_blocks, g_perfect, g_rewards, g_free,
             InterlockedCompareExchange(&g_rearm_writes, 0, 0),
             InterlockedCompareExchange(&g_purged_total, 0, 0),
             InterlockedCompareExchange(&g_hp_restores, 0, 0),
             InterlockedCompareExchange(&g_hp_restored, 0, 0),
             InterlockedCompareExchange(&g_hp_skipped, 0, 0),
             InterlockedCompareExchange(&g_buff_started, 0, 0),
             InterlockedCompareExchange(&g_buff_ended, 0, 0),
             InterlockedCompareExchange(&g_buff_failed, 0, 0),
             buff_installed_mask(),
             (long long)pg_buff_left_ms(&g_buff_speed, now_ms()),
             (long long)pg_buff_left_ms(&g_buff_dmgcut, now_ms()),
             (long long)pg_buff_left_ms(&g_buff_armor, now_ms()));
}

// Called from the worker loop: fire the automatic dump once a player exists.
static void maybe_auto_dump(void) {
    if (g_dumped_layout || !g_base) return;
    void *player = *(void **)(ULONG_PTR)(g_base + 0x18A0490);
    if (!player) return;
    g_dumped_layout = 1;
    dump_state("player object appeared - now in a mission");

    // First-run guidance. Every test on this machine showed no controller
    // connected, and the shipped defaults assume one, so say plainly what to do
    // rather than letting the player wonder why nothing happens.
    int connected = 0;
    void *mgr = g_input_mgr_slot ? *(void **)(ULONG_PTR)g_input_mgr_slot : NULL;
    if (mgr) {
        for (int i = 0; i < 4; ++i) {
            unsigned char f = 0;
            SIZE_T got = 0;
            if (ReadProcessMemory(GetCurrentProcess(),
                                  (char *)mgr + 0x49E04 + i * 20, &f, 1, &got) && f)
                connected++;
        }
    }
    if (!connected) {
        log_line("NOTICE: no controller is connected (all four pad slots report "
                 "disconnected). If you play with keyboard and mouse, the pad mask "
                 "0x%04X will never match and no reward will ever trigger.",
                 g_cfg.guard_button_mask);
        if (g_cfg.guard_key_vk == 0) {
            log_line("NOTICE: fix it in two steps - set LearnButtons=1 in the INI "
                     "(applies live), press your guard key in game, then copy the "
                     "value from the 'LEARN key VK=.. -> GuardKeyVK=..' line into "
                     "GuardKeyVK. Common codes: left Shift=160, Ctrl=17, Space=32, "
                     "mouse left=1.");
        }
    } else {
        log_line("NOTICE: %d controller slot(s) connected; guard mask 0x%04X on "
                 "slot %d.", connected, g_cfg.guard_button_mask, g_cfg.pad_slot);
    }
}

// Only the *defender's* resource is charged, so the entry pointer tells us who
// blocked. Comparing it against the player's own resource table is what makes
// "the player blocked" distinguishable from "the player's attack was blocked".
//
// This matters: the guard flag being written means "this character's attack was
// guarded", so without a filter the mod would also celebrate an *enemy* blocking
// the player's attack.
static int entry_belongs_to_player(unsigned long long entry) {
    if (!g_base || !entry) return 0;
    void *player = *(void **)(ULONG_PTR)(g_base + 0x18A0490);
    if (!player) return 0;
    void *param = *(void **)((char *)player + 0x240);
    if (!param) return 0;
    // resource table: entries at param+0xBB0 + i*0x50 for i in 0..15
    unsigned long long lo = (unsigned long long)(ULONG_PTR)param + 0xBB0;
    unsigned long long hi = lo + 0x500;
    return entry >= lo && entry < hi;
}

// Restore the *visible* Ki, because editing the guard-cost field alone cannot be
// shown to move the bar.
//
// Both fields are now known to sit in the resource table based at
// [[char+0x240]+0xBA8] (16 entries x 0x50, entry i at base+8+i*0x50):
//   * guard cost subtracts from entry 7  -> base+0x238 = param+0xDE0, +0x0C
//   * the walker at 0x7B4CB0 runs base+0x40 with stride 0x50 over 16 entries
// so they are the same table, not two unrelated ones -- an earlier note in this
// file said otherwise and was wrong.
//
// What is still unproven is whether entry+0x0C is the number the Ki *bar* shows.
// The bar's own accessor, Refer::StaminaRate (0x6EC800), reads param+0x40 and
// param+0x44 and returns current/max as a 0..1 ratio, which is strong evidence
// that param+0x40 is the displayed value (see RE_NOTES 4.22). The table entry
// looks like a pending/accumulated effect amount that is applied to it later.
//
// So: scale the cost at the subtract site *and* top the confirmed Ki field back
// up here. Both halves are then covered no matter which field the bar follows.
static PgKiRestore g_ki;
static unsigned long long g_ki_restore_until = 0;
static int g_ki_log = 0;

// Per-block copies taken when the snapshot is made, plus the verdict counter.
static float g_ki_cost = -1.0f;
static long g_ki_block = 0;
static int g_ki_verdicts = 0;
static int g_ki_flag_verdicts = 0;
static float g_ki_live = -1.0f;      // Ki at the event, before any reference work
static int g_ki_ref_log = 0;

// ------------------------------------------------ rolling pre-event Ki samples --
// The flag event source fires *after* the engine charged the Ki, so a reference
// read at event time already contains the charge and the top-up hands nothing back
// (pg_ki_ref explains the failure and the fix). The 8ms input tick therefore keeps
// a ring of samples and the reference is the highest value in a short window before
// the event.
//
// The writer is the input thread and the reader is the exception handler on a game
// thread, so this is a seqlock: the writer bumps the sequence before and after
// touching the ring; the reader accepts its copy only if the sequence did not
// change. A torn read is not an error, it just falls back to the live value for
// that block -- and the reader must never spin, because it runs inside VEH.
#define PG_KI_RING 64                     // 64 * 8ms ~= 512ms of history
static PgKiSample g_ki_ring[PG_KI_RING];
static volatile LONG g_ki_ring_seq = 0;
static volatile LONG g_ki_ring_head = 0;

static void ki_ring_push(float ki, unsigned long long ms) {
    if (!(ki >= 0.0f) || ki > 100000.0f) return;
    LONG head = InterlockedCompareExchange(&g_ki_ring_head, 0, 0);
    InterlockedIncrement(&g_ki_ring_seq);              // odd: update in progress
    PgKiSample *slot = &g_ki_ring[head & (PG_KI_RING - 1)];
    slot->ki = ki;
    slot->ms = ms ? ms : 1;                            // 0 marks an unwritten slot
    InterlockedIncrement(&g_ki_ring_head);             // publish, data already in
    InterlockedIncrement(&g_ki_ring_seq);              // even: stable again
}

// Copy the newest samples out, newest first. Bounded work, no waiting.
static int ki_ring_copy(PgKiSample *out, int cap) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        LONG s1 = InterlockedCompareExchange(&g_ki_ring_seq, 0, 0);
        if (s1 & 1) continue;                          // writer mid-update
        LONG head = InterlockedCompareExchange(&g_ki_ring_head, 0, 0);
        int n = 0;
        for (int i = 0; i < cap; ++i) {
            int idx = (int)((head - 1 - i) % PG_KI_RING);
            if (idx < 0) idx += PG_KI_RING;
            PgKiSample s = g_ki_ring[idx];
            if (s.ms == 0) break;                      // the rest is older than that
            out[n++] = s;
        }
        if (InterlockedCompareExchange(&g_ki_ring_seq, 0, 0) == s1) return n;
    }
    return 0;
}

// The Ki this block is measured against. `live` is used whenever the window is
// disabled, the ring is empty, or a writer was caught mid-update.
static float ki_pre_event_ref(float live) {
    if (g_cfg.ki_topup_preevent_ms <= 0) return live;
    PgKiSample snaps[PG_KI_REF_MAX_SAMPLES];
    int n = ki_ring_copy(snaps, PG_KI_REF_MAX_SAMPLES);
    return pg_ki_ref(snaps, n, now_ms(), (unsigned long long)g_cfg.ki_topup_preevent_ms,
                     live);
}

// One sample per input tick, so the ring always holds the value from just before
// whatever happens next.
static void ki_ring_tick(void) {
    if (!g_base || g_cfg.ki_topup_preevent_ms <= 0) return;
    void *player = *(void **)(ULONG_PTR)(g_base + 0x18A0490);
    if (!player) return;
    void *param = *(void **)((char *)player + 0x240);
    if (!param) return;
    ki_ring_push(*(float *)((char *)param + 0x40), now_ms());
}

// `from_flag` selects the reference rule: the flag source fires after the charge
// and needs the pre-event value, the cost source fires before it and must use the
// live one (a window there would re-refund a spend this block did not cause).
static void ki_snapshot(int from_flag) {
    if (!g_base) return;
    void *player = *(void **)(ULONG_PTR)(g_base + 0x18A0490);
    if (!player) return;
    void *param = *(void **)((char *)player + 0x240);
    if (!param) return;
    float ki = *(float *)((char *)param + 0x40);
    if (ki >= 0.0f && ki < 100000.0f) {
        g_ki_live = ki;
        float ref = from_flag ? ki_pre_event_ref(ki) : ki;
        if (ref > ki && g_ki_ref_log < 20) {
            log_line("KIREF pre-event reference %.5g vs live %.5g (window %dms): the "
                     "flag source fires after the charge, so the top-up is measured "
                     "against the value from before it",
                     ref, ki, g_cfg.ki_topup_preevent_ms);
            g_ki_ref_log++;
        }
        // The charge is read at the subtract site before the gate is evaluated, so
        // it is already available here; 0 means "not captured", which only disables
        // the refund cap rather than breaking anything.
        pg_ki_begin(&g_ki, ref, g_last_cost > 0.0f ? g_last_cost : 0.0f, now_ms());
        g_ki_cost = g_last_cost;      // charge this block was told to apply
        g_ki_block = g_last_cost_block;
        g_ki_restore_until = now_ms() + 400;
    }
}

// Top the visible Ki back up by exactly `reduction%` of the loss this block
// caused. The arithmetic lives in pg_logic.h (pg_ki_step) and is covered by
// tools/test_logic.c; the short version is that latching the peak loss and the
// total already handed back reaches `snapshot - peak_loss*(100-r)/100` exactly,
// where re-adding a fraction of the *remaining* loss each tick would have
// converged on a full restore and made the setting meaningless.
//
// Turns the one remaining static-analysis gap into a printed conclusion.
//
// We know the charge D the engine was told to apply to the resource-table entry
// (captured pre-scale, so it is the un-reduced cost), and we know L, the largest
// loss the *confirmed* Ki field actually took. Comparing them says which field a
// guard cost really moves. The classification itself lives in pg_logic.h
// (pg_kiv_classify) so it can be unit tested without the game.
//
// It refuses to answer when the configuration cannot separate the models: at the
// shipped default of 100% reduction, "scaled to zero" and "never moved" are the
// same number, and the original if/else chain resolved that by confidently
// picking the wrong one.
static const char *kiv_model_name(int model) {
    switch (model) {
    case PG_KIV_NO_MOVE:
        return "the confirmed Ki field did NOT move for this charge, so the "
               "resource-table entry is not what the bar shows";
    case PG_KIV_FULL_COST:
        return "the confirmed Ki field took the FULL cost: the scaling at the "
               "subtract site does not reach the bar, and the top-up is what "
               "spares Ki";
    case PG_KIV_SCALED_COST:
        return "the confirmed Ki field took the SCALED cost: the subtract-site "
               "scaling already reaches the bar, so the top-up double-counts";
    default:
        return NULL;
    }
}

static void ki_verdict(void) {
    if (g_ki_cost <= 0.0f) {
        // The flag event source: no charge was ever visible, so the cost-site
        // models say nothing about it. What still has to be reported is whether the
        // pre-event reference found a drop and the top-up handed it back -- that is
        // the question a user's report raised, and it used to be answered with
        // silence, which read as "the setting is broken" rather than "look here".
        if (g_ki_flag_verdicts >= 20) return;
        float l = pg_ki_attributed(&g_ki);
        if (l <= 0.0f) return;               // no drop was attributed: nothing to say
        unsigned long long delay =
            g_ki.first_loss_ms ? g_ki.first_loss_ms - g_ki.block_ms : 0;
        log_line("KIV-FLAG block #%ld reference R=%.5g live at event %.5g attributed "
                 "loss L=%.5g handed back %.5g/%d%% first loss after %llums -> the "
                 "flag source saw the drop the engine applied before the event",
                 g_ki_block, g_ki.snapshot, g_ki_live, l, g_ki.given,
                 g_cfg.ki_reduction_percent, delay);
        g_ki_flag_verdicts++;
        return;
    }
    if (g_ki_verdicts >= 40) return;
    float d = g_ki_cost;
    // Classify on the *attributed* loss (the part seen within PG_KI_ATTR_MS and
    // capped at the known charge). Using the whole-window peak would let an
    // unrelated Ki spend -- an immediate counterattack after the guard -- look
    // like the guard cost and corrupt the conclusion.
    float l = pg_ki_attributed(&g_ki);
    float scaled = d * (float)(100 - g_cfg.ki_reduction_percent) / 100.0f;
    PgKivVerdict v = pg_kiv_classify(d, l, g_cfg.ki_reduction_percent);
    const char *name = kiv_model_name(v.model);

    // How long the engine actually took to apply the charge. This is the number
    // that says whether the 400ms observation window is long enough at all.
    unsigned long long delay = g_ki.first_loss_ms ? g_ki.first_loss_ms - g_ki.block_ms : 0;
    const char *late = "";
    if (g_ki.first_loss_ms == 0) late = " [no loss seen at all]";
    else if (g_ki.attrib_loss <= 0.0f) late = " [loss arrived after the attribution window]";

    if (name) {
        log_line("KIV block #%ld charge D=%.5g visible loss L=%.5g scaled "
                 "D*(1-r)=%.5g residual=%.0f%% first loss after %llums peak=%.5g%s -> %s",
                 g_ki_block, d, l, scaled, v.residual * 100.0f, delay,
                 g_ki.peak_loss, late, name);
    } else if (!pg_kiv_resolvable(g_cfg.ki_reduction_percent)) {
        log_line("KIV block #%ld charge D=%.5g visible loss L=%.5g -> INCONCLUSIVE by "
                 "configuration: at KiDamageReductionPercent=%d the 'never moved' and "
                 "'scaled to zero' models predict the same loss. Set it to 50 and "
                 "guard a few more times to separate them.",
                 g_ki_block, d, l, g_cfg.ki_reduction_percent);
    } else if (v.models_overlap) {
        log_line("KIV block #%ld charge D=%.5g visible loss L=%.5g -> INCONCLUSIVE: "
                 "the loss lands between models that are too close to separate at "
                 "reduction=%d", g_ki_block, d, l, g_cfg.ki_reduction_percent);
    } else {
        log_line("KIV block #%ld charge D=%.5g visible loss L=%.5g scaled "
                 "D*(1-r)=%.5g residual=%.0f%% first loss after %llums peak=%.5g%s -> "
                 "INCONCLUSIVE: no model fits (partial propagation, or regeneration "
                 "ate the sample)",
                 g_ki_block, d, l, scaled, v.residual * 100.0f, delay,
                 g_ki.peak_loss, late);
    }
    g_ki_verdicts++;
}

static void ki_restore_tick(void) {
    if (g_ki.snapshot < 0.0f) return;
    if (now_ms() > g_ki_restore_until) {
        ki_verdict();                 // the window is over; say what it means
        pg_ki_end(&g_ki);
        return;
    }
    if (!g_base) return;
    void *player = *(void **)(ULONG_PTR)(g_base + 0x18A0490);
    if (!player) return;
    void *param = *(void **)((char *)player + 0x240);
    if (!param) return;
    float *ki = (float *)((char *)param + 0x40);
    float before = *ki;
    if (before < 0.0f) return;

    // Always sample -- the loss and the latency are what the verdict is built from
    // -- but only write when the top-up is enabled. KiTopUp=0 is the correct
    // setting in the world where the subtract-site scaling already reaches the bar,
    // because there the top-up would double-count.
    int effective = g_cfg.ki_topup ? g_cfg.ki_reduction_percent : 0;
    float after = pg_ki_step(&g_ki, before, effective, now_ms());
    if (after == before) return;
    *ki = after;
    if (g_ki_log < 30) {
        log_line("KIRESTORE %.4g -> %.4g (snapshot %.4g, attributed %.4g of charge "
                 "%.4g, peak loss %.4g, gave %.4g/%d%%)",
                 before, after, g_ki.snapshot, pg_ki_attributed(&g_ki), g_ki.charge,
                 g_ki.peak_loss, g_ki.given,
                 g_cfg.ki_reduction_percent);
        g_ki_log++;
    }
}

// Temporary diagnostic that resolves a real ambiguity:
//
// The guard-cost site subtracts from the resource-table entry at
// [[char+0x240]+0xBA8]+8+7*0x50, i.e. [[char+0x240]+0xDEC]. The engine's own
// Refer::Stamina getter reads a *different* field, [[char+0x240]+0x40]. Those
// are not the same address, so it is not yet proven that making the cost zero
// empties the Ki bar the player sees.
//
// Sampling both settles it: log whenever the entry moves (so we see the Ki at
// that instant), plus a slow periodic sample for the overall curve.
static float g_trace_ecur = -1.0f;
static float g_trace_ki = -1.0f;
static int g_trace_pass = 0;
static PgThrottle g_trace_thr;

// The whole point of this trace is to establish which of the two candidate Ki
// fields a *guard* actually moves, so it must watch both and say which one
// changed. Watching only entry7 meant that taking a hit without guarding -- which
// moves the visible Ki but may leave the guard entry alone -- produced no line at
// all until the slow heartbeat, and the reader could not tell which field had
// moved anyway.
//
// The entry-7 value is also cleared repeatedly by the engine's table walker
// (0x7B4CB0 writes 2 and 0 into every entry), so "changed" is true on nearly every
// tick; the throttle (pg_logic.h, unit tested) is what keeps it to one line per
// second, and the cap keeps a long session from bloating a file the user has to
// send back.
#define KITRACE_MIN_MS 1000
#define KITRACE_MAX_LINES 1200

static void ki_trace_sample(void) {
    if (!g_cfg.ki_trace || !g_base) return;
    void *player = *(void **)(ULONG_PTR)(g_base + 0x18A0490);
    if (!player) return;
    void *param = *(void **)((char *)player + 0x240);
    if (!param) return;

    float ki = 0.0f, maxki = 0.0f, ecur = 0.0f, ecost = 0.0f;
    int eflag = 0;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (char *)param + 0x40, &ki, 4, &got))
        return;
    ReadProcessMemory(GetCurrentProcess(), (char *)param + 0x44, &maxki, 4, &got);
    ReadProcessMemory(GetCurrentProcess(), (char *)param + 0xDE0, &eflag, 4, &got);
    ReadProcessMemory(GetCurrentProcess(), (char *)param + 0xDEC, &ecur, 4, &got);
    ReadProcessMemory(GetCurrentProcess(), (char *)param + 0xDF0, &ecost, 4, &got);

    int ki_moved = (g_trace_ki < 0.0f) || (ki != g_trace_ki);
    int e_moved = (g_trace_ecur < 0.0f) || (ecur != g_trace_ecur);
    g_trace_ki = ki;
    g_trace_ecur = ecur;

    const char *tag = "";
    if (ki_moved && e_moved) tag = "  <-- changed: ki AND entry7";
    else if (ki_moved)       tag = "  <-- changed: ki";
    else if (e_moved)        tag = "  <-- changed: entry7";

    int heartbeat = (++g_trace_pass % 200) == 0;      // ~5s
    if (!ki_moved && !e_moved && !heartbeat) return;

    switch (pg_throttle(&g_trace_thr, heartbeat, now_ms(), KITRACE_MIN_MS,
                        KITRACE_MAX_LINES)) {
    case PG_THROTTLE_CAPPED:
        log_line("KITRACE capped at %d lines; further samples suppressed "
                 "(set KiTrace=0 to silence it entirely)", KITRACE_MAX_LINES);
        return;
    case PG_THROTTLE_EMIT:
        log_line("KITRACE ki=%.4g/%.4g | entry7 flag=%d cur=%.4g cost=%.4g%s",
                 ki, maxki, eflag, ecur, ecost, tag);
        return;
    default:
        return;
    }
}

// Cancel the guard recovery so the player can act sooner.
//
// Refer::MotionFrame reads the float at [[char+0x38]+0x60], so advancing that
// value skips the rest of the current action's animation. Writing animation
// state blind is the riskiest thing this mod does, so it is off by default, it
// only ever *adds* to a value that already looks like a motion frame, and it
// bails out rather than writing anything implausible.
static void apply_cancel_recovery(void *player) {
    if (!g_cfg.cancel_recovery || !player) return;
    void *mobj = *(void **)((char *)player + 0x38);
    if (!mobj) return;
    float *frame = (float *)((char *)mobj + 0x60);
    float v = *frame;
    if (!(v >= 0.0f && v < 10000.0f)) return;      // not a motion frame: do nothing
    *frame = v + g_cfg.cancel_recovery_frames;
    if (g_recovery_log < 20) {
        log_line("RECOVERY cancel: motion frame %.3g -> %.3g", v, *frame);
        g_recovery_log++;
    }
}

// Enemy effects for "Ki damage / HP damage on the attacker".
//
// The hit context carries both characters: the dispatcher at 0x6FE5B0 loads
// [ctx+0x100] and [ctx+0xE8] and only accepts a pointer whose type word at +4 is
// zero (the engine's own validity test). Whichever of the two is not the player
// is the attacker. Both use the same Character layout the player does, so Ki and
// HP live at [[char+0x240]+0x40] and [[char+0x240]+0x20] - the exact fields the
// engine's Refer::Stamina and Refer::Hp getters read.
static void apply_enemy_effects(void *ctx) {
    if (!ctx) return;
    if (g_cfg.enemy_ki_damage <= 0.0f && g_cfg.enemy_hp_damage <= 0.0f) return;
    if (!g_base) return;

    void *cand[2];
    cand[0] = *(void **)((char *)ctx + 0x100);
    cand[1] = *(void **)((char *)ctx + 0xE8);
    void *player = *(void **)(ULONG_PTR)(g_base + 0x18A0490);

    // Only ever write to the *other* combatant once the player has been located
    // among the two. Picking "whichever is not the player" without that check
    // would, in any context that does not contain the player, hand us an
    // arbitrary Character and write Ki/HP damage into it.
    int player_seen = 0;
    for (int i = 0; i < 2; ++i) if (cand[i] && cand[i] == player) player_seen = 1;
    if (!player_seen) {
        if (g_enemy_log < 5) {
            log_line("ENEMY effects skipped: the player (0x%llX) is neither ctx+0x100 "
                     "(0x%llX) nor ctx+0xE8 (0x%llX); refusing to guess",
                     (unsigned long long)player, (unsigned long long)cand[0],
                     (unsigned long long)cand[1]);
            g_enemy_log++;
        }
        return;
    }

    void *enemy = NULL;
    for (int i = 0; i < 2; ++i) {
        void *p = cand[i];
        if (!p || p == player) continue;
        unsigned short type = 0;
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (char *)p + 4, &type, 2, &got) ||
            got != 2 || type != 0) {
            continue;               // same validity test the engine applies
        }
        enemy = p;
        break;
    }
    if (!enemy) return;

    void *param = *(void **)((char *)enemy + 0x240);
    if (!param) return;

    if (g_cfg.enemy_ki_damage > 0.0f) {
        float *ki = (float *)((char *)param + 0x40);
        float v = *ki;
        if (v >= 0.0f && v < 100000.0f) {          // sanity gate before writing
            float was = v;
            v -= g_cfg.enemy_ki_damage;
            if (v < 0.0f) v = 0.0f;
            *ki = v;
            if (g_enemy_log < 30) {
                log_line("ENEMY ki damage %.3g -> %.3g (enemy=0x%llX)", was, v,
                         (unsigned long long)enemy);
                g_enemy_log++;
            }
        }
    }
    if (g_cfg.enemy_hp_damage > 0.0f) {
        int *hp = (int *)((char *)param + 0x20);
        int v = *hp;
        if (v > 0 && v < 100000000) {              // sanity gate before writing
            int was = v;
            v -= (int)g_cfg.enemy_hp_damage;
            if (v < 0) v = 0;
            *hp = v;
            if (g_enemy_log < 30) {
                log_line("ENEMY hp damage %d -> %d (enemy=0x%llX)", was, v,
                         (unsigned long long)enemy);
                g_enemy_log++;
            }
        }
    }
}

// ------------------------------------------- hardware-breakpoint self-test ----
//
// Everything in this mod rests on one assumption that reading code cannot
// confirm, and that a play session cannot distinguish from "the player never
// blocked": that an x64 execution breakpoint in DR0..DR3, dispatched through our
// VEH, actually fires inside nioh.exe. A wrong DR7 encoding -- or a kernel or
// hypervisor policy that virtualises the debug registers -- would silently
// produce zero events and no crash whatsoever.
//
// So we prove it on our own code, with no game state involved: arm DR0 on a probe
// function, call it five times from a throwaway thread, and count how many of
// those calls the VEH dispatcher actually saw. Measured on this toolchain,
// Dr7 = L0 only (RW=00, LEN=00 -> execute, 1 byte) traps 5/5, the resume flag
// stops it re-firing on the same instruction, and a non-matching target traps 0/5
// while all five calls still execute.

static volatile LONG g_selftest_hits = 0;
static volatile LONG g_selftest_calls = 0;
static unsigned long long g_selftest_target = 0;
static char g_selftest_result[192] = "not run";

__attribute__((noinline)) static void selftest_probe(void) {
    InterlockedIncrement(&g_selftest_calls);
}

static DWORD WINAPI selftest_thread(LPVOID unused) {
    (void)unused;
    for (int i = 0; i < 5; ++i) selftest_probe();
    return 0;
}

// A new thread's DR7 is 0 on this platform, but preserving whatever reserved
// bits the OS did install (bit 10 in particular) costs nothing and avoids
// depending on that measurement holding everywhere.
static DWORD64 dr7_for_exec_slot0(DWORD64 current) {
    DWORD64 d = current;
    d &= ~(DWORD64)0xFF;            // L0..G3
    d &= ~(DWORD64)0xFFFF0000;      // RW/LEN fields of all four slots
    return d | 0x1;                 // L0 set, RW=00 LEN=00 -> execute, 1 byte
}

static int run_breakpoint_selftest(void) {
    g_selftest_target = (unsigned long long)(ULONG_PTR)&selftest_probe;
    InterlockedExchange(&g_selftest_hits, 0);
    InterlockedExchange(&g_selftest_calls, 0);

    DWORD tid = 0;
    HANDLE th = CreateThread(NULL, 0, selftest_thread, NULL, CREATE_SUSPENDED, &tid);
    if (!th) {
        snprintf(g_selftest_result, sizeof(g_selftest_result),
                 "FAILED: cannot create the probe thread (err=%lu)", GetLastError());
        g_selftest_target = 0;
        return 0;
    }

    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    int armed = 0;
    if (GetThreadContext(th, &ctx)) {
        ctx.Dr0 = g_selftest_target;
        ctx.Dr1 = ctx.Dr2 = ctx.Dr3 = 0;
        ctx.Dr7 = dr7_for_exec_slot0(ctx.Dr7);
        armed = SetThreadContext(th, &ctx) ? 1 : 0;
    }
    ResumeThread(th);
    WaitForSingleObject(th, 3000);
    CloseHandle(th);

    LONG hits = InterlockedCompareExchange(&g_selftest_hits, 0, 0);
    LONG calls = InterlockedCompareExchange(&g_selftest_calls, 0, 0);
    g_selftest_target = 0;

    if (!armed) {
        snprintf(g_selftest_result, sizeof(g_selftest_result),
                 "FAILED: could not write DR0 on the probe thread (err=%lu)",
                 GetLastError());
        return 0;
    }
    if (hits == 5 && calls == 5) {
        snprintf(g_selftest_result, sizeof(g_selftest_result),
                 "OK: 5/5 probe calls trapped by the VEH (DR0=%p, Dr7=L0 exec)",
                 (void *)(ULONG_PTR)selftest_probe);
        return 1;
    }
    snprintf(g_selftest_result, sizeof(g_selftest_result),
             "FAILED: only %ld/%ld probe calls trapped -- execution breakpoints do "
             "not fire in this process, so no anchor can ever trigger", hits, calls);
    return 0;
}

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    unsigned long long rip = c->Rip;
    if (g_selftest_target && rip == g_selftest_target) {
        c->EFlags |= 0x10000;
        InterlockedIncrement(&g_selftest_hits);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    for (int i = 0; i < g_anchor_count; ++i) {
        if (!g_anchor[i].enabled) continue;
        if (rip != g_base + g_anchor[i].rva) continue;
        // Resume Flag: without it the execution breakpoint fires again immediately.
        c->EFlags |= 0x10000;
        g_last_ctx = (unsigned long long)c->Rsi;   // hit context in this function
        if (i == 0 || i == 1) {
            on_guard_flag(i, c);
        } else if (i == 2) {
            on_guard_cost(c);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ------------------------------------------------------------------- hotkey -

static void poll_hotkey(void) {
    if (!g_cfg.diagnostic_hotkey) return;
    static int was_down = 0;
    int down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
               (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
               (GetAsyncKeyState(VK_F10) & 0x8000);
    if (down && !was_down) {
        log_line("MANUAL EXPORT: version=%s blocks=%ld perfect=%ld rewards=%ld "
                 "freeguards=%ld", MOD_VERSION, g_blocks, g_perfect, g_rewards,
                 g_free);
        dump_state("hotkey Ctrl+Shift+F10");
    }
    was_down = down;
}

// ---------------------------------------------------------------- worker ----

// Separate fast poller: an 8 ms cadence is needed to catch the guard-press edge,
// while the main worker loop runs at 100 ms for the heavier thread enumeration.
static DWORD WINAPI input_thread(LPVOID param) {
    (void)param;
    Sleep(2500);
    while (1) {
        poll_guard_button();
        action_cancel_followup();
        ki_restore_tick();
        // After the restore, so the ring records the value the player will actually
        // have if a block lands before the next tick.
        ki_ring_tick();
        // Owns the timed-buff watchdog: applying/removing engine state happens
        // here and nowhere else, never in the exception handler.
        buff_watchdog_config();
        buff_tick();
        lw_refresh_state();
        lw_diag();
        lw_scan();
        poll_hotkey();
        Sleep(8);
    }
    return 0;
}

// Sound is played by a thread that does nothing but wait for the "play" event.
//
// The worker used to drain requests on its 25ms tick, so the parry sound could
// start up to 25ms late -- on top of the audio engine's own buffering -- which is
// audible as "the sound lags behind the guard". Waiting on an event removes the
// polling delay entirely. This thread, not the worker, now owns the COM apartment:
// CoInitializeEx and XAudio2Create happen here, and so does every playback.
static DWORD WINAPI audio_thread(LPVOID param) {
    (void)param;
    if (!g_cfg.sound_enabled) {
        log_line("SOUND disabled by configuration");
        return 0;
    }
    char wav[MAX_PATH * 2];
    snprintf(wav, sizeof(wav), "%sSounds\\%s", g_dir, g_cfg.sound_file);
    for (int attempt = 0; attempt < 40 && !g_audio_ok; ++attempt) {
        if (!g_wav && !wav_load(wav)) {
            log_line("SOUND disabled: cannot open %s", wav);
            return 0;                       // a missing file will not appear later
        }
        audio_init();
        if (!g_audio_ok) Sleep(250);
    }
    if (!g_audio_ok) {
        log_line("SOUND thread: the audio engine never became ready");
        return 0;
    }
    log_line("SOUND ready for immediate playback (event driven, no polling delay)");
    while (1) {
        WaitForSingleObject(g_sound_event, INFINITE);
        if (g_audio_ok) audio_play();
    }
    return 0;
}

static DWORD WINAPI worker(LPVOID param) {
    (void)param;
    Sleep(1500);

    lstrcpyA(g_ini_path, g_dir);
    lstrcatA(g_ini_path, "Nioh1PerfectGuard.ini");
    lstrcpyA(g_log_path, g_dir);
    lstrcatA(g_log_path, "Nioh1PerfectGuard.gameplay.log");

    log_line("=== Nioh1PerfectGuard %s ===", MOD_VERSION);
    config_load(1);
    // After the log path exists (so the notice lands in the gameplay log).
    state_load();
    log_line("CONFIG enabled=%d window=%dms reduction=%d%% topup=%d recovery=%d "
             "gate_timely=%d",
             g_cfg.enabled, g_cfg.window_ms, g_cfg.ki_reduction_percent,
             g_cfg.ki_topup, g_cfg.ki_recovery_mode, g_cfg.gate_timely);

    g_base = (unsigned long long)(ULONG_PTR)GetModuleHandleA("nioh.exe");
    if (!g_base) {
        g_base = (unsigned long long)(ULONG_PTR)GetModuleHandleA(NULL);
        log_line("WARNING nioh.exe module not found; using main module base=0x%llX", g_base);
    } else {
        log_line("base(nioh.exe)=0x%llX", g_base);
    }

    // Sound initialisation and playback belong to the dedicated audio thread (see
    // audio_thread): it owns the COM apartment, and it waits on an event so the
    // sound starts immediately instead of on a 25ms poll.

    g_valid_count = anchor_verify();

    // The cost site is the authoritative "the player blocked" event: without it
    // the mod can observe and log but can never actually reward anything. Saying
    // "ACTIVE" in that state would be a silent no-op, so call it out.
    int can_reward = (g_anchor_count > 2) && g_anchor[2].enabled;
    int can_observe = g_anchor[0].enabled || g_anchor[1].enabled;
    if (!can_reward) {
        log_line("WARNING: anchor '%s' failed to verify, so no reward can ever be "
                 "applied. The mod will still observe and log blocking, but "
                 "reduction/recovery/enemy effects will do nothing.",
                 g_anchor[2].name);
    }
    if (!can_observe) {
        log_line("WARNING: neither guard-flag anchor verified; block logging will be "
                 "empty. This build's guard-resolution code does not match.");
    }

    if (g_cfg.enabled && g_valid_count >= 2) {
        log_line("INSTALL step=veh");
        g_veh = AddVectoredExceptionHandler(1, veh_handler);
        log_line("INSTALL step=arm veh=%p", g_veh);
        if (!g_veh) {
            // Arming without a handler is not a degraded mode, it is a crash:
            // the CPU raises EXCEPTION_SINGLE_STEP at the armed address, nothing
            // handles it, and the process dies. So refuse outright.
            log_line("WARNING: AddVectoredExceptionHandler failed (err=%lu); refusing "
                     "to arm any breakpoint at all", GetLastError());
        } else {
            // Set before arming: arm_new_threads()/rearm_batch() refuse to touch a
            // debug register unless a handler is already in place.
            g_installed = 1;
            if (g_cfg.diag_disable & 1) {
                // Diagnostic: handler in place but nothing armed, so a crash can be
                // attributed to the arming rather than to the DLL being loaded.
                g_no_arm = 1;
                log_line("NOTICE: DiagDisable=1 -- no breakpoint will be armed; the "
                         "mod cannot detect anything in this state");
            } else {
                arm_new_threads();
            }
            log_line("INSTALL step=arm done");
            // Prove the trap mechanism works in *this* process before relying on it.
            if (g_cfg.diag_disable & 8) {
                snprintf(g_selftest_result, sizeof(g_selftest_result),
                         "skipped (DiagDisable=8)");
            } else {
                run_breakpoint_selftest();
            }
            log_line("SELFTEST breakpoint: %s", g_selftest_result);
            if (g_cfg.diag_disable & 2) {
                log_line("NOTICE: DiagDisable=2 -- guard input polling is off, so no "
                         "press can ever be detected");
            } else {
                HANDLE ith = CreateThread(NULL, 0, input_thread, NULL, 0, NULL);
                log_line("INSTALL step=inputthread ith=%p", ith);
                if (ith) CloseHandle(ith);
            }
            // Event-driven playback thread: see audio_thread.
            HANDLE ath = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
            log_line("INSTALL step=audiothread ith=%p", ath);
            if (ath) CloseHandle(ath);
        }
        snprintf(g_status, sizeof(g_status),
                 "%s anchors=%d/%d reduction=%d%% recovery=%d gate=%d "
                 "mask=0x%04X slot=%d sound=%d",
                 !g_veh ? "NOT INSTALLED(no handler)"
                        : (can_reward ? "ACTIVE" : "DEGRADED(no-reward)"),
                 g_valid_count, g_anchor_count, g_cfg.ki_reduction_percent,
                 g_cfg.ki_recovery_mode, g_cfg.gate_timely,
                 g_cfg.guard_button_mask, g_cfg.pad_slot, g_audio_ok);
    } else if (!g_cfg.enabled) {
        snprintf(g_status, sizeof(g_status), "BYPASS: Enabled=0");
    } else {
        snprintf(g_status, sizeof(g_status),
                 "NOT INSTALLED: only %d/%d anchors verified", g_valid_count,
                 g_anchor_count);
    }
    log_line("STATUS %s", g_status);
    // One-time, read-only: resolve the engine's node names while we are inside the
    // process (see the node-registry section). Its output is what the remaining
    // reverse engineering needs, and it cannot be obtained from outside because the
    // registry lives on the heap.
    dump_node_registry_once();

    int pass = 0;
    while (1) {
        // Poll fast enough that the parry sound is not audibly late: the worker
        // owns the COM apartment, so it is the only thread allowed to play it.
        // At the old 100ms tick a perfect guard could be silent for a tenth of a
        // second, which reads as "the sound is broken" rather than "it is late".
        // The expensive work keeps its previous cadence (16 * 25ms = 400ms).
        if (++pass % 16 == 0) {
            if (!g_no_arm) arm_new_threads();
            // The rolling re-arm is OPT-IN and off by default: see the comment on
            // rearm_batch(). DiagDisable=4 turns it back on for investigation.
            if (g_cfg.diag_disable & 4) rearm_batch();
        }
        if (pass % 16 == 0) ini_poll();
        maybe_auto_dump();
        ki_trace_sample();
        Sleep(25);
    }
    return 0;
}

// --------------------------------------------------------------- exports ----

__declspec(dllexport) const char *PG_GetVersion(void) { return MOD_VERSION; }

__declspec(dllexport) const char *PG_GetStatus(void) { return g_status; }

// Lets a harness trigger the same full snapshot the hotkey produces. It must not
// permanently redirect the gameplay log, so the path is saved and restored.
__declspec(dllexport) int PG_DumpState(void) {
    char saved[MAX_PATH * 2];
    lstrcpynA(saved, g_log_path, sizeof(saved));
    lstrcpyA(g_log_path, g_dir);
    lstrcatA(g_log_path, "Nioh1PerfectGuard.selftest.log");
    dump_state("exported PG_DumpState");
    lstrcpynA(g_log_path, saved, sizeof(g_log_path));
    return 0;
}

__declspec(dllexport) long PG_GetBlocks(void) { return g_blocks; }
__declspec(dllexport) long PG_GetPerfect(void) { return g_perfect; }
__declspec(dllexport) long PG_GetRewards(void) { return g_rewards; }
__declspec(dllexport) long PG_GetFreeGuards(void) { return g_free; }
__declspec(dllexport) const char *PG_GetInputInfo(void) {
    static char buf[256];
    snprintf(buf, sizeof(buf),
             "slot=0x%llX mgr=0x%llX buttons=0x%04X lt=%u rt=%u guard_down=%d "
             "presses=%ld last_press_age=%llums",
             g_input_mgr_slot, g_input_mgr_va, g_last_buttons,
             g_trig_lt, g_trig_rt, g_guard_in.down,
             g_guard_presses,
             g_guard_in.press_ms ? now_ms() - g_guard_in.press_ms : 0);
    return buf;
}

__declspec(dllexport) int PG_ReloadConfig(void) { return config_load(0); }

// Runs the engine-independent parts (config + audio) and reports a one-line
// result, so the whole pipeline can be validated without launching the game.
//
// It reports EVERY configuration key, deliberately: tools/test_ini_encodings.py
// compares this output against the shipped INI, so a key whose compiled default
// drifts away from the INI is caught. Reporting only some of them left most keys
// (including three added later) outside that net.
__declspec(dllexport) const char *PG_SelfTest(void) {
    static char out[1024];
    char saved_ini[MAX_PATH * 2], saved_log[MAX_PATH * 2];
    Config saved_cfg;
    int saved_loaded;

    if (g_cfg_mutex) WaitForSingleObject(g_cfg_mutex, 2000);
    lstrcpynA(saved_ini, g_ini_path, sizeof(saved_ini));
    lstrcpynA(saved_log, g_log_path, sizeof(saved_log));
    saved_cfg = g_cfg;
    saved_loaded = g_cfg_loaded;

    lstrcpyA(g_ini_path, g_dir);
    lstrcatA(g_ini_path, "Nioh1PerfectGuard.ini");
    lstrcpyA(g_log_path, g_dir);
    lstrcatA(g_log_path, "Nioh1PerfectGuard.selftest.log");

    config_defaults(&g_cfg);
    g_cfg_loaded = 0;
    int rc = config_load_inner(1);

    char wav[MAX_PATH * 2];
    snprintf(wav, sizeof(wav), "%sSounds\\%s", g_dir, g_cfg.sound_file);
    int w = wav_load(wav);
    int a = w ? audio_init() : 0;
    if (a) audio_play();

    snprintf(out, sizeof(out),
             "config_rc=%d enabled=%d window=%d cancel=%d cancelframes=%g "
             "reduction=%d topup=%d preevent=%d recovery=%d fixed=%g gate=%d mask=0x%04X "
             "padslot=%d vk=%d learn=%d trace=%d enemyki=%g enemyhp=%g "
             "sound_enabled=%d vol=%.2f file=%s hotkey=%d diag=%d eventsrc=%d "
             "hpmode=%d hppct=%g hpfixed=%g spdpct=%g spdms=%d dmgcutpct=%g "
             "dmgcutms=%d armor=%d armorms=%d lwgauge=%d lwpct=%d lwext=%d "
             "lwextpct=%d | wav=%d audio=%d",
             rc, g_cfg.enabled, g_cfg.window_ms, g_cfg.cancel_recovery,
             (double)g_cfg.cancel_recovery_frames, g_cfg.ki_reduction_percent,
             g_cfg.ki_topup, g_cfg.ki_topup_preevent_ms, g_cfg.ki_recovery_mode,
             (double)g_cfg.fixed_recovery,
             g_cfg.gate_timely, g_cfg.guard_button_mask, g_cfg.pad_slot,
             g_cfg.guard_key_vk, g_cfg.learn_buttons, g_cfg.ki_trace,
             (double)g_cfg.enemy_ki_damage, (double)g_cfg.enemy_hp_damage,
             g_cfg.sound_enabled, g_cfg.sound_volume, g_cfg.sound_file,
             g_cfg.diagnostic_hotkey, g_cfg.diag_disable, g_cfg.block_event_source,
             g_cfg.hp_recovery_mode, (double)g_cfg.hp_restore_percent,
             (double)g_cfg.hp_restore_fixed, (double)g_cfg.speed_buff_percent,
             g_cfg.speed_buff_ms, (double)g_cfg.damage_cut_percent,
             g_cfg.damage_cut_ms, g_cfg.armor_buff, g_cfg.armor_buff_ms,
             g_cfg.lw_gauge_on, g_cfg.lw_gauge_percent, g_cfg.lw_extend_on,
             g_cfg.lw_extend_percent, w, a);

    g_cfg = saved_cfg;
    g_cfg_loaded = saved_loaded;
    lstrcpynA(g_ini_path, saved_ini, sizeof(g_ini_path));
    lstrcpynA(g_log_path, saved_log, sizeof(g_log_path));
    if (g_cfg_mutex) ReleaseMutex(g_cfg_mutex);
    return out;
}

// Reports the in-process hardware-breakpoint self-test, so a harness can confirm
// the trap mechanism without reading the gameplay log.
__declspec(dllexport) const char *PG_GetSelfTest(void) {
    return g_selftest_result;
}

// Numeric fingerprint of the shared decision logic, computed by the *shipped*
// binary. tools/test_logic_digest.py compares it against the value the standalone
// test build prints, so a codegen difference between the two (FP contraction,
// reassociation, a different inlining decision that changes an intermediate
// rounding) cannot silently invalidate the unit tests.
__declspec(dllexport) const char *PG_GetLogicDigest(void) {
    static char buf[32];
    snprintf(buf, sizeof(buf), "%08X", pg_logic_digest());
    return buf;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        char *slash = g_dir;
        for (char *p = g_dir; *p; ++p) if (*p == '\\') slash = p;
        if (slash) slash[1] = 0;
        g_log_mutex = CreateMutexA(NULL, FALSE, NULL);
        g_cfg_mutex = CreateMutexA(NULL, FALSE, NULL);
        g_sound_event = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (InterlockedCompareExchange(&g_started, 1, 0) == 0) {
            HANDLE th = CreateThread(NULL, 0, worker, NULL, 0, NULL);
            if (th) CloseHandle(th);
        }
    }
    return TRUE;
}
