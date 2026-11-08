// Unit tests: resolvePumpOutput precedence for the pump gate (guard window plan v2).
// Host-side, no ESP32/Arduino runtime — pio test -e native -f test_pump_output.

#include <unity.h>

#include <peripherals/PumpOutput.h>

void setUp(void) {}
void tearDown(void) {}

void test_commanded_power_passthrough(void) {
    PumpOutputIntent intent;
    intent.power = 42;
    TEST_ASSERT_EQUAL_INT(42, resolvePumpOutput(intent));
}

void test_power_clamped_to_range(void) {
    PumpOutputIntent high;
    high.power = 150;
    TEST_ASSERT_EQUAL_INT(100, resolvePumpOutput(high));
    PumpOutputIntent low;
    low.power = -5;
    TEST_ASSERT_EQUAL_INT(0, resolvePumpOutput(low));
}

void test_staleness_overrides_everything(void) {
    PumpOutputIntent intent;
    intent.zcStale = true;
    intent.guardActive = true;
    intent.power = 80;
    TEST_ASSERT_EQUAL_INT(-1, resolvePumpOutput(intent));
}

void test_guard_forces_full_conduction(void) {
    PumpOutputIntent intent;
    intent.guardActive = true;
    intent.power = 0;
    TEST_ASSERT_EQUAL_INT(100, resolvePumpOutput(intent));
}

void test_staleness_beats_guard(void) {
    PumpOutputIntent intent;
    intent.zcStale = true;
    intent.guardActive = true;
    TEST_ASSERT_EQUAL_INT(-1, resolvePumpOutput(intent));
}

void test_binary_mode_maps_command_to_levels(void) {
    PumpOutputIntent on;
    on.binaryMode = true;
    on.binaryOn = true;
    on.power = 37;
    TEST_ASSERT_EQUAL_INT(100, resolvePumpOutput(on));
    PumpOutputIntent off;
    off.binaryMode = true;
    off.binaryOn = false;
    off.power = 37;
    TEST_ASSERT_EQUAL_INT(0, resolvePumpOutput(off));
}

void test_guard_beats_binary_mode(void) {
    PumpOutputIntent intent;
    intent.guardActive = true;
    intent.binaryMode = true;
    intent.binaryOn = false;
    TEST_ASSERT_EQUAL_INT(100, resolvePumpOutput(intent));
}

void test_zero_defaults_are_safe(void) {
    PumpOutputIntent intent;
    TEST_ASSERT_EQUAL_INT(0, resolvePumpOutput(intent));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_commanded_power_passthrough);
    RUN_TEST(test_power_clamped_to_range);
    RUN_TEST(test_staleness_overrides_everything);
    RUN_TEST(test_guard_forces_full_conduction);
    RUN_TEST(test_staleness_beats_guard);
    RUN_TEST(test_binary_mode_maps_command_to_levels);
    RUN_TEST(test_guard_beats_binary_mode);
    RUN_TEST(test_zero_defaults_are_safe);
    return UNITY_END();
}
