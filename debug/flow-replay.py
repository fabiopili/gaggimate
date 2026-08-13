#!/usr/bin/env python3
# Offline replay of the controller's pump flow model against recorded shots.
#
# Model (PressureController): Q(u, P) = u * (F(P) + S(P)) - S(P), u = duty/100.
# F is the full-drive flow polynomial (up to cubic in P), S the slip polynomial.
# The logged `fl` is a low-passed forward evaluation of the same form, giving a
# built-in self-check that this reimplementation matches the firmware.
#
# Ground truth is the scale flow `vf`. Cup flow equals pump flow only when
# stored water in the puck and circuit is constant AND the duty is steady
# enough for the 250 ms sample to represent it, so the pump fit uses flat-
# pressure, steady-duty windows. Declining-pressure windows are analysed
# separately to measure the storage coefficient an observer would need.

import json
import glob
import sys

TIER1 = (0.0, 0.0, -0.289, 6.074, 0.445, -0.036)  # Fa,Fb,Fc,Fd, s0,s1
LAG = 4           # samples of BLE scale lag (1.0 s at 250 ms)
MIN_WEIGHT = 4.0  # g in cup before cup flow is trusted
FLAT_SLEW = 0.05  # bar/s over 2 s: flat pressure
MAX_DUTY_SPREAD = 8.0  # points of pp movement over +-0.5 s: steady duty
MIN_DUTY_FIT = 15.0
MIN_VF = 0.4

def model(th, u, P):
    Fa, Fb, Fc, Fd, s0, s1 = th
    F = ((Fa * P + Fb) * P + Fc) * P + Fd
    S = s0 + s1 * P
    return u * (F + S) - S

def load_shots():
    shots = {}
    for f in sorted(glob.glob("debug/shot-*.json")):
        d = json.load(open(f))
        s = d.get("samples", [])
        if s and any((x.get("pp") or 0) > 0 for x in s):
            shots[d["id"]] = s
    return shots

def vf_lagged(samples, i):
    return sum(samples[i + LAG + k].get("vf") or 0 for k in (-1, 0, 1)) / 3

def trend(samples, i, half):
    return (samples[i + half]["cp"] - samples[i - half]["cp"]) / (2 * half * 0.25)

def usable(samples, i, min_duty):
    x = samples[i]
    return ((x.get("pp") or 0) >= min_duty and (x.get("tf") or 0) > 0
            and (x.get("v") or 0) >= MIN_WEIGHT)

def duty_steady(samples, i):
    pps = [samples[i + k]["pp"] for k in (-2, -1, 0, 1, 2)]
    return max(pps) - min(pps) <= MAX_DUTY_SPREAD

def flat_samples(samples):
    out = []
    for i in range(4, len(samples) - 4 - LAG - 1):
        if not usable(samples, i, MIN_DUTY_FIT) or not duty_steady(samples, i):
            continue
        if abs(trend(samples, i, 4)) > FLAT_SLEW or abs(trend(samples, i, 2)) > 0.08:
            continue
        vf = vf_lagged(samples, i)
        if vf < MIN_VF:
            continue
        x = samples[i]
        out.append((x["pp"] / 100.0, x["cp"], vf, x.get("fl") or 0))
    return out

def decline_samples(samples):
    out = []
    for i in range(4, len(samples) - 4 - LAG - 1):
        if not usable(samples, i, 8.0):
            continue
        t2, t4 = trend(samples, i, 2), trend(samples, i, 4)
        if not (-0.5 <= t4 <= -0.05 and -0.6 <= t2 <= -0.03):
            continue
        vf = vf_lagged(samples, i)
        if vf < 0.3:
            continue
        x = samples[i]
        out.append((x["pp"] / 100.0, x["cp"], vf, t4))
    return out

def solve(A, b):
    n = len(b)
    for col in range(n):
        piv = max(range(col, n), key=lambda r: abs(A[r][col]))
        A[col], A[piv] = A[piv], A[col]
        b[col], b[piv] = b[piv], b[col]
        for r in range(col + 1, n):
            f = A[r][col] / A[col][col]
            for cc in range(col, n):
                A[r][cc] -= f * A[col][cc]
            b[r] -= f * b[col]
    th = [0.0] * n
    for r in range(n - 1, -1, -1):
        th[r] = (b[r] - sum(A[r][cc] * th[cc] for cc in range(r + 1, n))) / A[r][r]
    return th

