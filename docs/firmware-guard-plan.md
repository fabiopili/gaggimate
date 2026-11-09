# Guard-window firmware plan v2: pump/valve coordination and zero-cross safety

> Status: v2, revised after the critique round. The oracle's critique (physics) returned "implement with amendments"; the reviewer's critique (implementation) returned "redesign the guard mechanism" with two fatal findings against the v1 draft: the guard's own latency defeats its purpose at shot start, and the drafted `forceOff()` body fires the TRIAC on recovery. This version merges the oracle's phase-two valve hold-off into the guard, drops the library vendoring entirely, and adopts the reviewer's amendments A1 to A10. The critique artifacts are quoted in the session log; the v1 findings that shaped this revision are summarised in the risks section.

**Goal:** Protect the pump TRIAC channel (D6 and its driver optotriac U4) at brew boundaries by keeping D6 conducting across valve transitions with correct ordering, and add a firmware safety net that can de-energise the pump when zero-cross pulses are lost.

**Architecture:** Controller-side only. `DimmedPump` becomes the single writer of the pump gate and the coordinator of the brew valve GPIO: on a valve transition it forces full conduction, writes the valve GPIO once conduction is proven by observation (a fired zero-cross decision after the forced publish, with a 60 millisecond hard cap as fallback), and holds a 50 millisecond guard from the valve write before resuming the commanded power. A zero-cross staleness watchdog forces the gate low without any PSM library change. The display firmware is untouched.

**Tech stack:** PlatformIO, ESP32-S3 Arduino framework, the PSM pulse-skip library (pinned by commit, not modified), Unity native tests on the host.

---

## Evidence this builds on

The post-mortem of the 2026-11-08 board found U4, the MOC3021 optotriac driving D6's gate, leaky at 7.2 kilohm across its output while D6's own gate junction measured healthy. A leak of that size feeds the sensitive gate from line through R5 with roughly 45 milliamp peaks at mains amplitude, which triggers the TRIAC every half-cycle and runs the pump at full power from mains alone before the controller boots. That is the exact failure signature of all four deaths. The `reworked` board measured healthy at every point probed (D6, U4, R4, C12 at 9.34 nanofarad and open), so its runs-then-stops symptom remains unexplained by the pump channel. Full detail lives in `docs/triac-investigation-review.md` and the measurement checklist under `temp/triac-postmortem/`.

Two failure theories were in play, and they agree on the fact this plan exploits. Under the overvoltage theory confirmed for the newest death, U4 is exposed to the full mains exactly while D6 blocks. Under the dV/dt false-trigger theory favoured for the earlier deaths, the gate can only be falsely fired while D6 blocks. In both cases the dangerous state is a blocking TRIAC with a transient arriving, and a conducting TRIAC is the safe state: conduction collapses the gate network to a volt or two, protecting U4, and a false trigger is meaningless on a latched part. The firmware therefore does not need to know which theory is right; it needs D6 conducting across the moments when transients are generated, and it needs to get there before the valve switches.

The transients happen at shot boundaries, where the valve solenoid switches and the pump starts or stops, and every observed death coincided with that moment. Today the firmware opens the valve immediately and the pump gate takes effect at the next zero-cross decision, so at shot start the valve transient lands on a blocking channel. The v1 guard tried to fix this with an override alone, but the reviewer's interleaving analysis showed the override arrives too late: the valve command precedes the pump command in the comms frame, the pump loop tick adds up to 17 milliseconds, and at divider 2 the PSM decides only once per mains cycle, so the gate cannot conduct until up to 37 milliseconds after the valve has already switched. Version two therefore owns the ordering itself: the valve GPIO waits until conduction is established.

## Goals and non-goals

Goals: make `updatePower()` the single writer of the pump gate and route all gate changes through one resolver that honours the guard and the watchdog; sequence each valve transition as force-conduction, wait for the gate to be established, then switch the valve; detect zero-cross loss and force the gate low for as long as it lasts, with no PSM library modification; pin the PSM dependency by commit; keep the change small enough to propose upstream.

Non-goals: no display-side settings interface yet; no soft-start ramp, rejected because it increases the fraction of half-cycles D6 spends blocking when the valve transient arrives; no change to the PSM interrupt filter timing; no binary-mode stopgap; no vendoring of the PSM library, because the watchdog design below needs no library change and vendoring carried its own failure modes; no hardware work in this plan.

## Design

### Single gate writer

