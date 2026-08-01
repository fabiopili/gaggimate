# Flow detachment fix

Documentation for the changes on `fix/flow-detachment`, implemented 2026-08-01 against upstream commit `7e9fddea`. The investigation that motivated this work is preserved verbatim in `debug/report.md`, together with the two shot logs it analyses (`debug/normal-shot-3.json`, `debug/bad-shot-6.json`). Read that report first; this document only describes what was changed in response.

## The problem in one paragraph

In flow controlled phases the controller runs the pump entirely open loop: it inverts a static pump model to pick a duty cycle and never consults any measurement. The flow it reports back is the same model evaluated forward, so the charted flow is algebraically pinned to the setpoint and cannot show a deviation even in principle. When the real pump delivers more than the model predicts, the shot can run away to high pressure while every instrument in the interface reads perfectly on target. The Bluetooth scale is the only instrument in the system that observes real flow.

## What changed

### Shot log format v6 (report item R6)

`src/display/models/shot_log_format.h` bumps `SHOT_LOG_VERSION` to 6 and adds a per sample field `pp` (bit 13 of the fields mask, sample size 26 to 28 bytes): the controller reported pump duty in percent at 0.1 resolution, taken from `getCurrentPumpPower()` each 250 ms sample. This is the measurement that separates duty nonlinearity from pressure curve error in future data. The web parser (`web/src/pages/ShotHistory/parseBinaryShot.js`) is mask driven and reads `pp` only for v6 or newer files; older files parse exactly as before. `rebuildIndex()` in `ShotHistoryPlugin.cpp` now reads each file's own record size instead of assuming the compiled size, which also repairs index rebuilds over pre v5 files.

### Honest charting and statistics (report item R1)

The `fl` series is the model output, not a sensor, so every user facing label now says so: "Pump Flow (modelled)" in the legacy shot chart, the analyzer (via the display label mapping in `labelVisuals.js`, which also covers the compare views), the metric cards, the statistics tables and the trend chart. The scale derived series (`vf`, "Weight Flow") was already plotted in both chart stacks; the analyzer shot details additionally gained an average weight flow card. A "Pump Duty" series is plotted in both stacks (hidden by default in the analyzer, shown only when the shot actually carries duty data).

The headline `avgFlow` in the shot index is now computed from the scale flow whenever at least eight positive scale flow samples exist (two seconds at the sample rate), falling back to the model average otherwise. Live recording, the end of shot stats event and index rebuilds all use the same rule; rebuilds exclude extended recording samples via the system info bit, mirroring the live gating.

### Scale based flow trim (report item R2)

`src/display/core/FlowTrimmer.h` implements a deliberately slow integral only outer loop, applied in `Controller::updateControl()` for flow targeted brew phases only, behind the setting `flowTrimEnabled` (NVS key `ft_en`, web toggle "Scale Flow Compensation" in Settings, Machine tab). Default is off, and it should stay off until validated against real shots.

Design parameters and safeguards:

- Integral gain 0.10 (ml/s per second) per (g/s) of error, far below the bandwidth of the scale lag (about 1 s BLE latency plus the 4 s least squares rate window).
- Engages only when the measured cup flow reaches 0.3 g/s with a healthy Bluetooth source; frozen (held, not reset) when the scale drops out mid shot.
- Never integrates upward while a pressure limit is binding (measured pressure at 90 percent or more of the phase's cap), preventing wind up that would discharge as an overshoot when the cap lifts.
- Command clamped between 25 and 150 percent of the requested flow, quantised to 0.05 ml/s so the delta gated BLE link carries at most a few frames per second.
- Exact pass through while the trim has never engaged, so enabled with no scale is bit identical to disabled.
- Reset at process start; carries across phases within a shot, because the pump model error it compensates does not reset at phase boundaries.
- The trim can never feed on the flow estimation source (nightly builds): it requires the Bluetooth volumetric source explicitly, so the model's own estimate cannot loop back.

The logged target flow (`tf`) keeps the profile's requested value; only the commanded `pump.flow` carries the trim. The controller's reported `fl` tracks the trimmed command, so the shot chart shows request, command and reality as three separate lines.

### Profile editor input guard (report item R4)

All numeric inputs in both profile editors previously wrote `parseFloat` output straight into state. A cleared box became NaN, which a number input renders as blank (invisible), `JSON.stringify` serialises as null, and ArduinoJson reads back as 0, the documented "ignore" sentinel for limit fields. That is the most plausible way the investigated profile silently lost its 9 bar ceiling. `web/src/utils/number.js` provides `parseFloatOr`, and every editor input now refuses a cleared value by falling back to the previous one; an explicit 0 must be typed.

The firmware semantics of `pressure: 0` (no ceiling) are unchanged; that policy decision from report item R4 remains open.

### Final weight capture

`header.finalWeight` was read after extended recording ends, by which time the scale has typically re-tared or the cup was removed (the investigated bad shot stored 0.1 g against about 43 g in the cup). The plugin now tracks the highest plausible scale weight of the shot (steps larger than 5 g per sample are ignored, and weight only accumulates during a shot) and stores that instead.

## Verification

An adversarial code review ran over the full diff. `FlowTrimmer.h` was compiled standalone under `-std=gnu++17 -Wall -Wextra -Werror` and exercised with a behavioural harness covering no scale, scale loss, over delivery and pressure capped cases. The web bundle builds cleanly and ESLint reports no new problems. Scale-less operation with the trim off was verified to be exactly unchanged. The display firmware build (`pio run -e display`) is the remaining verification step before flashing.

## Deferred work

Deliberately not implemented, per the staged plan in the report:

- R3, live cross check of scale versus model puck conductance as a detachment detector.
- R5, ungating the pump slip term for vibration pumps; its parameters are not identifiable without a calibration that measures delivered flow at several fixed duties.
- The dead `pf`/`ev` channels (the controller's `virtualScale()` puck state machine never leaves state 0); controller firmware.
- A policy for `pressure: 0` on flow phases (a machine level default ceiling would bound the runaway region; the profile editor guard only prevents accidental stripping).

## Validation plan for the first shots

The report makes a falsifiable prediction: flow phase shots that stay below about 3 bar should track their target within about 10 percent, and above that the detachment grows with operating pressure regardless of grind. With v6 logs, compare "Weight Flow" against "Pump Flow (modelled)" and watch the duty series: the failure signature is duty climbing with pressure while the modelled flow stays flat on target and the weight flow rises above it. Only after that picture is confirmed on this machine should the trim be enabled, initially with a low value profile.
