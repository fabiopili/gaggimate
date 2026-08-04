# Pump Duty/Flow Surface Calibration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A guided web UI tool that measures delivered flow as a function of both commanded pump duty and pressure, producing the (duty, pressure, flow) surface that the current two-coefficient pressure-only model cannot represent.

**Architecture:** Web UI only, no firmware change. A phase whose `pump` field is a bare integer already sets `pumpIsSimple` (`src/display/models/profile.h:296-297`), which routes through `BrewProcess::getPumpValue()` to `PumpControlMode::Power` and straight to `_psm.set()`, so fixed duty is expressible in a generated profile today. One run per duty level: the tool generates and selects a profile holding that duty, the operator diverts brew water to the steam wand with a blind filter and walks pressure across the range with the steam valve, and the tool then reads back the `.slog`, extracts steady-pressure windows, and converts scale weight deltas into flow points. Points accumulate across runs into a surface which is displayed, fitted against a candidate separable model, and exported as JSON.

**Out of scope, deliberately:** writing the surface back to the machine. No firmware model can consume it as a surface yet, and choosing the on-device representation is a separate decision that this tool's output should inform. See "Consuming the surface" at the end, which sets out three tiers ranging from no code change at all to a full table with a numerical inversion.

**Tech Stack:** Preact, Vite, Vitest (added by Task 1), the existing `ApiService` WebSocket and `/api/history` endpoints.

---

## Why this shape

The existing `PumpFlowCalibration` tool drives *pressure-targeted* phases, so the duty is whatever the pressure loop happens to settle at. That is why it can only ever produce a pressure curve. Across all seven shots recorded to date, duty and pressure move together almost perfectly:

```
pressure \ duty     20     30     40     50     60     70     80     90
    1 bar           53    109      .      .      .      .      .      .
    3 bar            .      .     39      7      .      .      .      .
    5 bar            .      .     60     33      .      .      .      .
    7 bar            .      .      2     19      3      .      .      .
   10 bar            .      .      .      .      .     48      4      .
```

A diagonal, not a plane. The duty dimension is unidentifiable from it, exactly as `debug/report.md` predicted for R5. This tool exists to fill the off-diagonal cells by holding duty fixed and varying pressure independently.

## The cheaper alternative, and why it does not replace this

The obvious question is whether the surface could be harvested from steady states in ordinary shots instead of a dedicated tool, which would cost nothing and need no rig. It was tested rather than assumed: the extractor specified in Task 3 and Task 4 was run over every shot recorded to date.

| shots | usable points each | duty reached |
| --- | --- | --- |
| 12, 13, 17 (wand rig, operator working the valve) | 2, 2, 4 | 32 % to 89 % |
| 18, 19, 20, 21 (espresso, no trim or early trim) | **0** | none |
| 22 (espresso, settled trim) | 2 | 36 % and 37 % |

Two findings, and they point in opposite directions.

The encouraging one is that a working flow trim makes the machine self-instrumenting. Shots 18 to 21 yielded nothing because pressure never held steady for the four seconds a window needs. Shot 22 yielded points precisely because the trim settled it. The better the trim gets, the more the machine calibrates itself as a by-product of making coffee.

The limiting one is that every harvested point lands on the same diagonal: 32 % at 1.3 bar, 36 % at 2.0, 45 % at 3.8, 53 % at 4.0, 58 % at 5.5, 89 % at 10.6. That is not a sampling accident. In a real shot the profile sets flow, the model picks duty from the current pressure, and the puck decides what pressure results, so duty and pressure are locked together by the puck. Nothing in a normal shot varies them independently, so no amount of harvesting can fill the off-diagonal.

The conclusion is that harvesting complements this tool rather than replacing it. It is the right mechanism for keeping the model honest where the machine actually brews, continuously and for free at roughly one to two points per shot, and it is worth building afterwards as an online refinement. Only holding duty fixed while pressure is varied by hand decouples the two, which is what this tool does.

A middle path exists and was rejected on cost: deliberately pulling shots at unusual flow targets would also decouple them, since duty is target over `Q(P)`. That is a calibration campaign made of coffee and pucks rather than water.

## File Structure

- `web/src/utils/pumpDutySurface.js` — pure analysis: slew filtering, steady-window extraction, per-window flow, line fitting, model fit. No I/O, no state, no Preact.
- `web/src/utils/pumpDutySurface.test.js` — unit tests for the above.
- `web/src/components/PumpDutyCalibration/constants.js` — duty ladder, thresholds, timings.
- `web/src/components/PumpDutyCalibration/profile.js` — generates the fixed-duty profile.
- `web/src/components/PumpDutyCalibration/profile.test.js` — unit tests for the generator.
- `web/src/components/PumpDutyCalibration/usePumpDutyCalibration.js` — run state machine.
- `web/src/components/PumpDutyCalibration/index.jsx` — UI shell.
- `web/src/pages/Settings/tabs/CalibrationTab.jsx` — mount point (modify).

`api.js` in the existing `PumpFlowCalibration` folder already wraps the endpoints we need (`fetchShotIndex`, `fetchAndParseShot`) and is imported rather than duplicated.

---

### Task 1: Add a test runner

There is currently no test infrastructure under `web/`. The analysis math in this feature is pure and is the part most worth testing, so add Vitest before writing any of it. It shares Vite's config and needs no setup file.

**Files:**
- Modify: `web/package.json`

- [ ] **Step 1: Add the dependency and script**

```bash
cd web && npm install --save-dev vitest@^3
```

- [ ] **Step 2: Add the test script to `web/package.json`**

In the `"scripts"` object, add:

```json
    "test": "vitest run",
```

- [ ] **Step 3: Verify the runner starts**

Run: `cd web && npm test`
Expected: exits 0 with "No test files found" (there are none yet).

- [ ] **Step 4: Commit**

```bash
git add web/package.json web/package-lock.json
git commit -m "chore: Add Vitest for web unit tests"
```

---

### Task 2: Pressure slew filter

The steady-window detector needs a rate of pressure change. The raw difference is dominated by sensor quantisation at 250 ms, so it is smoothed with the same first-order filter and time constant the firmware trim uses (`src/display/core/FlowTrimmer.h`), keeping the two consistent.