`DimmedPump::setPower()` currently ends with a direct `_psm.set()` (`DimmedPump.cpp:46`), reached from the pump command handler and both safety paths, while `updatePower()` writes the PSM twice more from the pump loop task. Three writers make both the guard and the watchdog unenforceable, so the direct write is deleted. `setPower()` keeps updating `_power` and `_controllerPower`, and `updatePower()` publishes the mode-appropriate value on its next tick, at most 17 milliseconds later at the 60 Hz control rate. The cost is a bounded stop latency on the safety paths, which also retires the pre-existing race where an in-flight `updatePower()` could overwrite a stop command for one tick.

All gate values now flow through one pure resolver, which is also the unit under test:

```cpp
struct PumpOutputIntent {
    bool zcStale;
    bool guardActive;
    bool binaryMode;
    int controllerPower;   // 0..100, from the pressure controller
    int commandedPower;    // 0..100, setPower() value
};

// Returns the value to hand to _psm.set(), or -1 meaning "do not touch the PSM".
int resolvePumpOutput(const PumpOutputIntent &intent);
```

Precedence: stale beats everything and returns -1 (the caller has already forced the gate low); guard beats command and returns 100; binary mode maps the command to 0 or 100; otherwise the commanded power. The function is header-only with no Arduino dependencies so the native environment compiles it directly.

### Valve transition sequencing

`DimmedPump` takes over the brew-valve GPIO write. `GaggiMateController::onRelayControl` for index 0 calls `pump->requestValve(open)` instead of `valve->set(open)`, and `DimmedPump` runs a two-stage sequence on each edge:

1. Immediately force conduction and record the publish time of the forced `set(100)`, which the next pump tick performs. The gate goes high at the first zero-cross decision after that publish.
2. Hold the valve GPIO until conduction is proven, then write the new valve state. The proof is observational, not a timer: the resolver's forced 100 percent is published on a pump tick, and `PSM::getCounter()` increments only on fired (non-skipped) decisions, so the first pump tick that sees the counter move past the value snapshotted at that publish knows a zero-cross decision fired after the forced value was published, driving the gate high and conducting a half-cycle. A hard cap of 60 milliseconds covers the fallback where the zero-cross path is dead or dying, in which case the watchdog owns the gate anyway; if the cap expires the valve write proceeds and the staleness log tells the story. The cap is 60 rather than 40 because decisions are sparser than pump ticks: worst case the forced publish waits up to 17 milliseconds for a tick, the next decision up to 20 more, and the observing tick up to 17 more. Because the proof is a counter observation rather than a timer, the mechanism is mains-frequency independent and needs no per-mains constant. The oracle's arithmetic check killed the v2 blind timer: a fixed 25 millisecond hold-off could still energise the solenoid up to 12 milliseconds before the channel conducted in the worst interleaving.
3. Arm the 50 millisecond guard window at the moment of the valve write, not at the original edge, and resume the commanded power when it expires. The anchoring matters: the plunger seats 10 to 30 milliseconds after the electrical valve-on, and with a hold-off of up to 40 milliseconds that transient can land as late as 70 milliseconds after the edge, past a guard anchored to the edge. Anchored to the write, the guard covers the seating window by construction.

The same sequence runs on both edges, so at shot end the pump is conducting while the valve closes and releases to the commanded zero afterwards. The safety paths (`handlePingTimeout`, `thermalRunawayShutdown`) also route their valve close through `requestValve(false)`, which both sequences the close correctly and fixes a v1 defect: those paths never called `setValveState()`, so the stored valve state went stale and the first shot after any safety event would have lost its guard. The edge source is `SimpleRelay::getState()`, so the guard tracks the physical valve rather than a cached flag. Mid-shot valve toggles from profile phase changes get the same 25 millisecond delay and blip; that is a visible behaviour change and is documented as intended.

### Zero-cross watchdog, no library change

The gate pin is written only inside the zero-cross interrupt, so if pulses stop while the gate is high the pump runs at full power until mains is removed and nothing in the firmware can stop it. The watchdog in `DimmedPump::loop()` compares `PSM::getLastMillis()` against `millis()`; while the stream is stale by more than `ZC_STALE_MS` (100 milliseconds, see below) it sets `_zcStale`, and on every stale tick it calls `_psm.set(0)` and drives the SSR pin low directly (`digitalWrite(_ssr_pin, LOW)`). The resolver returns -1 while stale, so no other writer can re-arm the PSM value until the stream recovers.

This design needs no `forceOff()` in the library, which matters because the v1 draft of that function was itself dangerous: setting `_a = _range` with `_value = 0` makes the first interrupt after recovery compute `_a >= _range`, clear the skip flag and drive the gate high for a full decision window. The recovery path here is safe by the library's own invariant: with `_value = 0` the accumulator can never reach `_range`, so every interrupt after recovery keeps the gate low until the flag clears and real control resumes. On the tick where staleness clears, the normal `updatePower()` write re-arms the value with correct gate timing because the ISR is running again.

