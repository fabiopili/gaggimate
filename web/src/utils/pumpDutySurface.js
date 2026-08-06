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
  // Matches MAX_DT_S in FlowTrimmer.h. Capping the divisor makes a gap in the
  // log read as a steep rate rather than a steady one, so a stretch of time
  // we know nothing about is refused rather than trusted.
  maxDtS: 0.5,
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
    let dt = (samples[i].t - samples[i - 1].t) / 1000;
    if (dt > DEFAULT_OPTIONS.maxDtS) {
      dt = DEFAULT_OPTIONS.maxDtS;
    }
    if (dt > 0) {
      const raw = (samples[i].cp - samples[i - 1].cp) / dt;
      slew += (raw - slew) * Math.min(1, dt / tauS);
    }
    out[i] = slew;
  }
  return out;
}

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
