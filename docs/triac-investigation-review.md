# Pump TRIAC failure investigation — firmware review (reviewer subagent, 2026-11-08)

Scope: could any code change between the fork's v1.8.x-era firmware and stock upstream v1.9.0 cause or permit the four pump TRIAC deaths? Operator-provided timeline fact folded in afterwards: death #1 was May 2026 on v1.8.0/v1.8.1-era firmware, so the series spans four firmware generations. Verdict: no code change explains the deaths; the P1 items below are hardening work, not regressions that caused the failures.

---

# Review
- **Correct:** the artefacts let me verify every claim below against a concrete diff hunk or a source line; the two central premises in the brief check out (v1.8.9 ctor `divider=2`, v1.9.0 ctor `divider=1` + `setDivider(2)`; both use a fixed 30 ms pump loop with a fixed `0.03f` controller dt).
- **Fixed:** nothing — review-only task, no edits made.
- **Finding:** P1 ×2 (divider fallback is the only path that can put the gate on a single half-cycle; gate cannot be forced LOW without a zero-cross interrupt), P2 ×4 (unpinned PSM dependency, latent `stop()/fullPower()` declarations, `getPowerTarget()` always 0, stop-vs-loop `_psm.set()` race). Details and evidence in §8.
- **Merge verdict:** OK with notes — **no code change is justified by this evidence as the cause of the TRIAC deaths**; the two P1s are hardening items, not regressions introduced by v1.9.0.

---

# Pump TRIAC failure investigation — v1.8.9 vs v1.9.0 (and master)

## 0. Scope, method, evidence

Target: could any code change between the fork's v1.8.x-era firmware (`v1.8.9`) and stock upstream `v1.9.0` (tag = `43d2a7ba`) plausibly increase electrical stress on the pump TRIAC (D6, Z0107MN-class, MOC3021 U4) on a GaggiMate **Pro Rev 1.1** (board autodetect 4)?

Evidence used (all verified, none assumed):

| Evidence | Path |
|---|---|
| Diff of the controller path, `v1.8.9..v1.9.0` | `temp/triac/v1.8.9_to_v1.9.0_controller.diff` |
| Diff of the display/comms path, `v1.8.9..v1.9.0` | `temp/triac/v1.8.9_to_v1.9.0_display_comms.diff` |
| Diff of `src/display/core/process` + `src/display/models` | `temp/triac/v1.8.9_to_v1.9.0_display_process_models.diff` |
| Verbatim `v1.9.0:DimmedPump.cpp` | `temp/triac/v1.9.0_DimmedPump.cpp` |
| `git log --oneline v1.9.0..master` + `--stat` (truncated to 38 lines) | `temp/triac/v1.9.0_to_master.log` (n/a — file is `v1.9.0_to_master_log.txt`) |
| Working tree (master) | read directly |
| PSM library actually compiled | `.pio/libdeps/controller/PSM/src/PSM.{cpp,h}` |

**Path coverage of the diff artefacts** (matters for "no hunk ⇒ no change" claims): the controller diff covers all of `lib/GaggiMateController/src` and `lib/NayrodPID`; the display diff covers `src/display/core/Controller.cpp` and `lib/NanoPbComm`. Therefore *absence of a hunk* is proof of no change for `SimpleRelay.{cpp,h}`, `PressureSensor.{cpp,h}`, `Pump.h`, `ControllerConfig.h`, `GaggiMateController::onPumpControl/onRelayControl/handlePingTimeout/thermalRunawayShutdown`, `BrewProcess.h` phase/pump-accessor logic, and `CoalescingPriorityQueue.h`. Not covered (stated as a gap): `lib/GaggiMateController/library.json`, `platformio.ini`, `src/display/core/Settings.h`, `src/display/plugins/*`, `src/display/ui/*`.

Baseline hardware/config (verified, master, and untouched by the diff): `GM_PRO_REV_11` — `pumpPin 9`, `pumpSensePin 21`, `pumpOn 1`, `valvePin 10`, `valveOn 1`, `capabilities.dimming+pressure`, `adcRates {500,0,0,0}` (`lib/GaggiMateController/src/ControllerConfig.h:216-253`).

---

## 1. Verified facts about the gate drive (identical in all three versions unless stated)

