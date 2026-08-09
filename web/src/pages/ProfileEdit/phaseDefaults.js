// A flow phase's pressure value is the arbitration ceiling on the machine;
// 0 disables that ceiling entirely and the pump follows the flow target at
// whatever pressure the puck produces. Running unconstrained is a valid
// choice, but it has to be an explicit one: switching a phase into flow
// mode must never produce a capless phase by accident, so the switch
// defaults the ceiling and 0 has to be typed.
export const DEFAULT_FLOW_PRESSURE_LIMIT = 9;

export function flowPhasePump(previousPump) {
  const previous =
    typeof previousPump === 'object' && previousPump !== null ? previousPump : {};
  return {
    target: 'flow',
    pressure: previous.pressure > 0 ? previous.pressure : DEFAULT_FLOW_PRESSURE_LIMIT,
    flow: Math.max(previous.flow || 0, 0),
  };
}
