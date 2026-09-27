// Offline unit tests for the perfect-guard decision logic.
//
// Includes mod/pg_logic.h -- the exact header the shipped DLL compiles -- so a
// passing test is evidence about the mod, not about a copy of it.
//
// Build and run (no game required):
//   zig cc -target x86_64-windows-gnu -O2 -o test_logic.exe test_logic.c
#include <stdio.h>
#include "../mod/pg_logic.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (cond) { ++g_pass; }                                             \
        else { ++g_fail; printf("  FAIL %s:%d: ", __FILE__, __LINE__);      \
               printf(__VA_ARGS__); printf("\n"); }                         \
    } while (0)

static void test_gate(void) {
    printf("gate / window\n");
    // timely gate disabled -> every block counts, even with no press at all
    CHECK(pg_gate_open(0, 0, 12345, 250) == 1, "gate_timely=0 must always open");
    CHECK(pg_gate_open(0, 100, 100, 250) == 1, "gate_timely=0 must always open");

    // never pressed -> closed
    CHECK(pg_gate_open(1, 0, 5000, 250) == 0, "no press must keep the gate closed");

    // boundaries: inclusive at exactly window_ms
    CHECK(pg_gate_open(1, 1000, 1000, 250) == 1, "0ms after the press is inside");
    CHECK(pg_gate_open(1, 1000, 1250, 250) == 1, "exactly window_ms is inside");
    CHECK(pg_gate_open(1, 1000, 1251, 250) == 0, "window_ms+1 is outside");

    // a held guard does not extend the window
    CHECK(pg_gate_open(1, 1000, 2000, 250) == 0, "stale press must not open the gate");

    // clock going backwards must not underflow into "inside the window"
    CHECK(pg_gate_open(1, 5000, 4000, 250) == 0, "backwards clock must close the gate");

    // window of 0 still allows an instantaneous block
    CHECK(pg_gate_open(1, 700, 700, 0) == 1, "0ms window allows same-ms block");
    CHECK(pg_gate_open(1, 700, 701, 0) == 0, "0ms window rejects 1ms late");
}

static void test_edge(void) {
    printf("press edge detection (%d frames)\n", 50);
    int was = 0, presses = 0;
    // hold guard for 50 frames: exactly one press must be registered
    for (int i = 0; i < 50; ++i) {
        int down = pg_down(0x0100, 0x0100, 0);
        if (pg_is_new_press(was, down)) ++presses;
        was = down;
    }
    CHECK(presses == 1, "a held guard must register exactly 1 press, got %d", presses);

    // tap twice
    was = 0; presses = 0;
    const int seq[] = {1, 1, 0, 0, 1, 1, 0};
    for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); ++i) {
        int down = pg_down((unsigned short)(seq[i] ? 0x0100 : 0), 0x0100, 0);
        if (pg_is_new_press(was, down)) ++presses;
        was = down;
    }
    CHECK(presses == 2, "two taps must register 2 presses, got %d", presses);
}

static void test_mask(void) {
    printf("button mask / keyboard\n");
    // L1 on a pad
    CHECK(pg_down(0x0100, 0x0100, 0) == 1, "pad 0x0100 with mask 0x0100 is down");
    CHECK(pg_down(0x0000, 0x0100, 0) == 0, "no pad bits is up");
    CHECK(pg_down(0x0200, 0x0100, 0) == 0, "a different pad bit must not match");
    CHECK(pg_down(0x0101, 0x0100, 0) == 1, "extra bits alongside the mask still match");
    // keyboard-only player
    CHECK(pg_down(0x0000, 0x0100, 1) == 1, "keyboard must work with no pad at all");
    // a mask of 0 must not report "always down"
    CHECK(pg_down(0xFFFF, 0x0000, 0) == 0, "mask 0 must never report down");
    // keyboard off (vk 0) -> key_down always 0, pad decides
    CHECK(pg_down(0x0000, 0x0100, 0) == 0, "no device pressed is up");
}

