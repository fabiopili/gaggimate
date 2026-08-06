import { useCallback, useContext, useEffect, useRef, useState } from 'preact/hooks';
import { ApiServiceContext, machine } from '../../services/ApiService.js';
import { fetchAndParseShot, fetchShotIndex } from '../PumpFlowCalibration/api.js';
import {
  extractSteadyWindows,
  findMeasurePhaseNumber,
  measureWindow,
  mergeSurfacePoints,
} from '../../utils/pumpDutySurface.js';
import {
  MODE_BREW,
  PHASE,
  POST_MODE_SETTLE_MS,
  POST_SHOT_SETTLE_MS,
  SHOT_END_TIMEOUT_MS,
  SURFACE_STORAGE_KEY,
} from './constants.js';
import { DUTY_PROFILE_ID, buildDutyProfile } from './profile.js';

function loadStoredSurface() {
  try {
    const raw = window.localStorage.getItem(SURFACE_STORAGE_KEY);
    const parsed = raw ? JSON.parse(raw) : [];
    return Array.isArray(parsed) ? parsed : [];
  } catch {
    return [];
  }
}

function storeSurface(points) {
  try {
    window.localStorage.setItem(SURFACE_STORAGE_KEY, JSON.stringify(points));
  } catch {
    // Storage being unavailable costs resumability, not correctness.
  }
}

/**
 * usePumpDutyCalibration
 * Drives one fixed-duty calibration run at a time and accumulates the
 * resulting (duty, pressure, flow) points into a surface.
 *
 * Returns:
 * - phase, logs, busy — as in usePumpFlowCalibration
 * - surface: Array<{duty, pressure, flow, seconds, grams}>
 * - activeDuty: number | null — duty level of the run in flight
 * - runDuty: (duty: number) => Promise<void>
 * - clearSurface: () => void
 * - reset: () => void
 */
