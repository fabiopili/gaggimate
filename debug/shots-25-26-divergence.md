# Shots 25 and 26: the ceiling-less divergence, 2026-08-06

Two shots on the updated "Pure Flow" profile (`m5zwi3VR3J`, recorded in `profile-m5zwi3VR3J.json`), which differed from the version validated by shots 23 and 24 in two ways: the flow targets rose from 2.0 to 2.4 ml/s, and the extraction phase pressure went from 9 to 0. Both shots diverged: weight flow crept 0.3 to 0.6 g/s above target, the pump surged audibly, and shot 26 climbed to 10.3 bar at 77.5 % duty. The firmware was unchanged from the validated v1.8.3 configuration, so this is a characterisation of what those two profile edits expose, and it motivated three fixes.

## Pressure 0 removes the ceiling entirely

`PressureController::update` only arbitrates between the flow and pressure branches when the pressure setpoint is positive, so the edited profile ran the extraction phase as pure feed-forward. The logged duty is reproducible to a fraction of a point as `flow_cmd / (6.7125 - 0.3125 * P) * 100`, the affine 6.4/3.9 model, all the way up the climb, confirming nothing bounded it. The display-side `pressureCapped` guard on the trimmer also depends on a positive limit, so the v1.8.3 anti-windup protection was inoperative in the same phase. Fixed by substituting a 9 bar default on the display when a flow phase carries no positive limit (`PumpLimits.h`).

## The trim entered extraction railed high and could not recover in time

`fl` sat pinned at 2.65, which is exactly 2.4 times 1.1 quantised to 0.05, for about seven seconds of shot 26. The 2.4 ml/s target lives at 45 to 77 % duty and 5 to 10 bar, where the pump model under-predicts delivered flow by up to roughly 45 % (settled command near 2.05 against 3.0 g/s on the scale at shot 25's cut), so the settled trim would need to be around minus 0.5 to 0.7 ml/s. Starting from the +10 % clamp with KI 0.30, much of it slew-gated during the climb, it never got there within the shot. This is the model error the duty surface calibration exists to remove; no gain retuning was done from these shots.

## The audible surging was the slew gate toggling the proportional term

In both tails the pressure decay slew hovered right at the 0.30 bar/s gate, and each boolean flip moved the whole proportional term (about 0.16 to 0.2 ml/s here) in and out of the command, which the feed-forward converted into duty steps of several points at these pressures. Shot 24's tail held within a point under the identical trimmer because its model residual, and therefore its proportional term, was near zero. Fixed by fading trim authority linearly to zero between 0.30 and 0.45 bar/s (`FlowTrimmer.h`, tests in `test/test_display_controls`).

## Restoring the ceiling makes the arbitration chatter operational

At 2.4 ml/s on these pucks the 9 bar ceiling will actually engage, and the arbitration zeroed the pressure branch integral on every cycle the flow branch won, so engagement started from a fresh integral, collapsed the duty towards the raw error term and relax-oscillated. The closed-loop test against a toy puck that wants 10.1 bar reproduces this: the old code never even holds the ceiling. Fixed with override tracking, conditioning the inactive branch's integral so its output meets the winning duty at handover (`PressureController.cpp`, tests in `test/test_pressure_arbitration`).

## Secondary observations, not acted on

Shot 26 started at 6.3 bar residual from the back-to-back shot and the controller's puck estimator never armed: the conductance state machine needs the headspace-fill transient, so `pf` and `ev` stayed zero for the whole shot. The volumetric cut still worked from the real scale. Left on the list.

The slew gate also has a blind spot the fade does not close: shot 25's ceiling-less climb ran slowly enough to slip under the gate, letting the trimmer integrate hydraulic cup-flow lag as model error mid-shot. With the ceiling restored the long slow climbs should not recur, and the duty surface calibration shrinks what the trim has to do; revisit only if it shows up again.
