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
