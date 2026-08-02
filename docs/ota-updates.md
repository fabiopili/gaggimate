# Pushing updates over the air from the fork

How to ship new firmware to the machine without a cable, using the fork at `github.com/fabiopili/gaggimate`. This works once the display is running a build from the `fork-ota` branch, whose `RELEASE_URL` (`src/display/plugins/WebUIPlugin.h`) points at the fork. Until that first USB flash, the machine still looks at the upstream repository.

## How the mechanism works

The display is the only board with network access and it performs all updates. On the stable channel it follows the GitHub redirect for `releases/latest` to discover the newest tag; on the nightly channel it fetches `version.txt` from the release tagged `nightly`. It downloads assets by exact filename: `display-firmware.bin` for itself and `board-firmware.bin` for the controller, which it stages to its own flash and then streams to the controller over BLE. The controller never needs a cable or a network for a normal update. OTA writes only application images; profiles and shot history on LittleFS are never touched.

## Publishing a stable release (the normal flow)

1. Get the changes onto `fork-ota`. Day to day work happens on feature branches or `fix/flow-detachment`; merge or rebase into `fork-ota` so the fork specific commit (release URL and CI adjustments) stays on top.
2. Tag a clean semver version and push branch and tag to the fork:

   ```shell
   git checkout fork-ota
   git tag v1.2.4        # leading v, strictly greater than what the machine runs
   git push fork fork-ota
   git push fork v1.2.4
   ```

3. The `Build` workflow fires on the tag push and publishes a GitHub release with all firmware images (display, headless, controller, bootloaders, partition tables and `version.txt`). Check it finished green: `gh run list --repo fabiopili/gaggimate --limit 3`.
4. On the machine: Settings, System, check for updates, then update. Updating "display" flashes the display; selecting the controller component updates the controller over BLE. Both come from the same release, so tag once and update both from the UI.

## Rules that will bite if ignored

- The version must compare strictly greater by semver than what is installed, for the display and the controller independently. Re-publishing the same tag is a no-op; every iteration needs a new tag.
- The first fork release must be `v1.8.2` or higher. The fork already carries the upstream tags through `v1.8.1`, so that number is taken, and the machine currently runs `v1.8.1-152-g672d7170-dirty`, which semver treats as a prerelease of 1.8.1 and therefore lower than plain `v1.8.1`. Publishing a release from the inherited `v1.8.1` tag would offer the machine a downgrade to upstream code that drops the flow detachment fix.
- Keep tags clean `vX.Y.Z`. Locally built firmware carries `git describe` output, and suffixes like `-4-gabcdef-dirty` are treated as prereleases with surprising ordering.
- The fork must stay public; the downloads are unauthenticated.
- The release asset names are fixed. The standard display build publishes as `display-firmware.bin`, which is what this machine (LilyGo T-RGB) needs. Never rename assets.
- Changes to `lib/NanoPbComm/proto/gaggimate.proto` change the protocol version: display and controller must then be updated together, controller first is safest, and the display refuses to drive a mismatched controller.
- Bootloader or partition table changes cannot ship over the air; those need the USB installer.
- The display stages `board-firmware.bin` on its own LittleFS before relaying, so it needs free space there alongside profiles and history.

## Nightly channel (optional)

The nightly workflow fires on pushes to the fork's `master` and maintains a moving `nightly` prerelease. To use it, make `master` on the fork mirror `fork-ota`:

```shell
git push fork fork-ota:master
```

Then select the nightly channel in the UI. Note nightly builds compile with `NIGHTLY_BUILD`, which enables the flow estimation volumetric fallback. Keeping `master` tracking upstream and using only tagged stable releases is the simpler arrangement.

## Direct BLE route from the laptop (no GitHub)

`scripts/ota/ota_updater.py <ble-address> <firmware.bin>` speaks the controller's OTA service directly and skips all version checks. It only works while the display is powered off, because a connected display stops the controller advertising. Useful for controller recovery or when iterating on controller firmware without cutting releases.

## USB fallback

```shell
# Display (never add -t uploadfs: it would erase profiles and shot history)
pio run -e display -t upload --upload-port /dev/cu.usbmodem1101

# Controller
pio run -e controller -t upload --upload-port /dev/cu.usbmodem1101
```

Both boards enumerate with the same USB VID and PID, so pass the port explicitly whenever both are connected. The port number depends on which physical socket is used, so read it from `pio device list` each time rather than trusting a recorded value: the 2026-08-02 display flash came up as `/dev/cu.usbmodem2101`, not the `1101` noted previously.

Note this USB path preserves data exactly like OTA does. It writes only the application image, which is why the 2026-08-02 flash kept all eleven recorded shots and every setting. What erases LittleFS is `-t uploadfs` or the web installer, not USB flashing as such.

## CI notes specific to the fork

On `fork-ota` the six steps that upload firmware to the upstream maintainer's private update server run with `continue-on-error`, since the fork does not have those secrets; the version step tolerates the fork missing the `nightly` and `db` tags; and the nightly gh-pages publish is disabled. If upstream changes its workflows, re-apply these three adjustments when syncing.
