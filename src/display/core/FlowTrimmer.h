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
// integrates the difference between the requested flow and the scale-derived
// flow and nudges the commanded flow accordingly. The gain is bounded by the
// measurement rather than by the plant: the scale observes cup flow with
// roughly a second of BLE lag plus the smoothing window of the rate fit, so
// crossover has to stay well below that. KI of 0.30 works a 1 g/s error off in
// a little over three seconds, which fits inside a typical flow phase while
// keeping crossover near 0.4 rad/s and leaving usable phase margin.
class FlowTrimmer {
  public:
    // requestedFlow: the profile's requested flow (ml/s), measuredFlow: the
    // scale-derived flow (g/s), measurementValid: whether the scale is healthy
    // and the measurement is real, pressureCapped: whether a pressure limit is
    // currently holding the flow back. Returns the flow command to send.
    float update(float requestedFlow, float measuredFlow, bool measurementValid, bool pressureCapped) {
        const unsigned long now = millis();
        float dt = 0.0f;
        if (lastUpdateMs != 0) {
            dt = static_cast<float>(now - lastUpdateMs) / 1000.0f;
            if (dt > MAX_DT_S) {
                dt = MAX_DT_S;
            }
        }
        lastUpdateMs = now;

        if (requestedFlow <= 0.0f) {
            return requestedFlow;
        }

        // Only integrate once cup flow is established; during preinfusion the
        // scale sees nothing and there is no error signal to act on.
        if (measurementValid && measuredFlow >= MIN_MEASURED_FLOW && dt > 0.0f) {
            const float error = requestedFlow - measuredFlow;
            // While a pressure limit holds the flow back, raising the command
            // would only wind up against the limiter and discharge as an
            // overshoot when the cap lifts; trimming down remains safe.
            if (error < 0.0f || !pressureCapped) {
                trim += KI * error * dt;
            }
        }

        trim = std::clamp(trim, -MAX_TRIM_DOWN_RATIO * requestedFlow, MAX_TRIM_UP_RATIO * requestedFlow);

        // Exact pass-through until the loop has ever engaged, so running with
        // the setting on but no scale is bit-identical to running with it off.
        if (trim == 0.0f) {
            return requestedFlow;
        }

        // Quantise so the delta-gated BLE link does not carry a new frame every
        // control cycle while the trim creeps.
        float command = std::round((requestedFlow + trim) / QUANT_STEP) * QUANT_STEP;
        return command > 0.0f ? command : 0.0f;
    }

    void reset() {
        trim = 0.0f;
        lastUpdateMs = 0;
    }

    float getTrim() const { return trim; }

  private:
    static constexpr float KI = 0.30f;                 // integral gain, (ml/s per s) per (g/s) of error
    static constexpr float MIN_MEASURED_FLOW = 0.3f;   // g/s below which cup flow is not established
    static constexpr float MAX_TRIM_DOWN_RATIO = 0.75f; // command never falls below 25 % of the requested flow
    static constexpr float MAX_TRIM_UP_RATIO = 0.5f;    // command never exceeds 150 % of the requested flow
    static constexpr float QUANT_STEP = 0.05f;          // ml/s command resolution
    static constexpr float MAX_DT_S = 0.5f;             // guards against integration bursts after stalls

    float trim = 0.0f;
    unsigned long lastUpdateMs = 0;
};

#endif // FLOWTRIMMER_H
