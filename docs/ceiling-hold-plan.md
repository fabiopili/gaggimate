# Ceiling hold fix and display corrections

Design for the agreed reset after shots 54 and 55 (2026-08-09). Three changes, shipped one OTA at a time, each gated on a real shot before the next lands. Everything else from the recent trajectory stays parked.

## Background: what shots 54 and 55 actually showed

Both shots ran the Pure Flow profile (flow 2.2 g/s, ceiling 9 bar, cut at 43.5 g) on v1.8.6 firmware. The machine's live settings at the time read `pumpModelCoeffs 5.3,3.3` and `pumpSlipCoeffs 0,0,0,0` with flow trim enabled, so the Tier 1 duty surface coefficients were not active and the slip model multiplied by zero. The failures are not attributable to the recent model work.

Both pucks choked: at 9 bar they passed roughly 1 g/s at first, opening to 2.2 only near the end. The controller therefore spent about 27 s regulating at the ceiling, a regime no real shot had validated before. There, commanded duty saw-toothed between about 63 percent (the flow branch feedforward for 2.2 g/s at 8.5 bar) and 35 to 42 percent, with repeated full cuts to 0, at roughly one second period, while pressure swung 8.4 to 9.4 bar. The chop cut mean delivery, stretched shot 54 to 48 s, and its pressure swings held the flow trimmer's slew gate at zero authority, so the validated trim loop was effectively offline throughout.

## Root cause

`PressureController::update` arbitrates flow and pressure branches with `min()` and, since v1.8.4, calls `trackPressureBranch` on every cycle the flow branch wins. That conditioning makes the engagement bumpless, but it also rewrites the pressure integral to the flow branch's duty on every such cycle. On a choked puck the sustainable duty at the ceiling (about 38 percent) sits far below the flow feedforward (about 63 percent). Each time pressure dips below the setpoint, the flow branch undercuts the recovering pressure branch, wins one or more cycles, and wipes the integral's learned value, so the pressure loop restarts from 63 percent, overshoots, and the cycle repeats indefinitely.

The v1.8.4 test validated a plant whose feedforward to sustainable gap was about 3.5 duty points, where the alternation is benign. The real choked puck's gap was about 25 points.

Two secondary defects deepen the cuts. The anti-windup freeze in `getPumpDutyCycleForPressure` only engages when the raw output magnitude exceeds 1.0 (beyond 100 percent), so while the output is saturated at 0 the integral keeps accumulating through the virtual band between 0 and minus 100 percent. And `trackPressureBranch` uses `_lastKi` from the previous pressure evaluation, which is fine while both branches are evaluated every cycle but is worth preserving explicitly in the new structure.

This defect exists upstream in a worse form (the integral is zeroed on every flow win rather than conditioned), which is the shot 19 chatter. Reverting to the upstream deviation point would not avoid it.

## Step 1: arbitration latch in the controller

The one control change. While a flow phase is pressure capped, the pressure loop owns the pump; the flow branch may only reclaim it deliberately, not through noise.

Mechanism, all inside `PressureController`:

- New state: `bool _ceilingLatched`, plus two accumulators for entry and release persistence.
- Unlatched (both setpoints positive): behave exactly as today. Evaluate both branches, output the minimum, condition the pressure branch while the flow branch wins. This keeps the below-ceiling path bit-identical and the first engagement continuous.
- Entry: when the pressure branch has won continuously for `ENTRY_PERSIST_S` (0.15 s, about five cycles), set the latch. Persistence filters sensor noise spikes into the latch.
- Latched: output the pressure branch alone, integral evolving continuously with no conditioning. The flow branch is still evaluated (it is stateless) for the release comparison and for logging.
- Release: when `flowOutput < pressureOutput - RELEASE_MARGIN_DUTY` (3 duty points) continuously for `RELEASE_HOLD_S` (1.0 s), clear the latch and return to `min()` arbitration. Physically this is the moment the puck passes the full flow target below the ceiling, so the handover step is bounded by the margin and is inaudible.
- The latch clears in `reset()`, and whenever the arbitration precondition fails (either setpoint not positive), so phase changes into pure pressure or unconstrained flow behave as today.
- Anti-windup: extend conditional integration to both saturation bounds: freeze the integral when the error pushes further into an already saturated output, at 0 as well as at 100 percent.