1. The gate pin is written **only** from the zero-cross ISR: `PSM::onInterrupt() → calculateSkip() → updateControl() → digitalWrite(controlPin, _skip ? LOW : HIGH)` — `.pio/libdeps/controller/PSM/src/PSM.cpp:29-43, 65-101`. Nothing else in the tree touches the pump pin (only `DimmedPump` constructs `_psm`, `lib/GaggiMateController/src/peripherals/DimmedPump.cpp:7`).
2. `PSM::set()` changes **only** `_value`; it never writes the pin (`PSM.cpp:45-51`). Consequence: a commanded power of 0 does **not** de-assert the gate — the pin only goes LOW at the next zero-cross decision. See §8 P1-b.
3. `interruptMinTimeDiff = 4` (`DimmedPump.cpp:7`) filters edges closer than 4 ms ⇒ hard cap of 250 decisions/s regardless of noise; at 50 Hz, 100 pulses/s (2/cycle) decode normally.
4. Decisions are always taken **at a zero-crossing edge**, never mid-half-cycle. Every conduction window therefore starts at ~0 V in all three versions. This is the single most important fact for the "wrong phase" hypothesis: *no code path in any version can fire the gate off-zero*.
5. `cps()` (`PSM.cpp:103-126`) forces `_range=0xFFFF`, `_value=1`, `_divider=1`, `_a=0`, busy-waits 1000 ms via `delay(0)`, then returns `_a` (pulse count). Because `_value=1`, `_skip` is TRUE on every pulse during the window ⇒ the gate is driven LOW for the whole detection second (the first pulse after entry writes LOW). The detection window is therefore electrically safe.
6. The pump loop task period: **30 ms in v1.8.9 and v1.9.0** (`temp/triac/v1.9.0_DimmedPump.cpp:63-71`, `pdMS_TO_TICKS(30)`; the v1.8.9→v1.9.0 diff has no hunk in `loopTask`), and 17/17/16 ms in master (`lib/GaggiMateController/src/peripherals/DimmedPump.cpp:69` via `pressureControlIntervalMs`, `PressureTiming.h:11-13`, Pro = `Hz60`). Controller dt is the fixed `0.03f` in both tags, elapsed-based in master.
7. Pump and valve commands are **already batched into one BLE frame in v1.8.9** (`sendBatch` → `Endpoint::pump()` pops into `_txFrame.payloads`, `lib/NanoPbComm/src/Endpoint.cpp:203-208`; `CoalescingPriorityQueue.h` and per-component `PumpCommand`/`RelayCommand` with `index` are *context*, unchanged, in `v1.8.9_to_v1.9.0_display_comms.diff:563-606,1937-1940`). Because ties at equal priority break **newest-first** (`CoalescingPriorityQueue.h:28-30,120-125`, all of boiler/pump/relay are `PRIO_CONTROL`, `Protocol.h:56-62`), the frame order at shot start is **relay → pump → boiler**, i.e. *valve first, pump armed microseconds later, in the same frame*. Same in v1.8.9, v1.9.0 and master.

---

## 2. Task 1 — Diff inventory, v1.8.9 → v1.9.0

### 2.1 The one gate-path change
`lib/GaggiMateController/src/peripherals/DimmedPump.cpp` (`v1.8.9_to_v1.9.0_controller.diff`, DimmedPump.cpp hunks 1-2):

```
-    : ... _psm(_sense_pin, _ssr_pin, 100, FALLING, 2, 4) ...
+    : ... _psm(_sense_pin, _ssr_pin, 100, FALLING, 1, 4) ...
...
     _cps = _psm.cps();
     if (_cps > 70) {
         _cps = _cps / 2;
+        _psm.setDivider(2);
     }
```
Confirms the brief exactly. **Verdict: see §4 — NO-CHANGE when autodetect reads >70; otherwise BEHAVIOUR-CHANGE (see §4.3 for the only TRIAC-stress-relevant consequence).**

### 2.2 PressureController (NayrodPID) — the largest behavioural delta
All in `v1.8.9_to_v1.9.0_controller.diff` (PressureController.cpp/.h hunks). Removed from v1.9.0 (i.e. present in the fork, absent upstream):

