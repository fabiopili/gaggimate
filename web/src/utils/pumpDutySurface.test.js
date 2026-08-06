import { describe, expect, it } from 'vitest';
import {
  DEFAULT_OPTIONS,
  filteredSlew,
  extractSteadyWindows,
  findMeasurePhaseNumber,
  measureWindow,
  fitLine,
  fitSurface,
  mergeSurfacePoints,
  summariseCoverage,
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

// Synthetic surface from flow = (8 - 0.45 P (duty/45)) * (duty/100)^0.55. The
// pressure term is scaled by duty, so the pump's sensitivity to pressure
// grows relative to its intercept as duty rises. slope/intercept is
// -0.00125 * duty, not a constant, so this surface is not separable and the
// fit must report a spread that reflects that.
function nonSeparableSurface() {
  const points = [];
  for (const duty of [30, 45, 60, 75, 90]) {
    for (const pressure of [2, 4, 6, 8]) {
      points.push({
        duty,
        pressure,
        flow: (8 - 0.45 * pressure * (duty / 45)) * Math.pow(duty / 100, 0.55),
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
    expect(
      fitLine([
        { x: 1, y: 1 },
        { x: 1, y: 2 },
      ]),
    ).toBe(null);
  });
});

describe('fitSurface', () => {
  it('recovers the duty exponent of a separable surface', () => {
    expect(fitSurface(syntheticSurface(0.55)).gamma).toBeCloseTo(0.55, 2);
  });

  it('reports a near-zero separability spread for a separable surface', () => {
    expect(Math.abs(fitSurface(syntheticSurface()).separabilitySpread)).toBeLessThan(0.01);
  });

  it('reports a large separability spread when the surface is not separable', () => {
    const separableSpread = Math.abs(fitSurface(syntheticSurface()).separabilitySpread);
    const nonSeparableSpread = Math.abs(fitSurface(nonSeparableSurface()).separabilitySpread);
    expect(nonSeparableSpread).toBeGreaterThan(separableSpread * 10);
    expect(nonSeparableSpread).toBeGreaterThan(0.01);
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
