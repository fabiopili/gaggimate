# GaggiMate update process and flow-detachment fix: handoff

Written 2026-07-30 against commit `7e9fddea` on `master`. Covers three things: how to get locally
built code onto the machine over USB, how to point the machine's over-the-air updater at your own
GitHub fork, and what implementing the fix described in `debug/report.md` actually requires in
terms of which firmware you have to touch.

All file references are `path:line` at the commit above. Verify them before relying on them if the
tree has moved on.

---

## 0. Orientation: two firmwares, one machine

The machine runs two independent ESP32-S3s with separate firmware images, and almost every question
about deployment reduces to working out which of the two a change lands in.

The **controller** is the PCB inside the machine. Its firmware is built from `src/controller/` plus
`lib/GaggiMateController/`, and it owns the hardware: heater, pump, valve, pressure sensor, safety
cut-outs. Critically for section 3, it also owns the entire pump control loop, because
`PressureController` in `lib/NayrodPID/` is linked only by `DimmedPump`
(`lib/GaggiMateController/src/peripherals/DimmedPump.cpp`) and the display never compiles it.

The **display** is the LilyGo T-RGB or headless unit. Its firmware is built from `src/display/` and
embeds the Preact web UI from `web/`. It owns Wi-Fi, the web interface, profiles, shot history, the
Bluetooth scale, and the brew process state machine. It is also the only one of the two that talks
to the internet, so it is the only one that performs over-the-air updates.

The two are joined by a NimBLE link carrying nanopb messages defined in
`lib/NanoPbComm/proto/gaggimate.proto`. The display sends commands (`PumpControl`, `BoilerControl`,
`PumpSettings`, and so on) and the controller sends back telemetry (`SensorData`, `SystemInfo`).
When the display performs an OTA it downloads both images and relays the controller's image over
that same BLE link, so the controller never needs a network path or a cable of its own during a
normal update.

---

## 1. Local update over USB

There is no way to push a git branch to the board. Git never reaches the device. You check out the
branch locally, PlatformIO compiles it, and esptool writes the resulting binary over USB serial.
One command does all three.

### Prerequisites

PlatformIO CLI must be on your path, and Node 22 is needed only if you intend to rebuild the web UI.
The first build of either environment will download the ESP32 toolchain and take several minutes.
As of this writing nothing has been compiled in this checkout: `.pio/build/controller/` contains only
`idedata.json` from an IDE indexing run, and `src/display/webassets/` is empty.

### Controller

```shell
git checkout <your-branch>
pio run -e controller -t upload -t monitor
```

The serial port has previously been `/dev/cu.usbmodem1101` on this machine, recorded in
`monitor_serial.md`. Pass it explicitly when both boards are plugged in at once, because both
enumerate with the same USB VID and PID (`0x303A:0x1001`) and PlatformIO's autodetection simply
takes the first match:

```shell
pio run -e controller -t upload --upload-port /dev/cu.usbmodem1101 \
                      -t monitor --monitor-port /dev/cu.usbmodem1101
```

Drop `-t monitor` if you do not want the console attached afterwards. If the board does not
auto-reset into download mode, hold BOOT while connecting USB, or hold BOOT and tap RESET, then
re-run.

### Display

Decide first which environment matches your hardware. `display` is the LilyGo T-RGB with the LVGL
UI, `display-headless` is the no-screen build, and `display-headless-8m` is the headless build on a
Seeed XIAO ESP32-S3. This document assumes `display`.

```shell
scripts/build_webui.sh          # only when web/ has changed
pio run -e display -t upload
```

Two things to know. The web UI no longer ships in the filesystem image: it is gzipped, packed into a
blob by `scripts/embed_webui.py` and embedded in the firmware app image. The pre-build hook
`scripts/embed_webui_pre.py` will silently substitute an empty stub if `src/display/webassets/` has
no manifest, so a bare `pio run -e display` produces a working machine with a blank web interface.
Run `scripts/build_webui.sh` first unless you genuinely do not need the web UI on that flash.

Second, and more important: **do not run `-t uploadfs` on the display.** LittleFS now holds only your
profiles (`/p`) and shot history (`/h`), so writing a fresh filesystem image would destroy both. A
plain firmware upload leaves them untouched. The filesystem image is only for fresh installs on a
blank board.

### Which one do I flash?

