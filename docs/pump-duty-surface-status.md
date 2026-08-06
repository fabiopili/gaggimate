# Pump Duty Surface Calibration: implementation status

Companion to `docs/pump-duty-surface-plan.md`. The plan says what to build and why.
This says what was actually built, where it deviates, and what is left.

**State as of 2026-08-06:** all eleven plan tasks implemented on branch
`feat/pump-duty-surface` off `fork-ota`. Fifteen feature commits plus this
document. 32 web unit tests passing, `npm run build` clean, all four firmware
release targets building locally. **Not yet run on the rig. Not yet pushed.**

## How to pick this up

```bash
git checkout feat/pump-duty-surface
cd web && npm test        # expect 32 passing
```

The tool appears at Settings, Calibration, under "Pump Duty Surface Calibration",
below the existing Pump Flow Calibration section.

## Deviations from the plan, and why

The plan specified exact code for every file, and it was transcribed faithfully.
Each task was verified byte-for-byte against the plan by an independent reviewer.
Five deliberate departures:

1. **Test counts are higher than the plan states.** The plan predicted 28 tests at
   the end. There are 32, because three tests were added by review (below) and the
   plan's running totals were written before them. Nothing was deleted to reach a
   stated number.

2. **Imports were consolidated.** The plan shows mid-file `import` statements in
   the test file. ESLint's `no-duplicate-imports` rejects those, so every import
   from `./pumpDutySurface.js` lives in one statement at the top.

3. **The slew filter caps its timestep** (`e6ae3024`). See defect 2 below.

4. **The extended-recording tail is excluded** (`358af12f`). See defect 1 below.

5. **The operator is asked for four pressures, not three** (`2e97d533`). See
   defect 3 below.

## Defects found by review, and fixed

All three produce plausible but wrong calibration data rather than an error, which
is the failure mode that matters here. A miscalibration on 2026-08-02 made real
shots measurably worse, so silent bad data is the thing to guard against.

### 1. The scale re-tare destroyed the run's final steady window (critical)

After a run the firmware records for roughly 1.75 s more. During that tail the
Bluetooth scale re-tares and the live weight collapses to zero, while pump duty
`pp` is still non-zero and pressure `cp` is still steady. Those samples carry the
measure phase's `phaseNumber`, so they landed inside the last window and made its
weight delta negative, so `measureWindow` discarded the whole window.

Because nothing tells the operator how long is left, they naturally hold the last
pressure until the run ends, which made this the normal case rather than an edge
case. Losing one window drops a level from three usable points to two, and
`minPointsPerLevel` is three, so that duty level then contributes nothing to the
fit. A full five-level campaign could finish with every button green, no error
anywhere, and an empty results table with a null gamma and separability spread.

Verified against five real logs in `debug/` before fixing. The pattern is exact
and identical in all five: `extendedRecording` turns on precisely seven samples
before the end, and `v` collapses on the very next sample.

| file | samples | first ext idx | first v collapse idx |
| --- | --- | --- | --- |
| shot-18 | 136 | 129 | 130 |
| shot-19 | 128 | 121 | 122 |
| shot-21 | 135 | 128 | 129 |
| straight-flow-shot-12 | 246 | 239 | 240 |
| straight-flow-shot-17 | 246 | 239 | 240 |

The firmware already applies this exact exclusion for its own flow sums at
`ShotHistoryPlugin.cpp:1028`, commented "skipping the extended-recording
dribble". The web analysis was the one place that had not learned the rule.

Fixed in `extractSteadyWindows` at the phase filter, so the tail never reaches
`filteredSlew` and cannot perturb it either. Guarded by two tests, one synthetic
and one reading `debug/straight-flow-shot-17.slog` as a fixture.

### 2. The slew filter did not cap its timestep

`FlowTrimmer.h:44` clamps `dt` to `MAX_DT_S` before using it as the divisor for
the raw slope. The plan's JavaScript omitted that, so a gap in the log divided the
pressure change by the full gap and read as steady when the truth was unknown.
The plan's own stated intent was consistency with the firmware, so this was fixed
rather than accepted. `DEFAULT_OPTIONS.maxDtS` now mirrors `MAX_DT_S`.

### 3. Three holds against a three point minimum left no margin

The UI asked for three pressures and `minPointsPerLevel` is three, so any single
hold that drifted past `maxPressureSpreadBar`, got split, or was never reached
deleted an entire duty level invisibly. At 30 per cent duty the machine may never
make 9 bar at all. Now asks for roughly 3, 5, 7 and 9 bar, and the ladder buttons
distinguish three states: green at or above the minimum, amber below it, outline
when empty. The 45 s measure phase was deliberately NOT lengthened, because a
longer run risks overflowing the cup under the wand.

## Premises verified rather than assumed