**Files:**
- Create: `web/src/utils/pumpDutySurface.js`
- Test: `web/src/utils/pumpDutySurface.test.js`

- [ ] **Step 1: Write the failing test**

Create `web/src/utils/pumpDutySurface.test.js`:

```js
import { describe, expect, it } from 'vitest';
import { filteredSlew } from './pumpDutySurface.js';

function ramp(count, startPressure, barPerSecond) {
  return Array.from({ length: count }, (_, i) => ({
    t: i * 250,
    cp: startPressure + (barPerSecond * i * 250) / 1000,
  }));
}

describe('filteredSlew', () => {
  it('converges on the true slope of a linear ramp', () => {
    const slew = filteredSlew(ramp(40, 1, 0.8));
    expect(slew[slew.length - 1]).toBeCloseTo(0.8, 1);
  });

  it('returns zero for the first sample, which has no predecessor', () => {
    expect(filteredSlew(ramp(10, 1, 0.8))[0]).toBe(0);
  });

  it('reads near zero on a flat trace', () => {
    const slew = filteredSlew(ramp(40, 6, 0));
    expect(Math.abs(slew[slew.length - 1])).toBeLessThan(0.01);
  });
});
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd web && npm test`
Expected: FAIL, "Failed to resolve import ./pumpDutySurface.js".

- [ ] **Step 3: Write the implementation**

Create `web/src/utils/pumpDutySurface.js`:

```js
// Pure analysis for the pump duty/pressure/flow surface. No I/O, no state, so
// it is safe to call from anywhere including tests.
//
// The existing pump calibration measures a pressure curve at whatever duty the
// pressure loop settles at, which cannot describe a pump whose output is not
// proportional to duty. This module supports the opposite experiment: hold duty
// fixed, vary pressure by hand, and read flow off the scale.

export const DEFAULT_OPTIONS = Object.freeze({
  // Matches FlowTrimmer.h. Above this rate the system is compressing or giving
  // water back, so cup flow is not pump flow and the sample is not usable.
  maxSlewBarPerS: 0.3,
  slewFilterTauS: 0.5,
  // 16 samples at the 250 ms log interval is 4 s, long enough for a weight
  // delta to rise clear of scale noise.
  minWindowSamples: 16,
  // Guards against a window that drifted across pressures or straddled a duty
  // change, either of which would smear the measurement.
  maxPressureSpreadBar: 0.5,
  maxDutySpreadPct: 2,
  minWeightDeltaG: 2.0,
});

// First-order filtered dP/dt in bar per second, one entry per sample. Index 0
// is always 0 because it has no predecessor.
export function filteredSlew(samples, tauS = DEFAULT_OPTIONS.slewFilterTauS) {
  const out = new Array(samples.length).fill(0);
  let slew = 0;
  for (let i = 1; i < samples.length; i++) {
    const dt = (samples[i].t - samples[i - 1].t) / 1000;
    if (dt > 0) {
      const raw = (samples[i].cp - samples[i - 1].cp) / dt;
      slew += (raw - slew) * Math.min(1, dt / tauS);
    }
    out[i] = slew;
  }
  return out;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd web && npm test`
Expected: PASS, 3 tests.

- [ ] **Step 5: Commit**

```bash
git add web/src/utils/pumpDutySurface.js web/src/utils/pumpDutySurface.test.js
git commit -m "feat: Add filtered pressure slew for duty surface analysis"
```

---

### Task 3: Steady-window extraction

Split a measure phase into runs of consecutive samples where pressure is near steady. When the operator steps the steam valve, the pressure transient pushes slew over the threshold and naturally ends one window and starts the next, so no explicit step detection is needed.

**Files:**
- Modify: `web/src/utils/pumpDutySurface.js`
- Test: `web/src/utils/pumpDutySurface.test.js`

- [ ] **Step 1: Write the failing test**

Append to `web/src/utils/pumpDutySurface.test.js`:

```js
import { extractSteadyWindows, findMeasurePhaseNumber } from './pumpDutySurface.js';

// Builds a phase-2 trace: `hold` steady samples at `pressure`, then a fast
// `stepSamples`-long climb to `nextPressure`, then `hold` steady again.
function twoPlateaus(hold, pressure, nextPressure, stepSamples) {
  const out = [];
  let t = 0;
  const push = cp => {
    out.push({ t, cp, pp: 45, v: out.length * 0.5, phaseNumber: 2 });
    t += 250;
  };
  for (let i = 0; i < hold; i++) push(pressure);
  for (let i = 1; i <= stepSamples; i++) {
    push(pressure + ((nextPressure - pressure) * i) / stepSamples);
  }
  for (let i = 0; i < hold; i++) push(nextPressure);
  return out;
}

describe('extractSteadyWindows', () => {
  it('finds one window per plateau and excludes the step between them', () => {
    const windows = extractSteadyWindows(twoPlateaus(24, 3, 6, 4), 2);
    expect(windows).toHaveLength(2);
    expect(windows[0].samples.every(s => s.cp === 3)).toBe(true);
    expect(windows[1].samples.every(s => s.cp === 6)).toBe(true);
  });

  it('rejects a plateau shorter than minWindowSamples', () => {
    expect(extractSteadyWindows(twoPlateaus(8, 3, 6, 4), 2)).toHaveLength(0);
  });

  it('ignores samples belonging to other phases', () => {
    const trace = twoPlateaus(24, 3, 6, 4).map(s => ({ ...s, phaseNumber: 1 }));
    expect(extractSteadyWindows(trace, 2)).toHaveLength(0);
  });

  it('ignores samples with the pump off', () => {
    const trace = twoPlateaus(24, 3, 6, 4).map(s => ({ ...s, pp: 0 }));
    expect(extractSteadyWindows(trace, 2)).toHaveLength(0);
  });
});

describe('findMeasurePhaseNumber', () => {
  it('returns the phase number of the transition named Measure', () => {
    const shot = {
      phaseTransitions: [
        { sampleIndex: 0, phaseNumber: 0, phaseName: 'Prepare' },
        { sampleIndex: 20, phaseNumber: 1, phaseName: 'Measure 45' },
      ],
    };
    expect(findMeasurePhaseNumber(shot)).toBe(1);
  });

  it('returns null when no measure phase is present', () => {
    expect(findMeasurePhaseNumber({ phaseTransitions: [] })).toBe(null);
  });
});
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd web && npm test`
Expected: FAIL, "extractSteadyWindows is not a function".

