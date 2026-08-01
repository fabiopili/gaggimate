// parseFloat that never returns NaN. A cleared or partially typed number input
// yields NaN, which sticks invisibly in state (a type="number" input renders
// NaN as an empty box), serialises to null via JSON.stringify and is read back
// by the firmware as 0. For limit fields 0 is the documented "ignore" sentinel,
// so one accidental clear silently strips a profile's safety ceiling. Falling
// back to the previous value refuses the edit instead of storing the sentinel.
export function parseFloatOr(rawValue, fallback) {
  const parsed = parseFloat(rawValue);
  return Number.isFinite(parsed) ? parsed : fallback;
}