Changes under `src/controller/`, `lib/GaggiMateController/` or `lib/NayrodPID/` go to the controller.
Changes under `src/display/` or `web/` go to the display. Changes to
`lib/NanoPbComm/proto/gaggimate.proto` go to both, and both must be flashed together, because the
protocol carries a version check that will refuse a mismatched pair.

### Avoiding USB access to the controller

The controller does not normally need a cable, because the display can flash it over BLE. The
controller always exposes the fbiego OTA service `fe590001-54ae-4a28-9f74-dfccb248601d` alongside the
normal protocol service, through `BLE_OTA_DFU _otaDfu` in
`lib/NanoPbComm/src/ble/BleServerTransport.h:47`, started at `BleServerTransport.cpp:21-22`.

The normal route is the OTA flow described in section 2. Selecting a component in Settings → System
reaches `src/display/plugins/WebUIPlugin.cpp:136`, which calls
`ota->update(updateComponent != "display", updateComponent != "controller")`, so you can update the
controller alone. `ControllerOTA::update()` downloads `board-firmware.bin` to the display's LittleFS
and then streams it to the controller over that BLE service. Two conditions apply. The release
version must be strictly greater than the version the controller reports, because
`GitHubOTA::update()` gates on `update_required()`, so re-publishing the same tag is a no-op and each
iteration needs a version bump. And the display stages the image to its own LittleFS as
`/board-firmware.bin` first, so it needs free space there alongside profiles and shot history.

There is also a direct route from the laptop, `scripts/ota/ota_updater.py <ble-address> <firmware.bin>`,
which speaks the same OTA service and skips version checks entirely. It only works while the display
is not connected, because `BleServerTransport::onConnect` calls `server->stopAdvertising()`
(`BleServerTransport.cpp:54`), leaving the controller undiscoverable for as long as the display holds
the link. Powering the display down frees it.

Three cases still require physical access. Bootloader or partition-table changes, since OTA writes
only the app image while `board-bootloader.bin` and `board-partitions.bin` are used by the USB
installer alone. Reading the controller's serial log, which telemetry to the display does not
substitute for, as panic traces and `ESP_LOG` output never leave the wire. And recovery from a
controller build that fails to bring up BLE, which is the real risk: there is no second radio to fall
back on, so a bad flash is only recoverable with a cable. Given that, it is worth confirming how much
disassembly reaching the controller's USB port takes before relying on never needing it, and worth
flashing controller changes with the cable attached the first time.

---

## 2. Remote updates from your own GitHub fork

The point of this is convenience: once the display is running firmware that looks at your fork, you
can iterate by pushing a tag and updating from Settings → System, without opening the machine or
finding a cable.

### The one-line change

`src/display/plugins/WebUIPlugin.h:20`

```cpp
const String RELEASE_URL = "https://github.com/jniebuhr/gaggimate/releases/";
```

Point that at your fork, keeping the trailing slash. The code appends either `latest` or `tag/nightly`
depending on the channel selected in the UI, at `src/display/plugins/WebUIPlugin.cpp:65` (construction)
and `:554` (channel change).

There is a bootstrapping order here: the firmware currently installed is what decides where to look,
so the switch only takes effect after you flash the modified display build once over USB. From then
on it self-updates from your fork.

### What the updater expects of a release

The logic is in `lib/OTA/src/GitHubOTA.cpp` and `lib/OTA/src/common.cpp`, and it is picky in ways
that are easy to trip over.

On the **stable** channel it issues a no-follow GET against `<url>latest`, reads the `Location`
header, and rewrites `tag` to `download` to form the asset base URL. GitHub emits that redirect
automatically for whichever release is marked latest. The tag must be semver with a leading `v`, or
`checkForUpdates()` logs "not a valid version URL" and gives up.

On the **nightly** channel there is no redirect, because `<url>tag/nightly` resolves directly. The
code falls back to fetching `version.txt` from the release's download URL, which is why both
workflows write `out/version.txt` from `git describe`.

Assets are fetched by exact filename. The display downloads `display-firmware.bin` and
`board-firmware.bin`, hardcoded at `src/display/plugins/WebUIPlugin.cpp:74`. `display-filesystem.bin`
is still passed to the constructor but is no longer downloaded, since the web UI moved into the app
image and OTA deliberately never touches the LittleFS partition. Note that the firmware name is not
conditional on the build variant, so if you run a headless unit you must publish your headless build
under the name `display-firmware.bin`.

