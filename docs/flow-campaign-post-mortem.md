# Flow control campaign post mortem

Written 2026-08-14, the day the campaign ended. The operator pulled the plug after shots 64 and 65: audibly rough pump cycling, flow tracking still wrong in both directions, and the machine spontaneously rebooting on standby, a symptom never seen before. The machine returns to stock firmware via the reset tag `v1.8.11`, which is upstream `jniebuhr/gaggimate` master at `9e6a69bf` built through the fork's own release pipeline. This document records what happened, what was learned, what remains useful, and how to run the machine on stock.

## The terminal shots

Shots 64 and 65 (2026-08-14, Pure Flow, target 2.0 g/s, capless extraction, cup-truth coefficients of 2026-08-13, firmware v1.8.10 on both components) failed in opposite directions, which is itself the diagnosis.

Shot 64 met a fast puck. The extraction ran at 1.1 to 5.9 bar and over-delivered for its entire second half: settled scale flow averaged 2.52 g/s against the 2.0 target (RMS 0.57), with the trim cutting the command all the way to 0.85 and the real flow still at 2.0 to 2.8. At 1 to 2 bar and duty 15 to 25 the cup received two to three times what any fitted surface predicts, part pump behaviour in a corner no calibration covers, part the saturated bed draining under falling pressure, which no pump command can stop.

Shot 65 met a tight puck. With no pressure cap, the flow loop marched the pump up its curve to the overpressure valve: fifteen seconds above 9 bar, peak 11.0, duty at 90, the trim pinned at its new +30 percent bound. Flow was actually near target through the hold (1.9 to 2.2), but at 11 bar, which is a ruined espresso regardless. As the puck eroded the shot then over-delivered like every other.

The audible roughness is measurable. Mean per-tick duty movement in the 2 bar bloom phase doubled against shots 62/63 two days earlier (4.5 and 3.9 points per tick against 2.6, with 95th-percentile steps of 15 to 16 points and five direction reversals per ten seconds against one). The roughness concentrates where the 2 bar bloom cap, the ceiling latch and the trim all meet at low duty. The cup-truth surface's negative slip constant lowers commanded duty near zero flow and moved that operating point; shots 62/63 ran the identical firmware on the previous coefficients and were calmer, so the coefficient change is the leading suspect for the degradation, though the logs cannot fully separate it from the pucks.

The standby reboots are unexplained. They appeared within a day of the coefficient change, but nothing in the coefficient path runs on standby, and nothing else changed on the machine that day. The candidates are the fork display build (heap, BLE or WiFi pressure accumulated over five OTA generations) or unrelated hardware or power. The stock reset is the correct isolation: if the reboots stop on stock, the fork display image is implicated; if they persist, it is the hardware, and no firmware conclusion should be drawn.

## What the campaign was

A three-week arc, each step gated on real shots, recorded in `docs/validation-campaign.md` and the `debug/` notes:

- v1.8.3: the scale-informed flow trim, after diagnosing the original defect where the display showed flow on target while shots ran away. Validated on shots 23/24. The wind-up cause, cup flow legitimately lagging pump flow during puck saturation, was understood here and never stopped mattering.
- Shot log v6: observability, the foundation everything later was graded on.
- The duty surface campaign: a web-only calibration tool measuring the pump's (duty, pressure) to flow surface on a rig, seventeen points, cleanly separable, affine to three percent.
- v1.8.7/v1.8.9: the ceiling-hold arbitration work, a latch preserving the pressure integral and a cap on the handback. Passed its gentle gates; its real choke-day gate was never successfully run, twice defeated by the capless-zero trap.
- v1.8.8: explicit semantics, a flow phase's pressure 0 passing through as truly unlimited instead of a silent 9 bar substitution, plus the finished-screen weight hold.
- v1.8.10: trim upward authority 10 to 30 percent, after shots 60/61 showed the model over-promising at the pump knee with the clamp pinned.
- 2026-08-13: the cup-truth coefficient refit from the offline replay, entered as settings.

Throughout, volumetric stops landed within about half a gram across every puck the campaign saw. That subsystem never wavered.

## Why it did not converge

The goal was modelled flow tracking measured flow. Three irreducible obstacles emerged, and every fix moved error between them rather than removing it.

First, on fast pucks the cup is not fed by the pump alone. The bed stores 0.5 to 4 g per bar (measured across sixteen shots by the replay tool) and gives it back as pressure falls, and a draining bed at 1 to 2 bar delivers water no duty command controls. Scale flow is the only truth available, and on exactly the shots that go wrong it stops being a pump measurement.

Second, the pump surface is only trustworthy where it was measured. Below 3 bar the pump delivers 20 to 30 percent more than the calibrated lines extrapolate; above 10 bar the behaviour is scatter, not bias, wandering half a gram per second with puck and thermal state. The two regimes real shots actually visit are the two the calibration cannot pin.

Third, the correction loop is bandwidth-limited by physics: the scale reports a second late through BLE plus smoothing, so the trim must be slow, and a puck eroding or channelling changes the plant faster than any safe gain can follow. Widening the trim's authority fixed under-delivery at the knee and bought wind-up over-delivery on saturating pucks; narrowing it does the reverse. That trade is structural, not a tuning error.

