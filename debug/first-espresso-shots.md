# First espresso shots on the flashed firmware, 2026-08-03

Shots 18 and 19, the first real shots on a puck since the flow detachment work was flashed. Both ran the "Pure Flow" profile (`m5zwi3VR3J`), both exited on the volumetric target at 42.8 g and 43.1 g, and the operator judged both bad. Raw `.slog` files and web UI JSON exports sit alongside. This document records what they showed and corrects the conclusions of `pump-calibration-campaign.md`, which were drawn on a rig that does not represent real shots.

## Configuration during these shots

Display firmware carrying the flow detachment work, flashed 2026-08-02. The controller was not reflashed and is byte identical to upstream master, so nothing in the pump control path had changed. Pump model coefficients were `5.592,3.263` from the calibration run the previous day. Scale Flow Compensation was off. The phase 3 pressure limit had been restored to 9 bar, where the shots in `report.md` carried 0.

The only difference in preparation between the two was grind: shot 19 was finer so it would reach higher pressure.

## Establishing that the trim was off

Worth recording because it was determined from the logs before the operator confirmed it. In shot 18, `fl` sits at exactly 2.00 for sixty consecutive samples while `vf` runs at 2.5 to 3.0, which a trim at `KI` 0.10 would have driven strongly negative within seconds. In shot 19, `fl` swings 0.74 ml/s inside a single 250 ms sample, roughly fifteen times faster than the trimmer can move. Neither shot exercised any code we added to the control path.

## What the two shots did

| | shot 18 | shot 19 |
|---|---|---|
| Puck resistance `pr` in phase 3 | 0.9 to 1.5 | 1.4 to 4.5 |
| Phase 3 pressure | rose to 6.6 bar, decayed to 4.2 | reached 9.3 bar by t=15 s, held 7.7 to 9.3 |
| Pressure ceiling engaged | no, peak 2.4 bar below setpoint | yes, for two thirds of the phase |
| Real flow `vf` against a 2.0 target | 0.5 rising to 3.0 g/s | 0.3 rising to 2.9 g/s |
| Model claim over phase 3 | 34.6 ml | 29.5 ml |
| Scale caught over phase 3 | 41.9 g | 42.2 g |

Neither shot was flow controlled at any point. Shot 18 is the open loop runaway described in `report.md`, still present because the trim was off. Shot 19 was a 9 bar pressure shot wearing a flow profile's clothes.

## The model error, measured against commanded duty

The `pp` field added in shot log v6 makes this a measurement rather than an inference for the first time. Real duty agreed with duty inferred from `fl/Q_geo(cp)` to within 0.4 percentage points in both shots, which retrospectively validates the inference used before the `.slog` files were pulled.

Dividing scale flow by real duty gives the pump's response with no model involved. Steady pressure windows only:

| | pressure | duty | real full drive | model said | ratio |
|---|---|---|---|---|---|
| Shot 18 | 3 to 5 bar | 43.8 % | 6.50 ml/s | 4.56 | 1.42 |
| Shot 18 | 5 to 6.5 bar | 47.3 % | 5.49 ml/s | 4.23 | 1.30 |
| Shot 19 | 6.5 to 8 bar | 50.9 % | 4.88 ml/s | 3.63 | 1.34 |

Shot 19's 8 to 10 bar window is excluded deliberately. `vf` is smoothed over roughly a second while `pp` is instantaneous, so when the chattering duty momentarily collapses to 5 % the ratio explodes; it reads 5.80 and means nothing.

A line through the three trustworthy points gives roughly 8.0 ml/s at 1 bar and 4.0 at 9 bar. Against that, the calibrated `5.592,3.263` was 30 to 42 % low across the band and the earlier `6.4,3.9` is about 20 % low at the bottom and close to correct at 9 bar.

## Why the calibration made real shots worse