- [ ] **Step 3: Write the implementation**

Append to `web/src/utils/pumpDutySurface.js`:

```js
// The measure phase is located by name rather than by index, because the
// generated profile is the only thing that names a phase "Measure <duty>".
export function findMeasurePhaseNumber(shot) {
  const transitions = shot?.phaseTransitions || [];
  const match = transitions.find(t => String(t.phaseName || '').startsWith('Measure'));
  return match ? match.phaseNumber : null;
}

// Runs of consecutive samples inside `phaseNumber` where pressure is near
// steady and the pump is running. Each returned window carries its own slice
// of samples so measureWindow can work on it directly.
export function extractSteadyWindows(samples, phaseNumber, options = {}) {
  const opts = { ...DEFAULT_OPTIONS, ...options };
  const phase = samples.filter(s => s.phaseNumber === phaseNumber);
  if (phase.length === 0) return [];

  const slew = filteredSlew(phase, opts.slewFilterTauS);
  const windows = [];
  let start = -1;

  const close = endIndex => {
    if (start >= 0 && endIndex - start + 1 >= opts.minWindowSamples) {
      windows.push({ samples: phase.slice(start, endIndex + 1) });
    }
    start = -1;
  };

  for (let i = 0; i < phase.length; i++) {
    const steady = Math.abs(slew[i]) <= opts.maxSlewBarPerS && phase[i].pp > 0;
    if (steady) {
      if (start < 0) start = i;
      if (i === phase.length - 1) close(i);
    } else {
      close(i - 1);
    }
  }

  return windows;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd web && npm test`
Expected: PASS, 9 tests.

- [ ] **Step 5: Commit**

```bash
git add web/src/utils/pumpDutySurface.js web/src/utils/pumpDutySurface.test.js
git commit -m "feat: Extract steady-pressure windows from a calibration run"
```

---

### Task 4: Convert a window into a surface point

Flow comes from the raw cumulative weight `v`, not from `vf`, because `vf` is an EMA over weight deltas and would lag the window edges. This mirrors what the existing `analyze()` in `pumpFlowCalibration.js` does.

**Files:**
- Modify: `web/src/utils/pumpDutySurface.js`
- Test: `web/src/utils/pumpDutySurface.test.js`

- [ ] **Step 1: Write the failing test**

Append to `web/src/utils/pumpDutySurface.test.js`:

```js
import { measureWindow } from './pumpDutySurface.js';

// 20 samples at 250 ms is 4.75 s between first and last.
function window20({ cp = 5, pp = 45, gramsPerSample = 0.5 } = {}) {
  return {
    samples: Array.from({ length: 20 }, (_, i) => ({
      t: i * 250,
      cp: typeof cp === 'function' ? cp(i) : cp,
      pp: typeof pp === 'function' ? pp(i) : pp,
      v: i * gramsPerSample,
    })),
  };
}

describe('measureWindow', () => {
  it('computes flow from the weight delta over the window', () => {
    const point = measureWindow(window20({ gramsPerSample: 0.5 }));
    // 19 gaps * 0.5 g = 9.5 g over 4.75 s = 2.0 g/s
    expect(point.flow).toBeCloseTo(2.0, 3);
    expect(point.pressure).toBeCloseTo(5, 3);
    expect(point.duty).toBeCloseTo(45, 3);
  });

  it('rejects a window whose pressure drifted too far', () => {
    expect(measureWindow(window20({ cp: i => 5 + i * 0.05 }))).toBe(null);
  });

  it('rejects a window whose duty was not constant', () => {
    expect(measureWindow(window20({ pp: i => 45 + i }))).toBe(null);
  });

  it('rejects a window with too little weight gain, e.g. no scale', () => {
    expect(measureWindow(window20({ gramsPerSample: 0 }))).toBe(null);
  });
});
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd web && npm test`
Expected: FAIL, "measureWindow is not a function".

- [ ] **Step 3: Write the implementation**

Append to `web/src/utils/pumpDutySurface.js`:

```js
function mean(values) {
  return values.reduce((a, b) => a + b, 0) / values.length;
}

function spread(values) {
  return Math.max(...values) - Math.min(...values);
}

// One surface point, or null when the window is not trustworthy. Flow is taken
// from raw cumulative weight rather than the EMA-smoothed vf, which would lag
// the window edges and bias short windows.
export function measureWindow(window, options = {}) {
  const opts = { ...DEFAULT_OPTIONS, ...options };
  const samples = window?.samples || [];
  if (samples.length < 2) return null;

  const first = samples[0];
  const last = samples[samples.length - 1];
  const seconds = (last.t - first.t) / 1000;
  const grams = last.v - first.v;
  if (!(seconds > 0)) return null;
  if (!(grams >= opts.minWeightDeltaG)) return null;

  const pressures = samples.map(s => s.cp);
  const duties = samples.map(s => s.pp);
  if (spread(pressures) > opts.maxPressureSpreadBar) return null;
  if (spread(duties) > opts.maxDutySpreadPct) return null;

  return {
    duty: mean(duties),
    pressure: mean(pressures),
    flow: grams / seconds,
    seconds,
    grams,
  };
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd web && npm test`
Expected: PASS, 13 tests.

- [ ] **Step 5: Commit**

```bash
git add web/src/utils/pumpDutySurface.js web/src/utils/pumpDutySurface.test.js
git commit -m "feat: Convert a steady window into a duty surface point"
```

---

### Task 5: Fit the surface and test separability

Two stages, both plain least squares, so nothing nonlinear runs in the browser. Within each duty level, fit flow against pressure. Then regress the log of flow at a reference pressure against the log of duty fraction, whose slope is the exponent in `flow = Q(P) · duty^γ`.

The separability spread is the deliverable that decides the follow-on work. If `slope/intercept` is roughly the same at every duty level, the surface is separable and a single extra parameter replaces the whole table. If it varies, a table or a richer form is needed.

**Files:**
- Modify: `web/src/utils/pumpDutySurface.js`
- Test: `web/src/utils/pumpDutySurface.test.js`

- [ ] **Step 1: Write the failing test**

Append to `web/src/utils/pumpDutySurface.test.js`:

