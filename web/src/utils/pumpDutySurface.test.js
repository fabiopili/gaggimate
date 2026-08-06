import { describe, expect, it } from 'vitest';
import { DEFAULT_OPTIONS, filteredSlew } from './pumpDutySurface.js';

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
