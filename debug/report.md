# Flow detachment investigation

Analysis of `normal-shot-3.json` and `bad-shot-6.json` (profile "Pure Flow", id `m5zwi3VR3J`) against the current source tree. No code changes were made; this is a findings report. Revised after cross-checking eleven independent analyses of the same data; every addition was re-verified against the code and logs before inclusion.

## Summary

The detachment is not a scale problem and not a transient glitch. It is a structural property of how flow phases are executed. Flow control is pure open loop: the firmware computes a PSM duty cycle by inverting the configured pump model at the current pressure and never checks any measured flow. The "current pump flow" it reports back (`fl` in the shot log) is the same model evaluated in the forward direction, so the inversion cancels algebraically and `fl` is pinned to the setpoint by construction. It cannot show a deviation even in principle. When the real pump delivers more than the model predicts, which the logs show it does increasingly with pressure and duty, the real flow rises without any correction, pressure climbs, the duty compensation raises drive further, and the shot runs away until a pressure limiter or the hardware OPV intervenes. In the bad shot no software pressure limit was active (the executed phase had `tp = 0`, see below), so the loop ran to 7.7 bar and delivered up to 3.44 g/s against a 2.0 ml/s target while the graph showed a perfectly flat calculated flow of exactly 2.00.

The apparent randomness is explained by the data as well: both shots had essentially the same puck by the end (measured conductance about 1.2 to 1.3 ml/s per √bar), but the same plant has two stable operating points under this control scheme, and the state of the puck at the moment the preinfusion pressure cap lifts selects which one the shot falls into.

## How flow phases are actually executed

The display sends each advanced phase to the controller as a target plus a limit (`src/display/core/Controller.cpp:933-941`). A flow phase arrives via `DimmedPump::setFlowTarget(flow, pressureLimit)` (`lib/GaggiMateController/src/peripherals/DimmedPump.cpp:75-80`). Note that `PressureController::setFlowLimit()` and `setPressureLimit()` are empty stubs (`lib/NayrodPID/src/PressureController/PressureController.h:22-23`); limits only exist through the arbitration in `PressureController::update()` (`PressureController.cpp:60-78`), which takes `min(flowDuty, pressureDuty)` when both setpoints are positive. When the pressure limit is zero, the flow branch runs alone with no ceiling of any kind.

The flow branch is `getPumpDutyCycleForFlowRate()` (`PressureController.cpp:110-118`):

```
duty = (flowSetpoint + slip) / Q_geo(P) * 100
```

`Q_geo(P)` is the configured full-drive curve evaluated at the current filtered pressure. With `pumpModelCoeffs = "6.4,3.9"` the two-value path (`GaggiMateController.cpp:189-190`, `PressureController.cpp:120-126`) produces the line `Q(P) = 6.7125 - 0.3125 P` ml/s and slip is zero for a vibration pump. There is no feedback term. Nothing measured (not the scale, not anything derived from pressure) ever adjusts this duty.

`virtualScale()` (`PressureController.cpp:162-166`) then estimates pump flow as `pumpFlowModel(duty) = duty * Q_geo(P) - slip`. Substituting the duty above gives exactly `flowSetpoint`. The exported value is only a low-pass filtered copy of that constant, and it is what the controller reports as current flow (`DimmedPump.cpp:23`), what the display stores as `fl` (`ShotHistoryPlugin.cpp:157`), and what the graph plots. In a flow phase, `fl` is the target wearing a different name. The logs confirm this: in phase 3, `fl` equals `tf` in 95 % (normal) and 92 % (bad) of active samples, and the residual samples are purely the low-pass transient at the phase boundary. In the bad shot `fl` converges smoothly onto 2.00 and stays there while the actual pressure quadruples.

The scale reaches exactly one place in the control path: `updateVolume()` for the volumetric exit condition (`Controller.cpp:1163`). That is why the shot still stopped at the right weight. `BrewProcess::currentFlow`, used for `waterPumped` accumulation and flow-based phase exits, is fed the model estimate, not the scale (`Controller.cpp:264, 602-603`).

## What the two logs show