| Removed in v1.9.0 | Consequence |
|---|---|
| `updateFlowArbitration()` + the whole ceiling latch (`_ceilingLatched`, `_latchEntryPersistS`, `_latchWinningDuty`, handback cap/fade) | flow-mode pressure ceiling no longer has hysteresis/anti-oscillation memory |
| `trackPressureBranch()` override tracking (`_lastKi`) | when the flow branch wins, the pressure branch's integral is now simply zeroed (`+ _errorIntegral = 0.0f; // Reset error buildup in flow target`) instead of being continuously conditioned |
| Two-sided conditional integration `if ((error>0 && duty<0) \|\| (error<0 && duty>1))` | replaced by `if ((sign(error) == -sign(_pumpDutyCycle)) && (fabs(_pumpDutyCycle) > 1.0f))` |

The replacement anti-windup is the crux: `_pumpDutyCycle` is in 0..1 units (scaled by 100 only at the return, `PressureController.cpp` v1.9.0 hunk `@@ -438,7 +310,6`), so `fabs(_pumpDutyCycle) > 1.0f` only trips beyond **±100 %**. At the *lower* rail (output clamped at 0 %, pressure above setpoint) the integral keeps integrating `error*dt` **without any unwind** until the raw duty passes −100 %. **Verdict: BEHAVIOUR-CHANGE, and the one regression with a plausible electrical coupling** — see §6 and §8 P1-a.

Note also the fork's own comments in the `-` side document the previously observed failure mode of exactly this code: *"duty saw-toothing between the feed-forward and the claw-back, with full cuts"*, *"the recovery undershot into a long cut (shots 54/55)"*. v1.9.0 is that pre-fix code.

### 2.3 `getPumpDutyCycleForFlowRate()`
v1.9.0 **adds** `if (*_rawFlowSetpoint <= 0.0f) return 0.0f;`. With slip coefficients zero this is inert (v1.8.9 computes `(0+0)/Q*100 = 0` anyway). **Verdict: NO-CHANGE** in practice.

### 2.4 Shot-start duty trajectory (the "hard 100 % jump vs ramp" question)
The pressure-mode control law (`_pumpDutyCycle = Ceq/Qa*(-conv*error - K*sat_s) - iterm`) and the 1 Hz setpoint filter are **untouched context lines** in the diff; `tare()` zeroes `_errorIntegral` in both, and the display tares before the first pump command in both (`comms.tare()` at `activate()`, context line 1967 of the display diff). Therefore the first control cycles of a pressure-target phase are **identical**:

- cycle 1 (t = 0…30 ms): setpoint filter output ≈ 0.27 bar for a 9 bar target (rate-limited to 9 bar/s), error −0.27, `sat_s → −0.995`, ⇒ duty ≈ **73 %**;
- by ~0.1 s the law rails at **100 %** and stays there until pressure builds.

A flow-target phase starts at `duty = flow/Q_geo ≈ 37 %` for 4 ml/s. **Verdict: NO-CHANGE** — the shot-start step to full power is pre-existing, not a v1.9.0 regression (this is the most useful negative result in the report).

Display side, `src/display/core/Controller.cpp` hunks in the display diff:
- `pump.pressure = brewProcess->getPumpPressure() + settings.getPressureOffset()` (v1.9.0 always adds the offset; v1.8.9 passed it raw) — **CALIBRATION-ONLY** (closed-loop target shifted by the mushroom-valve offset).
- flow-trimmer (`FlowTrimmer`) removed — **CALIBRATION-ONLY**.
- `startProcess()` now calls `updateControl()` immediately (`v1.8.9_to_v1.9.0_display_comms.diff:1841`; `src/display/core/Controller.cpp:787`), and `activate()` moved its `delay(200)` inside the volumetric branch — **BEHAVIOUR-CHANGE, benign** (the pump+valve pair is emitted one display-loop iteration earlier, still as one frame, still valve-then-pump).
- `activate()` guards (`!loaded || !comms.isConnected() || !isReady()`, confirm-on-warning) and `afterDeactivate()` (immediate stop + tare at process end) — **BEHAVIOUR-CHANGE, benign**; `afterDeactivate()` only makes the shot-end pump-off earlier by one loop iteration.