// One block, engine charged D and it either did or did not propagate our scaling
// to the visible field. The mod must land on D*(1-r) in the "did not" world and
// must never exceed the snapshot in either.
static float restore_run(float D, int r, int engine_applied_scaling, int ticks) {
    PgKiRestore s;
    pg_ki_begin(&s, 100.0f, D, 1000);
    float now = 100.0f - (engine_applied_scaling ? D * (100 - r) / 100.0f : D);
    for (int t = 0; t < ticks; ++t) {
        float next = pg_ki_step(&s, now, r, 1000 + (unsigned)t * 8);
        if (next == now) break;      // settled
        now = next;
    }
    return now;
}

static void test_restore(void) {
    printf("Ki top-up arithmetic\n");
    const float D = 40.0f;

    // reduction 0: the mod must not add anything at all
    CHECK(restore_run(D, 0, 0, 50) == 60.0f, "0%% must leave the full loss");
    CHECK(restore_run(D, 0, 1, 50) == 60.0f, "0%% must leave the full loss (scaled world)");

    // engine did NOT propagate the scaling -> land exactly on D*(1-r)
    CHECK(restore_run(D, 20, 0, 50) == 100.0f - D * 0.80f, "20%% -> loss 32, got %g",
          D - restore_run(D, 20, 0, 50));
    CHECK(restore_run(D, 50, 0, 50) == 100.0f - D * 0.50f, "50%% -> loss 20, got %g",
          D - restore_run(D, 50, 0, 50));
    CHECK(restore_run(D, 100, 0, 50) == 100.0f, "100%% -> full restore");

    // engine DID propagate -> the top-up double-counts (known, reported by KIV),
    // but it must still never exceed the snapshot
    float over = restore_run(D, 50, 1, 50);
    CHECK(over <= 100.0f, "must never exceed the snapshot, got %g", over);

    // the old buggy behaviour is what we are guarding against: re-adding
    // reduction%% of the *remaining* loss each tick converges on a full restore
    {
        float now = 60.0f;
        for (int t = 0; t < 50; ++t) {
            float loss = 100.0f - now;
            now += loss * 0.20f;
        }
        CHECK(now > 99.9f, "the old per-tick formula really did restore everything");
        float fixed = restore_run(D, 20, 0, 50);
        CHECK(fixed < now, "the new formula must NOT restore everything (%g < %g)",
              fixed, now);
    }

    // idempotent: once settled, more ticks change nothing
    {
        PgKiRestore s;
        pg_ki_begin(&s, 100.0f, D, 0);
        float now = 60.0f;
        for (int t = 0; t < 10; ++t) now = pg_ki_step(&s, now, 50, 1000 + t * 8);
        float settled = now;
        for (int t = 0; t < 10; ++t) now = pg_ki_step(&s, now, 50, 1000 + t * 8);
        CHECK(now == settled, "must be idempotent once settled (%g vs %g)", now, settled);
    }

    // no window -> never touch anything
    {
        PgKiRestore s;
        pg_ki_end(&s);
        CHECK(pg_ki_step(&s, 10.0f, 100, 0) == 10.0f, "closed window must not modify Ki");
    }

    // Ki regenerating above the snapshot must not be pulled back down
    {
        PgKiRestore s;
        pg_ki_begin(&s, 100.0f, D, 0);
        CHECK(pg_ki_step(&s, 120.0f, 100, 0) == 120.0f, "above snapshot must be left alone");
    }
}

