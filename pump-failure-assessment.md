# Gaggimate Pro — Pump-Stuck-At-100% Failure Assessment

## Symptom

While pulling a regular shot, the pump went to 100% power and stayed on after interrupting the shot and closing the solenoid. After power-cycling, pump now runs continuously the moment AC mains is applied, even before the ESP32 boots. No visible burn marks. Shot log shows estimated flow dropped to zero, while measured weight-flow and pressure stayed elevated.

## Findings from the firmware

The repo contains the KiCad schematic for the **Standard** Gaggimate only (`pcb/Gaggimate.kicad_sch`, uses a relay on net `RELAY_PUMP`). The **Pro** schematic is not in the repo. However, the firmware library makes the Pro pump topology unambiguous:

- `lib/GaggiMateController/src/peripherals/DimmedPump.cpp:6`
  `PSM(_sense_pin, _ssr_pin, 100, FALLING, 2, 4)`
  Pro uses **phase-skipping modulation (burst-fire) over a TRIAC**, with zero-cross detection on `_sense_pin`.
- `lib/GaggiMateController/src/ControllerConfig.h` — for `GM_PRO_REV_1x`, `GM_PRO_REV_11`, `GM_PRO_LEGO`:
  - `pumpPin = 9` — gate-drive output to the opto-triac driver.
  - `pumpSensePin = 21` — zero-cross detector input.
  - `capabilities: dimming=true, pressure=true, ssrPump=false`.

## Inferred Pro pump-driver topology

```
ESP32 GPIO9   -> opto-triac driver IC (MOC30xx) -> gate of main TRIAC -> AC pump
ESP32 GPIO21  <- zero-cross opto (PC817 / H11AA1 / 4N25) <- mains live (FALLING edge)
```

Typical discrete components in this stage:

- **Main TRIAC** (TO-220, on heatsink or copper pour, near AC pump connector). Common candidates: BTA16-600B, BTA12-600B, Z0107, MAC97 family.
- **Opto-driver IC** (6-pin DIP near the TRIAC): random-fire MOC3023 / MOC3052 / MOC3041, or zero-cross MOC3041 / MOC3061 / MOC3063.
- **Gate resistor** (~180–470 Ω) between opto pin 4/6 and TRIAC gate.
- **Snubber** across TRIAC MT1–MT2: ~100 Ω + 100 nF X2.
- **Zero-cross opto** with two high-value series resistors (470 kΩ–1 MΩ) on the mains side feeding GPIO21.
- May also be a **series safety relay** on the pump line.

## What the symptom tells us

The pump runs at full mains before the MCU boots (GPIO9 is high-Z at that stage). That means the load is being switched on by something **downstream of GPIO9** — a hardware-only path. The estimated flow going to zero confirms firmware was commanding 0% duty; the discrepancy with measured flow proves the failure is electrical, not logical.

## Most likely failure point (ordered by probability)

1. **Main TRIAC shorted MT1 ↔ MT2.** Single most common silent failure. Caused by inductive transient when the solenoid de-energised mid-shot (back-EMF), often with no visible damage. **Primary suspect.**
2. **Opto-driver internal triac fused closed** — passes gate current continuously even with no LED drive, turning the main TRIAC on every half-cycle.
3. **Snubber capacitor shorted** — leaks current but rarely enough to run a vibratory pump at full speed. Lower probability given "100% power".
4. **Welded series relay** alone cannot cause this — the TRIAC must also be conducting.

## Diagnostic procedure (unplug from mains first)

1. Multimeter on diode / continuity across the main TRIAC's **MT1 ↔ MT2**: must read open both directions. Short = dead TRIAC.
2. Same across the opto-driver pins **4 ↔ 6**: must read open both directions. Short = dead opto.
3. Visually inspect the snubber resistor for open circuit (often blackened or cracked) — a precursor to TRIAC death. The cap may also be shorted.
4. After lifting the TRIAC, re-test the rest of the gate-drive chain in isolation with a bench supply on GPIO9 / GPIO21 to confirm only one device failed.

## Replacement strategy

Replace the **TRIAC + snubber RC + opto-driver as a set**, even if only the TRIAC reads short. The opto often has degraded internal junctions after the same transient. Cheap insurance.

Use the exact part numbers silkscreened on your board if legible. If not, a canonical safe combination is:

- TRIAC: **BTA16-600BWRG** (snubberless, 16 A, 600 V) or **BTA12-600B**.
- Opto-driver: **MOC3052** (random-fire) or **MOC3023**.
- Snubber: **100 Ω + 100 nF X2** across MT1–MT2.
- Gate resistor: **360 Ω** between opto pin 4/6 and TRIAC gate.

## Root-cause mitigation

The pump-side TRIAC is most likely a victim of **solenoid valve flyback**. Before reinstalling, verify that the solenoid has a snubber or MOV across its terminals, and add one if missing. Otherwise, the replacement TRIAC will die the same way on the next shot interrupt.

