# Pushing updates over the air from the fork

How to ship new firmware to the machine without a cable, using the fork at `github.com/fabiopili/gaggimate`.

The standing arrangement since 2026-08-19 is that the machine runs upstream code but takes its updates from the fork. `fork-ota` is upstream `master` plus three small commits, one line of which sits outside `.github`: a CI guard that lets a fork build a release at all, the `RELEASE_URL` pointer, and a check that refuses to publish a release aimed at another repository. Tracking upstream this way costs almost nothing to rebase, while keeping the release channel under our control so local work can be added on top whenever it is wanted.

Remotes in this working copy, easy to get backwards: `origin` is upstream `jniebuhr/gaggimate` and the fork is the remote named `fork`. Since 2026-08-12 the repo config pins `remote.pushDefault` and the `fork-ota` upstream to `fork`, so a bare `git push` goes to the right place, but never write `git push origin` for a release.

## How the mechanism works

The display is the only board with network access and it performs all updates. On the stable channel it follows the GitHub redirect for `releases/latest` to discover the newest tag; on the nightly channel it fetches `version.txt` from the release tagged `nightly`. It downloads assets by exact filename: `display-firmware.bin` for itself and `board-firmware.bin` for the controller, which it stages to its own flash and then streams to the controller over BLE. The controller never needs a cable or a network for a normal update. OTA writes only application images; profiles and shot history on LittleFS are never touched.

Which repository the display polls is decided at compile time by `RELEASE_URL` in `src/display/plugins/WebUIPlugin.h`. There is no runtime override, which makes the setting a one way door: a display can only be moved to a different channel by flashing it over USB. This is the single most important fact on this page and it is the subject of its own section below.

## Syncing with upstream

```shell
git fetch origin
git rebase origin/master fork-ota
```

Three commits replay and conflicts are unlikely, since only `WebUIPlugin.h` is shared with upstream and only on one line. Build and test before tagging, because upstream changes arrive unreviewed:

```shell
pio run -e display -e controller -e display-headless -e display-headless-8m
pio test -e native_autotune
```

## Publishing a stable release (the normal flow)

1. Get the changes onto `fork-ota`, whether that is an upstream sync or local work merged in on top.
2. Tag a clean semver version and push branch and tag to the fork:

   ```shell
   git checkout fork-ota
   git tag -a v1.2.4 -m "..."    # leading v, strictly greater than what the machine runs
   git push fork fork-ota
   git push fork v1.2.4
   ```

3. The `Build` workflow fires on the tag push and publishes a GitHub release with all firmware images (display, headless, controller, bootloaders, partition tables and `version.txt`). Check it finished green: `gh run list --repo fabiopili/gaggimate --limit 3`.
4. On the machine: Settings, System, check for updates, then update. Updating "display" flashes the display; selecting the controller component updates the controller over BLE. Both come from the same release, so tag once and update both from the UI.

## The RELEASE_URL trap, and what it cost

A release cut from stock upstream content carries upstream's `RELEASE_URL`. Installing it therefore moves the display onto upstream's channel, and since the setting is compile time the machine cannot be moved back by anything published on the fork. This is exactly what happened on 2026-08-14: `v1.8.11` was a deliberate reset to stock content, nothing in the build objected, and the machine silently stopped following the fork. Upstream's newest tag was `v1.8.1`, below the installed version, so the machine sat on a channel with no reachable updates at all. `v1.8.13` is the recovery: it restores the fork pointer, but it can only arrive over USB, because the machine it is meant to fix is not listening to the fork.

The `Build` workflow now compares `RELEASE_URL` against the repository doing the building and fails the tag build on a mismatch, so the same mistake cannot reach a release again. The check passes when upstream builds upstream content, so it is not fork specific and would suit an upstream pull request.

The rule that follows: a stock reset must be treated as a USB operation, not an OTA one. Publishing vanilla upstream content to the fork will now be refused by CI, and rightly so.

## Rules that will bite if ignored

- The version must compare strictly greater by semver than what is installed, for the display and the controller independently. Re-publishing the same tag is a no-op; every iteration needs a new tag. The machine runs `v1.8.11` until the `v1.8.13` USB flash lands, after which the next release must be `v1.8.14` or higher.
- Keep tags clean `vX.Y.Z`. Locally built firmware carries `git describe` output, and suffixes like `-4-gabcdef-dirty` are treated as prereleases with surprising ordering. Create the tag before building anything you intend to flash, because `version.h` is generated at build time: a build made just before tagging stamps the previous tag's describe output and will look like a downgrade.
- The fork must stay public; the downloads are unauthenticated.
- The release asset names are fixed. The standard display build publishes as `display-firmware.bin`, which is what this machine (LilyGo T-RGB) needs. Never rename assets.
- Changes to `lib/NanoPbComm/proto/gaggimate.proto` change the protocol version: display and controller must then be updated together, controller first is safest, and the display refuses to drive a mismatched controller. Changes to `lib/NanoPbComm` that leave the `.proto` alone do not force lockstep, but anything touching the BLE transports still lands on both boards and is best applied to both.
- Bootloader or partition table changes cannot ship over the air; those need the USB installer.
- The display stages `board-firmware.bin` on its own LittleFS before relaying, so it needs free space there alongside profiles and history.
- If Actions misses the tag event (the v1.8.5 precedent), delete the remote tag and push it again; a plain re-push of an existing tag name is refused:

  ```shell
  git push fork :refs/tags/v1.2.4
  git push fork v1.2.4
  ```

- Never move a tag that has already published a release; cut the next number instead (the v1.8.12 precedent). Force pushing the tag does re-fire the `Build` workflow, so the absence of a run is not the failure mode to watch for. The problem is subtler: a release for that tag already exists, carrying the assets from the first build, and which images end up attached after a second run is not obvious from the outside. A fresh number costs nothing and leaves a record that can be read at a glance, which matters because the machine picks its download by following `releases/latest` and matching filenames, with no way to tell two builds of one tag apart.

- Ignore the "Node.js 20 is deprecated" annotation in build logs. GitHub stamps it on every run that uses common actions and it never fails a build; when a run fails, the cause is in a step, not in that banner.

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

Upstream's `upload-firmware` composite action posts every image to the maintainer's private update server using `UPDATE_SERVER_HOST` and `UPDATE_SERVER_API_KEY` secrets that only their repository carries, and a failed upload kills the build before the GitHub release job runs. The guard on `fork-ota` skips the upload when no server is configured, leaving the artifact archive and the release job untouched. It changes nothing outside `.github`, so the built firmware stays identical to upstream, and like the `RELEASE_URL` check it would make a reasonable upstream pull request since it is what lets any fork build a release at all.

Both guards are additive to upstream's workflows rather than edits to them, so an upstream sync that rewrites the workflow files should still replay cleanly. The older approach of marking the upload steps `continue-on-error` and disabling the nightly gh-pages publish is no longer used; the guards supersede it.

## Where the flow campaign went

The pump flow work of July and August 2026 was abandoned on 2026-08-14 and is not part of this branch. It is preserved as `archive/flow-campaign`, rebased onto upstream `70648cb1` and building green with its 20 native tests passing, and as `archive/flow-campaign-pre-rebase` for the branch exactly as it stood beforehand. The post mortem is `docs/flow-campaign-post-mortem.md` on those tags. Restoring any of it means cherry-picking onto `fork-ota` and cutting a new release, with the caveat that its two test suites depend on the campaign's own sources.
