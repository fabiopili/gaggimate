#ifndef FLOWTRIMMER_H
#define FLOWTRIMMER_H

#include <Arduino.h>
#include <algorithm>
#include <cmath>

// Slow outer-loop trim for flow-targeted phases. The controller executes flow
// targets open loop by inverting its pump model, so a pump model error becomes
// a real flow error that nothing corrects, and the flow it reports back is the
// same model evaluated forward (see debug/report.md). The Bluetooth scale is
// the only instrument in the system that observes real flow, so this class
// compares the requested flow against the scale-derived flow and nudges the
// commanded flow accordingly.
//
// Two properties of the measurement shape the design, both learned from shots
// 20 and 21 (see debug/first-espresso-shots.md and the trim writeup):
//
// The scale observes cup flow with roughly a second of BLE lag plus the
// smoothing window of the rate fit, so loop bandwidth has to stay well below
// that. The proportional term supplies immediate response and phase lead, the
// integral removes the residual, and neither is fast enough to chase the lag.
//
// More importantly, cup flow only equals pump flow while pressure is steady.
// During the pressure ramp the pump is filling headspace and compressing the
// puck, and during the decay the system gives that water back, so in both
// cases the difference is real hydraulics rather than a model error. Trimming
// against it is what drove the first attempt to its clamp within five seconds
// of the phase starting, before any meaningful cup flow existed, so both terms
// are gated on the pressure being close to steady.
class FlowTrimmer {
  public:
    // requestedFlow: the profile's requested flow (ml/s), measuredFlow: the
    // scale-derived flow (g/s), pressure: current brew pressure (bar), used
    // only to decide whether the measurement is meaningful, measurementValid:
    // whether the scale is healthy and the measurement is real, pressureCapped:
    // whether a pressure limit is currently holding the flow back. Returns the
    // flow command to send.
    float update(float requestedFlow, float measuredFlow, float pressure, bool measurementValid, bool pressureCapped) {
        const unsigned long now = millis();
        float dt = 0.0f;
        if (lastUpdateMs != 0) {
            dt = static_cast<float>(now - lastUpdateMs) / 1000.0f;
            if (dt > MAX_DT_S) {
                dt = MAX_DT_S;
            }
        }
        lastUpdateMs = now;

        // Filtered pressure slew. The raw difference is dominated by sensor
        // quantisation at this update rate, so it is smoothed before use.
        if (dt > 0.0f && havePreviousPressure) {
            const float rawSlew = (pressure - previousPressure) / dt;
            pressureSlew += (rawSlew - pressureSlew) * std::min(1.0f, dt / SLEW_FILTER_TAU_S);
        }
        previousPressure = pressure;
        havePreviousPressure = true;

        if (requestedFlow <= 0.0f) {
            return requestedFlow;
        }

        // Cup flow has to be established before the scale says anything about
        // delivery, and pressure has to be near steady before cup flow and pump
        // flow are the same quantity. Steadiness is a fade rather than a gate:
        // in a shot's tail the decay slew sits right on the threshold, and a
        // boolean gate toggled the whole proportional term in and out of the
        // command each sample, which the feed-forward turned into an audible
        // duty sawtooth (shots 25 and 26). Full authority below the gate keeps
        // the validated behaviour; authority reaches zero at the upper bound.
        const bool established = measurementValid && measuredFlow >= MIN_MEASURED_FLOW;
        float authority = 0.0f;
        if (established && dt > 0.0f) {
            const float absSlew = std::fabs(pressureSlew);
            if (absSlew <= MAX_PRESSURE_SLEW_BAR_S) {
                authority = 1.0f;
            } else if (absSlew < SLEW_ZERO_AUTHORITY_BAR_S) {
                authority =
                    (SLEW_ZERO_AUTHORITY_BAR_S - absSlew) / (SLEW_ZERO_AUTHORITY_BAR_S - MAX_PRESSURE_SLEW_BAR_S);
            }
        }

        const float error = requestedFlow - measuredFlow;

        if (authority > 0.0f) {
            // While a pressure limit holds the flow back, raising the command
            // would only wind up against the limiter and discharge as an
            // overshoot when the cap lifts; trimming down remains safe.
            if (error < 0.0f || !pressureCapped) {
                integral += KI * error * dt * authority;
            }
        }

        const float upLimit = MAX_TRIM_UP_RATIO * requestedFlow;
        const float downLimit = -MAX_TRIM_DOWN_RATIO * requestedFlow;
        integral = std::clamp(integral, downLimit, upLimit);

        const float proportional = KP * error * authority;
        trim = std::clamp(integral + proportional, downLimit, upLimit);

        // Exact pass-through while the loop has nothing to say, so running with
        // the setting on but no usable measurement is bit-identical to off.
        if (trim == 0.0f) {
            return requestedFlow;
        }

        // Quantise so the delta-gated BLE link does not carry a new frame every
        // control cycle while the trim creeps.
        float command = std::round((requestedFlow + trim) / QUANT_STEP) * QUANT_STEP;
        return command > 0.0f ? command : 0.0f;
    }

    void reset() {
        integral = 0.0f;
        trim = 0.0f;
        lastUpdateMs = 0;
        pressureSlew = 0.0f;
        previousPressure = 0.0f;
        havePreviousPressure = false;
    }

    float getTrim() const { return trim; }

  private:
    static constexpr float KP = 0.40f; // proportional gain, ml/s of command per g/s of error
    static constexpr float KI = 0.30f; // integral gain, (ml/s per s) per (g/s) of error
    static constexpr float MIN_MEASURED_FLOW = 0.3f; // g/s below which cup flow is not established
    // Above this rate of pressure change the system is compressing or giving
    // water back, so cup flow is not pump flow and the difference is not an
    // error to correct. 0.30 bar/s blocks about 85 % of the ramp in shots 20
    // and 21 while leaving roughly half the phase available to integrate.
    static constexpr float MAX_PRESSURE_SLEW_BAR_S = 0.30f;
    // Authority fades linearly from full at the gate to zero here. Real ramps
    // run well above 1 bar/s so they stay fully blocked; only the boundary
    // region where shot tails hover becomes gradual instead of a toggle.
    static constexpr float SLEW_ZERO_AUTHORITY_BAR_S = 0.45f;
    static constexpr float SLEW_FILTER_TAU_S = 0.5f;    // smooths sensor quantisation out of the slew estimate
    static constexpr float MAX_TRIM_DOWN_RATIO = 0.75f; // command never falls below 25 % of the requested flow
    // Sized from shots 60 and 61: near the pump's 9 to 11 bar knee, a regime
    // no calibration has measured, the affine model over-promises by about
    // 40 %, so a capless flow phase under-delivers with this clamp pinned
    // (the original 10 % was sized for a model that under-predicted
    // everywhere then measured). 30 % covers most of the observed error;
    // the slew gate and the pressure-capped hold keep wind-up bounded.
    static constexpr float MAX_TRIM_UP_RATIO = 0.30f;
    static constexpr float QUANT_STEP = 0.05f; // ml/s command resolution
    static constexpr float MAX_DT_S = 0.5f;    // guards against integration bursts after stalls

    float integral = 0.0f;
    float trim = 0.0f;
    unsigned long lastUpdateMs = 0;
    float pressureSlew = 0.0f;
    float previousPressure = 0.0f;
    bool havePreviousPressure = false;
};

#endif // FLOWTRIMMER_H
