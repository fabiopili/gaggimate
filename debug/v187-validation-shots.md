# v1.8.7 validation, shots 55 to 59

Shots analysed from exported logs: 55 (2026-08-09, the v1.8.6 choked limit cycle recorded before the OTA, kept as baseline), 56 and 57 (2026-08-10), 58 and 59 (2026-08-11). Controller and display both on v1.8.7 from shot 56 onward. The v1.8.8 display image is not installed.

## Effective ceilings, a correction to the day's assumptions

Setting the extraction phase pressure to 0 did not remove the ceiling. The v1.8.7 display substitutes 9.0 bar whenever a flow phase carries pressure 0 (`PumpLimits.h`, the v1.8.4 guard) and deliberately logs the raw profile value, so `tp` reads 0 in shots 57 to 59 while the machine arbitrated against 9.0 the whole time. Removing that silent substitution is exactly the v1.8.8 display change, which has not been applied. This also answers the OPV question: no shot could approach 11.5 bar because every shot ran under a 9 bar ceiling. Shot 56 additionally still had the explicit 9 in its profile; the 0 edit landed between shots 56 and 57.

## Measurements

All shots Pure Flow, extraction target 2.2 g/s (55, 56) or 2.0 g/s (57 to 59), volumetric cut near 43.5 g.

| shot | day | firmware | first drops | pr during extraction | bind window cp | bind window pp | duty step per tick, settled | verdict by ear |
|------|-----|----------|-------------|----------------------|----------------|----------------|------------------------------|----------------|
| 55 | 08-09 | v1.8.6 | 9.0 s | up to 19, erratic | 8.5 to 9.4 | 4 to 67, cuts to 0 | 23.9 | chop (recorded) |
| 56 | 08-10 | v1.8.7 | 11.5 s | 1.3 to 2.2, stable | 8.5 to 9.0 | 50 to 69, no cuts | 3.1 | quiet |
| 57 | 08-10 | v1.8.7 | 7.75 s | 1.1 to 2.6, stable | 8.6 to 9.1 | 41 to 63, no cuts | 2.6 | quiet |
| 58 | 08-11 | v1.8.7 | 12.0 s | spikes to 134 | 8.5 to 9.4 | 8 to 61, near-zero cuts | 11.8 | chop |
| 59 | 08-11 | v1.8.7 | 6.0 s | spikes to 61 | 8.5 to 9.3 | 2 to 58, near-zero cuts | 14.0 | chop |

Settled tracking (last 12 s of active extraction, vf against tf): 55 RMS 0.23, 56 RMS 0.23, 57 RMS 0.21, 58 RMS 0.41, 59 RMS 0.31. Final weights 43.4 to 43.9 g on all five, within half a gram of each other across wildly different pucks.

## Gate results

Normal shot away from the ceiling: pass. Shots 56 and 57 track inside the usual band (RMS 0.21 to 0.23), the trim settles, and the machine is quiet.

Quiet hold at a binding ceiling: pass on the gentle case. Shot 56 pinned at 9.0 for over ten seconds while genuinely binding (vf 1.5 to 1.7 against a 2.2 target early in the hold) with no cuts and per-tick duty movement of about 3 points. This behaviour did not exist on v1.8.6.

First choke day: fail. Shots 58 and 59 chop audibly at the ceiling. cp swings 8.5 to 9.4 against the 0.3 bar gate, pp spans roughly 50 points against the 8 point gate, with cuts to 2 to 8 percent. The operator report of looping counts as the failed gate on its own.

## Why yesterday was quiet and today looped

The firmware is the same on both days. The difference is the puck, and the logs separate the two regimes cleanly.

Yesterday's pucks passed nearly the full flow target at 9 bar (pr stable around 2, first drops early). The feed-forward duty for the target flow and the duty the ceiling hold needs were nearly the same number, so pressure approached the ceiling from below, overshoot stayed within 0.1 bar, and the pressure branch held the pump smoothly.

Today's pucks choked. Shot 58 spent eight seconds of preinfusion pinned at the 2 bar cap with 0.2 g/s through-flow, first drops at 12 s, and the conductance estimate collapsed and recovered repeatedly (pr spiking to 134, channelling). At 9 bar these pucks initially passed only 0.9 to 1.3 g/s against the 2.0 target, so the flow branch's feed-forward demanded around 45 to 58 percent duty while the ceiling hold wanted roughly 25 to 30. That gap is the amplitude of the loop: pressure overshoots to 9.1 to 9.4, the pressure branch claws back hard, cp sags to about 8.5, the min() arbitration hands the pump straight back to the feed-forward at full choke-overdrive duty, and the stiff puck re-slams the ceiling within two ticks. Period roughly 1 to 1.5 s, audible, fading in both shots once the puck eroded and the equilibrium fell below the ceiling (about 26 s in 58, 22 s in 59).

The v1.8.7 latch is engaged throughout (entry needs 0.15 s of pressure-branch wins; release needs cp 1.5 bar below the setpoint for 0.5 s, which never occurred) and it visibly helps: against shot 55 the cuts no longer reach zero and the per-tick duty step halves from 24 to 12 to 14 points. What remains is structural in the output law rather than the integral: while latched, nothing stops the flow branch from retaking the pump at its full unconstrained duty every time pressure dips a few tenths below the setpoint. A secondary effect worth noting is that the latched freeze discards integration only on feed-forward cycles, which are the below-ceiling ones, so the integral accumulates one-sidedly during the limit cycle and the claw-backs deepen as it runs (58: cuts from about 20 down to 8; 59: down to 2).

## Decision

The fix for what the shots showed becomes the next OTA and the queue waits, per the campaign. Proposed single change: while the ceiling latch is engaged, cap the flow branch's duty at the pressure branch's output plus a small margin (or slew-limit the handback), so a sub-ceiling dip cannot re-inject full feed-forward duty into a puck that cannot pass the target flow. Tier 1 coefficients stay parked until v1.8.7 or its successor passes a choke day. The v1.8.8 display image should go on when convenient, since today's confusion about the 0 cap is precisely what its explicit semantics exist to prevent; a deliberate capless shot should wait for a permissive puck as its gate already states.
