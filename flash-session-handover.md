# Flash session handover

Written 2026-08-01 to kickstart a fresh session whose only job is to build, flash and verify the display firmware carrying the flow detachment fix. Everything else is done: code implemented, reviewed, committed and pushed. Background reading if needed: `docs/flow-detachment-fix.md` (what changed and why), `docs/ota-updates.md` (how updates ship afterwards), `debug/report.md` (the original investigation).

## State at the end of the previous session

- Repo `~/Projetos/gaggimate`, checked out on branch `fork-ota`. This is the branch to flash: it is `fix/flow-detachment` (three commits: `56f29db2` shot log v6 and honest charting, `ae564254` scale based flow trim, `bec56814` profile editor guard) plus `38d458cb`, which points `RELEASE_URL` at `github.com/fabiopili/gaggimate` and adapts CI for the fork.
- Both branches are pushed to the fork (`fork` remote). Upstream remains `origin`.
- The web UI is already built and embedded: `src/display/webassets/` contains `web_ui.bin` (65 assets) plus manifest, generated from the final reviewed code. No need to run `scripts/build_webui.sh` again unless `web/` changes.
- The display firmware has NEVER been compiled: every attempt hit the nono sandbox (no `~/.platformio` access). This is the first task below. The code passed adversarial review including a standalone C++17 compile of the new `FlowTrimmer.h`, so surprises should be limited, but do not skip the build check.
- Hardware: LilyGo T-RGB H597 2.1 inch full circle, so the PlatformIO environment is `display`. The machine's controller board needs NO flashing this round: all changes are display firmware plus web assets and the BLE protocol is untouched.
- Scale Flow Compensation (the new trim) ships disabled by default. Leave it off for the first shots.

## Step 0: sandbox

The session must run with `~/.platformio` writable, which repeated ad hoc `--allow` flags failed to deliver. A profile draft with the grant is waiting:

```shell
nono profile promote claude-code-local
nono run --profile claude-code-local -- claude
```

Verify before anything else: `pio --version` must print the version without a `FileExistsError` traceback, and `ls ~/.platformio` must not say operation not permitted. `/dev` is already granted read and write by the profile's system group, so serial flashing needs nothing extra.

## Step 1: build

```shell
cd ~/Projetos/gaggimate
git status                 # expect branch fork-ota, clean apart from untracked session notes
pio run -e display
```

First build downloads the ESP32 toolchain; expect several minutes. Success looks like RAM and Flash usage lines and `[SUCCESS]`. If the linker complains about anything in `FlowTrimmer.h`, `Controller.cpp`, `Settings.cpp`, `ShotHistoryPlugin.cpp` or `WebUIPlugin.cpp`, those are the files this work touched; anything else is likely environmental.

## Step 2: flash the display

Only after a clean build, have the display board connected over USB, then:

```shell
ls /dev/cu.usbmodem*       # expect /dev/cu.usbmodem1101 (see monitor_serial.md)
pio run -e display -t upload --upload-port /dev/cu.usbmodem1101 \
                   -t monitor --monitor-port /dev/cu.usbmodem1101
```

Hard rules:

- NEVER run `-t uploadfs`. The filesystem image would wipe the profiles in `/p` and the entire shot history in `/h`.
- Both ESP32 boards enumerate with identical USB VID and PID (`0x303A:0x1001`). If both happen to be connected, pass the port explicitly, never rely on autodetection.
- If the board does not enter download mode, hold BOOT while plugging USB, or hold BOOT and tap RESET, then retry.
- Do not flash while the machine is mid brew.

## Step 3: verify

On the serial monitor: normal boot, no crash loop, Wi-Fi joins, controller connects over BLE.

In the web UI:

1. Settings, Machine tab shows the new "Scale Flow Compensation" toggle, off. Leave it off.
2. Profiles and shot history are intact (the flash must not have touched LittleFS).
3. Pull a shot (a flush profile works for a smoke test, though it records no history; a real short shot is better). The new shot's chart must show the "Pump Duty" series and the flow series labelled "Pump Flow (modelled)". A v6 file confirms the format end to end.
4. Old shots must still open and chart correctly (v5 files, 26 byte samples).
5. Settings, System: the update check should now query the fabiopili fork. No update will be offered, since no release tag exists yet; "no update found" against the fork URL is the expected result.

With a Bluetooth scale connected, a real shot should log `vf` (Weight Flow) alongside the modelled flow; the dashboard average flow for that shot then comes from the scale.

## Step 4: afterwards

- Keep the USB cable until at least one clean shot has been recorded and read back.
- Validation before enabling the trim: per `debug/report.md`, flow phase shots below about 3 bar should track target within about 10 percent; the detachment signature is duty climbing with pressure while modelled flow stays pinned and weight flow diverges. Confirm the picture on a few shots first.
- First trim trial, when ready: enable the toggle, use a moderate flow profile with a sensible pressure limit, watch weight flow versus target converge over roughly 10 seconds. The trim is clamped to 25 to 150 percent of the request and can be disabled mid session from Settings.
- Future updates go over the air: tag `vX.Y.Z` on `fork-ota`, push branch and tag to `fork`, wait for CI, update from the machine. Full detail in `docs/ota-updates.md`.

## Rollback

The flashed build reports a `git describe` based version. To return to stock, build and flash the upstream release tag over USB (`git checkout <upstream-tag> && scripts/build_webui.sh && pio run -e display -t upload ...`), or use the upstream web installer for a full clean install (that path rewrites the filesystem, so export any shot history first). OTA rollback from upstream is not possible once the fork URL is active, because upstream versions will not compare greater than the fork build's version string.

## Open items carried forward

- Controller side work is deliberately deferred: pump slip ungating needs multi duty calibration data, and the dead puck flow and estimated weight channels live in `lib/NayrodPID` (see `docs/flow-detachment-fix.md`, Deferred work).
- Decide eventually whether `fix/flow-detachment` goes upstream as a pull request; it is kept free of fork specific commits for that purpose.
- The firmware verify task (#7 in the session task list) closes when Step 1 succeeds.
