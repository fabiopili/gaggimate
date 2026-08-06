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