```js
import { fitLine, fitSurface } from './pumpDutySurface.js';

// Synthetic surface from flow = (8 - 0.45 P) * (duty/100)^0.55, which is
// separable by construction, so the fit must recover gamma and report a
// near-zero separability spread.
function syntheticSurface(gamma = 0.55) {
  const points = [];
  for (const duty of [30, 45, 60, 75, 90]) {
    for (const pressure of [2, 4, 6, 8]) {
      points.push({
        duty,
        pressure,
        flow: (8 - 0.45 * pressure) * Math.pow(duty / 100, gamma),
      });
    }
  }
  return points;
}

describe('fitLine', () => {
  it('recovers slope and intercept', () => {
    const line = fitLine([
      { x: 0, y: 1 },
      { x: 1, y: 3 },
      { x: 2, y: 5 },
    ]);
    expect(line.slope).toBeCloseTo(2, 6);
    expect(line.intercept).toBeCloseTo(1, 6);
  });

  it('returns null when there is nothing to fit', () => {
    expect(fitLine([{ x: 1, y: 1 }])).toBe(null);
    expect(fitLine([{ x: 1, y: 1 }, { x: 1, y: 2 }])).toBe(null);
  });
});

describe('fitSurface', () => {
  it('recovers the duty exponent of a separable surface', () => {
    expect(fitSurface(syntheticSurface(0.55)).gamma).toBeCloseTo(0.55, 2);
  });

  it('reports a near-zero separability spread for a separable surface', () => {
    expect(Math.abs(fitSurface(syntheticSurface()).separabilitySpread)).toBeLessThan(0.01);
  });

  it('returns one level per duty with its own pressure fit', () => {
    const fit = fitSurface(syntheticSurface());
    expect(fit.levels.map(l => l.duty)).toEqual([30, 45, 60, 75, 90]);
  });

  it('returns a null gamma when fewer than two duty levels have data', () => {
    const single = syntheticSurface().filter(p => p.duty === 45);
    expect(fitSurface(single).gamma).toBe(null);
  });
});
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd web && npm test`
Expected: FAIL, "fitLine is not a function".

- [ ] **Step 3: Write the implementation**

Append to `web/src/utils/pumpDutySurface.js`:

```js
// Ordinary least squares. Returns null when the x values carry no spread, which
// would otherwise divide by zero.
export function fitLine(points) {
  if (!points || points.length < 2) return null;
  const mx = mean(points.map(p => p.x));
  const my = mean(points.map(p => p.y));
  let sxy = 0;
  let sxx = 0;
  for (const p of points) {
    sxy += (p.x - mx) * (p.y - my);
    sxx += (p.x - mx) ** 2;
  }
  if (sxx === 0) return null;
  const slope = sxy / sxx;
  return { slope, intercept: my - slope * mx };
}

export const SURFACE_FIT_OPTIONS = Object.freeze({
  referencePressureBar: 6,
  dutyBinPct: 5,
  minPointsPerLevel: 3,
});

// Fits flow against pressure within each duty level, then fits the log of the
// resulting flow at a reference pressure against the log of duty fraction. The
// slope of that second fit is gamma in flow = Q(P) * duty^gamma.
//
// separabilitySpread is the range of slope/intercept across duty levels. That
// ratio is duty-independent if and only if the surface really is separable, so
// a small spread means one exponent can replace the whole table and a large one
// means it cannot.
export function fitSurface(points, options = {}) {
  const opts = { ...SURFACE_FIT_OPTIONS, ...options };
  const byDuty = new Map();
  for (const p of points || []) {
    const key = Math.round(p.duty / opts.dutyBinPct) * opts.dutyBinPct;
    if (!byDuty.has(key)) byDuty.set(key, []);
    byDuty.get(key).push(p);
  }

  const levels = [];
  for (const [duty, group] of [...byDuty.entries()].sort((a, b) => a[0] - b[0])) {
    if (group.length < opts.minPointsPerLevel) continue;
    const line = fitLine(group.map(p => ({ x: p.pressure, y: p.flow })));
    if (!line) continue;
    const flowAtReference = line.intercept + line.slope * opts.referencePressureBar;
    if (!(flowAtReference > 0)) continue;
    levels.push({ duty, ...line, flowAtReference, count: group.length });
  }

  if (levels.length < 2) {
    return { levels, gamma: null, separabilitySpread: null };
  }

  const logFit = fitLine(
    levels.map(l => ({ x: Math.log(l.duty / 100), y: Math.log(l.flowAtReference) })),
  );
  const ratios = levels.map(l => l.slope / l.intercept);

  return {
    levels,
    gamma: logFit ? logFit.slope : null,
    separabilitySpread: spread(ratios),
    referencePressureBar: opts.referencePressureBar,
  };
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd web && npm test`
Expected: PASS, 19 tests.

- [ ] **Step 5: Commit**

```bash
git add web/src/utils/pumpDutySurface.js web/src/utils/pumpDutySurface.test.js
git commit -m "feat: Fit the duty surface and report whether it is separable"
```

---

### Task 6: Fixed-duty calibration profile

The whole tool rests on `"pump": <integer>`. `src/display/models/profile.h:296` reads a bare integer into `pumpIsSimple`/`pumpSimple`, `BrewProcess::getPumpValue()` returns it, and `Controller::updateControl` falls through to `PumpControlMode::Power`. An object-valued `pump` would instead be parsed as an advanced flow or pressure target and silently defeat the experiment, so the test asserts the integer form explicitly.

`valve: 0` mirrors the existing calibration profile, which is the proven configuration for the steam-wand rig.

**Files:**
- Create: `web/src/components/PumpDutyCalibration/profile.js`
- Test: `web/src/components/PumpDutyCalibration/profile.test.js`

- [ ] **Step 1: Write the failing test**

Create `web/src/components/PumpDutyCalibration/profile.test.js`:

