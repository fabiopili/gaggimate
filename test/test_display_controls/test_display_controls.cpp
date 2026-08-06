// Unit tests for the display-side pump control policy headers:
//   A — PumpLimits: default ceiling for flow phases with no pressure limit
//       (shots 25/26: a profile pressure of 0 disables the controller's
//       flow/pressure arbitration entirely, leaving the feed-forward
//       unbounded; see debug notes of 2026-08-06)
//
// Host-side, no ESP32/Arduino runtime — pio test -e native_controls.

#include <unity.h>

#include <cmath>

// Host stand-in for the Arduino millis() declared in test/support/Arduino.h.
// Advanced explicitly by the tests.
static unsigned long g_now_ms = 0;
unsigned long millis() { return g_now_ms; }

#include "PumpLimits.h"

void setUp() { g_now_ms = 0; }
void tearDown() {}

// ---------------------------------------------------------------------------
// A — PumpLimits
// ---------------------------------------------------------------------------

static void test_zero_profile_limit_becomes_default_ceiling() {
    TEST_ASSERT_EQUAL_FLOAT(9.0f, effectiveFlowPressureLimit(0.0f));
}

static void test_negative_profile_limit_becomes_default_ceiling() {
    TEST_ASSERT_EQUAL_FLOAT(9.0f, effectiveFlowPressureLimit(-1.0f));
}

static void test_positive_profile_limit_passes_through() {
    TEST_ASSERT_EQUAL_FLOAT(2.0f, effectiveFlowPressureLimit(2.0f));
    TEST_ASSERT_EQUAL_FLOAT(9.0f, effectiveFlowPressureLimit(9.0f));
    TEST_ASSERT_EQUAL_FLOAT(12.0f, effectiveFlowPressureLimit(12.0f));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_zero_profile_limit_becomes_default_ceiling);
    RUN_TEST(test_negative_profile_limit_becomes_default_ceiling);
    RUN_TEST(test_positive_profile_limit_passes_through);
    return UNITY_END();
}
