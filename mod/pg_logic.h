// Pure decision logic for the perfect-guard mod.
//
// Everything here is deliberately free of Win32, engine state and globals, so it
// can be exercised offline by tools/test_logic.c without launching the game.
// The mod and the tests include this *same* header, so the tests cannot drift
// away from what actually ships.
//
// Nothing in this file may allocate, log, or touch memory outside its arguments.
#ifndef PG_LOGIC_H
#define PG_LOGIC_H

// The perfect-guard window: a block counts only if a guard press happened no
// more than `window_ms` before it.
//
// `press_ms == 0` means "no press has ever been seen", which must never open the
// gate. A clock that appears to go backwards (now < press) also closes it rather
// than underflowing into a huge elapsed time.
static inline int pg_gate_open(int gate_timely, unsigned long long press_ms,
                              unsigned long long now, int window_ms) {
    if (!gate_timely) return 1;              // configured as "every block counts"
    if (!press_ms) return 0;
    if (now < press_ms) return 0;
    return (now - press_ms) <= (unsigned long long)window_ms;
}

// Guard is "down" if either device says so.
static inline int pg_down(unsigned short buttons, unsigned short mask, int key_down) {
    return ((buttons & mask) != 0) || (key_down != 0);
}

// Rising-edge detector, kept as a pure function of (previous, current) so the
// "held guard must not re-arm the window" rule is testable.
static inline int pg_is_new_press(int was_down, int down) {
    return down && !was_down;
}

// ---------------------------------------------------------------------------
// Ki top-up and loss attribution.
//
// `KiDamageReductionPercent` is enforced by topping the confirmed Ki field back
// up. Three things have to be got right, and the first version got two of them
// wrong:
//
//  1. Re-adding "reduction% of the remaining loss" every tick converges
//     geometrically on a *full* restore (the tick is 8ms, so a 400ms window is
//     ~50 iterations; even 20% leaves 0.8^50 ~= 1e-5). So latch the peak and only
//     ever add the shortfall.
//
//  2. The player can spend Ki right after a perfect guard -- an immediate
//     counterattack is the natural response. Refunding a fraction of *whatever
//     the field lost* would refund that attack as if it were the guard cost, i.e.
//     make the follow-up free. The guard cost D is known (captured at the
//     subtract site), so the refund is capped at D's share.
//
//  3. Which loss counts as "the guard cost" matters for the diagnosis too. A
//     spend that arrives much later is probably unrelated, so the attribution
//     uses only the loss seen within PG_KI_ATTR_MS of the block, while the
//     whole-window peak is kept for reporting. The delay to the first observed
//     loss is also recorded: it measures how long the engine actually takes to
//     apply the charge, which is exactly what decides whether the window is long
//     enough at all.
typedef struct {
    float snapshot;      // Ki when the block happened; < 0 means "no window"
    float charge;        // un-scaled guard cost D the engine was told to apply
    float peak_loss;     // largest loss seen anywhere in the window
    float attrib_loss;   // largest loss within PG_KI_ATTR_MS of the block
    float given;         // total already handed back
    unsigned long long block_ms;
    unsigned long long first_loss_ms;   // 0 = no loss was ever observed
} PgKiRestore;

// How long after a block a Ki drop is still attributed to the guard cost. The
// charge lands within a frame or two; an unrelated spend needs the player to
// input an action, which takes far longer than this.
#define PG_KI_ATTR_MS 250

static inline void pg_ki_begin(PgKiRestore *s, float snapshot, float charge,
                              unsigned long long now_ms_v) {
    s->snapshot = snapshot;
    s->charge = charge;
    s->peak_loss = 0.0f;
    s->attrib_loss = 0.0f;
    s->given = 0.0f;
    s->block_ms = now_ms_v;
    s->first_loss_ms = 0;
}

static inline void pg_ki_end(PgKiRestore *s) {
    s->snapshot = -1.0f;
    s->charge = 0.0f;
    s->peak_loss = 0.0f;
    s->attrib_loss = 0.0f;
    s->given = 0.0f;
    s->first_loss_ms = 0;
}

// Observed loss attributed to this block, capped by the charge we know the engine
// was told to apply. Returns -1 when no charge is known (then nothing is capped).
static inline float pg_ki_attributed(const PgKiRestore *s) {
    float base = s->attrib_loss;
    if (s->charge > 0.0f && base > s->charge) base = s->charge;
    return base;
}