### 2.5 Unchanged by the diff (verified by absence of hunks)
`SimpleRelay.cpp/.h`; `PressureSensor.cpp/.h`; `Pump.h`; `DimmedPump::setPower()`/`setBinaryMode()`/`setValveState()`; controller `onPumpControl()`/`onRelayControl()`; `handlePingTimeout()` (pump→0, valve→0, both versions; master additionally drops the BLE link on the healthy→timeout transition); `thermalRunawayShutdown()`; `PING_TIMEOUT_SECONDS = 20`; `ControllerConfig`; `CoalescingPriorityQueue`; `BrewProcess::getPumpValue()/isRelayActive()/isAdvancedPump()` and phase pump targets (process/model diff: only `holdPhase`, `waterPumped` bookkeeping and an equivalent `transitionAlpha()` refactor). **Verdict: NO-CHANGE on valve/pump ordering, on the start-of-shot pump target, and on the watchdog/error paths.**

---

## 3. Task 2 — PSM edge cases as used by v1.9.0

### (a) `cps()` misdetection
`cps()` returns the number of accepted zero-cross edges in 1000 ms (`PSM.cpp:103-126`). Decision rule: `>70 ⇒ setDivider(2)`.

- **Mains absent at boot:** `cps() = 0` (or ~1) ⇒ `setDivider(2)` not called ⇒ **divider 1**. But on this machine the controller is powered from the machine's own supply, so "mains absent" means the whole board is off. The realistic variant is *ZC detector dead / sense line open* while the board is powered from a bench supply — then the machine boots on **divider 1** and stays there for the session.
- **A dropped chunk of pulses** (long ISR block, detector behaving oddly): 30 % of pulses must be lost to fall below 70 — the 4 ms filter makes this hard; the 10 ms nominal spacing passes with ~6 ms of margin. Low risk.
- **Noise doubling:** extra edges must be ≥4 ms apart to pass the filter. A doubled count (e.g. 200) still satisfies `>70` ⇒ divider 2 and `_cps = 100`. **Noise therefore biases towards the safe (v1.8.9-identical) side.**
- **50 Hz square-wave detector (1 edge/cycle, cps≈50):** ⇒ divider 1, i.e. *one decision per mains cycle* — which is the mathematically correct choice for that signal (v1.8.9 at divider 2 gave only 25 decisions/s there). Not a fault.
- **Direction of the error is the good direction:** every misdetection above changes the decision granularity from 20 ms to 10 ms, never the reverse; average power at a given `_value` is unchanged, and the worst case (full power, `_value = 100`) is bit-identical (gate continuously HIGH) in both.

⇒ **The machine can indeed end up on divider 1 instead of 2.** What that changes electrically is §4.

### (b) Zero-cross pulses stop while the gate was last driven HIGH
**Yes — the gate stays HIGH indefinitely.** Verified structurally: `updateControl()` is reachable only from `calculateSkip()`, which is reachable only from `onInterrupt()` (`PSM.cpp:29-43, 65-101`); `set()`, `setDivider()`, `shiftDividerCounter()`, `stopAfter()` and `resetCounter()` never touch the pin. There is no timeout, no compare against `getLastMillis()`, and no `PSM::off()`.

Consequence: a continuous DC gate current through the MOC3021 LED and the TRIAC gate; the TRIAC is held triggered and, for a load current that crosses zero at least once per cycle, behaves as a permanently-on device (100 % pump). The board's *safety* paths do not fix this either: `handlePingTimeout()`/`thermalRunawayShutdown()` call `pump->setPower(0)` ⇒ `_psm.set(0)` ⇒ `_value = 0`, which only takes effect at the *next* zero-cross interrupt. In the one fault case where the gate is stuck HIGH (no zero-cross), the safety path cannot turn the pump off. **Identical in v1.8.9, v1.9.0 and master** ⇒ NO-CHANGE for the version question, but a genuine hardening gap (§8 P1-b).

### (c) Race between `set()` (tasks) and `calculateSkip()` (ISR)
- `_value` is `volatile unsigned int` (`PSM.h:44`) — a single 32-bit store/load on Xtensa ⇒ **no torn value, no corrupted skip pattern**. `_a` is only touched by the ISR (and by `cps()` before the pump task exists).
- The worst-case effect is a *stale duty for one decision* (≤10 ms at divider 1, ≤20 ms at divider 2) or last-writer-wins between the comms task (`onPumpControl → setPower → _psm.set`) and the pump task (`updatePower → _psm.set`, every 30 ms). Both call sites exist unchanged in v1.8.9 ⇒ **NO-CHANGE**; and the phase is never wrong, because the pin write happens in the ISR at the edge.
- One real (shared) wrinkle: a stop command issued from the comms task can be overwritten by a pump-task `updatePower()` that read the OLD `_mode` before the stop and writes the OLD power after it ⇒ pump can stay on for up to one 30 ms tick. P2 in §8.

