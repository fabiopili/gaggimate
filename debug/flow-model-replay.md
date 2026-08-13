# Flow model replay: refitting the pump surface against cup truth

Analysis of 2026-08-13, tool at `debug/flow-replay.py`. Goal per the operator's framing: modelled flow must track measured flow, with the pump characterisation as the low-hanging fruit and real shots as the adjustment data.

## Method

The firmware model `Q(u, P) = u (F(P) + S(P)) - S(P)` is replayed against every shot log carrying both duty and scale flow: shots 22 to 26, 48, 50 to 63, seventeen in all, spanning the v1.8.3 era, the chop days, the ceiling holds, the capless knee excursions and the current firmware. The reimplementation is checked against the logged `fl`, which is a low-passed forward evaluation of the same model: agreement within 0.02 g/s on the Tier 1 shots.

Cup flow equals pump flow only when stored water is constant and the sampled duty is representative, so the surface fit uses only flat-pressure (under 0.05 bar/s over two seconds), steady-duty (under 8 points over one second) windows with at least 4 g in the cup, scale lag of one second applied. Declining-pressure windows are excluded from the fit and analysed separately, because there the puck gives stored water back and the scale legitimately reads above pump flow. The chop-era windows fall out naturally under the duty-steadiness filter, since 250 ms samples of a fast-moving duty do not represent it.

## Findings: three regimes, one systematic split

At 6 to 9 bar Tier 1 is already right: bias +0.01 g/s, RMS 0.04 on the pinned ceiling holds of shots 56 and 57. Below 3 bar, a corner the rig never measured, the pump delivers 20 to 30 percent more than the extrapolated line, consistently across eras (shots 22 to 24 from the v1.8.3 days and shot 62 yesterday agree). Above 10.5 bar both candidate surfaces are centred (bias under 0.1) but scatter at 0.56 RMS: the knee error is plant-state variation, not model bias, and no static surface will remove it.

Separately, the rig and the cup genuinely disagree by a few percent where they overlap. The rig plant is a stiff nearly closed valve; a puck circuit is compliant, and a vibratory pump's per-stroke delivery depends on the instantaneous pressure waveform, not the 250 ms mean. The joint fit therefore weights the shot samples equally with the rig rather than treating the rig as truth.

## The refit

Affine full-drive curve plus affine slip, fitted jointly on 89 flat shot samples and the 17 rig points, leave-one-shot-out validated (better than Tier 1 on six of seven held-out shots, marginal only on the knee shot whose error is scatter), monotone and positive through 11.5 bar:

```
F(P) = 5.865 - 0.2742 P        S(P) = 0.0167 P - 0.180
Pump Flow Coefficients = 0,0,-0.2742,5.865
Pump Slip Coefficients = 0,0,0.01673,-0.1799
```

Residuals by band, Tier 1 to candidate: below 2 bar bias +0.41 to +0.11, 2 to 3 bar +0.25 to -0.01, 3 to 6 bar +0.18 to +0.02, 6 to 9 bar unchanged at about zero, knee unchanged. The negative slip constant is not physical leakage; it carries the duty-curve curvature the cup data shows and is safe in the firmware (the geometric flow stays positive everywhere and the zero-target hold duty clamps at zero). A quadratic term buys nothing measurable and a cubic invites extrapolation wiggle, so the simplest surface wins.

## Storage, measured

On declining-pressure windows the excess of scale flow over the refitted model gives the storage coefficient directly: 0.5 to 3.8 g per bar depending on the puck (0.5 on the 56/57 pucks, 3.7 on the dense knee pucks of 60/61), median 1.9, releasing 1.5 to 4.7 g per shot through the decline. This is the part of the tracking error that no pump surface can fix, and it accounts for the remainder of the shots 62/63 mid-shot hump: with the refitted surface plus a discharge term of about 1.2 g/bar, shot 62's mid-shot residual closes to about zero. If the hump survives the coefficient change, the next single change is a `- C dP/dt` term in the trim's reference with C around 1.5 to 2, measured here, rather than another gate.

## Limitations

Shot data is thin at 3 to 6 bar (the rig carries that band). Nothing anywhere measures above 11.2 bar. The knee scatter is irreducible without feedback. C varies puck to puck by a factor of seven, so any fixed C is an approximation.

## Machine change

One settings save to the strings above; rollback is the Tier 1 pair (`0,0,-0.289,6.074` and `0,0,-0.036,0.445`). Gate for the next shots: the low-pressure false under-delivery should shrink, the trim should settle nearer zero, and settled RMS on an ordinary puck should move back toward the 0.2 to 0.3 band.
