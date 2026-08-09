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

## Step 1: arbitration latch in the controller (as built)

The one control change. Two defects are fixed together because the second amplifies the first, and both carry a failing-test proof in the native harness.

**Defect 1, integral wind-up at the 0 bound.** The anti-windup in `getPumpDutyCycleForPressure` only froze integration when the raw output magnitude exceeded 100 percent, so with the output railed at 0 and pressure above the setpoint the integral kept accumulating through the virtual band below zero, and recovery after an overpressure took tens of seconds of cut instead of under two. Fixed with conditional integration at both actuator bounds.

**Defect 2, the conditioning wipe.** The v1.8.4 override tracking conditions the pressure branch's integral to the flow branch's duty on every cycle the flow branch wins. Before first engagement that is exactly right (continuous handover, validated on shots 22 to 24). During an established ceiling hold it is the amplifier of the shots 54/55 saw-tooth: any measurement excursion that lets the feed-forward win a few cycles (a pump-stroke trough at high pressure, a sensor transient) rewrites the integral, erasing the sustainable duty the loop had learned, and re-engagement slams the pump from the feed-forward duty into a hard claw-back. Reproduced in the harness: after a converged 36 percent hold, a 2 bar measurement trough lasting 0.24 s left the old code cutting the pump to 0 for half a second and crashing pressure to 7.7 bar; the fixed code re-engages at the preserved 36 percent, never falls below 24, and resettles within 1.5 s.

Mechanism, all inside `PressureController`, simpler than first designed:

- The output law never changes: `min(feed-forward, pressure branch)` everywhere, so no handover step exists by construction. The latch changes only what happens to the pressure integral while the feed-forward wins.
- Unlatched: condition per cycle, exactly as v1.8.4 (first engagement stays continuous).
- Entry: the pressure branch has strictly won for 0.15 s (about five cycles), filtering noise spikes.
- Latched: while the feed-forward wins, the pressure integral freezes (this cycle's error integration is undone, unless the evaluation already froze at a saturation bound).
- Release: pressure more than 1.5 bar below the filtered setpoint continuously for 0.5 s, meaning the ceiling stopped binding (the puck opened, or the phase ceiling rose). The persistence stops deep pulsation troughs from unlatching mid-hold. Release timing is not output-critical because the output law never switches.
- The latch clears in `reset()` and in every non-arbitration mode branch.

The margin-and-hold release machinery from the earlier draft was dropped: with `min()` as the single output law it served no observable purpose.

Safety: the pressure branch keeps full, immediate down authority in every regime, and the profile's flow target remains an upper bound on delivery at all times. The change removes only the mechanism that re-slammed duty into an overpressured puck mid-regulation.

### Tests (native harness, `test_pressure_arbitration`, all passing; the two starred failed first on the old code)

1. *Measurement dip must not re-arm the feed-forward (the wipe, directly).
2. *Pinned overpressure must not wind into a cut spiral (the 0-bound wind-up).
3. Choked puck holds the ceiling quietly, with the plant carrying the real machine's stiffness (compliance about 3/P near the ceiling) and PSM actuation quantisation.
4. Puck erosion resettles without overshoot or saw-tooth.
5. Puck opening hands back to the feed-forward without a duty step.
6. A transient spike does not stick the latch.
7. The three pre-existing cases stay green: below-ceiling passthrough, continuous first engagement, regulation on the shot 25/26 plant.

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
