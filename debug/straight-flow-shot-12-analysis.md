# Shot 12 analysis: constant flow against a variable restriction

First validation run on the flashed flow detachment firmware, recorded 2026-08-02 on display build `v1.8.1-152-g672d7170-dirty`. Companion files: `straight-flow-profile.json` and `straight-flow-shot-12.json`. The raw log was read from the machine at `/api/history/000012.slog`.

## Method

Brew mode with the water diverted through the steam wand, using the wand valve as a variable restrictor to stand in for puck resistance, and a Bluetooth scale catching the output so `vf` gives ground truth. The profile is a single 60 second flow targeted phase requesting a constant 2 ml/s with no pressure limit (`"pressure": 0`). Scale Flow Compensation was off for the whole run, so this is an open loop baseline and nothing in the machine was closing a loop on delivered flow.

## Instrumentation verified

The recording confirms the v6 shot log end to end: version 6, sample size 28 bytes, fields mask `0x3fff`, 246 samples, file length 7400 bytes exactly matching header plus samples. The new `pp` duty channel carries real data across 239 samples spanning 0 to 61.9 percent, and `vf` carries real scale flow across 197 samples peaking at 3.37 ml/s.

## Result

Requested flow was 2.00 ml/s for the entire phase and `fl` reported 2.00 ml/s for the entire phase, because `fl` is the setpoint evaluated forward through the pump model rather than an observation. The scale tells a different story in every segment.

| Window | Pressure (bar) | Duty (%) | `fl` claims | Scale `vf` | Error |
| --- | --- | --- | --- | --- | --- |
| 0 to 15 s, valve shut | 0.3 to 11.2 | 30.0 to 61.9 | 2.00 | 0.01 | -99.7 % |
| 15 to 20 s, valve opened | 1.3 to 5.9 | 31.7 to 39.0 | 2.00 | 2.73 | +36.5 % |
| 20 to 35 s | 0.7 to 1.3 | around 31 | 2.00 | 2.08 | +3.9 % |
| 35 to 59 s, modulating | 1.0 to 2.4 | 31.2 to 33.6 | 2.00 | 1.67 | -16.7 % |
| Whole flowing period | 0.7 to 5.9 | 30.8 to 39.0 | 2.00 | 1.93 | -3.6 % |

For the first fifteen seconds the chart drew a flat and perfect 2 ml/s while nothing at all left the spout, with commanded duty climbing to 61.9 percent against 11.2 bar. This is the detachment signature described in `report.md` reproduced deliberately, and the `pp` channel added in v6 is what makes it visible. The whole flowing period average of -3.6 percent is misleading on its own, since it averages a +36.5 percent overshoot against a -16.7 percent shortfall.

## The error is concentrated in compression, not in the pump model

Integrating `fl` over the whole shot gives 119.5 g equivalent against a real final scale weight of 87.8 g, an over-claim of 36 percent. Integrating over the flowing period alone gives 88.0 g against 87.8 g actual, which is very nearly exact.

That decomposition matters. The pump model is not badly calibrated once water is actually moving. Essentially the entire discrepancy comes from the phase where the pump is compressing the system rather than delivering to the cup, and the model has no concept of that distinction, so it reports the requested flow as though it were leaving the spout.

## Limits of this run

The flowing period sat between 0.7 and 2.4 bar, far below the 6 to 9 bar where real shots operate, and pump model error is pressure dependent. The -16.7 percent shortfall therefore characterises the low pressure regime only. The one high pressure segment in this run carried no flow at all, so it says nothing about model accuracy under load. Nothing here should be generalised to espresso pressures.

## Response timing

The apparent sluggishness when the valve moves is measurement lag, not control lag, and there was no control loop running at all. At t=14.75 s `vf` reads 0.00 with pressure at 9.7 bar; one sample later at t=15.00 s it reads 1.50, and it peaks by t=16.0 s. That is roughly one 250 ms sample of delay, consistent with the 800 ms `brewDelay` setting plus the scale's smoothing window.

## Incidental findings

The `pf` and `ev` channels are not dead as previously assumed. Both populate from exactly t=35.0 s onward, `pf` reaching 2.02 and `ev` reaching 48.8 g. Why they begin at 35 seconds rather than at the start of the shot is unexplained and worth a separate look.

The JSON exported from the web UI omits the `pp` column although the raw `.slog` contains it and the chart renders it. `parseBinaryShot.js:68` and `chartSeries.js:72` both handle the field correctly, so the loss is in the export path rather than in recording.

## Bearing on the trim

`FlowTrimmer` only integrates once measured flow exceeds `MIN_MEASURED_FLOW` of 0.3 g/s, so it would have remained inert throughout the fifteen second dead period rather than winding up against a closed valve, which is the failure mode this run exposes most starkly. Combined with a real -16.7 percent steady state shortfall in the modulating window, there is genuine work for the trim to do and its guards match the hazard. Two or three further runs are wanted before enabling it, including at least one that sustains espresso pressure while passing water.

## Next test

Hold pressure plateaus with flow, rather than sweeping. Crack the valve just enough to sustain a target pressure while water still passes, hold for ten to fifteen seconds so the scale settles, then step to the next plateau. Aim for roughly 3, 6 and 9 bar. Repeat across flow setpoints of 1, 2 and 4 ml/s, which additionally supplies the multi duty calibration data that the deferred pump slip ungating work needs. Keep the trim off throughout so the baseline stays clean.
