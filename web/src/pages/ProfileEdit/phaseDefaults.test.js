import { describe, expect, it } from 'vitest';
import { DEFAULT_FLOW_PRESSURE_LIMIT, flowPhasePump } from './phaseDefaults.js';

// A flow phase's pressure value is the arbitration ceiling on the machine
// and 0 disables the ceiling entirely, so an unconstrained phase must be an
// explicit choice, never the accident of switching a phase into flow mode.

describe('flowPhasePump', () => {
  it('defaults the ceiling when coming from power mode', () => {
    expect(flowPhasePump(100)).toEqual({
      target: 'flow',
      pressure: DEFAULT_FLOW_PRESSURE_LIMIT,
      flow: 0,
    });
  });

  it('defaults the ceiling when the previous limit was 0', () => {
    expect(flowPhasePump({ target: 'pressure', pressure: 0, flow: 1.5 })).toEqual({
      target: 'flow',
      pressure: DEFAULT_FLOW_PRESSURE_LIMIT,
      flow: 1.5,
    });
  });

  it('keeps a positive previous pressure as the ceiling', () => {
    expect(flowPhasePump({ target: 'pressure', pressure: 2, flow: 0 })).toEqual({
      target: 'flow',
      pressure: 2,
      flow: 0,
    });
  });

  it('never carries a hold sentinel into the ceiling', () => {
    expect(flowPhasePump({ target: 'pressure', pressure: -1, flow: 0 }).pressure).toBe(
      DEFAULT_FLOW_PRESSURE_LIMIT,
    );
  });

  it('never carries a negative hold flow forward', () => {
    expect(flowPhasePump({ target: 'flow', pressure: 3, flow: -1 }).flow).toBe(0);
  });
});