```js
import { describe, expect, it } from 'vitest';
import { DUTY_PROFILE_ID, buildDutyProfile } from './profile.js';

describe('buildDutyProfile', () => {
  it('sets the measure phase pump to a bare integer so the firmware runs fixed duty', () => {
    const measure = buildDutyProfile(45).phases.find(p => p.name.startsWith('Measure'));
    expect(measure.pump).toBe(45);
    expect(Number.isInteger(measure.pump)).toBe(true);
  });

  it('names the measure phase so the analyser can find it in the slog header', () => {
    const measure = buildDutyProfile(45).phases.find(p => p.name.startsWith('Measure'));
    expect(measure.name).toBe('Measure 45');
    expect(measure.name.length).toBeLessThanOrEqual(24);
  });

  it('keeps the phase count within the 12 the slog header can record', () => {
    expect(buildDutyProfile(45).phases.length).toBeLessThanOrEqual(12);
  });

  it('keeps every phase inside the 300 s brew safety timeout', () => {
    for (const phase of buildDutyProfile(90, 45).phases) {
      expect(phase.duration).toBeLessThan(300);
    }
  });

  it('rounds and clamps duty into 0..100', () => {
    expect(buildDutyProfile(140).phases[1].pump).toBe(100);
    expect(buildDutyProfile(-5).phases[1].pump).toBe(0);
    expect(buildDutyProfile(45.6).phases[1].pump).toBe(46);
  });

  it('uses a stable id so runs replace rather than accumulate profiles', () => {
    expect(buildDutyProfile(30).id).toBe(DUTY_PROFILE_ID);
    expect(buildDutyProfile(90).id).toBe(DUTY_PROFILE_ID);
  });
});
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd web && npm test`
Expected: FAIL, "Failed to resolve import ./profile.js".

- [ ] **Step 3: Write the implementation**

Create `web/src/components/PumpDutyCalibration/profile.js`:

```js
// Auxiliary profile used exclusively by the pump duty calibration tool.
// Embedded rather than loaded from the machine so runs are reproducible.
//
// The measure phase carries `pump` as a bare integer. profile.h:296 reads that
// into pumpIsSimple/pumpSimple, BrewProcess::getPumpValue returns it unchanged,
// and Controller::updateControl routes it to PumpControlMode::Power, so the
// pump holds a fixed duty with no feedback of any kind. An object-valued pump
// would be parsed as an advanced flow or pressure target instead, which is
// exactly the closed loop this experiment has to avoid.

export const DUTY_PROFILE_ID = 'pump-duty-calibration';
export const DEFAULT_MEASURE_SECONDS = 45;

export function buildDutyProfile(dutyPercent, measureSeconds = DEFAULT_MEASURE_SECONDS) {
  const duty = Math.max(0, Math.min(100, Math.round(dutyPercent)));
  return {
    id: DUTY_PROFILE_ID,
    label: `[Calibration] Pump duty ${duty}%`,
    type: 'pro',
    description: 'Auto-generated by the pump duty calibration tool.',
    temperature: 30,
    phases: [
      {
        name: 'Prepare',
        phase: 'preinfusion',
        valve: 0,
        duration: 5,
        temperature: 0,
        transition: { type: 'instant', duration: 0, adaptive: true },
        pump: 0,
      },
      {
        name: `Measure ${duty}`,
        phase: 'brew',
        valve: 0,
        duration: measureSeconds,
        temperature: 0,
        transition: { type: 'instant', duration: 0, adaptive: true },
        pump: duty,
      },
    ],
  };
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd web && npm test`
Expected: PASS, 25 tests.

- [ ] **Step 5: Commit**

```bash
git add web/src/components/PumpDutyCalibration/profile.js web/src/components/PumpDutyCalibration/profile.test.js
git commit -m "feat: Generate a fixed-duty profile for surface calibration"
```

---

### Task 7: Feature constants

**Files:**
- Create: `web/src/components/PumpDutyCalibration/constants.js`

- [ ] **Step 1: Write the file**

Create `web/src/components/PumpDutyCalibration/constants.js`:

```js
// Feature-scoped constants for the duty surface calibration run.

export const PHASE = Object.freeze({
  IDLE: 'idle',
  RUNNING: 'running',
  ANALYZING: 'analyzing',
  DONE: 'done',
  ERROR: 'error',
});

// The ladder of duty levels to measure. Spread across the usable range rather
// than concentrated where espresso happens, because the point of the exercise
// is to see how output varies with duty, which needs the ends as well as the
// middle.
export const DUTY_LADDER = Object.freeze([30, 45, 60, 75, 90]);

export const MODE_BREW = 1;
export const SHOT_END_TIMEOUT_MS = 5 * 60 * 1000;
export const POST_MODE_SETTLE_MS = 1500;
export const POST_SHOT_SETTLE_MS = 1500;

// Surface points survive a page reload so a campaign can be finished in more
// than one sitting without repeating completed duty levels.
export const SURFACE_STORAGE_KEY = 'gm.pumpDutySurface.v1';
```

- [ ] **Step 2: Note on verification**

Nothing imports this file yet, and Vite will not compile an unreferenced module, so `npm run build` would pass regardless and is not a real check. It is first exercised by Task 9, which imports it.

- [ ] **Step 3: Commit**

```bash
git add web/src/components/PumpDutyCalibration/constants.js
git commit -m "feat: Add duty calibration feature constants"
```

---

### Task 8: Surface persistence

**Files:**
- Modify: `web/src/utils/pumpDutySurface.js`
- Test: `web/src/utils/pumpDutySurface.test.js`

- [ ] **Step 1: Write the failing test**

Append to `web/src/utils/pumpDutySurface.test.js`:

```js
import { mergeSurfacePoints, summariseCoverage } from './pumpDutySurface.js';

describe('mergeSurfacePoints', () => {
  it('replaces all points for a duty level rather than appending duplicates', () => {
    const existing = [
      { duty: 45, pressure: 3, flow: 2 },
      { duty: 60, pressure: 3, flow: 3 },
    ];
    const merged = mergeSurfacePoints(existing, 45, [{ duty: 45, pressure: 5, flow: 1.8 }]);
    expect(merged.filter(p => p.duty === 45)).toHaveLength(1);
    expect(merged.filter(p => p.duty === 60)).toHaveLength(1);
  });

  it('bins by duty so a measured 44.8 replaces the 45 level', () => {
    const merged = mergeSurfacePoints([{ duty: 45, pressure: 3, flow: 2 }], 45, [
      { duty: 44.8, pressure: 5, flow: 1.8 },
    ]);
    expect(merged).toHaveLength(1);
    expect(merged[0].pressure).toBe(5);
  });
});

describe('summariseCoverage', () => {
  it('counts points per requested duty level', () => {
    const coverage = summariseCoverage(
      [
        { duty: 45, pressure: 3, flow: 2 },
        { duty: 45, pressure: 6, flow: 1.8 },
        { duty: 60, pressure: 3, flow: 3 },
      ],
      [30, 45, 60],
    );
    expect(coverage).toEqual([
      { duty: 30, count: 0 },
      { duty: 45, count: 2 },
      { duty: 60, count: 1 },
    ]);
  });
});
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd web && npm test`
Expected: FAIL, "mergeSurfacePoints is not a function".