def fit(points, degF=2, ridge=1e-4):
    """Weighted OLS. points: (u, P, q, w). Returns full 6-tuple theta."""
    def regress(u, P):
        row = []
        if degF >= 3:
            row.append(u * P ** 3)
        if degF >= 2:
            row.append(u * P ** 2)
        row.append(u * P)
        row.append(u)
        row.append(u - 1)
        row.append((u - 1) * P)
        return row
    X = [regress(u, P) for u, P, q, w in points]
    W = [w for _, _, _, w in points]
    y = [q for _, _, q, _ in points]
    n = len(X[0])
    A = [[sum(W[k] * X[k][i] * X[k][j] for k in range(len(X))) + (ridge if i == j else 0)
          for j in range(n)] for i in range(n)]
    b = [sum(W[k] * X[k][i] * y[k] for k in range(len(X))) for i in range(n)]
    th = solve(A, b)
    th = [0.0] * (6 - len(th)) + th
    return tuple(th)

def rms(errs):
    return (sum(e * e for e in errs) / len(errs)) ** 0.5 if errs else float("nan")

def monotone_ok(th, lo=0.5, hi=11.5):
    vals = [model(th, 1.0, lo + k * 0.25) for k in range(int((hi - lo) / 0.25) + 1)]
    return all(vals[i + 1] <= vals[i] + 1e-6 for i in range(len(vals) - 1)) and min(vals) > 0