Both shots ran the same three flow phases: Infuse (6 ml/s, 2 bar limit), Infusion Bloom (2 ml/s, 2 bar limit), Pure Flowing (2 ml/s). Sample interval 250 ms. All figures below are from the phase 3 samples with `tf > 0`.

| | normal-shot-3 | bad-shot-6 |
|---|---|---|
| Phase 3 entry state (model puck resistance `pr`) | 1.05 | 2.44 |
| Duty admitted by the 2 bar cap at end of bloom (`fl`) | 1.35 ml/s | 0.77 ml/s |
| Pressure after cap lifts | 1.8 → 2.7 bar over 8 s, then settles 2.2-2.3 | 2.0 → 7.6 bar in about 1.5 s, peak 7.7 |
| Late-phase operating point | 1.79 g/s at 2.28 bar | 3.25 g/s at 6.63 bar |
| Deviation from 2.0 target | -10 % | +63 % average, +72 % peak |
| Model duty at that pressure | 33 % | 43 % |
| Measured puck conductance vf/√cp at end | 1.19 | 1.26 |
| Integral of `fl` over phase 3 vs scale delta | 49.1 ml vs 42.4 g | 29.3 ml vs 41.2 g |

Two things stand out. First, the pucks end up nearly identical (conductance 1.19 vs 1.26), so grind and dose do not explain the difference; the same plant settled at two different equilibria. Second, the model error has opposite signs at the two operating points: at 33 % duty and 2.3 bar the pump delivers about 10 % less than modelled, while at 43 % duty and 6.6 bar it delivers 63 to 72 % more. The implied full-drive flow at the bad shot's operating point would be 7.5 ml/s at 6.6 bar, which exceeds the pump's own zero-bar rating of 6.7 ml/s. A vibration pump cannot out-flow its zero-bar rating at 6.6 bar, so the proportional-in-duty assumption itself is violated: between roughly 33 % and 43 % duty the real relative output jumps from about 30 % to about 70 % of full drive. The `duty * Q_geo(P)` model is structurally wrong in exactly the region the bad shot entered.

A mass balance over the bad shot's steady tail removes any remaining doubt about which instrument is lying. Between 20.0 s and 28.75 s the cup gained 28.1 g, while the model's integrated pump flow over that window is 2.0 ml/s times 8.8 s, or 17.6 ml, and decompression from the falling pressure (7.6 down to 6.3 bar) can release at most about 1 ml from system compliance. Roughly 10 g of delivered water is unaccounted for unless the pump actually ran near 3.2 ml/s. This also disposes of any stored-water or compliance-release explanation for the excess: there is nowhere in the hydraulic path for 10 g to have been waiting.

The account is falsifiable against the wider shot history: any flow-phase shot whose brew pressure stays below about 3 bar should track its target within about 10 %, and above that the detachment should grow with the steady operating pressure, independently of grind or profile. A counterexample in the existing history would break this analysis and should be treated as such.

## The runaway mechanism and why it looks random

The feedforward creates positive feedback with no stabilising element. If real flow exceeds what the puck passes at the current pressure, pressure rises; the model then says less flow is available (`Q(P)` falls), so it raises duty to compensate; the real pump responds super-proportionally to that duty increase, delivering even more flow; pressure rises further. The loop only stops where the puck conductance grows enough to swallow the flow, or where a limiter cuts drive.

Which basin a shot falls into is decided in the first seconds of phase 3, in a race between the pressure ramp and the puck opening up. In the normal shot the puck was already passing 1.35 ml/s at 1.8 bar when the bloom cap lifted, its resistance was falling (`pr` 1.05 and dropping), and pressure equilibrated at 2.7 bar before duty ever left the well-behaved region. In the bad shot the puck was still tight (`pr` 2.44 and still rising to about 3.0 for another 1.5 s), so the unrestricted feedforward blew through 4 to 6 bar within two seconds, entered the super-proportional duty region, and the resulting 3 g/s hammering eroded the puck only after the operating point was already parked high. Normal grind and preparation variance moves this entry state around a threshold, which is why the failure appears random. Pressure spikes correlate because the pressure ramp is the runaway, not a separate cause of it.