// Returns the Ki value to write (unchanged when nothing should be added).
//
// Tracking is deliberately independent of the refund: even with the top-up
// switched off (or a reduction of 0) the loss, the attribution and the observed
// latency are still recorded, because that is exactly the evidence the `KIV` line
// needs in order to say whether the subtract-site scaling reaches the bar. An
// earlier version returned early on reduction<=0 and lost the measurement along
// with the write.
static inline float pg_ki_step(PgKiRestore *s, float now_value,
                              int reduction_percent, unsigned long long now_ms_v) {
    if (s->snapshot < 0.0f) return now_value;
    if (now_value < 0.0f) return now_value;

    float loss = s->snapshot - now_value;
    if (loss <= 0.0f) return now_value;              // at or above the snapshot

    if (s->first_loss_ms == 0) s->first_loss_ms = now_ms_v;
    if (loss > s->peak_loss) s->peak_loss = loss;
    if (now_ms_v >= s->block_ms && now_ms_v - s->block_ms <= PG_KI_ATTR_MS &&
        loss > s->attrib_loss) {
        s->attrib_loss = loss;
    }

    if (reduction_percent <= 0) return now_value;    // observe only, never write

    float want = pg_ki_attributed(s) * (float)reduction_percent / 100.0f;
    float delta = want - s->given;
    if (delta <= 0.0f) return now_value;             // already handed back our share

    float target = now_value + delta;
    if (target > s->snapshot) target = s->snapshot;
    float applied = target - now_value;
    if (applied <= 0.0f) return now_value;

    s->given += applied;
    return target;
}

// ---------------------------------------------------------------------------
// Restoring the player's HP on a perfect guard.
//
// This is the only reward in the mod that heals, so the safety rules matter more
// than the arithmetic:
//
//  * The field offsets are read, not inferred, and are sanity-gated before any
//    write. Max HP is an int at param+0x18 and current HP at param+0x20 -- the
//    same pair `LAYOUT` prints and the same pair the enemy effects write. A
//    moved offset would otherwise let the mod write a plausible-looking number
//    into whatever now lives there.
//  * Never revive. A guard event while current HP is <= 0 is not a heal, it is
//    the engine reporting a dead character; writing there could resurrect the
//    player and desync the death that is already in flight.
//  * Never exceed maximum, and never write when the amount rounds to zero: a
//    "successful" restore of 0 HP would be logged as a reward that visibly did
//    nothing.
#define PG_HP_OFF 0                  // no HP restore at all
#define PG_HP_PERCENT 1              // HpRestorePercent of maximum HP
#define PG_HP_FIXED 2                // HpRestoreFixed, flat
#define PG_HP_PERCENT_AND_FIXED 3    // both, added together

typedef enum {
    PG_HP_APPLIED = 0,      // write `after` to the current-HP field
    PG_HP_DISABLED,         // HpRecoveryMode=0, or the configured amount is 0
    PG_HP_FULL,             // already at maximum; nothing to do (normal, not an error)
    PG_HP_INVALID,          // the HP pair does not look like HP -> refuse to write
    PG_HP_DEAD,             // current HP <= 0: never revive
    PG_HP_NO_AMOUNT         // the configured amount rounds to 0 whole HP
} PgHpOutcome;

typedef struct {
    int before;
    int after;        // the value to write (== before when not applied)
    int amount;       // HP actually added (0 when not applied)
    int clamped;      // the restore would have overshot maximum
    PgHpOutcome outcome;
} PgHpResult;

// Upper bound on a believable maximum HP. Nioh 1 values are in the hundreds to
// low thousands; this only has to be loose enough never to reject a real build
// while still catching a pointer that now points at something else.
#define PG_HP_MAX_SANE 100000000

