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

The `~/.platformio` filesystem grant is already in the promoted `claude-code-local` profile, but the first build attempt then failed at library installation because the sandbox proxy blocks the PlatformIO registry (`HTTPClientError` on NimBLE-Arduino; all `*.platformio.org` hosts and `dl.espressif.com` returned no route while github.com passed). An updated profile draft adding those two domains is waiting:

```shell
nono profile promote claude-code-local
nono run --profile claude-code-local -- claude
```

Verify before anything else: `pio --version` prints cleanly, and `curl -sI https://api.registry.platformio.org/ -o /dev/null -w '%{http_code}'` returns an HTTP status rather than 000. `/dev` is already granted read and write, so serial flashing needs nothing extra.

Second attempt, 2026-08-02: the registry grant worked and dependency installation completed, but the build then failed one step further on. The nanopb generator needs the Python `protobuf` and `grpcio-tools` packages and pip could not reach pypi.org (`403 Forbidden: host pypi.org:443 is not in the allowlist`), leaving `nanopb_generator.py` with `ModuleNotFoundError: No module named 'google'`. No local workaround exists: no Python on the machine carries `protobuf`, the pip cache has no copy, no generated `.pb.c` or `.pb.h` is tracked in the repo, and the `generator/proto/google/protobuf` directory inside the Nanopb library holds only `.proto` definitions. The profile gained `pypi.org`, `*.pypi.org`, `files.pythonhosted.org` and `*.pythonhosted.org`, which fixed the download but exposed a second, filesystem problem: Homebrew's PlatformIO site-packages is read only, so pip fell back to a user install under `~/Library/Python/3.14`, which the sandbox does not grant, and the packages never landed. The fix needs no further sandbox change. Install them into the already writable PlatformIO tree and point Python at it:

```shell
PIOPY=/opt/homebrew/Cellar/platformio/6.1.19_2/libexec/bin/python
$PIOPY -m pip install --target ~/.platformio/pylibs protobuf 'grpcio-tools>=1.43.0'
```

Every subsequent build then needs `PYTHONPATH=~/.platformio/pylibs` in its environment. This survives across sessions because `~/.platformio` persists, so only the `PYTHONPATH` prefix is required from now on. Note also that `nono why --host pypi.org` wrongly reported ALLOWED while the live proxy refused the connection, so trust a real `curl` over the static check.

## Step 1: build

```shell
cd ~/Projetos/gaggimate
git status                 # expect branch fork-ota, clean apart from untracked session notes
PYTHONPATH=~/.platformio/pylibs pio run -e display
```

Done on 2026-08-02: `[SUCCESS]` in 58 seconds, RAM 28.5 percent (93240 of 327680 bytes), Flash 66.5 percent (4358685 of 6553600 bytes), zero compiler warnings, and none of the touched files raised a diagnostic. The build reports version `v1.8.1-152-g672d7170-dirty`, the `dirty` suffix coming from this uncommitted handover file.

The espressif32 platform is already cached in `~/.platformio` and project libraries are now installed under `.pio/libdeps/display`, so the build resumes at the nanopb generation step. Success looks like RAM and Flash usage lines and `[SUCCESS]`. If the compiler complains about anything in `FlowTrimmer.h`, `Controller.cpp`, `Settings.cpp`, `ShotHistoryPlugin.cpp` or `WebUIPlugin.cpp`, those are the files this work touched; anything else is likely environmental.

## Step 2: flash the display

Only after a clean build, have the display board connected over USB, then:

```shell
ls /dev/cu.usbmodem*       # expect /dev/cu.usbmodem1101 (see monitor_serial.md)
pio run -e display -t upload --upload-port /dev/cu.usbmodem1101 \
                   -t monitor --monitor-port /dev/cu.usbmodem1101
```

Done on 2026-08-02. The board enumerated as `/dev/cu.usbmodem2101` rather than `1101`, so check the port each time instead of trusting the number. Upload wrote 4359312 bytes in 38.6 seconds at an effective 902.7 kbit/s, the data hash verified, and the board hard reset into the new firmware. Note that `pio device monitor` cannot run from a non-interactive shell, since miniterm calls `termios.tcgetattr` on stdin and dies with `Operation not supported by device`. Read the port with pyserial directly instead.

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
5. Settings, System: switch the OTA channel from nightly to stable. The display is currently on the nightly channel, but the fork publishes stable tags only, so a nightly-channel check would query a `nightly` release that does not exist on the fork. On stable, the update check follows the fork's `releases/latest`; no update will be offered until the first tag is pushed, so "no update found" is the expected result.

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
