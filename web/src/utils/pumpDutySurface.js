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
