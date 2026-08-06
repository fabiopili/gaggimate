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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_flow_feedforward_unchanged_below_ceiling);
    RUN_TEST(test_ceiling_engages_without_duty_collapse);
    RUN_TEST(test_no_relaxation_chatter_against_ceiling);
    return UNITY_END();
}