def main():
    shots = load_shots()
    flat = {sid: pts for sid, pts in ((s, flat_samples(x)) for s, x in shots.items()) if len(pts) >= 6}
    decl = {sid: pts for sid, pts in ((s, decline_samples(x)) for s, x in shots.items()) if len(pts) >= 6}
    rig = json.load(open("debug/pump-duty-surface.json"))["surface"]

    print("== self-check: reimplemented model vs logged fl (Tier 1 shots) ==")
    for sid in ("62", "63"):
        errs = [model(TIER1, u, P) - fl for u, P, vf, fl in flat.get(sid, []) if fl > 0]
        if errs:
            print(f"  shot {sid}: mean {sum(errs)/len(errs):+.3f} g/s, rms {rms(errs):.3f} over {len(errs)} samples")

    print("\n== FLAT+steady-duty samples: Tier 1 vs scale ==")
    print("  shot   n    duty range   P range     vf/Q median   rms g/s")
    for sid in sorted(flat, key=lambda s: int(s)):
        pts = flat[sid]
        ratios = sorted(vf / model(TIER1, u, P) for u, P, vf, fl in pts if model(TIER1, u, P) > 0.2)
        errs = [vf - model(TIER1, u, P) for u, P, vf, fl in pts]
        us = [u for u, *_ in pts]; Ps = [P for _, P, *_ in pts]
        med = ratios[len(ratios) // 2] if ratios else float("nan")
        print(f"  {sid:>4} {len(pts):>4}   {min(us)*100:3.0f}-{max(us)*100:3.0f}     "
              f"{min(Ps):4.1f}-{max(Ps):4.1f}     {med:6.2f}      {rms(errs):5.2f}")

    shot_pts = [(u, P, vf, 1.0) for pts in flat.values() for u, P, vf, fl in pts]
    w_rig = len(shot_pts) / len(rig)  # equal total weight for the two sources
    rig_pts = [(r["duty"] / 100.0, r["pressure"], r["flow"], w_rig) for r in rig]
    joint = shot_pts + rig_pts

    candidates = {}
    for name, deg, pts in (("affine-joint", 1, joint), ("quad-joint", 2, joint), ("cubic-joint", 3, joint)):
        th = fit(pts, degF=deg)
        candidates[name] = th
    candidates["Tier1"] = TIER1

    print(f"\n== candidate surfaces ({len(shot_pts)} shot pts, {len(rig)} rig pts at weight {w_rig:.0f}) ==")
    print("  name          rig rms   shot rms   P<3 ratio   P>9 ratio   monotone")
    for name, th in candidates.items():
        e_rig = rms([q - model(th, u, P) for u, P, q, w in rig_pts])
        e_shot = rms([q - model(th, u, P) for u, P, q, w in shot_pts])
        lo = sorted(q / model(th, u, P) for u, P, q, w in shot_pts if P < 3 and model(th, u, P) > 0.2)
        hi = sorted(q / model(th, u, P) for u, P, q, w in shot_pts if P > 9 and model(th, u, P) > 0.2)
        rl = lo[len(lo) // 2] if lo else float("nan")
        rh = hi[len(hi) // 2] if hi else float("nan")
        print(f"  {name:12}  {e_rig:5.2f}     {e_shot:5.2f}      {rl:5.2f}       {rh:5.2f}       {monotone_ok(th)}")

    # leave-one-shot-out for the best-structured candidate (chosen after table)
    best = "affine-joint"
    print(f"\n== leave-one-shot-out rms per held-out shot ({best} refit each time) ==")
    print("  shot    Tier1    LOSO")
    per = {sid: [(u, P, vf, 1.0) for u, P, vf, fl in pts] for sid, pts in flat.items()}
    worse = 0
    deg = {"affine-joint": 1, "quad-joint": 2, "cubic-joint": 3}[best]
    for sid in sorted(per, key=lambda s: int(s)):
        train = [p for o, pts in per.items() if o != sid for p in pts] + rig_pts
        th = fit(train, degF=deg)
        e1 = rms([q - model(TIER1, u, P) for u, P, q, w in per[sid]])
        e2 = rms([q - model(th, u, P) for u, P, q, w in per[sid]])
        worse += e2 > e1
        print(f"  {sid:>4}   {e1:5.2f}    {e2:5.2f}")
    print(f"  (candidate worse than Tier 1 on {worse}/{len(per)} held-out shots)")

    th_best = candidates[best]
    print(f"\n== chosen candidate: {best} ==")
    Fa, Fb, Fc, Fd, s0, s1 = th_best
    print(f"  F(P) = {Fa:.4f} P^3 {Fb:+.4f} P^2 {Fc:+.4f} P {Fd:+.4f}")
    print(f"  S(P) = {s0:.4f} {s1:+.4f} P")
    print(f"  settings strings:  Pump Flow Coefficients = {Fa:.4g},{Fb:.4g},{Fc:.4g},{Fd:.4g}")
    print(f"                     Pump Slip Coefficients = 0,0,{s1:.4g},{s0:.4g}")
    print("  P:      " + "  ".join(f"{P:5.1f}" for P in (1, 2, 4, 6, 8, 9, 10, 11)))
    print("  Q(u=1): " + "  ".join(f"{model(th_best, 1, P):5.2f}" for P in (1, 2, 4, 6, 8, 9, 10, 11)))
    print("  Q(.5):  " + "  ".join(f"{model(th_best, 0.5, P):5.2f}" for P in (1, 2, 4, 6, 8, 9, 10, 11)))
    print("  Q(.3):  " + "  ".join(f"{model(th_best, 0.3, P):5.2f}" for P in (1, 2, 4, 6, 8, 9, 10, 11)))

    print("\n== residuals by pressure band, Tier 1 vs candidate (flat shot samples) ==")
    for label, lo, hi in (("P<2", 0, 2), ("P 2-3", 2, 3), ("P 3-6", 3, 6), ("P 6-9", 6, 9), ("P 9-10.5", 9, 10.5), ("P>10.5", 10.5, 99)):
        sel = [(u, P, q) for u, P, q, w in shot_pts if lo <= P < hi]
        if not sel:
            continue
        e1 = [q - model(TIER1, u, P) for u, P, q in sel]
        e2 = [q - model(th_best, u, P) for u, P, q in sel]
        print(f"  {label:9}: n={len(sel):4}  Tier1 bias {sum(e1)/len(e1):+.2f} rms {rms(e1):.2f}   candidate bias {sum(e2)/len(e2):+.2f} rms {rms(e2):.2f}")

    print("\n== DECLINE windows: storage coefficient per shot, candidate model ==")
    print("  shot   n    P range     C g/bar   grams explained")
    Cs = []
    for sid in sorted(decl, key=lambda s: int(s)):
        pts = decl[sid]
        num = sum((vf - model(th_best, u, P)) * (-d) for u, P, vf, d in pts)
        den = sum(d * d for u, P, vf, d in pts)
        C = num / den if den else float("nan")
        grams = sum((vf - model(th_best, u, P)) * 0.25 for u, P, vf, d in pts)
        Ps = [P for _, P, *_ in pts]
        Cs.append(C)
        print(f"  {sid:>4} {len(pts):>4}   {min(Ps):4.1f}-{max(Ps):4.1f}   {C:6.2f}      {grams:5.1f}")
    Cs.sort()
    if Cs:
        print(f"  pooled median C: {Cs[len(Cs)//2]:.2f} g/bar")

if __name__ == "__main__":
    sys.exit(main())
