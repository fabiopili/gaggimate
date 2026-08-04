// Shared normalisation for shot JSON export.
//
// Two places produce exported shots, the history card and the analyzer library,
// and each carried its own copy of this field list. They drifted: the pump duty
// field was added to one and not the other, so exports from the history card
// silently dropped the only channel that records what the pump was actually
// commanded to do. Keep the list here so the two cannot diverge again.

export function round2(v) {
  if (v == null || Number.isNaN(v)) return v;
  return Math.round((v + Number.EPSILON) * 100) / 100;
}

export function normalizeShotSampleForExport(sample = {}) {
  return {
    t: sample.t,
    tt: round2(sample.tt),
    ct: round2(sample.ct),
    tp: round2(sample.tp),
    cp: round2(sample.cp),
    fl: round2(sample.fl),
    tf: round2(sample.tf),
    pf: round2(sample.pf),
    vf: round2(sample.vf),
    v: round2(sample.v),
    ev: round2(sample.ev),
    pr: round2(sample.pr),
    systemInfo: sample.systemInfo,
    // Commanded pump duty, shot log v6 and later. Older shots carry no pp; the
    // undefined passes through round2 and JSON.stringify drops the key.
    pp: round2(sample.pp),
    phaseNumber: sample.phaseNumber,
    phaseDisplayNumber: sample.phaseDisplayNumber,
  };
}
