#ifndef PUMPLIMITS_H
#define PUMPLIMITS_H

// Flow-targeted phases execute with the profile's pressure value acting as a
// ceiling through the controller's flow/pressure arbitration, but that
// arbitration only runs when the setpoint is positive
// (PressureController::update), so a profile pressure of 0 does not mean "no
// preference", it removes the bound entirely and the feed-forward will chase
// the flow command up whatever pressure the puck produces (shots 25/26
// reached 10.3 bar). Substitute a conservative default so a profile can
// never switch the ceiling off by omission.
static constexpr float DEFAULT_FLOW_PRESSURE_LIMIT_BAR = 9.0f;

inline float effectiveFlowPressureLimit(float profileLimit) {
    return profileLimit > 0.0f ? profileLimit : DEFAULT_FLOW_PRESSURE_LIMIT_BAR;
}

#endif // PUMPLIMITS_H