- [ ] **Step 3: Write the implementation**

Append to `web/src/utils/pumpDutySurface.js`:

```js
// Shares SURFACE_FIT_OPTIONS.dutyBinPct so a level that merges as one bin also
// fits as one bin. Two separate constants would silently diverge.
function dutyBin(duty) {
  const bin = SURFACE_FIT_OPTIONS.dutyBinPct;
  return Math.round(duty / bin) * bin;
}

// Re-running a duty level replaces its points rather than adding to them, so a
// repeated run after a bad sweep supersedes the bad data instead of averaging
// with it.
export function mergeSurfacePoints(existing, dutyLevel, freshPoints) {
  const target = dutyBin(dutyLevel);
  const kept = (existing || []).filter(p => dutyBin(p.duty) !== target);
  return [...kept, ...(freshPoints || [])];
}

export function summariseCoverage(points, dutyLadder) {
  return dutyLadder.map(duty => ({
    duty,
    count: (points || []).filter(p => dutyBin(p.duty) === dutyBin(duty)).length,
  }));
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd web && npm test`
Expected: PASS, 28 tests.

- [ ] **Step 5: Commit**

```bash
git add web/src/utils/pumpDutySurface.js web/src/utils/pumpDutySurface.test.js
git commit -m "feat: Merge and summarise duty surface coverage"
```

---

### Task 9: The run state machine

Mirrors `usePumpFlowCalibration.js`, including its listener lifecycle, re-entry guard and profile restore, but runs one duty level at a time and accumulates points rather than writing coefficients.

**Files:**
- Create: `web/src/components/PumpDutyCalibration/usePumpDutyCalibration.js`

- [ ] **Step 1: Write the hook**

Create `web/src/components/PumpDutyCalibration/usePumpDutyCalibration.js`:

