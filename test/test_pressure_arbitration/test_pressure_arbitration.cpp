// Unit tests for the NayrodPID flow/pressure min() arbitration.
//
// The pressure branch used to have its integral zeroed on every cycle the
// flow branch won. That prevented wind-up, but it meant the branch engaged
// the ceiling from a fresh integral: its duty collapsed towards the raw
// error term at the first engagement and the loop relax-oscillated against
// the ceiling (the duty chatter documented in debug/first-espresso-shots.md,
// made operational by any flow target that actually reaches the ceiling,
// e.g. 2.4 ml/s on the shot 25/26 pucks).
//
// These tests pin the intended behaviour: below the ceiling the arbitration
// must keep returning the flow feed-forward unchanged, and the handover at
// the ceiling must be continuous (override tracking), both open loop and
// against a toy puck plant.
//
// Host-side, no ESP32/Arduino runtime — pio test -e native_controls.

#include <unity.h>

#include <algorithm>
#include <cmath>

// Host stand-in for the Arduino millis() declared in test/support/Arduino.h.
// Nothing under test here consumes time; the controller is driven by dt.
static unsigned long g_now_ms = 0;
unsigned long millis() { return g_now_ms; }

// Direct-include the TUs — native harness skips the Arduino-dependent
// siblings, same pattern as test_autotune_simc.
#include "PressureController/PressureController.cpp"
#include "SimpleKalmanFilter/SimpleKalmanFilter.cpp"

void setUp() {}
void tearDown() {}

namespace {

constexpr float kDt = 0.03f;         // DimmedPump loop period
constexpr float kCeiling = 9.0f;     // extraction ceiling under test (bar)
constexpr float kFlowTarget = 2.4f;  // ml/s, the shot 25/26 request

// This machine's affine pump model, setPumpFlowCoeff(6.4, 3.9):
// Q_available(P) = 6.7125 - 0.3125 * P, slip = 0.
float availableFlow(float pressure) { return 6.7125f - 0.3125f * pressure; }
float flowDuty(float pressure) { return kFlowTarget / availableFlow(pressure) * 100.0f; }

struct Rig {
    float pressureSetpoint = kCeiling;
    float flowSetpoint = kFlowTarget;
    float sensorPressure = 0.0f;
    float ctrlOutput = 0.0f;
    int valveStatus = 1;
    PressureController pc;

    Rig() : pc(kDt, &pressureSetpoint, &flowSetpoint, &sensorPressure, &ctrlOutput, &valveStatus) {
        pc.setPumpFlowCoeff(6.4f, 3.9f);
    }

    float update() {
        pc.update(PressureController::ControlMode::FLOW);
        return ctrlOutput;
    }
};

} // namespace

// Well below the ceiling the arbitration must be transparent: the output is
// the flow feed-forward, steady, with no influence from the pressure branch.
static void test_flow_feedforward_unchanged_below_ceiling() {
    Rig r;
    r.sensorPressure = 3.0f;
    float out = 0.0f;
    for (int i = 0; i < 400; ++i) {
        out = r.update(); // 12 s, filters fully settled
    }
    TEST_ASSERT_FLOAT_WITHIN(0.5f, flowDuty(3.0f), out);
    for (int i = 0; i < 50; ++i) {
        TEST_ASSERT_FLOAT_WITHIN(0.05f, out, r.update());
    }
}

// Open loop: force a quasi-static pressure climb through the ceiling, the
// trajectory the ceiling-less shots followed. The handover from the flow
// branch to the pressure branch must be continuous; the old integral reset
// collapsed the duty by tens of points at engagement.
static void test_ceiling_engages_without_duty_collapse() {
    Rig r;
    r.sensorPressure = 3.0f;
    for (int i = 0; i < 400; ++i) {
        r.update();
    }
    float prev = r.ctrlOutput;
    float maxDrop = 0.0f;
    while (r.sensorPressure < 9.6f) {
        r.sensorPressure += 0.2f * kDt; // 0.2 bar/s, as in shot 25
        float out = r.update();
        maxDrop = std::max(maxDrop, prev - out);
        prev = out;
    }
    TEST_ASSERT_TRUE_MESSAGE(maxDrop < 10.0f, "duty must hand over continuously at the ceiling");
}