### (d) divider 1 vs 2 for a half-wave (series-diode) pump
At 50 Hz with 100 pulses/s:

| | divider 2 (v1.8.9 always; v1.9.0 if cps>70) | divider 1 (v1.9.0 fallback) |
|---|---|---|
| Decisions/s | 50 (one per mains cycle) | 100 (one per half-cycle) |
| Gate HIGH window when firing | 20 ms — spans **both** half-cycles | 10 ms — a **single** half-cycle |
| Granularity of integral-cycle control | whole mains cycles | half-cycles |
| Gate HIGH duty at a given `_value` | V % | V % |
| Max TRIAC turn-on events/s | 50 | 100 |
| Single-half-cycle conduction possible | **no** | **yes** |

For a **half-wave pump** the two extra consequences are: (i) the delivered energy per `_value` is **halved** relative to the intent, because half the "fire" decisions land on the diode-blocked half-cycle, and the very worst case is a phase lock — e.g. `_value = 50` alternating fire/skip means the *same* half-cycle parity fires every time, which can be the blocked one ⇒ effectively 0 W at a 50 % command; (ii) each firing is a **single** half-cycle of an inductive load, whose current does not return to zero at the voltage zero but lags into the next half-cycle, so the TRIAC must commutate off against the full reapplied mains voltage — the classic commutation-dv/dt exposure that whole-cycle (divider 2) operation avoids. That is the only mechanism in the whole diff by which the firmware can put the TRIAC into a mode that is electrically different *in kind* rather than in degree. **It is unreachable unless `cps() ≤ 70` at boot.**

---

## 4. Task 3 — Is the divider change a no-op on this machine?

**Yes, if and only if the boot-time autodetect reads >70 (`_cps = 100` on a 100-pulse/s 50 Hz signal) — which is the expected case.**

- v1.8.9: ctor `divider = 2`; `cps()` saves `divider`, forces 1 for its window, then **restores 2** (`PSM.cpp:104-125`) ⇒ running divider = 2, gate HIGH 20 ms per fired cycle, i.e. whole-cycle integral-cycle control at 50 decisions/s. `_cps` was computed but used only for a water estimate (now dead code, `DimmedPump.cpp:73-86` v1.9.0).
- v1.9.0 with `cps ≈ 100`: `100 > 70` ⇒ `_cps = 50`, `setDivider(2)` ⇒ **byte-for-byte the same gate waveform as v1.8.9**, including the `_dividerCounter` state (it stays 1 through both the cps window and the setDivider call, so the first post-setup decision is a `calculateSkip` in both).
- Master: identical to v1.9.0 for this path (only the `ESP_LOGI` of the detected frequency is new, commit `960589bf`, which appears in `v1.9.0_to_master_log.txt`).

**Conclusion for the working hypothesis: the divider change cannot be the cause of the failures** on a machine whose autodetect succeeds. It becomes relevant only in the fallback case of §3(d), and even then its effect is *halved and asymmetric pump power* (loop saturates at 100 %, where the gate is continuously HIGH — the least switching stress) plus **single-half-cycle commutation**, which is a genuine stress mechanism but a conditional one. **The decisive measurement is the boot log line `Detected ZC frequency: %dHz` — present in master (`DimmedPump.cpp:15`) but NOT in the flashed v1.9.0** (added afterwards by `960589bf`).

---

## 5. Task 4 — What master adds over the flashed v1.9.0

From the working tree (and the truncated `v1.9.0_to_master_log.txt`, whose 25 subjects contain nothing pump-drop-related except `960589bf Add CPS logging in DimmedPump`):