The version must compare greater than what is installed, by semver. Your local `BUILD_GIT_VERSION`
comes from `git describe --tags --dirty` in `scripts/auto_firmware_version.py`, so a dirty tree
yields something like `v1.2.3-4-gabcdef-dirty`, which semver treats as a prerelease and orders in
ways you will not expect. Keep clean `vX.Y.Z` tags on the fork.

The fork must stay public, since there is no authentication on the download. TLS is fine as long as
you stay on github.com, because the ESP32 CA bundle is already configured.

### CI changes needed in the fork

The workflows already do the whole job. `.github/workflows/build.yml` fires on any tag push and
publishes a release from `out/*.bin`. `.github/workflows/build-nightly.yml` fires on pushes to master
and maintains the `nightly` prerelease.

The one thing that will break is the `./.github/actions/upload-firmware` steps, which POST the
binaries to the maintainer's private update server using `UPDATE_SERVER_HOST` and
`UPDATE_SERVER_API_KEY` secrets you will not have. The composite action runs under `set -euo pipefail`
with `curl --fail-with-body`, so with empty secrets the step fails, which fails the `build` job, which
means the `release` job never runs because it declares `needs: build`. Delete those steps in your
fork or add `continue-on-error: true` to each. The nightly workflow additionally publishes to
`gh-pages`, which is harmless but probably unwanted.

---

## 3. The flow-detachment fix: what it requires

The analysis itself is in `debug/report.md` and is not repeated here. In one sentence: in
flow-target phases the pump runs entirely open-loop from a static model, the flow figure the firmware
reports is that same model re-applied to the duty it just produced, so the reported flow is
algebraically forced onto the setpoint and a large real error renders as a flat line exactly on
target.

### The architectural facts that decide the split

Three observations determine which firmware each fix lands in.

The pump control loop is controller-only. `PressureController` is linked exclusively by `DimmedPump`,
and the inversion at `lib/NayrodPID/src/PressureController/PressureController.cpp:110` together with
its mirror image at `:162` is where the tautology lives.

The scale is display-only, and there is no channel to send its readings to the controller. The
display-to-controller half of the `Payload` oneof is Ping, BoilerControl, PumpControl, RelayControl,
PidSettings, PumpSettings, AutotuneRequest, PressureScale, Tare and LedControl. `VolumetricMeasurement`
travels the other way. Adding measured flow as controller input therefore means a proto change and a
synchronised flash of both boards.

But that is avoidable, because the display already owns both ends of what a trim needs. It sends the
flow setpoint itself every cycle at `src/display/core/Controller.cpp:938`, where
`pump.flow = brewProcess->getPumpFlow()` feeds a `PumpControl` message. And it already computes a
measured flow from the scale in `BrewProcess::volumetricRateCalculator`
(`src/display/core/predictive.h:7`), fed by `updateVolume()` at the 100 ms `PROGRESS_INTERVAL`.

So the cascade trim recommended in the report can be computed on the display and applied to the flow
target before it goes on the wire. The controller keeps its feedforward model unchanged and never
knows the difference. No proto change, no controller reflash.

### Which firmware each fix needs

| Fix (report item) | Firmware | Notes |
|---|---|---|
| Stop reporting setpoint as measurement (R1) | Display and web | `vf` already logged beside `fl` |
| Scale trim as outer loop (R2) | Display only | via the existing `PumpControl.flow` |
| Instrument cross-check (R3) | Display to flag, controller to clamp | do it display-side |
| Pressure limit of zero (R4) | Display and web | includes web-editor input validation, see below |
| Ungate `slip` for vibration pumps (R5) | Both | no proto change needed |
| Log commanded duty in shot history (R6) | Display only | `pump_power` already arrives in telemetry |
| Dead `pf` and `ev` (report, Secondary findings) | Controller only | `PressureController.cpp` state machine |
| Final weight after tare (report, Secondary findings) | Display only | `ShotHistoryPlugin.cpp:251` |

The first item is nearly free. `sample.fl` (modelled) and `sample.vf` (scale-derived) are already
written side by side at `src/display/plugins/ShotHistoryPlugin.cpp:157` and `:160`, so making the
divergence visible is largely a charting and labelling job in `web/`. That also gives you the
instrument for validating everything else, including the report's own testable prediction, which can
be checked against existing shot history before any control code is written.