// Closed loop against a toy puck: compliance 1.4 ml/bar and a resistance
// chosen so the flow target wants ~10 bar, i.e. the ceiling must engage and
// hold. The old reset produced a relaxation oscillation (duty collapsing and
// recovering repeatedly); the fixed arbitration must regulate smoothly.
static void test_no_relaxation_chatter_against_ceiling() {
    Rig r;
    const float puckResistance = 4.2f; // bar.s/ml -> equilibrium 10.1 bar at 2.4 ml/s
    const float compliance = 1.4f;     // ml/bar
    float pressure = 1.0f;
    r.sensorPressure = pressure;

    bool engaged = false;
    int settledCycles = 0;
    float minOut = 1e9f;
    float maxOut = -1e9f;
    const int totalCycles = 3000; // 90 s
    const int windowCycles = 1000; // assert over the final 30 s
    for (int i = 0; i < totalCycles; ++i) {
        float out = r.update();
        float pumpFlow = out / 100.0f * availableFlow(pressure);
        float puckFlow = pressure / puckResistance;
        pressure += (pumpFlow - puckFlow) / compliance * kDt;
        pressure = std::max(pressure, 0.0f);
        r.sensorPressure = pressure;
        if (pressure >= 8.9f) {
            engaged = true;
        }
        if (i >= totalCycles - windowCycles) {
            ++settledCycles;
            minOut = std::min(minOut, out);
            maxOut = std::max(maxOut, out);
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(engaged, "plant must reach the ceiling");
    TEST_ASSERT_EQUAL_INT(windowCycles, settledCycles);
    TEST_ASSERT_FLOAT_WITHIN(0.75f, kCeiling, r.sensorPressure);
    TEST_ASSERT_TRUE_MESSAGE(maxOut - minOut < 15.0f, "duty must not chatter against the ceiling");
}

// ---------------------------------------------------------------------------
// Ceiling latch — the shot 54/55 regime
// ---------------------------------------------------------------------------

namespace {

struct WindowStats {
    float minOut = 1e9f, maxOut = -1e9f;
    float minP = 1e9f, maxP = -1e9f;
    void add(float out, float p) {
        minOut = std::min(minOut, out);
        maxOut = std::max(maxOut, out);
        minP = std::min(minP, p);
        maxP = std::max(maxP, p);
    }
};

} // namespace

// Shot 54 regime: a puck so tight the flow target is unreachable below the
// ceiling by a wide margin. Linear resistance 6.4 bar.s/ml passes 1.41 ml/s
// at 9 bar (sustainable duty about 36 %) while the feed-forward for
// 2.4 ml/s at 9 bar wants 61.5 %, a 25 point gap.
//
// The plant carries the two ingredients the real machine has and a clean
// toy lacks, both load-bearing for the regression:
// - Hydraulic compliance near the ceiling is about 3/P (the controller's
//   own virtualScale model), ~0.33 ml/bar at 9 bar, four times stiffer
//   than the 1.4 the control law assumes.
// - PSM actuation: duty resolves to whole mains half-cycles and a 30 ms
//   control period spans only ~3.6 of them, so delivered duty quantises
//   to ~28 point steps with error diffusion carrying the remainder.
// The quantisation noise through the stiff plant keeps the pressure
// branch's raw duty brushing the feed-forward value, and with per-cycle
// override conditioning each brush wipes the integral and re-arms the
// feed-forward: the sustained 63/36 saw-tooth with cuts of shots 54/55.
static void test_choked_puck_holds_ceiling_quietly() {
    Rig r;
    const float puckResistance = 6.4f;
    const float compliance = 0.35f;
    float pressure = 1.0f;
    r.sensorPressure = pressure;
    float psmCarry = 0.0f;
    const float psmStep = 100.0f / 3.6f;
    WindowStats w;
    const int totalCycles = 3000;  // 90 s
    const int windowCycles = 1000; // final 30 s
    for (int i = 0; i < totalCycles; ++i) {
        float out = r.update();
        float want = out + psmCarry;
        float applied = std::clamp(psmStep * std::floor(want / psmStep + 0.5f), 0.0f, 100.0f);
        psmCarry = want - applied;
        float pumpFlow = applied / 100.0f * availableFlow(pressure);
        float puckFlow = pressure / puckResistance;
        pressure = std::max(0.0f, pressure + (pumpFlow - puckFlow) / compliance * kDt);
        r.sensorPressure = pressure;
        if (i >= totalCycles - windowCycles) {
            w.add(out, pressure);
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(w.minP > kCeiling - 0.35f && w.maxP < kCeiling + 0.35f,
                             "pressure must hold the ceiling within the PSM ripple band");
    TEST_ASSERT_TRUE_MESSAGE(w.maxOut - w.minOut < 12.0f, "commanded duty must not saw-tooth against the ceiling");
    TEST_ASSERT_TRUE_MESSAGE(w.minOut > 20.0f, "duty must never cut towards zero at the ceiling");
}

// The latch must hand back to the feed-forward when the puck opens enough
// that the target fits below the ceiling, and the handover must not step
// the duty audibly.
static void test_latch_releases_when_puck_opens() {
    Rig r;
    const float compliance = 0.35f; // stiff near the ceiling, see above
    float resistance = 6.4f;
    float pressure = 1.0f;
    r.sensorPressure = pressure;
    float prevOut = 0.0f;
    float maxStep = 0.0f;
    bool held = false;
    const int holdCycles = 1000; // 30 s choked
    const int rampCycles = 1000; // puck erodes open over 30 s
    const int tailCycles = 600;  // 18 s to settle at the open equilibrium
    for (int i = 0; i < holdCycles + rampCycles + tailCycles; ++i) {
        if (i >= holdCycles && i < holdCycles + rampCycles) {
            resistance -= (6.4f - 2.0f) / rampCycles;
        }
        float out = r.update();
        if (i >= holdCycles) {
            maxStep = std::max(maxStep, std::fabs(out - prevOut));
        }
        prevOut = out;
        float pumpFlow = out / 100.0f * availableFlow(pressure);
        float puckFlow = pressure / resistance;
        pressure = std::max(0.0f, pressure + (pumpFlow - puckFlow) / compliance * kDt);
        r.sensorPressure = pressure;
        if (i < holdCycles && pressure > 8.9f) {
            held = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(held, "plant must first reach the ceiling");
    // With R = 2.0 the flow target fits below the ceiling (equilibrium
    // 4.8 bar), so the feed-forward must own the pump again.
    TEST_ASSERT_TRUE_MESSAGE(r.sensorPressure < 8.5f, "feed-forward must resume once the target fits below the ceiling");
    TEST_ASSERT_FLOAT_WITHIN(0.75f, flowDuty(r.sensorPressure), r.ctrlOutput);
    TEST_ASSERT_TRUE_MESSAGE(maxStep < 8.0f, "release must not step the duty");
}

// The amplifier behind the shots 54/55 saw-tooth, tested directly. During
// a ceiling hold, any deep measurement excursion below the setpoint (a
// pump-stroke pulsation trough, a sensor transient) lets the flow branch
// win a cycle or two, and the per-cycle override conditioning then rewrites
// the pressure integral to the feed-forward duty, erasing the sustainable
// duty the loop had learned. On re-engagement the loop restarts from the
// feed-forward (61.5 % into a puck sustaining 36 %), overshoots the
// ceiling, claws back down, and every following excursion repeats the
// slam: tops at exactly the feed-forward duty, which is what the shot logs
// show. The dip is kept to two cycles so the physical over-pumping during
// it is negligible; what is being tested is that the integral survives.
static void test_measurement_dip_does_not_rearm_feedforward() {
    Rig r;
    const float compliance = 0.35f; // stiff near the ceiling, see above
    const float resistance = 6.4f;
    float pressure = 1.0f;
    r.sensorPressure = pressure;
    for (int i = 0; i < 1500; ++i) { // 45 s: converge the choked hold
        float out = r.update();
        float pumpFlow = out / 100.0f * availableFlow(pressure);
        pressure = std::max(0.0f, pressure + (pumpFlow - pressure / resistance) / compliance * kDt);
        r.sensorPressure = pressure;
    }
    // A 2.0 bar measurement trough for 8 cycles (0.24 s) while the plant
    // stays put; the Kalman filter passes enough of it for the flow branch
    // to win those cycles. Both the old and new code push extra water
    // during the spoofed dip (the pressure loop is doing its job); the
    // difference is entirely in the aftermath. Old code: the integral was
    // conditioned to the dip-regime feed-forward, so re-engagement slams
    // the output to a hard cut (0 % for half a second) and the pressure
    // crashes 1.6 bar below the ceiling before a slow rebuild. New code:
    // the integral survives, the loop re-engages near the sustainable duty
    // and the excursion damps out.
    for (int i = 0; i < 8; ++i) {
        r.sensorPressure = pressure - 2.0f;
        float out = r.update();
        float pumpFlow = out / 100.0f * availableFlow(pressure);
        pressure = std::max(0.0f, pressure + (pumpFlow - pressure / resistance) / compliance * kDt);
    }
    r.sensorPressure = pressure;
    float minP = 1e9f;
    float minOut = 1e9f;
    for (int i = 0; i < 334; ++i) { // 10 s aftermath
        float out = r.update();
        float pumpFlow = out / 100.0f * availableFlow(pressure);
        pressure = std::max(0.0f, pressure + (pumpFlow - pressure / resistance) / compliance * kDt);
        r.sensorPressure = pressure;
        minP = std::min(minP, pressure);
        minOut = std::min(minOut, out);
    }
    TEST_ASSERT_TRUE_MESSAGE(minOut > 10.0f, "re-engagement must not slam the pump into a cut");
    TEST_ASSERT_TRUE_MESSAGE(minP > 8.4f, "the aftermath must not crash the pressure below the ceiling band");
    TEST_ASSERT_FLOAT_WITHIN(0.15f, kCeiling, pressure);
}

// A channel opening mid-hold (puck erosion) sags the pressure well below
// the setpoint. The loop must chase it and resettle without overshooting
// the ceiling or breaking into a saw-tooth.
static void test_puck_erosion_does_not_rearm_feedforward() {
    Rig r;
    const float compliance = 0.35f; // stiff near the ceiling, see above
    float resistance = 6.4f;
    float pressure = 1.0f;
    r.sensorPressure = pressure;
    for (int i = 0; i < 1500; ++i) { // 45 s: converge the choked hold
        float out = r.update();
        float pumpFlow = out / 100.0f * availableFlow(pressure);
        pressure = std::max(0.0f, pressure + (pumpFlow - pressure / resistance) / compliance * kDt);
        r.sensorPressure = pressure;
    }
    resistance = 4.2f; // channel opens; still choked (sustainable ~55 %)
    float maxP = 0.0f;
    WindowStats tail;
    const int aftermathCycles = 500; // 15 s
    for (int i = 0; i < aftermathCycles; ++i) {
        float out = r.update();
        float pumpFlow = out / 100.0f * availableFlow(pressure);
        pressure = std::max(0.0f, pressure + (pumpFlow - pressure / resistance) / compliance * kDt);
        r.sensorPressure = pressure;
        maxP = std::max(maxP, pressure);
        if (i >= aftermathCycles - 167) { // final 5 s
            tail.add(out, pressure);
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(maxP < kCeiling + 0.25f, "erosion recovery must not overshoot the ceiling");
    TEST_ASSERT_FLOAT_WITHIN(0.15f, kCeiling, pressure);
    TEST_ASSERT_TRUE_MESSAGE(tail.maxOut - tail.minOut < 6.0f, "duty must settle after erosion, not saw-tooth");
}

// With the output railed at 0 under pinned overpressure the integral must
// freeze instead of winding on through the virtual band below zero; when
// the overpressure clears the pump must come back promptly rather than
// spiralling through a long cut.
static void test_no_cut_spiral_after_pinned_overpressure() {
    Rig r;
    const float compliance = 0.35f; // stiff near the ceiling, see above
    float pressure = 1.0f;
    r.sensorPressure = pressure;
    for (int i = 0; i < 1500; ++i) { // 45 s: reach and hold the ceiling
        float out = r.update();
        float pumpFlow = out / 100.0f * availableFlow(pressure);
        float puckFlow = pressure / 6.4f;
        pressure = std::max(0.0f, pressure + (pumpFlow - puckFlow) / compliance * kDt);
        r.sensorPressure = pressure;
    }
    r.sensorPressure = 9.8f; // pinned overpressure, e.g. a blocked path
    for (int i = 0; i < 165; ++i) { // ~5 s
        r.update();
    }
    // Effectively off: conditional integration parks the raw output at the
    // 0 bound, so it may hover a fraction of a point above exact zero.
    TEST_ASSERT_FLOAT_WITHIN(1.5f, 0.0f, r.ctrlOutput);
    r.sensorPressure = 8.8f; // overpressure clears just below the setpoint
    float out = 0.0f;
    int cycles = 0;
    while (cycles < 700 && out <= 5.0f) {
        out = r.update();
        ++cycles;
    }
    TEST_ASSERT_TRUE_MESSAGE(out > 5.0f, "output must recover after overpressure clears");
    TEST_ASSERT_TRUE_MESSAGE(cycles <= 50, "recovery must take under 1.5 s, not a wound-up cut spiral");
}

// A short pressure spike below the ceiling must not stick the pump to the
// pressure branch: the feed-forward must resume promptly when it clears.
static void test_transient_spike_does_not_stick_latch() {
    Rig r;
    r.sensorPressure = 5.0f;
    for (int i = 0; i < 400; ++i) {
        r.update();
    }
    const float steady = r.ctrlOutput;
    const int spikeCycleCounts[2] = {3, 10}; // under and over any entry persistence
    for (int s = 0; s < 2; ++s) {
        for (int i = 0; i < spikeCycleCounts[s]; ++i) {
            r.sensorPressure = 9.4f;
            r.update();
        }
        r.sensorPressure = 5.0f;
        float out = 0.0f;
        int cycles = 0;
        while (cycles < 100 && std::fabs((out = r.update()) - steady) > 1.0f) {
            ++cycles;
        }
        TEST_ASSERT_TRUE_MESSAGE(cycles < 12, "flow feed-forward must resume promptly after a spike");
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_flow_feedforward_unchanged_below_ceiling);
    RUN_TEST(test_ceiling_engages_without_duty_collapse);
    RUN_TEST(test_no_relaxation_chatter_against_ceiling);
    RUN_TEST(test_choked_puck_holds_ceiling_quietly);
    RUN_TEST(test_measurement_dip_does_not_rearm_feedforward);
    RUN_TEST(test_puck_erosion_does_not_rearm_feedforward);
    RUN_TEST(test_latch_releases_when_puck_opens);
    RUN_TEST(test_no_cut_spiral_after_pinned_overpressure);
    RUN_TEST(test_transient_spike_does_not_stick_latch);
    return UNITY_END();
}