static void test_attribution(void) {
    printf("loss attribution and the refund cap\n");
    const float D = 20.0f;

    // A Ki spend *after* the attribution window must not be refunded as if it were
    // the guard cost: perfect guard, then immediately attack. Without the cap the
    // follow-up attack would come out free.
    {
        PgKiRestore s;
        pg_ki_begin(&s, 100.0f, D, 1000);
        // the real guard cost lands promptly, and at r=100 it is refunded in full
        float v = pg_ki_step(&s, 100.0f - D, 100, 1016);
        CHECK(v == 100.0f, "the guard cost is refunded in full at r=100, got %g", v);
        CHECK(pg_ki_attributed(&s) == D, "the prompt loss is attributed");
        // ...then the player counterattacks 600ms later and spends 50 of their own
        v = pg_ki_step(&s, 100.0f - 50.0f, 100, 1600);
        CHECK(pg_ki_attributed(&s) == D,
              "a late spend must not be attributed, got %g", pg_ki_attributed(&s));
        CHECK(v == 100.0f - 50.0f,
              "the unrelated spend must survive intact, got loss %g", 100.0f - v);
    }

    // Even inside the attribution window, the refund is capped at the charge: a
    // loss larger than D cannot all be guard cost.
    {
        PgKiRestore s;
        pg_ki_begin(&s, 100.0f, D, 1000);
        float v = pg_ki_step(&s, 100.0f - (D + 30.0f), 100, 1010);
        CHECK(pg_ki_attributed(&s) == D, "attribution is capped at the charge");
        CHECK(v == 100.0f - 30.0f, "only the charge is refunded, got loss %g", 100.0f - v);
    }

    // With no charge captured the cap is disabled but nothing else breaks.
    {
        PgKiRestore s;
        pg_ki_begin(&s, 100.0f, 0.0f, 1000);
        float v = pg_ki_step(&s, 90.0f, 50, 1005);
        CHECK(pg_ki_attributed(&s) == 10.0f, "uncapped attribution, got %g",
              pg_ki_attributed(&s));
        CHECK(v > 90.0f && v <= 100.0f, "still tops up, got %g", v);
    }

    // The latency to the first observed loss is recorded -- this is the number
    // that says whether the observation window is long enough.
    {
        PgKiRestore s;
        pg_ki_begin(&s, 100.0f, D, 5000);
        pg_ki_step(&s, 100.0f, 50, 5008);          // nothing yet
        CHECK(s.first_loss_ms == 0, "no loss seen yet");
        pg_ki_step(&s, 90.0f, 50, 5032);
        CHECK(s.first_loss_ms == 5032, "first loss time recorded, got %llu",
              (unsigned long long)s.first_loss_ms);
        CHECK(s.peak_loss == 10.0f, "peak loss recorded");
    }

    // A loss arriving after the attribution window is *reported* as such rather
    // than silently counted as "the field never moved".
    {
        PgKiRestore s;
        pg_ki_begin(&s, 100.0f, D, 1000);
        pg_ki_step(&s, 100.0f - D, 50, 1400);      // 400ms later, past PG_KI_ATTR_MS
        CHECK(s.attrib_loss == 0.0f, "too late to attribute");
        CHECK(s.peak_loss == D, "but the peak is still reported");
        CHECK(s.first_loss_ms == 1400, "and the delay is recorded, so the reader knows");
    }

    // With the top-up switched off (reduction 0) the measurement must survive: the
    // verdict is built from it, and KiTopUp=0 is precisely the setting that has to
    // stay diagnosable.
    {
        PgKiRestore s;
        pg_ki_begin(&s, 100.0f, D, 2000);
        float v = pg_ki_step(&s, 100.0f - D, 0, 2016);
        CHECK(v == 100.0f - D, "top-up off must not write, got %g", v);
        CHECK(s.first_loss_ms == 2016, "but the latency is still measured");
        CHECK(s.attrib_loss == D, "and the loss is still attributed, got %g",
              s.attrib_loss);
        CHECK(s.given == 0.0f, "and nothing was handed back");
    }
}

