# Pump model calibration campaign, 2026-08-02

Three validation shots on the flashed flow detachment firmware, spanning the built-in pump flow calibration. Shot 12 is dissected separately in `straight-flow-shot-12-analysis.md`, which covers the detachment mechanism in detail. This document covers the campaign as a whole and its conclusions. Raw `.slog` files and web UI JSON exports for all three shots sit alongside.

Rig throughout: brew mode with water diverted through the steam wand, its valve standing in for puck resistance, and a Bluetooth scale catching the output so `vf` provides ground truth. Scale Flow Compensation stayed off for every run, so all three are open loop measurements of the pump model itself.

> **Superseded in part, 2026-08-03.** The central conclusion below, that calibration made the
> model accurate to +2 % under 7 bar, does not hold for real espresso shots and this document
> overstates it. The rig ran at 56 to 87 % duty because the wand valve had to be fought; real
> shots sit at 37 to 51 %, where the pump is super proportional and the model under-predicts
> delivered flow by 30 to 42 %. The calibration also raised the flow loop's gain by 26 %, and the
> coefficients were reverted to `6.4,3.9` on 2026-08-03. See `first-espresso-shots.md`, which
> measures the same quantities against real commanded duty rather than inferring them.

## What changed

The calibration tool was run between shots 13 and 17. It drives a pressure targeted profile holding 1 bar and then 9 bar, computes real flow from the scale weight delta and estimated flow from the integral of the reported model flow, and scales each coefficient by the ratio. Coefficients moved from `6.4,3.9` to `5.592,3.263`, factors of 0.874 and 0.837 respectively.

## Results

| Shot | Setpoint | Dead phase | Phantom flow claimed | Flowing claim | Actually delivered | Flowing error |
| --- | --- | --- | --- | --- | --- | --- |
| 12, before | 2.00 | 15.0 s | 28.9 g | 90.6 g | 87.8 g | +3.1 % |
| 13, before | 2.50 | 14.8 s | 35.5 g | 113.7 g | 98.9 g | +14.9 % |
| 17, after | 2.50 | 6.5 s | 14.6 g | 135.6 g | 126.6 g | +7.1 % |

Whole shot over-claim fell from 51 percent on shot 13 to 19 percent on shot 17. Part of that is the calibration and part is a shorter compression phase, since shot 17 started with the valve already cracked and so spent 6.5 seconds building pressure instead of about 15.

## The calibration worked on this rig, and the residual is a hardware limit

The finding in this section is real for the duty range it was measured over and does not generalise.
Everything below holds at 56 to 87 % duty; see the note above for what happens at the 37 to 51 %
that real shots use.


Splitting shot 17 by pressure separates two very different regimes.

| Regime | Samples | Mean duty | Target | Delivered | Error |
| --- | --- | --- | --- | --- | --- |
| Below 7 bar | 134 | 55.9 % | 2.44 | 2.49 | **+2.0 %** |
| 9 bar and above | 67 | 86.5 % | 2.39 | 2.00 | **-16.4 %** |

Below 7 bar the model is now essentially exact, against errors of -8.9 to -17 percent across the same band before calibration. The remaining shortfall appears only where mean duty reaches 86.5 percent and peaks at 95.7 percent, which is the pump running out of headroom rather than the model being wrong. No coefficient can recover flow that the pump cannot physically produce, so above roughly 9 bar at 2.5 g/s the machine is simply at its ceiling.

That is a useful operating boundary in its own right. It also means the earlier shots were measuring two different faults at once and attributing both to the model.

## Corrections to earlier readings in this campaign

Predictions made before the calibration ran, recorded so the comparison stays honest, were a factor near 0.98 at 1 bar and near 0.87 at 9 bar. The 9 bar prediction was close, the actual being 0.837. The 1 bar prediction was badly wrong, the actual being 0.874.

The structural claim was wrong too. Both coefficients moved by a similar factor, so the model error was much closer to a flat gain error than to a pressure dependent slip term. The earlier reading of rising slip came from comparing efficiency across shots 12 and 13, which used different flow setpoints over disjoint pressure ranges. That limitation was noted at the time and it turned out to account for most of the apparent trend. The within-shot efficiency trends remain valid, they were simply smaller than the cross-shot comparison implied.

## What still stands

The compression artefact is untouched by calibration and is inherent to the approach. The model reports the requested flow while the pump is pressurising the system, so every shot claims delivery before anything reaches the cup, 14.6 g even in the well-run shot 17. Only the phase length changed, because the valve started cracked. Correcting this needs the model to distinguish compression from delivery rather than better coefficients.

Shot 17 was recorded with unstable plateaus by the operator's own account, so the per-window figures in the plateau table are indicative rather than precise. The aggregate split above rests on 134 and 67 samples respectively and is the more reliable statement.

## Next steps

Real shots on a puck are now the better instrument, since a puck holds pressure passively where the valve has to be fought, particularly at the low end. What to watch on those is the residual error below 7 bar, which should stay near the +2 percent seen here, and whether duty approaches saturation at the pressures actually used.

The trim remains off and is still worth trialling. With the model now accurate below saturation it would start near zero and have only puck specific residuals to absorb, well inside its 25 to 150 percent clamp. It cannot help with either the compression artefact or duty saturation, since the scale reads nothing during the first and there is no headroom during the second.