// Percent is truncated to whole HP (3% of 880 = 26, not 27). Truncation is the
// conservative direction and keeps the behaviour explainable in the log.
static inline PgHpResult pg_hp_restore(int cur, int max, int mode,
                                       float percent, float fixed) {
    PgHpResult r;
    r.before = cur;
    r.after = cur;
    r.amount = 0;
    r.clamped = 0;
    r.outcome = PG_HP_APPLIED;

    if (mode == PG_HP_OFF) { r.outcome = PG_HP_DISABLED; return r; }
    if (max <= 0 || max > PG_HP_MAX_SANE || cur < 0 || cur > max) {
        r.outcome = PG_HP_INVALID;
        return r;
    }
    if (cur <= 0) { r.outcome = PG_HP_DEAD; return r; }
    if (cur >= max) { r.outcome = PG_HP_FULL; return r; }

    int add = 0;
    if (mode == PG_HP_PERCENT || mode == PG_HP_PERCENT_AND_FIXED) {
        if (percent > 0.0f) {
            float p = (float)max * percent / 100.0f;
            if (p > (float)PG_HP_MAX_SANE) p = (float)PG_HP_MAX_SANE;
            if (p > 0.0f) add += (int)p;
        }
    }
    if (mode == PG_HP_FIXED || mode == PG_HP_PERCENT_AND_FIXED) {
        if (fixed > 0.0f) {
            float f = fixed;
            if (f > (float)PG_HP_MAX_SANE) f = (float)PG_HP_MAX_SANE;
            add += (int)f;
        }
    }
    if (add <= 0) { r.outcome = PG_HP_NO_AMOUNT; return r; }

    int target = cur + add;                 // bounded: both sides <= PG_HP_MAX_SANE
    if (target > max) { target = max; r.clamped = 1; }
    r.amount = target - cur;
    if (r.amount <= 0) { r.outcome = PG_HP_NO_AMOUNT; return r; }
    r.after = target;
    return r;
}

// ---------------------------------------------------------------------------
// Timed buffs: a perfect guard grants a temporary effect (speed / damage cut /
// armour). This is the first *stateful* reward in the mod -- everything before
// it was "write a few fields at the event and be done".
//
// The rules the tests pin down, all of which have a way to go wrong:
//
//  * One proc **refreshes** the timer to the full duration; repeated procs never
//    stack the magnitude (the caller applies a fixed value, not a sum).
//  * Expiry fires **exactly once**, on the tick that observes it. A buff that
//    reported expiry on every later tick would, in the mod, issue its "remove the
//    engine state" request forever.
//  * A duration of 0 means "off", not "expires immediately": an operator who
//    sets `SpeedBuffMs=0` should get no buff at all rather than a buff that is
//    applied and removed every tick.
//  * A clock that jumps backwards must not extend a buff forever. `until_ms` is
//    an absolute deadline, so a backwards clock only makes the buff look longer
//    than it is until the clock catches up -- and `pg_buff_step` still expires it
//    the moment `now` reaches the deadline, never before.
typedef struct {
    int active;
    long procs;                    // procs seen (arming + refreshes)
    unsigned long long started_ms; // when the current window began
    unsigned long long until_ms;   // absolute deadline; 0 when inactive
} PgBuff;

static inline void pg_buff_init(PgBuff *b) {
    b->active = 0;
    b->procs = 0;
    b->started_ms = 0;
    b->until_ms = 0;
}

// Arm the buff, or refresh it if already active. Returns 1 only on the
// inactive -> active transition (that is the moment worth logging).
//
// A non-positive duration means "this proc grants nothing": it does not arm, and
// it deliberately leaves an existing window untouched (an operator who sets the
// duration to 0 should get no *new* buff, not have a running one cut short). The
// invariant `until_ms != 0  <=>  active` holds in every path, which is what makes
// the state unambiguous when dumped or logged.
static inline int pg_buff_start(PgBuff *b, unsigned long long now, int duration_ms) {
    if (duration_ms <= 0) return 0;
    int was = b->active;
    b->procs++;
    b->started_ms = now;
    b->until_ms = now + (unsigned long long)duration_ms;
    b->active = 1;
    return !was;
}

// Call once per tick. Returns 1 exactly once, on the tick that expires it.
static inline int pg_buff_step(PgBuff *b, unsigned long long now) {
    if (!b->active) return 0;
    if (now >= b->until_ms) {
        b->active = 0;
        b->until_ms = 0;
        return 1;
    }
    return 0;
}

static inline long long pg_buff_left_ms(const PgBuff *b, unsigned long long now) {
    if (!b->active) return 0;
    if (now >= b->until_ms) return 0;
    return (long long)(b->until_ms - now);
}

// ---------------------------------------------------------------------------
// Diagnostic-stream throttle.
//
// A "log whenever the value changed" stream is only safe if the value changes
// rarely. The entry-7 Ki value is cleared repeatedly by the engine's table
// walker, so that condition is true every tick -- which is how an unthrottled
// diagnostic turns into megabytes per hour of open/write/close in the game
// process.
typedef struct {
    int lines;
    int capped;
    unsigned long long last_ms;
} PgThrottle;