```js
import { useCallback, useContext, useEffect, useRef, useState } from 'preact/hooks';
import { ApiServiceContext, machine } from '../../services/ApiService.js';
import { fetchAndParseShot, fetchShotIndex } from '../PumpFlowCalibration/api.js';
import {
  extractSteadyWindows,
  findMeasurePhaseNumber,
  measureWindow,
  mergeSurfacePoints,
} from '../../utils/pumpDutySurface.js';
import {
  MODE_BREW,
  PHASE,
  POST_MODE_SETTLE_MS,
  POST_SHOT_SETTLE_MS,
  SHOT_END_TIMEOUT_MS,
  SURFACE_STORAGE_KEY,
} from './constants.js';
import { DUTY_PROFILE_ID, buildDutyProfile } from './profile.js';

function loadStoredSurface() {
  try {
    const raw = window.localStorage.getItem(SURFACE_STORAGE_KEY);
    const parsed = raw ? JSON.parse(raw) : [];
    return Array.isArray(parsed) ? parsed : [];
  } catch {
    return [];
  }
}

function storeSurface(points) {
  try {
    window.localStorage.setItem(SURFACE_STORAGE_KEY, JSON.stringify(points));
  } catch {
    // Storage being unavailable costs resumability, not correctness.
  }
}

/**
 * usePumpDutyCalibration
 * Drives one fixed-duty calibration run at a time and accumulates the
 * resulting (duty, pressure, flow) points into a surface.
 *
 * Returns:
 * - phase, logs, busy — as in usePumpFlowCalibration
 * - surface: Array<{duty, pressure, flow, seconds, grams}>
 * - activeDuty: number | null — duty level of the run in flight
 * - runDuty: (duty: number) => Promise<void>
 * - clearSurface: () => void
 * - reset: () => void
 */
export function usePumpDutyCalibration() {
  const apiService = useContext(ApiServiceContext);

  const [phase, setPhase] = useState(PHASE.IDLE);
  const [logs, setLogs] = useState([]);
  const [surface, setSurface] = useState(loadStoredSurface);
  const [activeDuty, setActiveDuty] = useState(null);

  const statusListenerRef = useRef(null);
  const inFlightRef = useRef(false);
  const safetyIdRef = useRef(null);
  const waitRejectRef = useRef(null);

  const detachStatusListener = useCallback(() => {
    if (statusListenerRef.current !== null) {
      apiService.off('evt:status', statusListenerRef.current);
      statusListenerRef.current = null;
    }
    if (safetyIdRef.current !== null) {
      clearTimeout(safetyIdRef.current);
      safetyIdRef.current = null;
    }
    if (waitRejectRef.current !== null) {
      const reject = waitRejectRef.current;
      waitRejectRef.current = null;
      reject(new Error('Calibration cancelled.'));
    }
  }, [apiService]);

  useEffect(() => detachStatusListener, [detachStatusListener]);

  const pushLog = useCallback((msg, tone = 'info') => {
    setLogs(prev => [...prev, { key: prev.length, msg, tone }]);
  }, []);

  const reset = useCallback(() => {
    detachStatusListener();
    setPhase(PHASE.IDLE);
    setLogs([]);
    setActiveDuty(null);
  }, [detachStatusListener]);

  const clearSurface = useCallback(() => {
    setSurface([]);
    storeSurface([]);
  }, []);

  const waitForShotEnd = useCallback(
    () =>
      new Promise((resolve, reject) => {
        let sawActive = false;
        waitRejectRef.current = reject;
        safetyIdRef.current = setTimeout(() => {
          safetyIdRef.current = null;
          waitRejectRef.current = null;
          detachStatusListener();
          reject(new Error('Timeout waiting for run to finish (5min).'));
        }, SHOT_END_TIMEOUT_MS);
        statusListenerRef.current = apiService.on('evt:status', m => {
          const active = m.process?.a === 1;
          if (active) sawActive = true;
          if (sawActive && !active) {
            if (safetyIdRef.current !== null) {
              clearTimeout(safetyIdRef.current);
              safetyIdRef.current = null;
            }
            waitRejectRef.current = null;
            detachStatusListener();
            resolve();
          }
        });
      }),
    [apiService, detachStatusListener],
  );

  const runDuty = useCallback(
    async duty => {
      if (inFlightRef.current) return;
      if (!apiService) {
        pushLog('Internal error: ApiService unavailable.', 'err');
        setPhase(PHASE.ERROR);
        return;
      }
      inFlightRef.current = true;
      setLogs([]);
      setActiveDuty(duty);
      setPhase(PHASE.RUNNING);

      const previousProfileId = machine.value.status.selectedProfileId;
      const profileToRestore =
        previousProfileId && previousProfileId !== DUTY_PROFILE_ID ? previousProfileId : null;

      try {
        pushLog(`Saving calibration profile at ${duty} % duty...`);
        await apiService.request({ tp: 'req:profiles:save', profile: buildDutyProfile(duty) });

        pushLog('Selecting calibration profile...');
        await apiService.request({ tp: 'req:profiles:select', id: DUTY_PROFILE_ID });

        pushLog('Switching to BREW mode...');
        apiService.send({ tp: 'req:change-mode', mode: MODE_BREW });
        await new Promise(r => setTimeout(r, POST_MODE_SETTLE_MS));

        pushLog('Snapshotting shot history...');
        const before = await fetchShotIndex();
        const preIds = new Set(before.map(e => e.id));

        pushLog(
          'Starting run. Hold the steam valve at roughly 3, then 6, then 9 bar for about eight seconds each.',
          'ok',
        );
        const shotEnd = waitForShotEnd();
        apiService.send({ tp: 'req:process:activate' });
        await shotEnd;

        pushLog('Run finished. Fetching history...', 'ok');
        await new Promise(r => setTimeout(r, POST_SHOT_SETTLE_MS));
        const after = await fetchShotIndex();
        const fresh = after
          .filter(e => !preIds.has(e.id))
          .sort((a, b) => b.timestamp - a.timestamp);
        if (!fresh.length) {
          throw new Error('New run did not appear in history — was it cancelled?');
        }

        setPhase(PHASE.ANALYZING);
        const shot = await fetchAndParseShot(fresh[0].id, msg => pushLog(msg, 'warn'));
        pushLog(`Parsed ${shot.samples.length} samples (v${shot.version}).`);

        const measurePhase = findMeasurePhaseNumber(shot);
        if (measurePhase === null) {
          throw new Error('No "Measure" phase found in the recorded run.');
        }

        const windows = extractSteadyWindows(shot.samples, measurePhase);
        const points = windows.map(w => measureWindow(w)).filter(Boolean);
        pushLog(`${windows.length} steady windows, ${points.length} usable points.`);
        if (!points.length) {
          throw new Error(
            'No usable points. Hold each pressure steadier, or check the scale is connected.',
          );
        }

        setSurface(prev => {
          const merged = mergeSurfacePoints(prev, duty, points);
          storeSurface(merged);
          return merged;
        });
        pushLog('Points added to the surface.', 'ok');
        setPhase(PHASE.DONE);
      } catch (err) {
        detachStatusListener();
        pushLog(`Error: ${err.message}`, 'err');
        setPhase(PHASE.ERROR);
      } finally {
        if (profileToRestore) {
          try {
            pushLog('Restoring previous profile...');
            await apiService.request({ tp: 'req:profiles:select', id: profileToRestore });
          } catch (e) {
            pushLog(`Could not restore previous profile: ${e.message}`, 'warn');
          }
        }
        try {
          await apiService.request({ tp: 'req:profiles:delete', id: DUTY_PROFILE_ID });
        } catch (e) {
          pushLog(`Could not delete calibration profile: ${e.message}`, 'warn');
        }
        setActiveDuty(null);
        inFlightRef.current = false;
      }
    },
    [apiService, detachStatusListener, pushLog, waitForShotEnd],
  );

  const busy = phase === PHASE.RUNNING || phase === PHASE.ANALYZING;

  return { phase, logs, surface, activeDuty, busy, runDuty, clearSurface, reset };
}
```

- [ ] **Step 2: Verify it builds**

Run: `cd web && npm run build`
Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add web/src/components/PumpDutyCalibration/usePumpDutyCalibration.js
git commit -m "feat: Drive fixed-duty calibration runs and accumulate the surface"
```

---

### Task 10: The UI

**Files:**
- Create: `web/src/components/PumpDutyCalibration/index.jsx`

- [ ] **Step 1: Write the component**

Create `web/src/components/PumpDutyCalibration/index.jsx`:

```jsx
import { computed } from '@preact/signals';
import { machine } from '../../services/ApiService.js';
import { downloadJson } from '../../utils/download.js';
import { fitSurface, summariseCoverage } from '../../utils/pumpDutySurface.js';
import { DUTY_LADDER, PHASE } from './constants.js';
import { usePumpDutyCalibration } from './usePumpDutyCalibration.js';

const connected = computed(() => machine.value.connected);