The threshold is 100 milliseconds rather than the 50 the v1 draft used, because the reviewer identified a false-positive source the v1 plan missed: flash operations (NVS preference writes, OTA) block the GPIO interrupt for the duration of the erase and write, and a plain flash-resident ISR misses pulses for that time. One hundred milliseconds means ten consecutive missed pulses at the nominal 10 millisecond spacing, which rides out ordinary flash stalls while still catching a genuine zero-cross death within a tenth of a second. Clearing requires two consecutive fresh checks, so a single late pulse cannot flap the state.

At boot with no mains present the staleness flag is set from the first pump tick and the gate is held low until real pulses arrive, which is the correct failure direction and is covered by a test.

### Dependency pinning

`lib/GaggiMateController/library.json` pins the dependency to the exact upstream commit: `"PSM": "https://github.com/gaggimate/PSM.Library.git#ae3718cb"`. The upstream library is unchanged since June 2025, so this changes no behaviour; it removes the risk of a clean rebuild pulling different gate-drive code. Because the watchdog needs no library change, vendoring is dropped.

### Interactions with existing paths

The watchdog outranks the guard: during staleness the resolver returns -1 and the gate is low even inside a guard window. The ping-timeout and thermal-runaway paths now route their valve close through `requestValve(false)` and their pump zero through `setPower(0)`; the pump zero takes effect on the next tick, and their valve close is sequenced like any other edge. Binary mode is unaffected on this machine (it is only enabled with the gear-pump addon) but the resolver handles it uniformly. The 60 Hz control loop changes only how often `updatePower()` runs. Target is a master build, which brings the CPS boot logging that records the divider autodetect result. Two pre-existing hazards are noted for the maintainer rather than fixed here: the PSM constructor attaches the interrupt before `_range` is initialised, and `DimmedPump::stop()`/`fullPower()` are declared but never defined; the dead declarations are removed in the same pass as the single-writer change.

### Boundary protection only

The guard and the sequencing protect shot boundaries. They do not touch U4's idle standing exposure, which is a 400 volt part across 325 volt peaks for more than 99 percent of the machine's life, and the MOC3052 substitution on both boards remains the primary fix for the confirmed mechanism. Nobody should expect this software alone to end the deaths. A hardware note for the checklist: confirm whether the machine's heater solid state relay is zero-crossing or random-phase. It switches far more often than shots occur yet all four deaths clustered at shot start, so the evidence exonerates it as the cause, but a random-phase heater relay would be worth a hardening note to the maintainer.

## Tasks

### Task 1: Output resolver and native tests

**Files:**
- Create: `lib/GaggiMateController/src/peripherals/PumpOutput.h` (the `PumpOutputIntent` struct and `resolvePumpOutput()`)
- Create: `test/test_pump_output/test_pump_output.cpp`

Test cases, following the Unity pattern in `test/test_heater_coordinator/`: precedence stale over guard over command; guard returns 100 in both binary and proportional paths; binary maps command to 0 or 100 only; stale returns -1; command passthrough when neither flag is set; boundary at guard expiry.

### Task 2: Sequencing and watchdog in DimmedPump

**Files:**
- Modify: `lib/GaggiMateController/src/peripherals/DimmedPump.h` (guard state, pending valve write with `uint32_t` deadline in the `millis()` domain, `_zcStale`, previous valve state as `bool`, remove the dead `stop()`/`fullPower()` declarations)
- Modify: `lib/GaggiMateController/src/peripherals/DimmedPump.cpp` (delete the direct `_psm.set()` from `setPower()`; edge detection from `SimpleRelay::getState()`; guard arming; scheduled valve write in `loop()`; staleness handling in `loop()` and the resolver call in `updatePower()`)
- Modify: `lib/GaggiMateController/src/GaggiMateController.cpp` (index-0 valve writes and both safety paths call `pump->requestValve()`)

Constants: `VALVE_HOLD_OFF_CAP_MS` 80 (fallback only; the normal release is the fired-decision observation), `GUARD_MS` 50 (armed at the valve write), `ZC_STALE_MS` 100, with the reasoning of this document in the comments.

### Task 3: Dependency pin

**Files:**
- Modify: `lib/GaggiMateController/library.json` (append `#ae3718cb` to the PSM URL)

### Task 4: Verification