## Verified diagnosis (bench test)

DMM on the failed board with all power removed.

**Pinout on this board** (reading the PCB silkscreen, "G-A2-A1" convention; the centre lead and the tab are both annotated "A2" because they are the same net = MT2):

```
Left   = G  = Gate
Centre = A2 = MT2 (= tab)
Right  = A1 = MT1
```

This is the mirror of the Z01xx datasheet's "T1-T2-G" lead numbering — the device is laid out such that Gate sits on the left, MT1 on the right.

**Findings:**

- **Z0107MN TRIAC** (SOT-223, marking "Z7M F439", confirmed Z0107MN family): centre ↔ right pins **shorted** (= **MT1 ↔ MT2 short**). Working donor board reads open at the same point. Classic TRIAC fail-short: the two main terminals welded together. The device conducts continuously on both half-cycles regardless of gate state. The moment AC mains is applied the pump sees full line — explains the "pump on before MCU boots" symptom precisely.
- **MOC3021 opto-driver** (marking "L2410 MOC3021"): pins 4 ↔ 6 open both ways → output triac healthy. Pin 4 connects to TRIAC MT2 (centre lead / tab), pin 6 connects to TRIAC gate (left lead) via a 470 Ω gate resistor — textbook MOC3021 reference circuit.
- **Gate resistor**: 470 Ω, in spec.
- **Snubber RC**: in spec.

Failure is fully isolated to the Z0107MN. Topology and supporting components are healthy.

## Pump electrical context

This machine uses a **Silent Green K09541L vibratory pump, 35 W**. At 230 V_AC that's roughly **0.15 A RMS continuous** (vibratory pumps run at ~50% duty internally), well within the Z0107MN's 1 A rating. Even peak inrush of 2–4 A is within the TRIAC's surge envelope.

Conclusion: **steady-state pump current was not the killer.** The TRIAC died from electrical stress unrelated to the pump's average draw — most likely repeated inductive transients from the **3-way solenoid valve** when it de-energises mid-shot, possibly compounded by the underrated 400 V MOC3021 letting line spikes through.

Silent Green pumps include some internal mechanical damping that softens their own back-EMF compared to an Ulka, which makes the solenoid valve an even more likely transient source by elimination.

## Note on the MOC3021 rating (calibrated)

MOC3021 is rated 400 V V_DRM. For 230 V European mains (peaks ±325 V) that gives ~75 V headroom — legal but tight. The MOC's pins 4-6 sit across MT2-to-gate, which is essentially line voltage when the main TRIAC is off, so V_DRM is what matters here (the opto carries no load current, only the brief gate trigger pulse).

**Empirically, in this failure the MOC3021 survived intact** — it tested healthy and the killing transient went through the load path (MT1-MT2), not the gate-drive path. The board developer confirmed this is the intended design and the part choice has held up in the field.

Upgrading to **MOC3052 (600 V V_DRM, drop-in)** is **optional headroom**, not a fix. Worth doing only if you're ordering parts anyway, since it costs the same. The two changes that actually address the failure mechanism are the TRIAC replacement and the solenoid-coil MOV.

## Conformal coating

The failed board has **no conformal coating**. The donor board is heavily coated with what behaves like epoxy.