On the over-pressure valve: the setup notes provided alongside the parallel analyses put the OPV near 10.5 bar. If that is right, the 7.5 to 7.7 bar plateau is not the OPV at all but the natural equilibrium where the over-delivering pump meets the still-eroding puck, and the valve played no part in either shot. Even without relying on that setting, the OPV cannot explain the observation: diversion removes water from the group and would make cup flow lower than commanded, while the bad shot shows the opposite, and the detachment is already about +30 % by 5.8 bar. The OPV hypothesis is therefore wrong as a trigger; at most the valve would bound a more extreme runaway that the software currently does not.

## The phase 3 pressure limit was not active

Both logs record `tp = 0` for every phase 3 sample, while phases 1 and 2 correctly record `tp = 2`, and `debug/profile.json` says phase 3 should carry `pressure: 9`. The encoding works (phases 1 and 2 prove it) and parsing is direct (`src/display/models/profile.h:303`), so the profile stored on the machine at the time of both shots had 0 (or no value) as the phase 3 pressure. The copy in `debug/profile.json` does not match what ran; it was probably edited after these shots. Worth checking the profile actually stored on the device.

Two further details support the file-versus-device mismatch. The bad shot logged a 93 °C target temperature against the file's 90 (the normal shot logged 90), so at least one other field differed at run time; note the on-device temperature control writes into the loaded profile, so this particular difference could also be a manual adjustment rather than an edit. More concretely, there is a silent path for the limit to become 0: the web editor parses the maximum-pressure field with `parseFloat` (`web/src/pages/ProfileEdit/ExtendedPhase.jsx:310`), so a cleared input produces NaN, which serialises to null and loads back as 0, and 0 is this field's documented "Ignore" sentinel. Clearing and retyping that field once is enough to strip a profile's ceiling without any visible sign.

Two consequences. With `tp = 0`, `update()` skips the min arbitration entirely and there is no software ceiling at all, which is what let the bad shot run unchecked. But restoring the 9 bar limit would not have prevented this shot: the sliding-mode pressure branch only starts undercutting the flow branch as pressure approaches its setpoint band, and at the observed peak of 7.7 bar a 9 bar limiter still outputs more duty than the flow branch, so `min()` keeps selecting the flow branch and the trajectory is unchanged. The limit trims the extreme; it does not touch the detachment, which begins around 3 to 4 bar.

The same applies to flow limits on pressure phases: they pass through the identical open-loop duty computation (`DimmedPump.cpp:82-87`), so any profile "limit" on flow is enforced against the model, not against reality.

## Secondary findings

The puck flow and estimated weight channels are dead in both logs: `pf` and `ev` are 0 in every sample. The estimator's state machine in `virtualScale()` (`PressureController.cpp:204-234`) never reached state 1, so `_coffeeFlowRate` and `_coffeeOutput` stayed at zero for the whole shot in both cases. This matters beyond cosmetics because the estimated weight is the fallback exit signal when no scale is connected.

`header.finalWeight` is captured from `currentBluetoothWeight` after extended recording ends (`ShotHistoryPlugin.cpp:251-252`), by which time the scale has re-tared or the cup was removed; both logs end with `v` dropping to 0 and the bad shot's stored volume is 0.1 g despite about 43 g in the cup.

`waterPumped` (`BrewProcess.h:142`) accumulates the pinned model flow, so pumped-water transitions and targets inherit the same fiction: in the bad shot the model under-counted real water by 29 % over phase 3, in the normal shot it over-counted by 16 %. Flow-based exit conditions (`profile.h:135`) compare against the same pinned value and can therefore never fire from a real flow excursion.

The pinning also propagates into the shot summaries. `avgFlow` is accumulated from `sample.fl` (`ShotHistoryPlugin.cpp:197-199`), so the two shots report headline averages of 2.29 and 2.33 g/s despite real deliveries differing by around 60 % in the main phase. Every view that trusts `fl` inherits the blindness, which is why the fault produced no visible anomaly in the history list.