The honest conclusion: open-loop model inversion trimmed by a lagged scale is sufficient for pressure-dominated shots and calm pucks (shots 56/57 tracked at RMS 0.07 to 0.11), and insufficient for capless flow targets on pucks at either extreme. Closing that gap needs either a pressure cap bounding the operating envelope, or a real observer separating pump flow from storage flux in the reference, which exists half-built upstream in `PressureController::virtualScale()` but was dormant on five of six shots examined because of a brittle trigger state machine.

## What remains useful

`debug/flow-replay.py` is the piece most worth keeping. It replays the firmware's exact pump model (`Q(u,P) = u(F(P)+S(P)) - S(P)`, coefficients as the settings strings) against any exported shot log, self-checks against the logged `fl` (which is a low-passed forward evaluation of the same model, agreement 0.02 g/s), extracts flat-pressure steady-duty windows where cup flow genuinely equals pump flow, fits candidate surfaces by weighted least squares with leave-one-shot-out validation, and measures the storage coefficient from declining-pressure windows. Run it with `python3 debug/flow-replay.py` from the repo root; it reads every `debug/shot-*.json` plus `debug/pump-duty-surface.json`. Any future model claim should pass through it before touching the machine: it turns every recorded shot into a regression test that costs no coffee.

The duty-surface calibration tool (plan and status in `docs/pump-duty-surface-plan.md` and `docs/pump-duty-surface-status.md`) remains in the web UI on the fork branch: fixed-duty holds against the steam wand, windowed scale analysis, surface export. Its three review-caught defects (the extended-recording tail poisoning windows, the uncapped slew timestep, margin-less level coverage) are documented and fixed and are the kind that silently corrupt calibration data, worth rereading before any future rig session.

The shot corpus, shots 18 to 65 in `debug/`, spans every regime this machine reaches: calm pucks, gushers, chokes, limit cycles, capless OPV marches, with the v6 fields needed to replay them. The analysis notes (`debug/v187-validation-shots.md`, `debug/v189-shots-60-61.md`, `debug/v1810-shots-62-63.md`, `debug/flow-model-replay.md`) carry the per-day evidence. `debug/report.md` and the model eval built on it are a separate reusable asset.

Fork firmware pieces that proved themselves and would make clean upstream contributions, independent of the flow campaign: the finished-screen weight hold, the explicit no-ceiling semantics with its editor warning, the FlowTrimmer as an optional feature for pressure-bounded profiles, and the slip-model ungating for vibratory pumps. The ceiling latch and handback cap are plausible but were never validated on their target regime; they should not be offered without that shot.

## Hard-won process lessons

The operator's ear outranks the 250 ms log; every limit cycle was heard before it was measurable. One change per OTA with a real-shot gate was the only reason cause and effect stayed attributable across eleven releases. Silent substitutions are traps: the display quietly turning pressure 0 into 9 bar cost two test days, and hand-entered coefficient strings with no save-time guard ran a shot on transposed values. Guards and observability must land before or with what they protect. And a validated fallback one save or one OTA away is what made three weeks of experimentation on a daily-use machine tolerable.

## Running on stock: recommended settings

Stock firmware executes flow phases open loop through the two-point model, a line through the flow at 1 and 9 bar scaled by duty, with no scale trim to correct errors. From the joint fit of all flat-window shot data (89 samples across eight shots spanning 1.2 to 11 bar) and the seventeen-point rig surface:

```
Pump Flow Coefficients = 5.72,3.40
Pump Slip Coefficients = 0,0,0,0
```

This pair sits within about 4 percent of measured cup flow everywhere real shots ran except below 2 bar, where it under-predicts by about 10 percent (partly bed drainage the scale sees and the pump does not supply). The pure cup-truth alternative is `6.09,3.47`, which centres every shot band at the cost of about 7 percent against the rig; the difference between the two pairs is about 6 percent at low pressure and negligible above 6 bar. Replace the fork's four-value strings with the two-value pair after the reset; stock parses two values as the 1 bar and 9 bar flows and gates the polynomial path behind the gear pump addon.

Two operating notes for stock. Expect open-loop flow phases to run 10 to 30 percent hot on fast pucks and cold on choked ones, since nothing corrects the model there. And give flow phases an explicit pressure cap near 9 bar: on any firmware that honours a flow target, stock included, a capless flow phase on a tight puck will ride the pump curve to the overpressure valve, exactly as shot 65 did.

## The reset

`v1.8.11` is an annotated tag on upstream master `9e6a69bf` (2026-08-07). Pushed to the fork, the fork's Actions build it with upstream's own workflow (which triggers on any tag) and publish a release on `fabiopili/gaggimate`; the machine's current firmware points its OTA at the fork and updates only when the remote version is strictly newer, which v1.8.11 over v1.8.10 satisfies. The flashed firmware is pure upstream, so from then on the machine looks at `jniebuhr/gaggimate` releases. One consequence to know: upstream's newest tag is v1.8.1, so the machine will not see an upstream OTA until upstream's numbering passes v1.8.11; a USB flash of any genuine upstream release rejoins the official version line at any time. The `fork-ota` branch, all tags and this history remain on the fork for any future resumption.