static void test_throttle(void) {
    printf("diagnostic throttle\n");
    PgThrottle t;
    pg_throttle_init(&t);

    // first sample always goes out
    CHECK(pg_throttle(&t, 0, 100000, 1000, 5) == PG_THROTTLE_EMIT, "first sample emits");
    // a change 1ms later is dropped
    CHECK(pg_throttle(&t, 0, 100001, 1000, 5) == PG_THROTTLE_DROP, "within the gap drops");
    // ...and keeps dropping until the gap is up
    CHECK(pg_throttle(&t, 0, 100999, 1000, 5) == PG_THROTTLE_DROP, "still inside the gap");
    CHECK(pg_throttle(&t, 0, 101000, 1000, 5) == PG_THROTTLE_EMIT, "gap elapsed emits");

    // a heartbeat bypasses the gap
    CHECK(pg_throttle(&t, 1, 101001, 1000, 5) == PG_THROTTLE_EMIT, "heartbeat bypasses gap");

    // hitting the cap: the 5th line still goes out, and the call *after* it
    // announces the cap exactly once, then goes quiet forever
    CHECK(pg_throttle(&t, 1, 102000, 1000, 5) == PG_THROTTLE_EMIT, "4th line");
    CHECK(pg_throttle(&t, 1, 102500, 1000, 5) == PG_THROTTLE_EMIT, "5th line");
    CHECK(pg_throttle(&t, 1, 103000, 1000, 5) == PG_THROTTLE_CAPPED, "cap notice once");
    CHECK(pg_throttle(&t, 1, 103500, 1000, 5) == PG_THROTTLE_DROP, "quiet after the notice");
    for (int i = 0; i < 5; ++i)
        CHECK(pg_throttle(&t, 1, 104000 + (unsigned)i * 1000, 1000, 5) == PG_THROTTLE_DROP,
              "silent after the cap notice");
    CHECK(t.lines == 5, "must not count lines past the cap, got %d", t.lines);

    // this is the bug being guarded against: without the gap, N samples in the
    // same millisecond would all emit
    PgThrottle u;
    pg_throttle_init(&u);
    int emitted = 0;
    for (int i = 0; i < 50; ++i)
        if (pg_throttle(&u, 0, 500000, 1000, 600) == PG_THROTTLE_EMIT) ++emitted;
    CHECK(emitted == 1, "50 samples in one millisecond must emit once, got %d", emitted);

    // steady stream at 10ms stays near one line per second, not ten
    PgThrottle v;
    pg_throttle_init(&v);
    emitted = 0;
    for (unsigned long long ms = 0; ms < 10000; ms += 10)
        if (pg_throttle(&v, 0, ms, 1000, 600) == PG_THROTTLE_EMIT) ++emitted;
    CHECK(emitted >= 9 && emitted <= 11, "10s at 100Hz must emit ~10 lines, got %d", emitted);
}

static void test_kiv(void) {
    printf("guard-cost model classification (KIV)\n");
    const float D = 6.4f;

    // The case the old if/else chain got WRONG: at the shipped default of 100%
    // reduction, "scaled to zero" and "never moved" are the same number, so the
    // answer must be refused rather than guessed.
    CHECK(pg_kiv_classify(D, 0.0f, 100).model == PG_KIV_INCONCLUSIVE,
          "reduction=100 must not claim 'the field did not move'");
    CHECK(pg_kiv_classify(D, 0.0f, 0).model == PG_KIV_FULL_COST ||
          pg_kiv_classify(D, 0.0f, 0).model == PG_KIV_INCONCLUSIVE,
          "reduction=0 must never report the scaled model as distinct");
    CHECK(!pg_kiv_resolvable(100), "100 pct is not resolvable");
    CHECK(!pg_kiv_resolvable(0), "0 pct is not resolvable");
    CHECK(!pg_kiv_resolvable(10), "10 pct is too fine to separate");
    CHECK(pg_kiv_resolvable(50), "50 pct is the recommended setting");
    CHECK(pg_kiv_resolvable(20) && pg_kiv_resolvable(80), "20..80 are resolvable");
    CHECK(!pg_kiv_resolvable(90), "90 pct is too fine to separate");

    // At a resolvable setting each model is identified from its own signature.
    CHECK(pg_kiv_classify(D, 0.0f, 50).model == PG_KIV_NO_MOVE,
          "loss 0 at r=50 -> the field never moved");
    CHECK(pg_kiv_classify(D, D, 50).model == PG_KIV_FULL_COST,
          "loss D at r=50 -> full cost reached the bar");
    CHECK(pg_kiv_classify(D, D * 0.5f, 50).model == PG_KIV_SCALED_COST,
          "loss D/2 at r=50 -> the scaling already reaches the bar");

    // A little noise must not flip the verdict...
    CHECK(pg_kiv_classify(D, D * 0.5f + 0.4f, 50).model == PG_KIV_SCALED_COST,
          "small positive noise must not change the answer");
    CHECK(pg_kiv_classify(D, D - 0.4f, 50).model == PG_KIV_FULL_COST,
          "small negative noise must not change the answer");
    // ...but a loss sitting between models must be refused, not arbitrated.
    // At r=50 the models are 0, D and D/2, so 0.30D is 0.20D from D/2 and 0.30D
    // from 0 -- too close to the runner-up to be called.
    CHECK(pg_kiv_classify(D, D * 0.30f, 50).model == PG_KIV_INCONCLUSIVE,
          "a loss between models must be inconclusive");
    CHECK(pg_kiv_classify(D, D * 0.30f, 50).weak_margin == 1,
          "and it must say the margin is weak");
    // a genuinely bad fit is flagged as poor too
    {
        PgKivVerdict w = pg_kiv_classify(D, D * 0.30f, 50);
        CHECK(w.poor_fit == 0 || w.poor_fit == 1, "poor_fit is a boolean");
        CHECK(w.residual > 0.15f, "residual is reported, got %.3f", w.residual);
    }

    // Degenerate / invalid inputs must not produce a confident answer.
    CHECK(pg_kiv_classify(0.0f, 0.0f, 50).model == PG_KIV_INCONCLUSIVE,
          "zero charge is not classifiable");
    CHECK(pg_kiv_classify(-1.0f, 0.0f, 50).model == PG_KIV_INCONCLUSIVE,
          "negative charge is not classifiable");
    // The residual is reported as a fraction of D, for the log line.
    CHECK(pg_kiv_classify(D, D, 50).residual == 0.0f, "exact fit has zero residual");
}

