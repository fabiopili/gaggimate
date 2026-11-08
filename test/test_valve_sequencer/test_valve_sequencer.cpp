// Unit tests: brew valve sequencer and zero-cross watchdog interleavings
// (guard window plan v3). Host-side, no ESP32/Arduino runtime —
// pio test -e native -f test_valve_sequencer.

#include <unity.h>

#include <peripherals/ValveSequencer.h>

namespace {
constexpr uint32_t HOLD_OFF_CAP = 60;
constexpr uint32_t GUARD = 50;

uint32_t fakeNow = 0;
long fakeCounter = 0;
std::vector<bool> writes;

bool writeCb(bool state) {
    writes.push_back(state);
    return state;
}
} // namespace

ValveSequencer makeSequencer() {
    return ValveSequencer({HOLD_OFF_CAP, GUARD}, [] { return fakeNow; }, [] { return fakeCounter; },
                          [](bool state) { writes.push_back(state); });
}

void setUp(void) {
    fakeNow = 0;
    fakeCounter = 0;
    writes.clear();
}

void test_first_tick_after_request_writes_nothing(void) {
    // Regression: the counter snapshot is only valid after the forced
    // publish, so a stale snapshot from a previous transition must not
    // release the valve before full power was published.
    ValveSequencer seq = makeSequencer();
    fakeCounter = 100; // counter already moved past any old snapshot
    seq.request(true);
    TEST_ASSERT_EQUAL(ValveSequencer::Event::None, seq.tick(false));
    TEST_ASSERT_TRUE(writes.empty());
    TEST_ASSERT_TRUE(seq.forcingConduction());
}

void test_valve_writes_after_fired_decision(void) {
    ValveSequencer seq = makeSequencer();
    seq.request(true);
    TEST_ASSERT_EQUAL(ValveSequencer::Event::None, seq.tick(false));
    seq.notePublish();     // forced 100 percent published
    fakeCounter += 1;      // a zero-cross decision fired after the publish
    TEST_ASSERT_EQUAL(ValveSequencer::Event::ValveWritten, seq.tick(false));
    TEST_ASSERT_EQUAL(1, writes.size());
    TEST_ASSERT_TRUE(writes[0]);
    TEST_ASSERT_FALSE(seq.forcingConduction());
    TEST_ASSERT_TRUE(seq.guardActive());
}

void test_no_write_without_publish_even_if_counter_moved(void) {
    ValveSequencer seq = makeSequencer();
    seq.request(true);
    fakeCounter += 5; // decisions fired before the forced publish
    TEST_ASSERT_EQUAL(ValveSequencer::Event::None, seq.tick(false));
    TEST_ASSERT_TRUE(writes.empty());
}

void test_guard_expires_after_window(void) {
    ValveSequencer seq = makeSequencer();
    seq.request(true);
    seq.tick(false);
    seq.notePublish();
    fakeCounter += 1;
    seq.tick(false);
    fakeNow = GUARD + 1;
    TEST_ASSERT_FALSE(seq.guardActive());
}

void test_guard_does_not_latch_after_long_idle(void) {
    // Regression: an idle machine crossing the millis() sign bit must not
    // read an expired guard as active.
    ValveSequencer seq = makeSequencer();
    fakeNow = 0x90000000; // 2^31 passed, no transition ever armed a guard
    TEST_ASSERT_FALSE(seq.guardActive());
    TEST_ASSERT_EQUAL(ValveSequencer::Event::None, seq.tick(false));
    TEST_ASSERT_FALSE(seq.guardActive());
}

void test_guard_survives_millis_wraparound(void) {
    ValveSequencer seq = makeSequencer();
    fakeNow = 0xFFFFFFF0;
    seq.request(true);
    seq.tick(false);
    seq.notePublish();
    fakeCounter += 1;
    seq.tick(false); // valve write just before the wrap, guard until wrap+50
    fakeNow = 30;    // wrapped past 2^32, inside the guard window
    TEST_ASSERT_TRUE(seq.guardActive());
    fakeNow = 80; // past the guard window
    TEST_ASSERT_FALSE(seq.guardActive());
}