1. **Pump loop rate 33 Hz → ~59 Hz.** `PressureControlRate {Hz50, Hz60}` (`PressureTiming.h`), Pro Rev 1.1 = `Hz60`, interval pattern 17/17/16 ms (`ControllerConfig.h:301`, `DimmedPump.cpp:69`). Duty is pushed to the PSM ~1.8× more often.
2. **Elapsed-time based control dt** instead of the fixed `0.03f` (`DimmedPump::updatePower(fresh, sampleTime, elapsed)`), plus `PressureSensor::consumeAverage()` with the real window length, `_sampleTime` clamped to 0.5–100 ms, and an averaged (not single-sample) pressure (`PressureSensor.cpp:26-45`). This materially reduces duty noise versus the single raw sample per 30 ms used by both tags.
3. **A third anti-windup rewrite**: `const bool integrating = fabsf(error) < epsilon;` and `if (integrating && ((duty > 1 && error < 0) || (duty < 0 && error > 0)))` (`PressureController.cpp:295-312`). This *fixes* exactly the v1.9.0 windup described in §2.2 — i.e. **the regression is already repaired in master**.
4. CPS logging in `DimmedPump::setup()` (`960589bf`).
5. Comms v2 on the controller (`onPumpControl(index, …)`, `onRelayControl(index, …)`, `sendUnreliableBatch`, errorState gating, LED health check, `handlePingTimeout` now also `_comms.disconnect()` on the transition), `HeaterCoordinator`, dual-boiler refill/hot-water valves, pump slip coefficients only for the gear-pump addon.

Net: a rebuild from master changes the *rate* of duty updates and removes the duty-windup regression; it does not change the gate's phase discipline or the valve/pump ordering.

---

## 6. Ranked shortlist of code-related contributors

| # | Candidate | Mechanism (electrical) | Verdict | Confidence |
|---|---|---|---|---|
| 1 | **divider=1 fallback** (only if boot `cps()` ≤ 70) | permits single-half-cycle conduction of an inductive half-wave load ⇒ commutation dv/dt at a non-zero reapplied voltage; halves delivered power (loop saturates at 100 %); up to 100 turn-ons/s instead of 50 | TRIAC-STRESS *(conditional)* | mechanism certain; **reachability unknown and probably nil** (needs `cps ≤ 70`) |
| 2 | **Ceiling-latch + anti-windup regression in v1.9.0** | flow-mode with a pressure ceiling: integral is zeroed whenever the flow branch wins and never unwound at the 0 % rail ⇒ duty saw-tooths and can hold **0 % for ~0.5–2 s** then step to **100 %** in one 30 ms tick (documented by the fork as "full cuts", "long cut, shots 54/55"). Each return is a fresh 0→100 step into a depressurised head (pump inrush) and each cut is a full de-energisation. Always zero-cross switched, so no phase error | TRIAC-STRESS *(weak/plausible)* | behavioural difference certain; electrical coupling hypothetical |
| 3 | pressure offset / flow-trimmer / slip-coefficient changes | shift closed-loop targets by ~1 bar and setpoint duty by a few % ⇒ duty sits slightly higher/lower on average | CALIBRATION-ONLY | certain |
| 4 | `startProcess()` immediate `updateControl()`, `afterDeactivate()`, activation guards | pump/valve pair emitted one loop iteration earlier; stop+tare at shot end a few ms earlier | BEHAVIOUR-CHANGE (benign) | certain |
| 5 | valve-before-pump in one frame; 100 % step at shot-start; 33 Hz duty; no forced gate-off | unchanged across v1.8.9/v1.9.0/master | NO-CHANGE | certain |
| 6 | unpinned PSM dependency | a clean rebuild may compile different PSM code than the local mirror (`ae3718cb`) | process risk, not a v1.9.0 change | — |

**Bottom line for the machine:** the diff does **not** contain a change that plausibly explains four TRIAC deaths that always coincide with valve+pump co-activation. The co-activation, the zero-phase discipline, the 100 % shot-start step and the absence of a gate-off safety path are **identical** in the firmware that survived months and the firmware that is running now. The version correlation is therefore more likely to be a red herring (or a proxy for something environmental — e.g. a rebuild, a different PSM resolution, or a hardware state that changed at the same time) than a code cause. If any failures pre-date 2026-10-18, the code hypothesis is dead outright; that single fact should be established first.

---

## 7. Measurements that would settle it (in order of value)

1. **Boot log `Detected ZC frequency:`** from a master build, or scope on GPIO 21 (pumpSensePin): is it ~100 (⇒ divider 2, §4 no-op confirmed) or ~50/none (⇒ §3(d) is live)?
2. **Were any of the four failures before 2026-10-18?** This alone decides whether a code hunt is worthwhile.
3. Shot log around a failure: does the logged pump duty show the 0↔100 saw-tooth of §6-2 in the seconds before the failure, or a clean 100 % hold?
4. Scope the pump current plus the valve coil during the first 200 ms after the brew command: is the TRIAC current waveform symmetric (whole-cycle) or alternating half-cycles (divider-1 signature)?
5. Confirm the PSM source that was compiled into the *flashed* v1.9.0 image is `ae3718cb` (`.pio/libdeps/controller/PSM/.git/logs/HEAD`: clone of `gaggimate/PSM.Library` at 1778322513 ≈ 2026-05-09, no later fetch recorded).

