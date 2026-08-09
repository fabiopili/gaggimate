// Unit tests for the display-side FlowTrimmer slew-gate fade: trim
// authority must fade continuously across the pressure-slew gate rather
// than toggle, because in the shot tails the decay slew sits right on the
// gate and each toggle moved the whole proportional term in and out of the
// command (the audible duty sawtooth on shots 25/26).
//
// The PumpLimits substitution tests that used to live here left with the
// header itself: a profile pressure of 0 on a flow phase passes through and
// means no ceiling, with the semantics made explicit in the profile editor
// instead (see docs/ceiling-hold-plan.md, step 3).
//
// Host-side, no ESP32/Arduino runtime — pio test -e native_controls.

#include <unity.h>

#include <algorithm>
#include <cmath>

// Host stand-in for the Arduino millis() declared in test/support/Arduino.h.
// Advanced explicitly by the tests.
static unsigned long g_now_ms = 0;
unsigned long millis() { return g_now_ms; }

#include "FlowTrimmer.h"

void setUp() { g_now_ms = 0; }
void tearDown() {}

// Advance one 250 ms control step, the shot log cadence the trimmer was
// tuned against.
static float stepTrimmer(FlowTrimmer &t, float requested, float measured, float pressure) {
    g_now_ms += 250;
    return t.update(requested, measured, pressure, true, false);
}

// ---------------------------------------------------------------------------
// FlowTrimmer slew-gate fade
// ---------------------------------------------------------------------------

static void test_full_authority_below_slew_gate() {
    FlowTrimmer t;
    float cmd = stepTrimmer(t, 2.4f, 2.4f, 5.0f); // primes dt/pressure state
    for (int i = 0; i < 4; ++i) {
        cmd = stepTrimmer(t, 2.4f, 2.4f, 5.0f); // steady pressure, zero error
    }
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.4f, cmd);
    // Under-delivery of 0.3 g/s at steady pressure: P contributes 0.12,
    // one integral step 0.0225, quantised to the 0.05 grid.
    cmd = stepTrimmer(t, 2.4f, 2.1f, 5.0f);
    TEST_ASSERT_FLOAT_WITHIN(5e-3f, 2.55f, cmd);
}

static void test_partial_authority_inside_fade_band() {
    FlowTrimmer t;
    float pressure = 3.0f;
    stepTrimmer(t, 2.4f, 2.4f, pressure);
    // Converge the slew filter to 0.375 bar/s, halfway across the fade band,
    // with zero flow error so no trim accumulates while it settles.
    float cmd = 0.0f;
    for (int i = 0; i < 20; ++i) {
        pressure += 0.375f * 0.25f;
        cmd = stepTrimmer(t, 2.4f, 2.4f, pressure);
    }
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.4f, cmd);
    // Under-delivery of 0.5 g/s at half authority: P contributes 0.10, one
    // integral step 0.01875, so the command must move to 2.50 rather than
    // stay switched off at 2.40.
    pressure += 0.375f * 0.25f;
    cmd = stepTrimmer(t, 2.4f, 1.9f, pressure);
    TEST_ASSERT_FLOAT_WITHIN(5e-3f, 2.50f, cmd);
}

static void test_no_authority_above_fade_band() {
    FlowTrimmer t;
    float pressure = 1.0f;
    stepTrimmer(t, 2.4f, 2.4f, pressure);
    float cmd = 0.0f;
    for (int i = 0; i < 20; ++i) {
        pressure += 0.60f * 0.25f; // 0.60 bar/s, above the whole band
        cmd = stepTrimmer(t, 2.4f, 2.4f, pressure);
    }
    pressure += 0.60f * 0.25f;
    cmd = stepTrimmer(t, 2.4f, 1.9f, pressure);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.4f, cmd); // exact pass-through
}

static void test_trim_moves_continuously_across_gate_wobble() {
    FlowTrimmer t;
    float pressure = 6.0f;
    stepTrimmer(t, 2.4f, 2.4f, pressure);
    // Alternating raw slew of 0.40/0.24 bar/s settles the filtered slew into
    // a steady alternation of about 0.347/0.293 bar/s, crossing the 0.30
    // gate every sample, exactly as the shot 25/26 tails did.
    bool hi = false;
    for (int i = 0; i < 20; ++i) {
        pressure += (hi ? 0.40f : 0.24f) * 0.25f;
        hi = !hi;
        stepTrimmer(t, 2.4f, 2.4f, pressure);
    }
    // Over-delivery of 0.2 g/s, the tail regime: the trim may drift but must
    // not toggle the whole proportional term between samples.
    float maxDelta = 0.0f;
    float prevTrim = 0.0f;
    bool havePrev = false;
    for (int i = 0; i < 7; ++i) {
        pressure += (hi ? 0.40f : 0.24f) * 0.25f;
        hi = !hi;
        stepTrimmer(t, 2.4f, 2.6f, pressure);
        if (havePrev) {
            maxDelta = std::max(maxDelta, std::fabs(t.getTrim() - prevTrim));
        }
        prevTrim = t.getTrim();
        havePrev = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(maxDelta <= 0.06f, "trim must fade across the slew gate, not toggle");
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_full_authority_below_slew_gate);
    RUN_TEST(test_partial_authority_inside_fade_band);
    RUN_TEST(test_no_authority_above_fade_band);
    RUN_TEST(test_trim_moves_continuously_across_gate_wobble);
    return UNITY_END();
}