#define PG_THROTTLE_DROP 0
#define PG_THROTTLE_EMIT 1
#define PG_THROTTLE_CAPPED 2   // emit the "no more lines" notice, exactly once

static inline void pg_throttle_init(PgThrottle *t) {
    t->lines = 0;
    t->capped = 0;
    t->last_ms = 0;
}

// `urgent` bypasses the minimum gap (used for heartbeats and for the lines that
// matter most). Returns PG_THROTTLE_*.
static inline int pg_throttle(PgThrottle *t, int urgent, unsigned long long now,
                             unsigned long long min_gap_ms, int max_lines) {
    if (t->lines >= max_lines) {
        if (!t->capped) {
            t->capped = 1;
            return PG_THROTTLE_CAPPED;
        }
        return PG_THROTTLE_DROP;
    }
    if (!urgent && now >= t->last_ms && (now - t->last_ms) < min_gap_ms)
        return PG_THROTTLE_DROP;
    t->last_ms = now;
    t->lines++;
    return PG_THROTTLE_EMIT;
}

// ---------------------------------------------------------------------------
// Guard-cost model classification (the `KIV` line).
//
// We know D, the un-scaled guard cost the engine was told to charge, and L, the
// largest loss the confirmed Ki field actually took. Three models could explain L:
//
//   NO_MOVE      L = 0            the field does not follow the guard cost at all
//   FULL_COST    L = D            the subtract-site scaling never reaches it, so
//                                 the mod's own top-up is what spares Ki
//   SCALED_COST  L = D*(1-r/100)  the scaling does reach it, so the top-up
//                                 double-counts
//
// The original version was an if/else chain with a fixed 15% band, and it was
// confidently WRONG at the settings people actually run:
//
//   * r = 100 (the shipped default) makes SCALED_COST equal 0, i.e. identical to
//     NO_MOVE. The chain tested "L < band" first and therefore announced "the Ki
//     field did not move, so the resource table is not what the bar shows" -- a
//     false conclusion drawn from a configuration that cannot distinguish the two.
//   * r = 0 makes SCALED_COST equal FULL_COST, symmetrically.
//
// So classify by nearest model, and refuse to answer when two models are closer
// together than the measurement can resolve, or when the fit is simply poor.

#define PG_KIV_INCONCLUSIVE (-1)
#define PG_KIV_NO_MOVE 0
#define PG_KIV_FULL_COST 1
#define PG_KIV_SCALED_COST 2

// Two models closer than this fraction of D cannot be told apart by a single
// loss measurement. 0.20D separates r=20 comfortably and flags r=10 as hopeless.
#define PG_KIV_MIN_SEPARATION 0.20f

// A best-fit residual worse than this fraction of D means the loss matches no
// model well (partial propagation, or regeneration eating into the sample).
#define PG_KIV_MAX_RESIDUAL 0.25f

typedef struct {
    int model;         // PG_KIV_* (INCONCLUSIVE when the answer is refused)
    float residual;    // |L - model_L| / D  (always reported, for the log)
    int models_overlap;   // two candidate models are too close to separate
    int weak_margin;      // the winner is not clearly closer than the runner-up
    int poor_fit;         // even the winner is far away
} PgKivVerdict;

static inline float pg_kiv_abs(float v) { return v < 0.0f ? -v : v; }