export default function PumpDutyCalibration() {
  const { phase, logs, surface, activeDuty, busy, runDuty, clearSurface } =
    usePumpDutyCalibration();

  const coverage = summariseCoverage(surface, DUTY_LADDER);
  const fit = fitSurface(surface);

  return (
    <div className='flex flex-col gap-4 p-4'>
      <p className='text-sm'>
        Measures delivered flow against both pump duty and pressure, which the two-coefficient pump
        model cannot represent. Fit a blind filter, divert brew water to the steam wand, and put a
        scale under the wand. For each duty level below, start the run and hold the steam valve at
        roughly 3, 6 and 9 bar for about eight seconds each. Holding steady matters more than
        hitting the exact figure.
      </p>

      <div className='flex flex-wrap gap-2'>
        {coverage.map(({ duty, count }) => (
          <button
            key={duty}
            type='button'
            className={`btn btn-sm ${count > 0 ? 'btn-success' : 'btn-outline'}`}
            disabled={busy || !connected.value}
            onClick={() => runDuty(duty)}
          >
            {duty} %{count > 0 ? ` (${count})` : ''}
            {activeDuty === duty ? '...' : ''}
          </button>
        ))}
      </div>

      {logs.length > 0 && (
        <ul className='flex flex-col gap-1 font-mono text-xs'>
          {logs.map(l => (
            <li key={l.key} className={l.tone === 'err' ? 'text-error' : undefined}>
              {l.msg}
            </li>
          ))}
        </ul>
      )}

      {fit.levels.length > 0 && (
        <table className='table table-xs'>
          <thead>
            <tr>
              <th>Duty</th>
              <th>Points</th>
              <th>Flow at {fit.referencePressureBar ?? 6} bar</th>
              <th>Slope per bar</th>
            </tr>
          </thead>
          <tbody>
            {fit.levels.map(l => (
              <tr key={l.duty}>
                <td>{l.duty} %</td>
                <td>{l.count}</td>
                <td>{l.flowAtReference.toFixed(2)} ml/s</td>
                <td>{l.slope.toFixed(3)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}

      {fit.gamma !== null && (
        <p className='text-sm'>
          Duty exponent {fit.gamma.toFixed(3)}. Separability spread{' '}
          {fit.separabilitySpread.toFixed(4)}; small means a single exponent can replace the table,
          large means the surface is not separable and needs one.
        </p>
      )}

      {surface.length > 0 && (
        <div className='flex gap-2'>
          <button
            type='button'
            className='btn btn-sm'
            onClick={() => downloadJson({ surface, fit }, 'pump-duty-surface.json')}
          >
            Export JSON
          </button>
          <button
            type='button'
            className='btn btn-sm btn-ghost'
            disabled={busy}
            onClick={clearSurface}
          >
            Clear surface
          </button>
        </div>
      )}

      {phase === PHASE.ERROR && <p className='text-error text-sm'>Run failed, see log above.</p>}
    </div>
  );
}
```

- [ ] **Step 2: Verify it builds**

Run: `cd web && npm run build`
Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add web/src/components/PumpDutyCalibration/index.jsx
git commit -m "feat: Add pump duty surface calibration UI"
```

---

### Task 11: Mount it

**Files:**
- Modify: `web/src/pages/Settings/tabs/CalibrationTab.jsx:6` and `:204`

- [ ] **Step 1: Add the import**

After the existing import at line 6, add:

```jsx
import PumpDutyCalibration from '../../../components/PumpDutyCalibration/index.jsx';
```

- [ ] **Step 2: Render it below the existing tool**

The existing tool is wrapped by `Section`, and the component itself renders a plain padded div. Match that. Immediately after the closing `</Section>` of the Pump Flow Calibration block, add:

```jsx
      {/* Pump Duty Surface Section */}
      <Section title='Pump Duty Surface Calibration' className='h-full'>
        <PumpDutyCalibration />
      </Section>
```

- [ ] **Step 3: Verify the whole suite and the build**

Run: `cd web && npm test && npm run build`
Expected: 28 tests pass, build succeeds.

- [ ] **Step 4: Commit**

```bash
git add web/src/pages/Settings/tabs/CalibrationTab.jsx
git commit -m "feat: Mount pump duty surface calibration in settings"
```

---

## Manual verification on the machine

Unit tests cover the maths; only the rig proves the rest.

- [ ] Flash or OTA the build, open Settings → Calibration.
- [ ] Fit the blind filter, divert to the steam wand, scale under the wand, wand valve closed.
- [ ] Run the 30 % level. Confirm the pump audibly holds one steady rate for the whole measure phase rather than hunting, which is the check that fixed duty really is fixed.
- [ ] Confirm at least two usable points come back. If zero, the most likely causes are the scale not being paired or the valve being moved continuously rather than held.
- [ ] Complete the ladder, then export the JSON and commit it under `debug/`.
- [ ] Read the separability spread. That number decides which tier under "Consuming the surface" is worth building.

## Consuming the surface: three tiers, none of them in this plan

Feeding the result back into the machine is deliberately out of scope, but the options are worth recording because they differ enormously in cost and because the measurement is what chooses between them.

First a correction to a common assumption. `getAvailableFlow()` is already a **cubic in pressure** with four coefficients, and `setPumpFlowPolyCoeffs` can set all of them; the familiar `6.4,3.9` two-value form is only a convenience wrapper that fills in a straight line. The pressure axis is not the limitation. The limitation is that duty enters solely as a multiplier in `pumpFlowModel(alpha) = duty · Q_geo(P) − slip`, so no surface can live there.

**Tier 0, no code change at all.** Use the surface to choose the best single curve for the duty band the machine actually brews in. This is worth having on its own, because it is exactly the mistake made on 2026-08-02: that calibration fitted a slice at 56 to 87 % duty, where the wand valve had to be fought, and applied it to shots running at 37 to 51 %. It made real shots measurably worse. The surface turns choosing that slice from an accident into a decision, and it works against today's firmware.

**Tier 1, two lines in the controller.** The slip term is a second cubic in pressure and `slipA` through `slipD` already exist in the proto at `gaggimate.proto:124-128`. It is gated twice: the display sends zeros unless addon 7 is present (`Controller.cpp:789-793`), and `getSlip()` clamps non-negative at `PressureController.cpp:104` on the grounds that leakage never is. Remove both gates and the model becomes `flow = duty·Q_full(P) + slip(P)·(duty − 1)`, which with a negative slip is above proportional at partial duty, the shape this pump appears to need. Eight parameters across two cubics, no proto change, no display change beyond sending the coefficients, and the controller image flashes over BLE.

This supersedes an earlier claim in this document that the slip term could not serve. It cannot as written, because of the clamp; with the clamp removed it can. Worth naming honestly that this repurposes a term called leakage, which physically really is non-negative, as a general affine offset. That is mechanically fine and would need explaining if it ever went upstream.

**Tier 2, the full surface.** A table with interpolation, and the closed-form inversion in `getPumpDutyCycleForFlowRate` becomes a numerical solve. Much the largest change, and only justified if the surface is curved enough in duty that an affine fit over the operating range will not do.

The separability spread from `fitSurface` is what decides. A power law `Q(P)·duty^γ` and an affine `a(P)·duty + b(P)` are different shapes, but affine approximates a power law well over the narrow duty band a given profile actually uses, so Tier 1 may well be sufficient even if the surface is formally separable rather than affine. Deciding before measuring would be guessing, which is the whole reason this plan stops at producing the data.
