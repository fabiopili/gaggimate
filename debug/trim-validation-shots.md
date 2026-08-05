# Flow trim validation shots, 2026-08-05

Shots 23 and 24, the two shots planned after shot 22 to judge whether the corrected loop settles at real extraction pressure. Both ran the "Pure Flow" profile (`m5zwi3VR3J`), four minutes apart, with the same dose and the same grind, finer than shot 22 as planned. Both exited on the volumetric target, cutting at 43.9 g and 43.5 g. The operator's judgement was that the updates are working fully, and the data agrees. This document records the evidence that the loop settles, resolves the clamp question shot 22 left open, and notes what these shots still do not exercise.

## Configuration during these shots

Display firmware `v1.8.3`, shipped over the air 2026-08-04: both trimmer terms gated on filtered pressure slew within 0.30 bar/s, `KP` 0.40, `KI` 0.30, upward authority 10 % of the requested flow and downward 75 %. The controller remains byte identical to upstream. Pump model coefficients `6.4,3.9`. Scale Flow Compensation on. The 9 bar ceiling in place on the extraction phase.

A note for reading the logs: the `pr` field is puck resistance from the controller, not the trim. The trim is not logged directly; it is visible through `fl`, which in flow-controlled samples equals the trimmed command and is quantised to 0.05 ml/s. In pressure-capped samples `fl` follows the pressure branch instead and says nothing about the trim state.

## What the two shots did

| | shot 23 | shot 24 |
|---|---|---|
| Bloom length, weight at advance | 11.25 s, 0.8 g | 8.25 s, 0.9 g |
| Peak puck resistance `pr` | 2.78 | 1.72 |
| Peak extraction pressure | 6.0 bar | 4.0 bar |
| Flow-controlled extraction span | 22.5 s | 22.75 s |
| Settled cup flow against a 2.0 target | 2.09 g/s, sd 0.21 | 1.98 g/s, sd 0.17 |
| RMS error over the settled window | 0.23 | 0.17 |
| Command pinned at the +10 % clamp | 5.25 s, 29 % of settled | 5.5 s, 29 % of settled |
| Cut weight in the log | 43.9 g | 43.5 g |
| Total duration | 40.1 s | 37.0 s |

The settled window is every flow-controlled extraction sample from the first cup flow reading of 1.4 g/s or better, which excludes the ramp knee and nothing else.

## The loop settles

RMS error from target is 0.23 and 0.17 g/s against 0.31 on shot 22, 0.50 and 0.52 on the first trim build, and 0.71 with no trim. Every previous number was set at lower pressure; shot 22 peaked at 2.8 bar where these reached 6.0 and 4.0, so this is the first evidence at the operating point that matters. The overshoot and undershoot arc from shots 20 and 21 is absent. The worst excursion in either shot is a brief 2.39 g/s in shot 23's midsection while the command was still pinned high, and the loop unwound it without ringing.

## The wind-up regime was exercised this time, and the fix held

Shot 22 never crossed the slew gate, so `v1.8.3`'s actual defence was untested until now. These blooms are the exact regime that wound the first attempt to its clamp within five seconds: pressure capped at 2 bar with the duty pushed down to 8 to 15 %, and no established cup flow until 14.25 s in shot 23 and 11.25 s in shot 24. If the wind-up were still present, the first flow-controlled sample after the bloom would carry a command of 2.20. Instead both shots exited the bloom with the command within a step or two of the raw setpoint, and the clamp was only reached 3 and 7 seconds after the handoff, once pressure had steadied and the loop was integrating genuine under-delivery. The cap lift itself produced no discharge spike; cup flow rose monotonically to target over about six seconds in both shots.

## The clamp question is resolved

Shot 22 left a decision rule: if the trim goes negative at real pressure the asymmetric clamp is right, and if it stays pinned positive `MAX_TRIM_UP_RATIO` needs raising. It did not stay pinned. The command rode the +0.2 ml/s clamp for 29 % of the settled window in both shots, against 72 % in shot 22, all of it in the first six seconds of extraction while the puck was tightest. It then unwound and stayed inside its authority for the rest of the shot, between roughly minus 0.15 and plus 0.10 ml/s, negative through most of shot 23's tail and near zero in shot 24's. The clamp stays at 10 %.

The cost of the small upward authority is visible and accepted: about six seconds of cup flow at 1.4 to 1.9 g/s at the start of extraction, which is the price of bounding what any residual wind-up could do.

## Puck variation is the test, not a confound

The two pucks were prepared identically minutes apart and still differed by about 60 % in peak resistance, 2.78 against 1.72. Shot 23 choked for three seconds longer in the bloom and ran two bar higher throughout extraction. This is ordinary puck-to-puck variation in real use, and it is precisely the disturbance the trim exists to absorb, so it should be read as the substance of the validation rather than noise in it. The loop absorbed it: the flow-controlled extractions match to within a single 250 ms sample, the cuts landed 0.4 g apart, and the entire 3.1 s difference in total duration sits in the bloom, where puck variation belongs.

## What the settled trim says about the model

The settled trim is a direct in-place measurement of the pump model residual, since it converges on whatever correction makes delivered flow match the request. It reads within about 8 % of zero across 1.8 to 6 bar at 30 to 45 % duty. The shot 18 and 19 measurements implied the reverted `6.4,3.9` coefficients run roughly 20 % low at the bottom of the pressure band, which would have settled the trim near minus 0.4 ml/s; that expectation was formed at 44 to 51 % duty. Together with shot 22's finding of an 11 % over-prediction at 36.5 % duty, the picture is consistent: the model error depends strongly on operating point, which is the premise of the duty surface plan. Nothing should be retuned from these numbers; the surface should be measured.

## What remains unexercised

The 9 bar ceiling never engaged, since pressure peaked 3 bar below it, so the arbitration chatter documented in `first-espresso-shots.md` remains present upstream of any fix and untested by these shots. The trimmer's freeze on `pressureCapped` with established cup flow was only touched for a couple of samples at the end of each bloom, so a long capped stretch under full delivery is still unvisited. And the log zeroes the weight after the cut, so drip and final settled weight are not recorded; nothing here validates the volumetric predictor beyond the two cuts agreeing.

## Next

The correction loop is proven, which lifts the gate on the duty surface calibration in `docs/pump-duty-surface-plan.md`. That work needs no firmware change. The question of offering the flow work upstream is worth revisiting now that the control change has validation behind it.