export function usePumpDutyCalibration() {
  const apiService = useContext(ApiServiceContext);

  const [phase, setPhase] = useState(PHASE.IDLE);
  const [logs, setLogs] = useState([]);
  const [surface, setSurface] = useState(loadStoredSurface);
  const [activeDuty, setActiveDuty] = useState(null);

  const statusListenerRef = useRef(null);
  const inFlightRef = useRef(false);
  const safetyIdRef = useRef(null);
  const waitRejectRef = useRef(null);

  const detachStatusListener = useCallback(() => {
    if (statusListenerRef.current !== null) {
      apiService.off('evt:status', statusListenerRef.current);
      statusListenerRef.current = null;
    }
    if (safetyIdRef.current !== null) {
      clearTimeout(safetyIdRef.current);
      safetyIdRef.current = null;
    }
    if (waitRejectRef.current !== null) {
      const reject = waitRejectRef.current;
      waitRejectRef.current = null;
      reject(new Error('Calibration cancelled.'));
    }
  }, [apiService]);

  useEffect(() => detachStatusListener, [detachStatusListener]);

  const pushLog = useCallback((msg, tone = 'info') => {
    setLogs(prev => [...prev, { key: prev.length, msg, tone }]);
  }, []);

  const reset = useCallback(() => {
    detachStatusListener();
    setPhase(PHASE.IDLE);
    setLogs([]);
    setActiveDuty(null);
  }, [detachStatusListener]);

  const clearSurface = useCallback(() => {
    setSurface([]);
    storeSurface([]);
  }, []);

  const waitForShotEnd = useCallback(
    () =>
      new Promise((resolve, reject) => {
        let sawActive = false;
        waitRejectRef.current = reject;
        safetyIdRef.current = setTimeout(() => {
          safetyIdRef.current = null;
          waitRejectRef.current = null;
          detachStatusListener();
          reject(new Error('Timeout waiting for run to finish (5min).'));
        }, SHOT_END_TIMEOUT_MS);
        statusListenerRef.current = apiService.on('evt:status', m => {
          const active = m.process?.a === 1;
          if (active) sawActive = true;
          if (sawActive && !active) {
            if (safetyIdRef.current !== null) {
              clearTimeout(safetyIdRef.current);
              safetyIdRef.current = null;
            }
            waitRejectRef.current = null;
            detachStatusListener();
            resolve();
          }
        });
      }),
    [apiService, detachStatusListener],
  );

  const runDuty = useCallback(
    async duty => {
      if (inFlightRef.current) return;
      if (!apiService) {
        pushLog('Internal error: ApiService unavailable.', 'err');
        setPhase(PHASE.ERROR);
        return;
      }
      inFlightRef.current = true;
      setLogs([]);
      setActiveDuty(duty);
      setPhase(PHASE.RUNNING);

      const previousProfileId = machine.value.status.selectedProfileId;
      const profileToRestore =
        previousProfileId && previousProfileId !== DUTY_PROFILE_ID ? previousProfileId : null;

      try {
        pushLog(`Saving calibration profile at ${duty} % duty...`);
        await apiService.request({ tp: 'req:profiles:save', profile: buildDutyProfile(duty) });

        pushLog('Selecting calibration profile...');
        await apiService.request({ tp: 'req:profiles:select', id: DUTY_PROFILE_ID });

        pushLog('Switching to BREW mode...');
        apiService.send({ tp: 'req:change-mode', mode: MODE_BREW });
        await new Promise(r => setTimeout(r, POST_MODE_SETTLE_MS));

        pushLog('Snapshotting shot history...');
        const before = await fetchShotIndex();
        const preIds = new Set(before.map(e => e.id));

        pushLog(
          'Starting run. Hold the steam valve at roughly 3, then 5, then 7, then 9 bar for about eight seconds each.',
          'ok',
        );
        const shotEnd = waitForShotEnd();
        apiService.send({ tp: 'req:process:activate' });
        await shotEnd;

        pushLog('Run finished. Fetching history...', 'ok');
        await new Promise(r => setTimeout(r, POST_SHOT_SETTLE_MS));
        const after = await fetchShotIndex();
        const fresh = after
          .filter(e => !preIds.has(e.id))
          .sort((a, b) => b.timestamp - a.timestamp);
        if (!fresh.length) {
          throw new Error('New run did not appear in history — was it cancelled?');
        }

        setPhase(PHASE.ANALYZING);
        const shot = await fetchAndParseShot(fresh[0].id, msg => pushLog(msg, 'warn'));
        pushLog(`Parsed ${shot.samples.length} samples (v${shot.version}).`);

        const measurePhase = findMeasurePhaseNumber(shot);
        if (measurePhase === null) {
          throw new Error('No "Measure" phase found in the recorded run.');
        }

        const windows = extractSteadyWindows(shot.samples, measurePhase);
        const points = windows.map(w => measureWindow(w)).filter(Boolean);
        pushLog(`${windows.length} steady windows, ${points.length} usable points.`);
        if (!points.length) {
          throw new Error(
            'No usable points. Hold each pressure steadier, or check the scale is connected.',
          );
        }

        setSurface(prev => {
          const merged = mergeSurfacePoints(prev, duty, points);
          storeSurface(merged);
          return merged;
        });
        pushLog('Points added to the surface.', 'ok');
        setPhase(PHASE.DONE);
      } catch (err) {
        detachStatusListener();
        pushLog(`Error: ${err.message}`, 'err');
        setPhase(PHASE.ERROR);
      } finally {
        if (profileToRestore) {
          try {
            pushLog('Restoring previous profile...');
            await apiService.request({ tp: 'req:profiles:select', id: profileToRestore });
          } catch (e) {
            pushLog(`Could not restore previous profile: ${e.message}`, 'warn');
          }
        }
        try {
          await apiService.request({ tp: 'req:profiles:delete', id: DUTY_PROFILE_ID });
        } catch (e) {
          pushLog(`Could not delete calibration profile: ${e.message}`, 'warn');
        }
        setActiveDuty(null);
        inFlightRef.current = false;
      }
    },
    [apiService, detachStatusListener, pushLog, waitForShotEnd],
  );

  const busy = phase === PHASE.RUNNING || phase === PHASE.ANALYZING;

  return { phase, logs, surface, activeDuty, busy, runDuty, clearSurface, reset };
}
