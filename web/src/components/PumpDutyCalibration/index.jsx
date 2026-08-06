import { computed } from '@preact/signals';
import { machine } from '../../services/ApiService.js';
import { downloadJson } from '../../utils/download.js';
import { SURFACE_FIT_OPTIONS, fitSurface, summariseCoverage } from '../../utils/pumpDutySurface.js';
import { DUTY_LADDER, PHASE } from './constants.js';
import { usePumpDutyCalibration } from './usePumpDutyCalibration.js';

const connected = computed(() => machine.value.connected);

// A duty level contributes nothing to the fit until it has minPointsPerLevel
// points, so a level that is merely started must not read as finished. The
// threshold comes from the fit options so the two cannot drift apart.
function coverageTone(count) {
  if (count >= SURFACE_FIT_OPTIONS.minPointsPerLevel) return 'btn-success';
  return count > 0 ? 'btn-warning' : 'btn-outline';
}

export default function PumpDutyCalibration() {
  const { phase, logs, surface, activeDuty, busy, runDuty, clearSurface } =
    usePumpDutyCalibration();

  const coverage = summariseCoverage(surface, DUTY_LADDER);
  const fit = fitSurface(surface);

  return (
    <div className='flex flex-col gap-4 p-4'>
      <p className='text-sm'>
        Measures delivered flow against both pump duty and pressure, which the two-coefficient pump
        model cannot represent. Fit a blind filter, divert brew water to the steam wand, and put a
        scale under the wand. For each duty level below, start the run and hold the steam valve at
        roughly 3, 5, 7 and 9 bar for about eight seconds each. Holding steady matters more than
        hitting the exact figure.
      </p>

      <div className='flex flex-wrap gap-2'>
        {coverage.map(({ duty, count }) => (
          <button
            key={duty}
            type='button'
            className={`btn btn-sm ${coverageTone(count)}`}
            disabled={busy || !connected.value}
            onClick={() => runDuty(duty)}
          >
            {duty} %{count > 0 ? ` (${count})` : ''}
            {activeDuty === duty ? '...' : ''}
          </button>
        ))}
      </div>

      {logs.length > 0 && (
        <ul className='flex flex-col gap-1 font-mono text-xs'>
          {logs.map(l => (
            <li key={l.key} className={l.tone === 'err' ? 'text-error' : undefined}>
              {l.msg}
            </li>
          ))}
        </ul>
      )}

      {fit.levels.length > 0 && (
        <table className='table-xs table'>
          <thead>
            <tr>
              <th>Duty</th>
              <th>Points</th>
              <th>Flow at {fit.referencePressureBar ?? 6} bar</th>
              <th>Slope per bar</th>
            </tr>
          </thead>
          <tbody>
            {fit.levels.map(l => (
              <tr key={l.duty}>
                <td>{l.duty} %</td>
                <td>{l.count}</td>
                <td>{l.flowAtReference.toFixed(2)} ml/s</td>
                <td>{l.slope.toFixed(3)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}

      {fit.gamma !== null && (
        <p className='text-sm'>
          Duty exponent {fit.gamma.toFixed(3)}. Separability spread{' '}
          {fit.separabilitySpread.toFixed(4)}; small means a single exponent can replace the table,
          large means the surface is not separable and needs one.
        </p>
      )}

      {surface.length > 0 && (
        <div className='flex gap-2'>
          <button
            type='button'
            className='btn btn-sm'
            onClick={() => downloadJson({ surface, fit }, 'pump-duty-surface.json')}
          >
            Export JSON
          </button>
          <button
            type='button'
            className='btn btn-sm btn-ghost'
            disabled={busy}
            onClick={clearSurface}
          >
            Clear surface
          </button>
        </div>
      )}

      {phase === PHASE.ERROR && <p className='text-error text-sm'>Run failed, see log above.</p>}
    </div>
  );
}