- **Bare-integer `pump` really does run open loop.** `profile.h:295` reads it into
  `pumpIsSimple`, `BrewProcess::getPumpValue()` returns it, and the `!handled`
  branch at `Controller.cpp:966` sets `PumpControlMode::Power` with
  `targetPressure` and `targetFlow` both zeroed. The whole tool rests on this.
- **Protocol shapes match the backend.** `req:profiles:save`, `:select`,
  `:delete`, `req:change-mode` and `req:process:activate` were each checked
  against their handlers in `WebUIPlugin.cpp`, including payload field names.
  `request` versus `send` is correct: the two that never reply use `send`.
- **Units are right at every boundary.** `pp` is stored as duty times ten and
  `parseBinaryShot.js:15,68` scales it back, so parsed samples carry percent.
- **`separabilitySpread` genuinely discriminates.** Monte Carlo over five levels
  by three pressures: at 15 per cent flow noise the separable case stays under
  0.041 and the non-separable case above 0.059, so the distributions do not
  overlap. There is a test asserting the contrast, added because the plan's
  original assertion was near-tautological on a noiseless separable fixture.

## Known issues, deliberately deferred

None of these block the rig session. Listed roughly by value.

1. **No plausibility check on measured flow.** If the blind filter is not fitted,
   part of the pump's output bypasses the wand, the scale sees a fraction of true
   flow at a plausible pressure, and every guard passes. The result is a uniformly
   low-biased level, which is exactly what shifts `slope/intercept` and so the
   tier verdict. One point 50 per cent low at duty 90 and 3 bar moves the spread
   to 0.0855, well inside the non-separable band. The firmware's modelled flow
   `fl` is already in every parsed sample; a wide band on `flow / fl`, say 0.2x to
   5x, would catch it without tripping on the known 30 to 42 per cent model error.
   Worth adding before the tool is used unattended or by anyone else.
2. **`mergeSurfacePoints` appends fresh points without checking their bin.** A
   point whose measured duty bins elsewhere contaminates a neighbouring level and
   can only be removed by clearing the whole surface. Needs a duty deviation above
   2.5 points, which Power mode makes very unlikely.
3. **`fitSurface` inlines the same binning arithmetic as `dutyBin`.** Both read
   `SURFACE_FIT_OPTIONS.dutyBinPct` so they cannot diverge numerically today, but
   it is duplicated logic.
4. **`minWindowSamples: 16` hard-codes the 250 ms interval.** `shot.sampleInterval`
   is parsed and available but never consulted. The rest of the module is
   interval-agnostic because it works from real `t` deltas.
5. **Five constants duplicated** between `PumpDutyCalibration/constants.js` and
   `PumpFlowCalibration/constants.js`, identical today.
6. **`reset` is returned by the hook and never used** by the UI.
7. **`filteredSlew` reads `DEFAULT_OPTIONS.maxDtS` directly**, so a caller
   supplying `maxDtS` in an options object is ignored. No caller does.
8. **The hook and the UI have no tests.** Judged acceptable: the hook is a close
   clone of the proven `usePumpFlowCalibration`, and the plan's rig verification
   covers the I/O.

## What to expect on the rig

`maxSlewBarPerS` of 0.3 with `minWindowSamples` of 16 is a demanding pair for a
hand-worked steam valve. Across the existing fixtures, which are ordinary shots
rather than fixed-duty runs, three of five produce zero windows in their final
phase. A real run should do much better because duty will be pinned, which removes
the `maxDutySpreadPct` failure that killed one fixture's final window. But if the
first campaign shows amber buttons, those two thresholds are the place to look
before blaming operator technique.

Follow the manual verification checklist at the end of the plan. The single most
important early check is that the pump audibly holds one steady rate through the
measure phase rather than hunting, which is what proves fixed duty is really fixed.

## Next steps

1. Run the ladder on the rig, all five duty levels.
2. Export the JSON and commit it under `debug/`.
3. Read the separability spread. That number chooses between the three tiers
   described at the end of the plan, which range from no firmware change to a
   full table with a numerical inversion.

## Environment notes

The Homebrew platformio 6.1.19_2 upgrade briefly broke nanopb code generation
locally, since the formula's Python venv lost its protobuf install. That is
already resolved by granting a writable Python user site, so nanopb's own pip
fallback self-heals and stock `pio run` needs no `PYTHONPATH` prefix. Verified on
2026-08-06 by building all four release targets with the variable unset. CI was
never affected because it pip-installs platformio.

Release artefacts are built by `.github/workflows/build.yml` on tag push, which
runs `scripts/build_webui.sh` then `pio run` for `controller`, `display`
(including `-t buildfs`) and `display-headless`. All four verified building
locally on 2026-08-06.

The web UI reaches the machine embedded in the display firmware, not through
LittleFS. `scripts/build_webui.sh` gzips `web/dist` and
`scripts/embed_webui.py` packs it into `src/display/webassets/`, which is
gitignored and regenerated. LittleFS holds only profiles and shot history, so
OTA never touches user data.