static inline PgKivVerdict pg_kiv_classify(float D, float L, int reduction_percent) {
    PgKivVerdict v;
    v.model = PG_KIV_INCONCLUSIVE;
    v.residual = 0.0f;
    v.models_overlap = 0;
    v.weak_margin = 0;
    v.poor_fit = 0;
    if (!(D > 0.0f)) return v;

    float models[3];
    models[PG_KIV_NO_MOVE] = 0.0f;
    models[PG_KIV_FULL_COST] = D;
    models[PG_KIV_SCALED_COST] = D * (float)(100 - reduction_percent) / 100.0f;

    // (a) Can any two models be told apart by any measurement at all?
    float need = PG_KIV_MIN_SEPARATION * D;
    for (int i = 0; i < 3; ++i)
        for (int j = i + 1; j < 3; ++j)
            if (pg_kiv_abs(models[i] - models[j]) < need) v.models_overlap = 1;

    // nearest and runner-up
    int best = 0, second = 1;
    float bestd = pg_kiv_abs(models[0] - L);
    float secondd = pg_kiv_abs(models[1] - L);
    if (secondd < bestd) { float t = bestd; bestd = secondd; secondd = t; int ti = best; best = second; second = ti; }
    for (int i = 2; i < 3; ++i) {
        float d = pg_kiv_abs(models[i] - L);
        if (d < bestd) { secondd = bestd; second = best; bestd = d; best = i; }
        else if (d < secondd) { secondd = d; second = i; }
    }
    (void)second;

    v.residual = bestd / D;
    // (b) The winner must be clearly closer than the runner-up. Without this, a
    //     loss sitting between two models gets silently assigned to whichever is
    //     a hair nearer -- exactly the kind of confident-but-arbitrary answer this
    //     classifier exists to avoid.
    v.weak_margin = (bestd > 0.5f * secondd);
    // (c) ...and not absurdly far either.
    v.poor_fit = (v.residual > PG_KIV_MAX_RESIDUAL);

    v.model = best;
    if (v.models_overlap || v.weak_margin || v.poor_fit) v.model = PG_KIV_INCONCLUSIVE;
    return v;
}

// The reduction values at which the classification is even possible.
static inline int pg_kiv_resolvable(int reduction_percent) {
    // Mirrors PG_KIV_MIN_SEPARATION: |D - D*(1-r)| and |0 - D*(1-r)| must both be
    // at least 20% of D, i.e. r in [20, 80].
    return reduction_percent >= 20 && reduction_percent <= 80;
}

// ---------------------------------------------------------------------------
// The perfect-guard chain, as a state machine.
//
// The pieces above are individually trivial, and that is exactly how the worst
// bug in this project survived several rounds: the guard flag means "this
// character's attack was guarded", so an enemy blocking the player's attack ran
// the same reward path. What was wrong was the *ordering and the subject* of the
// decision, not any single test. So the chain itself lives here, where it can be
// driven offline through whole scenarios (tools/test_logic.c) instead of being
// re-assembled by hand in the mod and in the tests.
typedef struct {
    unsigned long long press_ms;   // when guard was last pressed; 0 = never
    int down;                      // currently held
    long presses;                  // rising edges seen
} PgGuardInput;

static inline void pg_guard_init(PgGuardInput *g) {
    g->press_ms = 0;
    g->down = 0;
    g->presses = 0;
}

// Feed one input sample. Returns 1 when this sample is a fresh press, which is
// the only moment the window (re)opens.
static inline int pg_guard_update(PgGuardInput *g, int down, unsigned long long now) {
    int fresh = pg_is_new_press(g->down, down);
    if (fresh) {
        g->press_ms = now;
        g->presses++;
    }
    g->down = down;
    return fresh;
}

static inline int pg_guard_gate(const PgGuardInput *g, int gate_timely,
                               unsigned long long now, int window_ms) {
    return pg_gate_open(gate_timely, g->press_ms, now, window_ms);
}

// What one guard-cost event means.
//
// The order matters and is part of the contract: who is blocking is decided
// FIRST, before any timing test. An enemy's block is not "the player blocked too
// late", it is not the player at all, and it must never consume the player's
// window or produce a reward.
typedef enum {
    PG_BLOCK_OTHER = 0,     // somebody else blocked; ignore entirely
    PG_BLOCK_LATE = 1,      // the player blocked, but not near a guard press
    PG_BLOCK_PERFECT = 2    // the player blocked within the window
} PgBlockOutcome;

static inline PgBlockOutcome pg_classify_block(int entry_is_player,
                                              const PgGuardInput *g, int gate_timely,
                                              unsigned long long now, int window_ms) {
    if (!entry_is_player) return PG_BLOCK_OTHER;
    if (!pg_guard_gate(g, gate_timely, now, window_ms)) return PG_BLOCK_LATE;
    return PG_BLOCK_PERFECT;
}

// When a "this attack was guarded" flag is written, the hit context names both
// combatants ([ctx+0x100], [ctx+0xE8]) and the attacker ([ctx+0x40]). The flag is
// set on the *attacker's* side, so the event means "the player blocked" exactly
// when the player is one of the two combatants and is NOT the attacker.
//
// The attacker test comes first and wins: when an enemy guards the player's own
// attack, the player IS the attacker, which is precisely the case that must not be
// rewarded (the mistake that motivated this whole check).
static inline int pg_flag_means_player_blocked(int a_is_player, int b_is_player,
                                              int attacker_is_player) {
    if (attacker_is_player) return 0;
    return a_is_player || b_is_player;
}