void test_cap_expiry_releases_on_fresh_zero_cross(void) {
    ValveSequencer seq = makeSequencer();
    seq.request(true);
    seq.tick(false);
    seq.notePublish(); // published, but no decision fires afterwards
    fakeNow = HOLD_OFF_CAP;
    TEST_ASSERT_EQUAL(ValveSequencer::Event::ValveWritten, seq.tick(false));
    TEST_ASSERT_EQUAL(ValveSequencer::ReleaseReason::HoldOffCap, seq.reason());
}

void test_open_refused_while_stale_on_cap_expiry(void) {
    ValveSequencer seq = makeSequencer();
    seq.request(true);
    seq.tick(false);
    seq.notePublish();
    fakeNow = HOLD_OFF_CAP;
    TEST_ASSERT_EQUAL(ValveSequencer::Event::OpenRefusedWhileStale, seq.tick(true));
    TEST_ASSERT_TRUE(writes.empty());
    // Recovery: fresh zero-cross stream, publish and proof complete.
    fakeCounter += 1;
    TEST_ASSERT_EQUAL(ValveSequencer::Event::ValveWritten, seq.tick(false));
    TEST_ASSERT_TRUE(writes[0]);
}

void test_close_proceeds_while_stale_on_cap_expiry(void) {
    ValveSequencer seq = makeSequencer();
    seq.request(true);
    seq.tick(false);
    seq.notePublish();
    fakeCounter += 1;
    seq.tick(false); // valve opens normally
    seq.request(false);
    seq.tick(false);
    seq.notePublish();
    fakeNow += HOLD_OFF_CAP;
    TEST_ASSERT_EQUAL(ValveSequencer::Event::ValveWritten, seq.tick(true));
    TEST_ASSERT_FALSE(writes.back());
}

void test_rapid_open_then_close_retargets(void) {
    ValveSequencer seq = makeSequencer();
    seq.request(true);
    seq.tick(false);
    seq.notePublish();
    seq.request(false); // close requested before the open was written
    fakeCounter += 1;
    TEST_ASSERT_EQUAL(ValveSequencer::Event::ValveWritten, seq.tick(false));
    TEST_ASSERT_FALSE(writes[0]); // the retargeted state wins
    TEST_ASSERT_FALSE(seq.valveState());
}

void test_abort_closes_immediately_without_guard(void) {
    ValveSequencer seq = makeSequencer();
    seq.request(true);
    seq.tick(false); // pending open, forced conduction armed
    seq.abortSequence();
    TEST_ASSERT_EQUAL(ValveSequencer::Event::ValveWritten, seq.tick(false));
    TEST_ASSERT_FALSE(writes[0]);
    TEST_ASSERT_FALSE(seq.forcingConduction());
    TEST_ASSERT_FALSE(seq.guardActive());
}

void test_no_edge_is_a_noop(void) {
    ValveSequencer seq = makeSequencer();
    seq.request(false); // already closed
    TEST_ASSERT_EQUAL(ValveSequencer::Event::None, seq.tick(false));
    seq.tick(false);
    TEST_ASSERT_TRUE(writes.empty());
}

void test_watchdog_trips_and_needs_two_fresh_checks(void) {
    ZeroCrossWatchdog watchdog;
    TEST_ASSERT_TRUE(watchdog.update(false));  // stale on the first stale check
    TEST_ASSERT_TRUE(watchdog.update(false));
    TEST_ASSERT_TRUE(watchdog.update(true));   // one fresh check is not enough
    TEST_ASSERT_FALSE(watchdog.update(true));  // second fresh check clears
    TEST_ASSERT_TRUE(watchdog.update(false));  // re-trips immediately
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_first_tick_after_request_writes_nothing);
    RUN_TEST(test_valve_writes_after_fired_decision);
    RUN_TEST(test_no_write_without_publish_even_if_counter_moved);
    RUN_TEST(test_guard_expires_after_window);
    RUN_TEST(test_guard_does_not_latch_after_long_idle);
    RUN_TEST(test_guard_survives_millis_wraparound);
    RUN_TEST(test_cap_expiry_releases_on_fresh_zero_cross);
    RUN_TEST(test_open_refused_while_stale_on_cap_expiry);
    RUN_TEST(test_close_proceeds_while_stale_on_cap_expiry);
    RUN_TEST(test_rapid_open_then_close_retargets);
    RUN_TEST(test_abort_closes_immediately_without_guard);
    RUN_TEST(test_no_edge_is_a_noop);
    RUN_TEST(test_watchdog_trips_and_needs_two_fresh_checks);
    return UNITY_END();
}