---

## 8. Findings

**P1-a — v1.9.0 regresses pump-duty windup that the fork had fixed.**
`lib/NayrodPID/src/PressureController/PressureController.cpp` v1.9.0 side of `v1.8.9_to_v1.9.0_controller.diff` (`@@ -420,16 +298,10`): `if ((sign(error) == -sign(_pumpDutyCycle)) && (fabs(_pumpDutyCycle) > 1.0f))` only corrects the integral beyond ±100 % raw duty, and the ceiling latch/override tracking were dropped. Evidence of impact is in the removed `-` lines themselves (fork comments citing "full cuts", "long cut", "shots 54/55"). Smallest fix: keep v1.9.0 upstream but restore the two-sided condition `(error > 0 && duty < 0) || (error < 0 && duty > 1)` (a three-line change, no structural revert) — or simply take master's `integrating` band version. Note this is *not* proposed as the cause of the D6 failures.

**P1-b — the pump gate cannot be forced off without a zero-cross event.**
`.pio/libdeps/controller/PSM/src/PSM.cpp:29-101` (pin written only in the ISR) + `lib/GaggiMateController/src/peripherals/DimmedPump.cpp:46` (`setPower` only calls `_psm.set()`) + `GaggiMateController.cpp` `handlePingTimeout()`/`thermalRunawayShutdown()` (`pump->setPower(0)`, `valve->set(false)`). If zero-cross pulses are lost with the last decision HIGH, the MOC3021 LED and the TRIAC gate stay driven until power is removed and the safety paths cannot clear it. Smallest fix (library is fork-owned, so patchable): add `PSM::forceOff()` (`_skip = true; digitalWrite(_controlPin, LOW);`) and call it from `DimmedPump::setPower()` when `setpoint == 0`, plus a staleness guard in the pump loop: `if (millis() - _psm.getLastMillis() > 50) { _psm.forceOff(); }` (`getLastMillis()` is already public). The residual ISR race is benign.

**P2-a — PSM dependency is unpinned.** `lib/GaggiMateController/library.json:22` → `"PSM": "https://github.com/gaggimate/PSM.Library.git"` with no `#ref`; not pinned in `platformio.ini` either. The whole gate-drive algorithm can change under a clean rebuild. Smallest fix: append a tag/commit (`#ae3718cb`).

**P2-b — `DimmedPump::stop()` / `fullPower()` are declared but never defined** (`DimmedPump.h:38-39`; no definition anywhere in `lib/`, verified). Latent link error and, worse, misleading: they look like the "force the pump off" path that finding P1-b shows does not exist.

**P2-c — `DimmedPump` does not override `Pump::getPowerTarget()`**, so `ESP_LOGI(...,"Pump Power: %.2f", dimmedPump->getPowerTarget())` in the serial `S` status always prints 0.00 (`GaggiMateController.cpp` `handleSerialCommand`, vs `DimmedPump.h:21` which only overrides `getPressureTarget/getFlowTarget`). Cosmetic/diagnostic only, but it makes the status output useless exactly when debugging a pump fault.

**P2-d — stop can be overwritten for one tick.** `onPumpControl` (comms task) and `DimmedPump::updatePower` (pump task, 30 ms) both write `_psm.set()`; a `updatePower()` that read the pre-stop `_mode` can re-arm the pump up to 30 ms after a stop. Shared by all three versions. Smallest fix: have `updatePower` read `_mode`/`_power` once into locals, and skip the `_psm.set()` if a stop generation counter changed.

---

### Verification status

*Verified from source/diff artefacts:* §1 (1)-(7); §2.1-2.5; §3(b),(c); §4; §5 items 1,2,4,5; all P1/P2 locations.
*Verified by arithmetic from unchanged code:* §2.4 duty figures (73 % → 100 %, using the shared law and default coefficients).
*Hypothesis (explicitly not verified):* the electrical coupling of §6-1 and §6-2 (no measurement or simulation was available); the actual ZC pulse rate on this specific board; whether any of the four failures pre-date 2026-10-18.