// ---------------------------------------------------------------------------
// "Guard pressed on its own" -- the input test for cancelling an attack.
//
// The request is: the guard key cancels the player's attack action, but only when
// guard is pressed *by itself*; guard + X/Y/A (a deliberate combination) must not
// cancel. Moving the stick is irrelevant -- movement is not consulted at all, which
// is also why this function takes no stick input.
//
// The subtlety is what "by itself" means while an attack is already running: the
// attack button is often still held from the attack being cancelled, so a rule of
// "no attack button may be down" would refuse to cancel exactly when the player
// wants it. What actually distinguishes a combination is that the attack button is
// pressed *at the same moment* as guard, so the test is edge-based.
//
// `strict_hold` offers the literal reading (any attack button held blocks it) for
// anyone who prefers that; which one feels right depends on how the player holds
// the buttons, so both exist and the INI chooses.
#define PG_COMBO_MS 100

// `attack` is the edge tracker for the attack buttons (pg_guard_update drives it);
// `guard_fresh` is 1 only on the sample where guard went down.
static inline int pg_guard_alone(int guard_fresh, const PgGuardInput *attack,
                                unsigned long long now, int combo_ms, int strict_hold) {
    if (!guard_fresh) return 0;
    if (strict_hold && attack->down) return 0;
    if (attack->press_ms) {
        if (now >= attack->press_ms) {
            if (now - attack->press_ms <= (unsigned long long)combo_ms) return 0;
        } else if (attack->press_ms - now <= (unsigned long long)combo_ms) {
            return 0;          // an attack press marginally later in the same press
        }
    }
    return 1;
}

// One physical block can be visible to both event sources: the engine may charge
// the guard Ki and set the guard flag for the same hit, and if the source decision
// changes between those two events, both would reward it. Collapse them by time.
//
// `last_ms` is the timestamp of the previous reward (0 = none yet).
static inline int pg_dedupe_ok(unsigned long long last_ms, unsigned long long now,
                              unsigned long long window_ms) {
    if (last_ms && now >= last_ms && now - last_ms < window_ms) return 0;
    return 1;
}

// ---------------------------------------------------------------------------
// Deterministic digest of this header's numeric behaviour.
//
// The unit tests and the shipped DLL compile the same header, but as different
// artefacts (an executable vs. a shared library, possibly with different flags).
// That makes "the arithmetic we tested is the arithmetic we ship" an assumption.
// This digest turns it into a check: tools/test_logic_digest.py compares the value
// exported by Nioh1PerfectGuard.dll against the value printed by test_logic.exe,
// so a codegen difference (FP contraction, reassociation, a different inlining
// decision that changes an intermediate rounding) shows up immediately instead of
// silently invalidating the test suite.
//
// It must stay free of libc: pure integer and float arithmetic on locals only.

static inline unsigned int pg_fold_bits(unsigned int h, unsigned int w) {
    h ^= w;
    h *= 16777619u;          // FNV-1a
    return h;
}

static inline unsigned int pg_fold_float(unsigned int h, float v) {
    union { float f; unsigned int u; } bits;
    bits.f = v;
    return pg_fold_bits(h, bits.u);
}

