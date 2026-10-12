# ytps5

A native YouTube app for jailbroken PS5 consoles. Title ID: `PPSA99350`.

It's built on [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) and the OpenGL UI kit [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui). Both are GPL-3.0-or-later, and so is this project.

## What it does

- Browse the home feed, search (with on-screen keyboard), open channels, watch your subscriptions
- Plays video without ads. Only the plain video and audio streams are used; the ad data in YouTube's responses is ignored
- Works with the controller, including rumble feedback (can be turned off in Settings)
- Sign in with a QR code or a TV code, or just use it as a guest
- Pick which codecs to allow (AVC, VP9, AV1) and a maximum resolution (720p to 2160p, default 1080p)

Playback is software-decoded by default. A hardware decoding mode exists but is experimental and currently falls back to software every time. See [docs/HARDWARE_DECODE.md](docs/HARDWARE_DECODE.md) for details. Because of that, 1440p and 2160p may stutter.

## Requirements

- A jailbroken PS5 with an ELF loader listening on port 9021 and a homebrew environment (etaHEN or ShadowMount+)
- [ps5upload](https://github.com/) or any other way to put files on the console

The ELF loader is also needed for in-app updates.

## Install

1. Download `PPSA99350.zip` from the [latest release](https://github.com/haasele/ytps5/releases/latest).
2. Open it in ps5upload and upload it to `/data/homebrew/`. If asked for a folder name, use `PPSA99350`.
3. Start the app from your homebrew launcher.

The ZIP is flat (`eboot.bin`, `sce_sys/`, `sce_module/`, `assets/` at the top level), so the folder ps5upload creates becomes the app folder.

If your launcher scans `/user/homebrew` instead, upload there. The layout is the same.

After an update, ShadowMount+ may keep showing the old title and icon until you re-mount or restart the console.

### Updating

The app checks for a newer release on startup and can download and install it itself. Alternatively, just upload the new ZIP again.

## Controls

| Button | Action |
| --- | --- |
| D-pad / left stick | Navigate |
| X | Confirm (swappable with Circle in Settings) |
| Circle | Back / leave the player |
| L1 / R1 | Switch section; skip ±10 s in the player |
| L2 / R2 | Page through the grid |
| Left (at the left edge) | Focus the sidebar |
| Touchpad | Focus or collapse the sidebar |
| Square | Pin the sidebar open; on the login screen: new code |
| Options | Quit |

In the sidebar, Up/Down moves between Home, Subscriptions and Settings, and X opens the section.

On a channel page, Left/Right switches between the tabs the channel offers.

## Signing in

You don't have to sign in. Home, search and settings work as a guest.

Some networks (data-centre and some home IPs) make YouTube answer logged-out requests with "Sign in to confirm you're not a bot". If you see "Sign-in required" in the player, press X to sign in. The video starts on its own afterwards. This is YouTube's decision and not something the app can work around.

Your login is stored in the app's save data (`/download0/hui/auth.json`) and survives reboots and re-jailbreaks. The app also keeps a backup copy as `yt-token.json` in its app folder (for example `/data/homebrew/PPSA99350/yt-token.json`). If you ever lose the save data, put that file back and you're signed in again. "Back up account" in Settings refreshes the copy.

## Troubleshooting

**Video doesn't start or hangs on "Loading video".** The app gives up after 30 seconds with "Playback timed out". The log shows where it got stuck:

```
/download0/hui/dev/yt.log
```

While the app is running you can read it over FTP at `/mnt/sandbox/PPSA99350_000/download0/hui/dev/yt.log`. Please attach it when you open an issue.

**No device code on the login screen.** The TV client ID may be outdated. The app normally reads a current one at sign-in and only falls back to a built-in value.

**Empty boxes instead of letters.** The built-in fonts cover Latin, Greek and Cyrillic. CJK and emoji aren't supported yet.

**Empty home feed.** Without an account, YouTube often returns no recommendations. Sign in to get your feed.

## Building

You need Linux or WSL with `clang-18`, `ninja`, `make`, `python3`, `curl` and `git`.

```bash
make deps                    # toolchain, ps5-payload-sdk, ps5-opengl, PacBrew sysroot
make                         # builds dist/PPSA99350/ and dist/PPSA99350.zip
make deploy PS5_HOST=<ip>    # optional: upload to the console over FTP (port 2121)
```

`DEPLOY_DRY_RUN=1 make deploy PS5_HOST=192.0.2.1` only prints what would be uploaded.

Libcurl, OpenSSL and ffmpeg come from PacBrew packages (`PACBREW_PACKAGES`). App identity (`titleId`, `conceptId`, `contentId`, `contentVersion`) lives in `sce_sys/param.json`. The boilerplate's `APP_*` switches work as before.

### Tests

```bash
make test-unit          # host unit tests (sanitizers on)
make test-integration   # build, deploy and ZIP tooling tests
make host-snapshots     # renders every page as PNG into build/snapshots
make smoke              # live check against YouTube (needs internet)
```

`make smoke SMOKE_ARGS=login`, `clients` and `channel` run the sign-in, player-client and channel-page checks. They don't test playback, since that depends on your network (see above).

### Releasing

Set `contentVersion` in `sce_sys/param.json` (format `NN.NNN.NNN`), commit, then tag with the same version and push:

```bash
git tag 01.000.001
git push origin 01.000.001
```

CI builds the ZIP and attaches it and `SHA256SUMS` to the GitHub release. The in-app updater compares the release tag to `contentVersion`.

## Project layout

```
src/yt/            network and logic (http, innertube, auth, qr, codec selection, player)
src/app/           screens (home, search, subscriptions, settings, login, player, ...)
src/platform/ps5/  console layer (display, pad, audio, ffmpeg player, libcurl, libc shims)
host/              PC builds: snapshot runner, tests, live smoke, player stub
docs/              kit documentation and console validation notes
```

## Legal

This is an independent homebrew project and is not affiliated with Google or YouTube. "YouTube", "PlayStation" and "PS5" belong to their respective owners.

Licensed under GPL-3.0-or-later. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