Coating absence did **not** cause this failure (coating doesn't affect device voltage ratings, surge handling, or transient absorption). However, applying coating *after* the rework is good practice for long-term reliability in a kitchen environment — protects against steam, condensation, and coffee dust bridging traces.

If applying, use **acrylic** (e.g. MG Chemicals 419D, Electrolube AFA, CRC Acrylic Conformal Coating). Avoid epoxy, which makes future rework impossible. Mask connectors, BLE antenna area, autodetect divider pads, pressure-sensor I2C connector, USB port, and firmware-upload test points before spraying. Two thin coats, 24 h cure before re-energising.

## Shopping list (final)

Order qty 2 of each for spares:

| Part | Spec | Notes |
|------|------|-------|
| **Z0109MN** | 1.5 A, 600 V, SOT-223, sensitive gate | Drop-in upgrade for the failed Z0107MN; more inrush headroom |
| **MOC3052** *(optional)* | 600 V, random-phase opto-triac driver, 6-pin DIP | Drop-in headroom upgrade for MOC3021; not required to fix the failure |
| **100 Ω 0.5 W resistor** | through-hole or 1206 SMD | Snubber R (only if existing one looks degraded) |
| **10 nF X2 0.63 kV capacitor** | film, X2 safety class | Snubber C (only if existing one looks degraded) |
| **MOV S10K275 (EPCOS/TDK) ×3** | 275 V_AC, 10 mm disc, ~28 J | One **across solenoid valve coil** (essential, root-cause fix). One **across pump** (optional belt-and-suspenders, catches line surges and SSR switching transients). One spare. |

Recommended sources: LCSC, Mouser, Digi-Key, Farnell.

Equivalents for the MOV: Littelfuse V275LA20A, Bourns MOV-10D431K. Don't drop below 275 V_RMS rating (will conduct on normal mains spikes and degrade) and don't go above ~320 V_RMS (clamp point too close to TRIAC's 600 V V_DRM to be useful).

## MOV wiring and mounting

Two MOVs are recommended: one across the solenoid valve coil (essential, addresses the root cause) and one across the pump (optional belt-and-suspenders, catches external surges and SSR-side transients).

Both go in parallel with the load, polarity-independent (AC parts are bidirectional).

```
Mains live → controller switch ─┬── load terminal A
                                │
                              [ MOV ]
                                │
                       Neutral ─┴── load terminal B
```

Mount priority (closest to the load = best clamping, smallest radiated transient):

1. **Soldered or crimped directly on the coil/pump spade terminals** — best. Use a piggyback connector or short crimp lugs.
2. On the wires entering the load, inside the boiler / electrical enclosure — fine.
3. At the controller board's output terminal — least effective but still works.

Insulate the MOV legs with heat-shrink. MOVs can fail short under a huge surge, so keep ≥3 mm clearance to grounded chassis and steam-path metalwork.

### Why the solenoid MOV is essential and the pump MOV is optional

- **Solenoid valve** is switched on/off discretely (relay-driven), so its de-energisation produces a large di/dt transient on the mains line. This is the dominant transient source and the most likely cause of the original TRIAC failure. Fitting a MOV here is the actual fix.
- **Pump** is TRIAC-driven with PSM phase-skipping, which switches at zero-current crossings — commutation transients are inherently gentle. Plus the existing RC snubber across MT1-MT2 already addresses commutation kick. A pump-side MOV adds protection against external line surges and any other appliance-driven transients (heater SSR, neighbouring devices, lightning), but it is not the primary fix.

### MOV and the existing RC snubber

The pump-side MOV does **not** conflict with the existing RC snubber across the TRIAC's MT1-MT2:

- RC snubber limits dV/dt (rate of voltage rise) at the device terminals.
- MOV clamps absolute voltage above its threshold at the load.
- Different mechanisms, different positions, fully complementary.

### Optional alternative: RC snubber instead of MOV

If you'd rather use an RC snubber across the solenoid coil (more permanent, no wear-out failure mode):

- 100 Ω 0.5 W resistor + 100 nF X2 0.63 kV cap in **series**, the pair across the coil.
- Slightly gentler clamp; never end-of-lifes.

**Use one or the other on each load, not both** — they slightly fight on dV/dt response when stacked on the same coil. MOV is the textbook solution for 230 V solenoid coils; RC snubber is the conservative alternative.

## Why salvaging from the donor board was abandoned

Donor board's conformal coating behaves like **epoxy** — rock-hard, doesn't scratch off with a multimeter probe tip, doesn't soften with isopropanol. Mechanical removal would risk damaging surrounding SMD parts and the SOT-223 lead frame itself. New parts cost trivially and ship in a few days; not worth the risk.

## Rework procedure when parts arrive

1. Desolder and replace **Z0107MN → Z0109MN**. Hot air at 350–380 °C, plenty of flux, lift the tab last.
2. Desolder and replace **MOC3021 → MOC3052**. Same pinout, no layout changes.
3. Inspect snubber RC under magnification; replace if any sign of stress.
4. **Fit the MOV (S10K275) across the solenoid valve coil** in the espresso machine — this is the actual root-cause fix. Without it, the new TRIAC will eventually die the same way. Optionally fit a second S10K275 across the pump for added protection against external surges.
5. Bench-test on USB only first (confirm boot logs match the donor board).
6. Mains-on idle test for 10 minutes (no shot, watch for unintended pump activity, touch TRIAC tab — should stay cool).
7. Pull a manual flush, then a real shot, watch closely.
8. (Optional) Apply acrylic conformal coating after a few successful shots.

## Iron settings and process

  - Iron temp: 320–340 °C for both desolder and resolder. Don't go above 380 °C — flux burns off too fast and the SOT-223 tab pad can lift.
  - Use extra liquid flux (no-clean rosin pen or paste) on every joint before touching it. Flux is more important than solder volume for clean rework.
  - For SOT-223 desoldering: tin all 3 leads with your leaded solder, bridge them, then rock that side free. The tab is the hard part — push extra solder through it for thermal coupling and use the widest tip you have.
  - For DIP-6 (MOC3052) desoldering: leaded solder + braid on each pin, or hot air the whole package off.
  - For new component placement: tack one corner pin first, check alignment, then solder the rest with normal joints.

## Recommended next step

Post a clear photo of the Pro board's pump-output area on the GaggiMate Discord (linked in `README.md`). The maintainer can confirm whether the original BOM specifies the same parts or whether a board revision changed any of the values.