static inline unsigned int pg_logic_digest(void) {
    unsigned int h = 2166136261u;

    // 1. window boundaries
    for (int r = 0; r <= 100; r += 10)
        for (int off = -2; off <= 2; ++off)
            h = pg_fold_bits(h, (unsigned)pg_gate_open(1, 1000, 1000 + off, r));
    h = pg_fold_bits(h, (unsigned)pg_gate_open(0, 0, 5000, 250));
    h = pg_fold_bits(h, (unsigned)pg_gate_open(1, 0, 5000, 250));
    h = pg_fold_bits(h, (unsigned)pg_gate_open(1, 5000, 4000, 250));

    // 2. button / edge logic
    for (unsigned mask = 0; mask <= 0x0300; mask += 0x0081)
        for (unsigned b = 0; b <= 0x0300; b += 0x0055) {
            h = pg_fold_bits(h, (unsigned)pg_down((unsigned short)b,
                                                  (unsigned short)mask, 0));
            h = pg_fold_bits(h, (unsigned)pg_down((unsigned short)b,
                                                  (unsigned short)mask, 1));
            h = pg_fold_bits(h, (unsigned)pg_is_new_press(1, 1));
        }

    // 3. top-up and attribution across a grid of charges, reductions and lags
    {
        const float charges[4] = {0.0f, 6.4f, 20.0f, 41.5f};
        const int reds[5] = {0, 20, 50, 80, 100};
        const unsigned lags[5] = {0, 8, 40, 250, 400};
        for (int ci = 0; ci < 4; ++ci)
            for (int ri = 0; ri < 5; ++ri)
                for (int li = 0; li < 5; ++li) {
                    PgKiRestore s;
                    pg_ki_begin(&s, 100.0f, charges[ci], 1000);
                    float v = 100.0f - charges[ci];
                    v = pg_ki_step(&s, v, reds[ri], 1000 + lags[li]);
                    h = pg_fold_float(h, v);
                    h = pg_fold_float(h, pg_ki_attributed(&s));
                    h = pg_fold_float(h, s.peak_loss);
                    h = pg_fold_float(h, s.attrib_loss);
                    h = pg_fold_float(h, s.given);
                    h = pg_fold_bits(h, (unsigned)s.first_loss_ms);
                    // a second, later, larger loss part-way through
                    v = pg_ki_step(&s, 100.0f - charges[ci] - 7.25f, reds[ri],
                                   1000 + lags[li] + 300);
                    h = pg_fold_float(h, v);
                    h = pg_fold_float(h, s.given);
                }
    }

    // 4. verdict classification over the same grid
    {
        const float ds[3] = {1.0f, 6.4f, 100.0f};
        const float fracs[7] = {0.0f, 0.15f, 0.3f, 0.5f, 0.7f, 0.9f, 1.0f};
        const int reds[5] = {0, 20, 50, 80, 100};
        for (int di = 0; di < 3; ++di)
            for (int fi = 0; fi < 7; ++fi)
                for (int ri = 0; ri < 5; ++ri) {
                    PgKivVerdict v = pg_kiv_classify(ds[di], ds[di] * fracs[fi],
                                                     reds[ri]);
                    h = pg_fold_bits(h, (unsigned)(v.model + 1));
                    h = pg_fold_float(h, v.residual);
                    h = pg_fold_bits(h, (unsigned)v.models_overlap);
                    h = pg_fold_bits(h, (unsigned)v.weak_margin);
                    h = pg_fold_bits(h, (unsigned)v.poor_fit);
                    h = pg_fold_bits(h, (unsigned)pg_kiv_resolvable(reds[ri]));
                }
    }

    // 5. throttle decisions
    {
        PgThrottle t;
        pg_throttle_init(&t);
        for (unsigned long long ms = 0; ms < 3000; ms += 137) {
            h = pg_fold_bits(h, (unsigned)pg_throttle(&t, 0, ms, 1000, 7));
            h = pg_fold_bits(h, (unsigned)t.lines);
        }
        for (int i = 0; i < 12; ++i)
            h = pg_fold_bits(h, (unsigned)pg_throttle(&t, 1, 4000 + i, 1000, 7));
    }

    // 6. the chain itself, so the ordering rules are covered by the
    //    tested==shipped check too
    {
        for (int window = 0; window <= 500; window += 125) {
            for (int timely = 0; timely < 2; ++timely) {
                PgGuardInput g;
                pg_guard_init(&g);
                int downs[6] = {1, 1, 0, 1, 0, 1};
                unsigned long long t = 1000;
                for (int i = 0; i < 6; ++i) {
                    h = pg_fold_bits(h, (unsigned)pg_guard_update(&g, downs[i], t));
                    h = pg_fold_bits(h, (unsigned)g.presses);
                    h = pg_fold_float(h, (float)g.press_ms);
                    for (unsigned lag = 0; lag <= 600; lag += 150) {
                        for (int subject = 0; subject < 2; ++subject) {
                            h = pg_fold_bits(h, (unsigned)pg_classify_block(
                                subject, &g, timely, t + lag, window));
                        }
                    }
                    t += 200;
                }
            }
        }
    }

    // 7. the flag-based subject test and the reward dedupe
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b)
            for (int atk = 0; atk < 2; ++atk)
                h = pg_fold_bits(h, (unsigned)pg_flag_means_player_blocked(a, b, atk));
    for (unsigned long long last = 0; last <= 4000; last += 1500)
        for (unsigned long long now = 0; now <= 4000; now += 500)
            h = pg_fold_bits(h, (unsigned)pg_dedupe_ok(last, now, 250));

    // 8. HP restore across the modes, the clamping boundary and the refusal cases
    {
        const int curs[5] = {0, 1, 550, 880, 881};
        const int maxs[3] = {0, 880, 100000001};
        const float pcts[4] = {0.0f, 3.0f, 50.0f, 250.0f};
        const float fixeds[3] = {0.0f, 50.0f, 100000.0f};
        for (int mode = 0; mode <= 3; ++mode)
            for (int ci = 0; ci < 5; ++ci)
                for (int mi = 0; mi < 3; ++mi)
                    for (int pi = 0; pi < 4; ++pi)
                        for (int fi = 0; fi < 3; ++fi) {
                            PgHpResult r = pg_hp_restore(curs[ci], maxs[mi], mode,
                                                         pcts[pi], fixeds[fi]);
                            h = pg_fold_bits(h, (unsigned)r.outcome);
                            h = pg_fold_bits(h, (unsigned)r.before);
                            h = pg_fold_bits(h, (unsigned)r.after);
                            h = pg_fold_bits(h, (unsigned)r.amount);
                            h = pg_fold_bits(h, (unsigned)r.clamped);
                        }
    }

    // 9. timed buffs: arm / refresh / expire-once / remaining time
    {
        const int durations[4] = {0, 1, 5000, 10000};
        const unsigned long long ticks[7] = {0, 1, 4999, 5000, 5001, 20000, 30000};
        for (int di = 0; di < 4; ++di) {
            PgBuff b;
            pg_buff_init(&b);
            h = pg_fold_bits(h, (unsigned)pg_buff_step(&b, 0));      // step while idle
            h = pg_fold_bits(h, (unsigned)pg_buff_start(&b, 100, durations[di]));
            h = pg_fold_bits(h, (unsigned)b.active);
            h = pg_fold_bits(h, (unsigned)b.procs);
            for (int ti = 0; ti < 7; ++ti) {
                unsigned long long t = 100 + ticks[ti];
                h = pg_fold_bits(h, (unsigned)pg_buff_step(&b, t));
                h = pg_fold_bits(h, (unsigned)pg_buff_left_ms(&b, t));
                h = pg_fold_bits(h, (unsigned)b.active);
            }
            // a refresh part-way through must extend, not stack
            h = pg_fold_bits(h, (unsigned)pg_buff_start(&b, 5200, durations[di]));
            for (int ti = 0; ti < 7; ++ti) {
                unsigned long long t = 5200 + ticks[ti];
                h = pg_fold_bits(h, (unsigned)pg_buff_step(&b, t));
                h = pg_fold_bits(h, (unsigned)pg_buff_left_ms(&b, t));
            }
            // a backwards clock must not wedge it active
            h = pg_fold_bits(h, (unsigned)pg_buff_start(&b, 100000, 5000));
            h = pg_fold_bits(h, (unsigned)pg_buff_step(&b, 100500));
            h = pg_fold_bits(h, (unsigned)pg_buff_step(&b, 99000));
            h = pg_fold_bits(h, (unsigned)pg_buff_left_ms(&b, 99000));
            h = pg_fold_bits(h, (unsigned)b.active);
        }
    }

    // 10. guard-alone: the combination window in both directions
    {
        const int combos[4] = {0, 50, 100, 250};
        const unsigned long long atk[6] = {0, 900, 1000, 1050, 1100, 1200};
        for (int ci = 0; ci < 4; ++ci) {
            for (int ai = 0; ai < 6; ++ai) {
                PgGuardInput a;
                pg_guard_init(&a);
                if (atk[ai]) pg_guard_update(&a, 1, atk[ai]);
                h = pg_fold_bits(h, (unsigned)pg_guard_alone(1, &a, 1000,
                                                             combos[ci], 0));
                h = pg_fold_bits(h, (unsigned)pg_guard_alone(1, &a, 1000,
                                                             combos[ci], 1));
                h = pg_fold_bits(h, (unsigned)pg_guard_alone(0, &a, 1000,
                                                             combos[ci], 0));
                // an attack button that is held but was pressed long ago
                pg_guard_update(&a, 1, 400);
                h = pg_fold_bits(h, (unsigned)pg_guard_alone(1, &a, 1000,
                                                             combos[ci], 0));
            }
        }
    }

    return h;
}

#endif // PG_LOGIC_H