- [ ] `pio test -e native -f test_pump_output` green, then the full native suite
- [ ] `pio run -e controller -t clean && pio run -e controller` clean for the Pro Rev 1.1 environment
- [ ] On hardware: serial log shows the sequence on valve edges (guard armed, valve written 25 milliseconds later, guard released at 50), and the CPS line confirms divider 2 at boot
- [ ] On hardware: brew start shows valve GPIO going high only after at least one gate-high decision, confirmed on a scope or by the log timestamps
- [ ] Watchdog: pull the zero-cross sense wire **while the pump is commanded non-zero**, confirm the log shows staleness and the gate stays low, reconnect and confirm control resumes; with the pump commanded zero the same test must produce no gate activity
- [ ] Shot log check: flow settles within about a second of guard release at both boundaries
- [ ] Run one backflush (cleaning) cycle on the new firmware: the cleaning schedule drives the same index-0 valve, so its edges now blip the pump to 100 percent for 50 milliseconds; confirm the cycle completes normally
- [ ] Bench-run the `reworked` board on this firmware and watch the staleness log during several shots: a marginal zero-cross path freezing the gate is the leading remaining explanation for its runs-then-stops symptom, and this firmware turns it into a log line

### Task 5: Upstream packaging

Three commits: the single-writer change with the resolver, the sequencing and watchdog, and the dependency pin. Draft the upstream issue text for jniebuhr/gaggimate; the watchdog and the sequencing are the pieces with general value, and the resolver keeps the guard entirely inside the controller library so upstream needs no PSM library change.

## Risks and open questions

The sequencing premise is that the killing transient is generated by the valve switch event or the pump switch event at shot boundaries, and that conducting through it is protective. If the pump's own inrush is the source, the sequencing makes valve transient and pump inrush coincide; the argument for accepting that is quantitative, since the Olab draws about 150 milliamps steady against a 1 amp part with a roughly 12 amp single-cycle surge rating, and the post-mortem supports it because D6's gate junction survived the event that killed U4. If the true source were grid-side or the heater relay, the sequencing adds nothing but costs only the blips.

The reviewer's redesign findings are recorded here as the reason v2 differs from v1: the override alone could not protect the valve-edge transient because of comms frame order, pump tick latency and the per-cycle PSM decision cadence, so ordering had to become part of the mechanism; the drafted `forceOff()` was itself a gate-fire hazard; `setPower()` was a third gate writer; and the guard's edge source went stale after every safety-path valve close. Two reviewer findings are adopted as maintainer notes rather than scope: the PSM constructor's interrupt-attachment before initialisation, and the flash-write false-positive analysis that drove the 100 millisecond threshold.

Open questions remaining: does the 50 millisecond guard suit fast-seating solenoids on other machines, or should it become a setting on field evidence? The windup question has a concrete check: a low-pressure pre-infusion phase before and after the forced window, observed in the shot log; in pressure mode at 9 bar the controller rails to full power within about a tenth of a second anyway, so the window is nearly invisible there. The descale and 60 Hz hold-off questions from v2 are closed: cleaning edges are covered by the backflush verification, and the hold-off is a counter observation rather than a timer, so it needs no per-mains constant.

## Bench prerequisites for the reworked board

Before bench-running the `reworked` board on this firmware, measure its zero-cross chain with a meter, ten minutes of work that tests the maintainer's "always U10 or D2" observation directly: the bridge or opto feeding GPIO21, the series resistor, and continuity from the sense pad. Runs-then-stops is exactly what an intermittent zero-cross detector produces with the gate low, and a meter reading decides it before any firmware is involved.

## Known limitations and upstream notes

The protocol bump to 9 means the display and controller must be flashed as a pair: flashing only the controller leaves the display inhibiting control until its own update, so the release procedure is display over USB first, controller over BLE second. The display latches any non-timeout error code permanently, so a machine that recovered from a genuine zero-cross loss stays on the error screen until the display is power-cycled, even though the controller has cleared its own state and accepts commands again; the controller re-sends the latched error every five seconds so a display that reconnects still learns of it; fixing the latch needs a display-side error reset, which is out of scope here. The valve-writer hook injected at setup is load-bearing: if it were ever left unset, the sequencer would advance its state without a physical GPIO write, and nothing would complain; today setup always installs it before any command can arrive.

Two pre-existing upstream items are recorded rather than fixed here. First, handlePingTimeout() latches ERROR_CODE_TIMEOUT unconditionally, so twenty seconds of display silence after a thermal-runaway shutdown rewrites the RUNAWAY latch, and the display's reconnect ping then clears it and re-engages the heater; the naive fix (latch only from a clean state) breaks the ping-timeout storm guard, because the transition flag derives from the TIMEOUT comparison, so the repair needs a dedicated one-shot flag and belongs upstream. Second, the boiler command handler refuses all commands while faulted, the same pattern the fail-safe gating change fixed for the relay and pump handlers, and the same energising-versus-de-energising split would apply there.
