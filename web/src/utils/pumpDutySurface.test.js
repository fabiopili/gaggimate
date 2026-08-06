import { describe, expect, it } from 'vitest';
import {
  DEFAULT_OPTIONS,
  filteredSlew,
  extractSteadyWindows,
  findMeasurePhaseNumber,
  measureWindow,
} from './pumpDutySurface.js';

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

  it('treats a gap in the log as a steep rate rather than a steady one', () => {
    const samples = [
      { t: 0, cp: 1 },
      { t: 5000, cp: 2 },
    ];
    const slew = filteredSlew(samples);
    expect(slew[1]).toBeGreaterThan(DEFAULT_OPTIONS.maxSlewBarPerS);
  });
});

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