Safety: the pressure branch keeps full, immediate down authority in every regime. The latch removes only the feedforward's ability to re-slam duty into an overpressured puck mid-regulation. No overpressure response path gets slower.

### Tests (native harness, `test_pressure_arbitration`)

1. Choked puck reproduction: plant with sustainable duty near 38 percent while the flow feedforward at the ceiling is near 63 percent (the shot 54 gap). Old behaviour oscillates; new behaviour must settle within plus or minus 0.15 bar of the setpoint with no duty excursion to 0 after the transient.
2. Existing cases stay green: below ceiling passthrough unchanged, continuous first engagement, regulation on the shot 25/26 style plant.
3. Puck opening: ramp conductance until the target flow fits below the ceiling; the latch must release and the output step at release must not exceed the margin plus tolerance.
4. Saturated integral: pinned overpressure with output railed at 0; the integral must stay bounded and recovery must not undershoot into a cut spiral.
5. Noise latch: a transient pressure spike must either fail the entry persistence or release within the hold time, with bounded over-delivery.

### Validation gate

Rig first: the steam wand nearly closed reproduces a pinned ceiling; check for a calm hold with no sustained duty cuts, without retuning any gains against that plant. Then one real shot on a normal grind (expect behaviour identical to v1.8.3 away from the ceiling), and the fix is considered validated when a tight-puck day holds the ceiling quietly. Only then does anything else land.

## Step 2: end of shot weight latch on the display

Every shot log since shot 18 shows the recorded scale weight collapsing to 0 within one 250 ms sample of brew end, exactly when the display sends `stopTimer` to the Bookoo, while the physical scale keeps showing the true weight. The upstream final weight defect (0.1 g recorded for a 43 g shot) was the same zero read at write time; the fork already works around it in the history record via peak tracking, but the finished screen still binds the live value.

Change: `ShotHistoryPlugin::endRecording` already knows the peak plausible weight at brew end; add it to the existing `evt:shot-finished-stats` event, and have the UI hold that value for the finished screen while live weight continues to drive everything else. The dribble after brew end may raise the recorded history weight slightly above the screen value; the screen shows the weight at the cut, which is what the operator watched.

Display only, no controller image change. Gate: one shot, screen shows the extraction weight after the shot ends.

## Step 3: pressure 0 means unconstrained again

Reverse the v1.8.4 silent substitution: remove `PumpLimits.h` and pass the profile's raw pressure value through for flow phases, so 0 disables the arbitration entirely, as upstream semantics had it. Adjust the display control tests that assert the substitution.

Make the semantics explicit in the editor instead of implicit in firmware: a flow phase with pressure 0 shows a visible "no pressure limit" state, and newly created flow phases default to a visible 9. The `parseFloatOr` guard already prevents a cleared field from silently becoming 0; zero has to be typed deliberately.

Trade-off stated plainly: with no ceiling, a flow phase is pure feedforward and will chase the flow target up whatever pressure the puck produces (shots 25 and 26 reached 10.3 bar this way, with the trimmer railed at plus 10 percent). Under the new semantics that is the operator's explicit choice, guarded by the editor default and the visible state rather than by a hidden firmware substitution.

Gate: one shot on a capless profile behaving as commanded, plus editor behaviour verified.

## Parked, deliberately

- Tier 1 coefficient entry (`0,0,-0.289,6.074` and `0,0,-0.036,0.445`) stays off the machine until step 1 is validated, and lands together with the planned save-time plausibility guard when it does.
- Slip stays zeroed; the ungating code is inert with zero coefficients and needs no action.
- No pressure loop gain retuning, and none against dead-headed rig plants.
- The trimmer's known refinement (gate hold-off after a fast pressure decline, shot 52) stays queued behind these three.
- A visible "pressure limited" indicator during shots, so a choked puck reads as a state instead of a mystery, is queued after step 3.
- The flow-via-pressure cascade stays a long-term idea only.

## What stays untouched because it works

Shot log v6 observability, the FlowTrimmer as validated on shots 22 to 24 with the v1.8.4 authority fade, and the duty surface calibration tool as an instrument. The original question, improving flow calculation and tracking from the Bluetooth scale, is answered by that validated trim loop; these three steps repair the regime it cannot reach (the ceiling), the end of shot display, and the profile semantics around it.