// Drive whole scenarios through the shared chain, rather than only testing the
// pieces. The worst bug in this project (an enemy blocking the player's attack
// also rewarding the player) was an ordering/subject bug: every individual
// predicate was right. These cases pin the composition.
static void test_chain(void) {
    printf("perfect-guard chain scenarios\n");
    const int W = 250;

    // press, then block inside the window -> perfect
    {
        PgGuardInput g;
        pg_guard_init(&g);
        pg_guard_update(&g, 1, 1000);
        CHECK(pg_classify_block(1, &g, 1, 1010, W) == PG_BLOCK_PERFECT,
              "block 10ms after the press is perfect");
    }
    // press, then block after the window -> late, not perfect
    {
        PgGuardInput g;
        pg_guard_init(&g);
        pg_guard_update(&g, 1, 1000);
        CHECK(pg_classify_block(1, &g, 1, 1400, W) == PG_BLOCK_LATE,
              "block 400ms after the press is late");
    }
    // hold guard from long ago, then block -> late (holding must not re-arm)
    {
        PgGuardInput g;
        pg_guard_init(&g);
        for (unsigned long long t = 1000; t <= 2000; t += 8)
            pg_guard_update(&g, 1, t);
        CHECK(g.presses == 1, "holding guard registers one press, got %ld", g.presses);
        CHECK(pg_classify_block(1, &g, 1, 2000, W) == PG_BLOCK_LATE,
              "a held guard must not keep the window open");
    }
    // never pressed -> late, never perfect
    {
        PgGuardInput g;
        pg_guard_init(&g);
        CHECK(pg_classify_block(1, &g, 1, 5000, W) == PG_BLOCK_LATE,
              "no press at all can never be perfect");
    }
    // someone else blocked -> OTHER, no matter how good the timing is
    {
        PgGuardInput g;
        pg_guard_init(&g);
        pg_guard_update(&g, 1, 1000);
        CHECK(pg_classify_block(0, &g, 1, 1005, W) == PG_BLOCK_OTHER,
              "an enemy's block must be OTHER even with perfect timing");
    }
    // ...and an enemy's block must not consume the player's window: the player
    // can still perfect-guard on the very next event.
    {
        PgGuardInput g;
        pg_guard_init(&g);
        pg_guard_update(&g, 1, 1000);
        CHECK(pg_classify_block(0, &g, 1, 1010, W) == PG_BLOCK_OTHER, "enemy blocks");
        CHECK(pg_classify_block(1, &g, 1, 1020, W) == PG_BLOCK_PERFECT,
              "the player's window survives an enemy's block");
    }
    // release and press again -> the window re-opens
    {
        PgGuardInput g;
        pg_guard_init(&g);
        pg_guard_update(&g, 1, 1000);
        pg_guard_update(&g, 0, 1200);
        pg_guard_update(&g, 1, 1500);
        CHECK(g.presses == 2, "two taps, got %ld", g.presses);
        CHECK(pg_classify_block(1, &g, 1, 1510, W) == PG_BLOCK_PERFECT,
              "the second press re-opens the window");
    }
    // a multi-hit flurry inside one window: every hit counts (documented)
    {
        PgGuardInput g;
        pg_guard_init(&g);
        pg_guard_update(&g, 1, 1000);
        int perfect = 0;
        for (unsigned long long t = 1005; t <= 1150; t += 50)
            if (pg_classify_block(1, &g, 1, t, W) == PG_BLOCK_PERFECT) ++perfect;
        CHECK(perfect == 3, "three hits in one window all count (got %d)", perfect);
        CHECK(pg_classify_block(1, &g, 1, 1400, W) == PG_BLOCK_LATE,
              "but a hit after the window does not");
    }
    // gate_timely=0 means every player block counts, but an enemy's still does not
    {
        PgGuardInput g;
        pg_guard_init(&g);
        CHECK(pg_classify_block(1, &g, 0, 9999, 0) == PG_BLOCK_PERFECT,
              "gate off: any player block counts");
        CHECK(pg_classify_block(0, &g, 0, 9999, 0) == PG_BLOCK_OTHER,
              "gate off must still ignore somebody else's block");
    }
    // jitter on the pad must not manufacture presses
    {
        PgGuardInput g;
        pg_guard_init(&g);
        const int seq[] = {0, 1, 1, 0, 0, 1, 1, 1, 0};
        unsigned long long t = 1000;
        for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); ++i, t += 8)
            pg_guard_update(&g, seq[i], t);
        CHECK(g.presses == 2, "two clean taps amid jitter, got %ld", g.presses);
    }
}