A small archaeological note: `DimmedPump.h:63` declares `_opvPressure`, which is never read or written anywhere in the codebase. An OPV term was evidently planned for the pump model at some point and never implemented.

## Assessment of possible directions

These are observations, not implemented changes. They are numbered for cross-reference from `update-and-fix-handoff.md`.

**R1, make the divergence visible.** Chart `vf` beside `fl` and relabel `fl` as modelled or commanded flow; both are already logged side by side (`ShotHistoryPlugin.cpp:157`, `:160`), so this is largely web work. It also repairs the `avgFlow` summary statistic, and it turns the falsifiable prediction above into something checkable against the existing shot history before any control code is written.

**R2, scale-flow trim as a slow outer loop.** The user-proposed direction, feeding measured scale flow back into control, is sound in principle and the scale is the only instrument in the system that observes real flow. The constraints are real: `vf` is an EMA over 250 ms weight deltas with roughly a second of lag plus BLE jitter, drops arrive 10+ seconds into a shot, and the scale measures cup flow, not pump flow, so during preinfusion there is nothing to control against. The practical shape is a cascade: keep the feedforward as the inner loop and trim its effective setpoint slowly, engaged only once weight flow is established and frozen otherwise. The trim can live entirely on the display; see the handoff, section 3.

**R3, cross-check the two instruments at runtime.** Puck conductance computed from the scale (`vf/√cp`) and from the model (`fl/√cp`) agree within a few percent on the good shot and diverge by about 70 % on the bad one. Comparing them each sample and flagging when they diverge is a cheap live detector that would have separated these two shots as they ran.

**R4, rethink the pressure-ceiling policy on flow phases.** A meaningful ceiling well below the OPV (for example 6 bar for this profile) bounds the damage, since the runaway needs the 4 to 8 bar region to develop; the profile's intended 9 bar would not have helped. Separately, decide whether `pressure: 0` should really mean no ceiling at all, and validate the web editor input, since the `parseFloat` path described above can strip a profile's limit silently.

**R5, give the pump model a duty dimension.** The two-point calibration tool (`web/src/utils/pumpFlowCalibration.js`) rescales the full-drive curve but keeps the proportional-in-duty assumption, so it cannot correct the 30 %-to-70 % relative-output jump between 33 % and 43 % duty that this data demonstrates; it also measures in pressure mode at whatever duty the pressure loop settles at, which is not the operating point flow phases use. A useful calibration must measure delivered flow at several fixed duties. The affine offset the model lacks already exists in the code as the slip term, but it is gated to gear pumps (`Controller.cpp:788-791`, `GaggiMateController.cpp:195-198`); ungating it for vibration pumps gives the model a shape capable of representing the observed response, with the caveat that its parameters are not identifiable from shots like these two, where duty and pressure move together.

**R6, log the commanded duty.** The controller already reports pump power in telemetry (`pump_power`, `lib/NanoPbComm/proto/gaggimate.proto:193`) but the shot log does not record it. Adding it to the slog is display-only and would separate duty nonlinearity from pressure-curve error directly in future data, which is exactly the ambiguity two shots cannot fully resolve.

One perverse interaction worth knowing about: the chart-derived `"6.4,3.9"` is lower than the shipped default `"10.205,5.521"` (`src/display/core/constants.h:20`), so the firmware commands 33 to 46 % duty where the default coefficients would command 21 to 29 %. The more accurate full-drive coefficients push the machine deeper into the duty region where the proportionality assumption fails hardest. Under this model, accuracy of the pump curve and accuracy of delivered flow are decoupled, which is also why the calibration tool's plausibility band would not flag this machine as anomalous.

## Answers to the specific questions raised

The scale and its readings are fine; the user's instinct that this is not a scale problem is confirmed. The suspicion about "the code that calculates pump flow" is exactly right, with the sharper statement that the calculated flow is not a measurement at all but the setpoint reflected back through the model. The pressure-limit and OPV suspicion is half right: the OPV bounds the excursion but does not cause it, and in these shots no software pressure limit was active because the executed profile carried 0 for phase 3. The randomness is deterministic sensitivity to the puck's conductance at the moment the preinfusion cap lifts.
