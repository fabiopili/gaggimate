# Validation campaign

The working protocol from 2026-08-09 onward: one behavioural change per OTA, each validated by real shots within a day, iterating from what the shots show. This document is the standing definition of the goals, the gates for each queued release, and the loop that turns a day's shots into the next decision.

## Goal of the programme

Scale-informed flow control that is calm in every regime it can reach: flow phases track the profile target where the puck allows it, hold the pressure ceiling quietly where it does not, and the machine never behaves in a way the operator cannot see and explain. The validated core (shot log v6 observability plus the v1.8.3 flow trim) stays the foundation; everything else earns its place through this campaign.

## The iteration cycle

1. Push one tag from the host and update the affected component from the machine UI. If GitHub Actions misses the tag event (it has before, on v1.8.5), delete the remote tag and push it again.
2. Pull the day's shots as JSON from the history page into `debug/`, with a settings snapshot if settings changed.
3. Grade by ear first, then against the release's gates below. The operator's hearing resolves what the 250 ms log aliases away; treat a bad-sound report as a failed gate even if the log looks clean.
4. Record the verdict in a short `debug/` note. Pass: promote the next queued change. Fail: the fix for what the shot showed becomes the next OTA, and the queue waits.

Measurements that decide, all available from one exported shot log: settled-window mean and RMS of `vf` against `tf`; the duty band (max minus min of `pp`) during any ceiling hold; the count of `pp` samples below 15 during holds; peak `cp` against the phase ceiling; and the finished-screen weight against the scale.

## Release gates

### v1.8.7, controller: ceiling hold

Contents: conditional integration at both actuator bounds, and the ceiling latch that preserves the pressure integral through flow-branch wins. Controller component must be updated; display is unchanged.

Verdict 2026-08-11, analysis in `debug/v187-validation-shots.md`: the normal-shot gate and the gentle ceiling hold passed on shots 56 and 57 (settled RMS 0.21 to 0.23, quiet pinned hold at 9.0). The first choke day failed on shots 58 and 59: with the puck's conductance fluctuating through the hold, every sag below the setpoint handed the pump back towards the full feed-forward and the loop surged audibly (duty 2 to 61, cp 8.5 to 9.4). The fix is v1.8.9 below.

Optional rig check before coffee: brew into the steam wand nearly closed, flow phase with a 9 bar ceiling. Pass: no rhythmic surging or cuts audible; `pp` stays in a band of about 10 points once pinned, with no samples near zero. Do not retune anything against this plant.

Shot gates:
- A normal shot must behave exactly as v1.8.6 away from the ceiling: trim settles, tracking within the usual band (settled RMS of `vf` against `tf` around 0.2 to 0.3).
- The first choke day is the real gate: at the ceiling, no audible chop, `cp` holding within about 0.3 bar, `pp` inside a band of about 8 points with no cuts to zero. The shot will still run long; that is the puck, and fixing it is grind, dose, or profile, not firmware.

Rollback: OTA the controller back to v1.8.6.

### v1.8.8, display: weight hold and explicit ceiling semantics

Contents: the finished screen holds the shot's peak weight until the next brew, grind, or mode change; a flow phase's pressure passes through raw with 0 meaning no ceiling; the editor defaults new flow phases to 9 bar and warns visibly when the ceiling is 0. Display component must be updated.

Not installed as of 2026-08-11: the display still runs v1.8.7, whose `PumpLimits.h` silently substitutes 9 bar for a flow-phase pressure of 0 while logging the raw value, which is why the capless edits on shots 57 to 59 changed nothing at the pump.

Gates:
- After any shot, the finished screen keeps showing the extraction weight, matching the scale, instead of resetting to 0.
- In the editor, switching a phase to flow shows 9 bar; typing 0 shows the no-limit warning; the value survives save and reload.
- Optional, deliberate: one capless shot on a permissive puck to confirm the pass-through, watching the pressure by eye.

### v1.8.9, controller: latched handback cap (the next OTA)

Contents: while the ceiling latch is engaged, the arbitrated output is capped at the remembered holding duty plus a headroom that opens quadratically with the distance below the setpoint. The memory learns only from settled pressure-branch wins near the setpoint, freezes through sags and claw-backs, and creeps upward while the cap pins a sag so an eroding-but-still-choked puck is re-held at the ceiling rather than deadlocked below it. Discriminating test: a choked toy puck whose conductance fluctuates across the choke boundary at one-second period, the shots 58/59 regime, which the v1.8.7/v1.8.8 controller fails. Controller component must be updated; display is unchanged.

Gates:
- A normal shot away from the ceiling and a gentle ceiling hold must behave exactly as shots 56 and 57 did: the latch never binds below the ceiling, so nothing may change.
- The next choked or channelling day: no audible surging, `pp` during the bind inside a band of about 15 points with no cuts towards zero, peak `cp` within about 0.3 bar of the ceiling.
- Expected and intended: when the puck flickers open mid-hold, the duty now stays put instead of chasing, so `cp` may sag quietly by up to about a bar before recovering. Quiet sag is a pass; surging is the fail.

Rollback: OTA the controller back to v1.8.7.

## Parked queue, in order, with entry criteria

1. Tier 1 duty-surface coefficients and slip (`0,0,-0.289,6.074` and `0,0,-0.036,0.445`), replacing the interim pair `5.3,3.3`. Enters only after v1.8.7 has passed a choke day, and lands together with the save-time plausibility guard on the coefficient strings, so a transposed entry cannot reach a shot (shot 48 did exactly that).
2. Trim slew-gate hold-off (the shot 52 discharge-tail wind-down). Enters when a real shot shows the trim pinned low after a fast pressure decline.
3. A "pressure limited" indicator during shots, so a choked puck reads as a visible state instead of a mystery. After 1.
4. Duty surface coverage below 3.4 bar (duty 30 to 60), one short rig session, when Tier 1 is in and the low-pressure error matters in practice.
5. The full pump matrix (table plus numerical inversion) only if the affine model leaves tracking errors above roughly 10 percent somewhere real shots go. The measured surface separates cleanly (spread about 0.012, gamma 1.12), so the expectation is that the affine form is the simplification that keeps almost all of the value.

## Standing rules

Guards and observability land before or with the features they protect. Prefer explicit, visible semantics over silent firmware substitutions. Keep the validated fallback one settings save or one OTA away, and say which in the release notes. Never retune the pressure loop against dead-headed rig plants.