// The flag-based event source: the flag is written on the attacker's side, so
// "the player blocked" is a statement about *who* is in the hit context.
static void test_flag_subject(void) {
    printf("flag-based subject test\n");
    CHECK(pg_flag_means_player_blocked(1, 0, 0) == 1,
          "player is combatant A, someone else attacked -> player blocked");
    CHECK(pg_flag_means_player_blocked(0, 1, 0) == 1,
          "player is combatant B, someone else attacked -> player blocked");
    CHECK(pg_flag_means_player_blocked(1, 0, 1) == 0,
          "player's OWN attack was guarded -> must NOT count");
    CHECK(pg_flag_means_player_blocked(1, 1, 0) == 1,
          "player on both sides and not the attacker -> blocked");
    CHECK(pg_flag_means_player_blocked(0, 0, 0) == 0,
          "player absent (AI vs AI, as in the attract demo) -> ignored");
    CHECK(pg_flag_means_player_blocked(0, 0, 1) == 0,
          "player absent, and flagged as the attacker -> ignored");
    // the attacker test must win over the combatant test
    CHECK(pg_flag_means_player_blocked(1, 1, 1) == 0,
          "attacker test takes precedence");
}

static void test_dedupe(void) {
    printf("cross-source reward dedupe\n");
    // no previous reward -> allowed, and it publishes the timestamp
    CHECK(pg_dedupe_ok(0, 5000, 250) == 1, "first reward is allowed");
    // the same block seen by the other source a millisecond later -> refused
    CHECK(pg_dedupe_ok(5000, 5001, 250) == 0, "a second event 1ms later is refused");
    CHECK(pg_dedupe_ok(5000, 5249, 250) == 0, "still refused at window-1");
    CHECK(pg_dedupe_ok(5000, 5250, 250) == 1, "allowed once the window has passed");
    // a genuine second block later in the fight must not be swallowed
    CHECK(pg_dedupe_ok(5000, 9000, 250) == 1, "a much later block is allowed");
    // a backwards clock must not wedge the gate shut
    CHECK(pg_dedupe_ok(9000, 8000, 250) == 1, "a backwards clock does not block");
}

int main(int argc, char **argv) {
    // --digest prints the shared logic's numeric fingerprint, for comparison with
    // the value the shipped DLL exports. See tools/test_logic_digest.py.
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-') {
        printf("logic_digest=%08X\n", pg_logic_digest());
        return 0;
    }
    test_gate();
    test_edge();
    test_mask();
    test_chain();
    test_flag_subject();
    test_dedupe();
    test_restore();
    test_attribution();
    test_throttle();
    test_kiv();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