R6 is nearly as cheap and pairs naturally with it. The controller already reports its commanded
duty every telemetry tick (`pump_power` in `lib/NanoPbComm/proto/gaggimate.proto:193`); the shot
log simply never records it. Adding a duty field to the slog is display-only plus a format-version
bump, and it is the measurement that separates duty nonlinearity from pressure-curve error in
future shots, which the existing two-shot data cannot fully do.

R4 gained a web dimension from the cross-review: the profile editor parses the maximum-pressure
field with `parseFloat` (`web/src/pages/ProfileEdit/ExtendedPhase.jsx:310`), so a cleared input
becomes NaN, serialises to null and reloads as 0, the documented "Ignore" sentinel. That is the
most plausible mechanism for how this profile silently lost its 9 bar ceiling. Whatever policy is
chosen for `pressure: 0`, the editor should refuse or confirm an empty field rather than storing
the sentinel by accident.

The slip ungating is the only item that genuinely requires both boards.
`Controller::setPumpModelCoeffs()` at `src/display/core/Controller.cpp:782` zeroes the slip array
unless addon 7 is present (`:788` to `:791`), and `lib/GaggiMateController/src/GaggiMateController.cpp:195`
only calls `setPumpSlipPolyCoeffs` when `gearpumpAddon != nullptr`. Both gates must go. No proto work
is needed, since `slipA` through `slipD` already exist in `PumpSettings`. But the report is candid
that those parameters are not identifiable without a new calibration that measures delivered flow at
several fixed duties rather than at two fixed pressures, so this is the most expensive path with the
weakest immediate payoff.

### Two traps in the display-side trim

`updateControl()` transmits a `PumpControl` only when `pump != lastPump`
(`src/display/core/Controller.cpp:963`), and `PumpCommand::operator==` at
`lib/NanoPbComm/src/GaggiMateComm.h:38` is exact float comparison. A continuously varying trim will
therefore emit a frame every 100 ms and turn a deliberately delta-gated BLE link into a steady
stream. Quantise the trimmed target, to something like 0.05 ml/s, or rate-limit the trim to a couple
of hertz.

Also, `targetFlow` and `pump.flow` are currently assigned from the same expression at
`src/display/core/Controller.cpp:938` and `:940`, and `targetFlow` is what is logged as `tf`. Keep
`targetFlow` on the profile's requested value and let only `pump.flow` carry the trim, otherwise the
log loses the ability to show what the trim did, which is the same class of failure as the bug being
fixed.

### Recommended sequence

Make the divergence visible first, since it is display-and-web only, carries no risk to the machine,
and validates the diagnosis against shots you already have. Add the trim second, behind a setting
that defaults to off. Take the pressure-limit-zero handling and the final-weight-after-tare bug third,
as they are small and independent. Only then decide whether the controller-side work, meaning the
slip ungating and the dead `virtualScale` puck state machine, is worth the additional calibration
effort it implies.

The practical consequence for deployment is that the first three stages are display-only, and the
display is the board that performs OTA. Flash it once over USB with the new `RELEASE_URL` and you can
iterate over the air from then on. The cable is only needed again if you reach the controller-side
items, and even then a fork release covers it, since the display relays `board-firmware.bin` over BLE.

---

## 4. Open decisions

Which display environment matches the hardware, `display`, `display-headless` or
`display-headless-8m`, needs confirming before the first flash. The fork URL needs choosing before the
`RELEASE_URL` change can be made. It is also worth deciding early whether this work is intended to go
back upstream as a pull request, because if so the `RELEASE_URL` change and the CI edits must stay on
a separate local branch and never appear in the PR.

One caveat carried over from the report: its conclusions rest on two shots and treat the Bluetooth
scale as ground truth. Before building a control loop on top of that assumption, the cheap
observability change in stage one should be used to confirm the predicted behaviour across a wider
sample.

---

## 5. Command reference

```shell
# Build both firmwares without flashing
pio run -e display -e controller

# Rebuild and embed the web UI (needed before flashing the display if web/ changed)
scripts/build_webui.sh

# Flash the controller and watch the console
pio run -e controller -t upload -t monitor --upload-port /dev/cu.usbmodem1101

# Flash the display (never add -t uploadfs: it erases profiles and shot history)
pio run -e display -t upload

# Desktop simulator, controller mocked, SDL window as the panel (needs brew install sdl2)
pio run -e display-sim -t run

# Formatting and static analysis before committing
scripts/format.sh
pio check -e display
pio check -e controller
```