The campaign concluded the model was accurate to +2 % below 7 bar. It was measured on a steam wand rig at mean duty of 56 to 87 %, because the valve had to be fought to build pressure. Real shots at 2 ml/s sit at 37 to 51 % duty. A vibration pump under phase skip modulation is super proportional at low duty, since spaced strokes complete more fully than back to back ones, so pressure droop and duty are entangled and a fixed pressure calibration at high duty cannot describe the low duty regime.

Because the calibration lowered the coefficients, it also raised the gain of a loop that is not usually described as one. Flow mode recomputes `duty = S / Q_geo(P)` from measured pressure on every controller tick at 30 ms, so `P` rising lowers `Q_geo`, which raises duty, which raises real flow, which raises `P`. That is positive feedback with no damping term. Its sensitivity is `S·|b|/Q²`, which at 5.4 bar the calibration moved from 2.47 to 3.12 % duty per bar, a rise of 26 %.

Coefficients were reverted to `6.4,3.9` on 2026-08-03.

## The restored pressure ceiling introduced duty chatter

An unintended consequence of acting on R4. Detrended commanded duty has an RMS of 2.77 % in shot 18 and 8.15 % in shot 19, and shot 19's lag-1 autocorrelation is +0.16, meaning successive 250 ms samples are nearly independent and the real oscillation is faster than the log can resolve. Its duty trace runs 48, 48, 50, 16, 40, 43, 5, 23, 44, 16, 19, 32, 49.

The mechanism is the arbitration at `PressureController.cpp:64-71`. When both setpoints are positive it takes `min(flowOutput, pressureOutput)` and zeroes `_errorIntegral` on every cycle where the flow branch wins, so as the branches trade near the ceiling the integrator is repeatedly wound up and wiped. All three campaign shots ran a profile with `pressure: 0` and never entered this path. Fixing it is controller side work and has not been attempted.

## What remains unexplained

The operator heard periodic surging in both shots. Shot 19 is fully accounted for above. Shot 18's commanded duty is smooth, wandering under 3 % RMS with no periodicity resolvable at the log's 250 ms sampling, so whatever was audible there is either generated below the command layer or is faster and smaller than the log can see. No explanation is offered here rather than fitting one to the data.

## Why R5 as it exists cannot fix this

The affine slip model recommended as R5 is already merged upstream and present in our tree, arriving via `4cd2874e` (PR #789). It is inert only because the slip coefficients are gated to gear pumps. Ungating it would not help: `Q_net = duty·Q_full + slip·(duty − 1)` is at or below proportional for any duty under 100 %, and `getSlip()` clamps to non-negative because leakage is never negative. These shots need output above proportional at partial duty. R5 is the wrong shape for this pump, not merely unidentifiable from the available data.

## Corrections made while analysing this data

Recorded because each was stated with confidence before being overturned.

The commanded duty in shot 18 was first argued to be steady on the grounds that `fl` was flat at exactly 2.00. That inference is invalid by construction: when the flow branch wins, `fl = duty × Q_geo(P)` and `duty = S / Q_geo(P)`, so `fl` equals the setpoint whatever the duty does. Using the pinned value as a measurement is the exact error this whole investigation concerns.

The pressure ceiling was first proposed as the source of shot 19's chatter, then withdrawn when the operator reported cycling in both shots, then confirmed by the duty data. It was correct for shot 19 and wrong for shot 18.

A phase skip modulation beat was proposed as a mechanism generated below the command layer, on the basis that the PSM firing sequence repeats every `2/gcd(duty, 100)` seconds. It remains a candidate for shot 18 only, and the claim that the calibration made it newly audible does not survive checking, since both the old and new duty ranges contain values with short and long repeat periods.

`feature/positive-displacement-flow` was described as an unmerged upstream branch. It is merged, by squash, which is why it is not an ancestor of master.

## Next

Two shots at shot 18's grind, coefficients at `6.4,3.9`, Scale Flow Compensation on for the first time, with `KI` raised from 0.10 to 0.30 so the loop settles a 1 g/s error in about three seconds rather than ten. Coarser grind is deliberate, to stay off the 9 bar ceiling and out of the arbitration chatter while the trim is evaluated on its own.
