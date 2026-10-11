# youtube-tv — native YouTube app for PS5 homebrew

A native YouTube application for jailbroken PlayStation 5 consoles
(title ID **PPSA99350**), built on the build and packaging system of
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)
with the OpenGL interface kit of
[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui).

The app builds as a ZIP, uploads via **ps5upload** and lands in the console's
homebrew directory (`/data/homebrew/PPSA99350`); the homebrew launcher
(etaHEN/ShadowMount+ environment) starts it like any other homebrew title.

## Features

- **Native controller support** with haptic feedback: rumble on confirm
  (`0.35`, 60 ms), a quiet impulse on focus change, a tick while dragging the
  scrubber. Can be switched off in the settings ("Haptics"); the light bar
  follows the accent colour (YouTube red).
- **Ad-free**: playback uses only the signed `googlevideo.com` streams from
  `streamingData.adaptiveFormats`. The InnerTube response is read only for
  `playabilityStatus`, `videoDetails` and `streamingData`; `adPlacements`,
  `adSlots`, `playerAds` and `adBreakHeartbeatParams` are never evaluated —
  there is no pre-roll or mid-roll advertising in the player.
- **Familiar YouTube interface**: sidebar on the left with icons —
  **Left** at the left edge of the content hands focus to the bar,
  **Up/Down** moves the cursor (Home → Subscriptions → Settings),
  **X** switches the section, **Right/Circle** hands focus back to the page,
  **Touchpad** focuses the bar directly, **Square** pins it open. Plus a home
  page with video grid, search with on-screen keyboard and a **channel row**
  (tapping a channel opens its uploads, Circle goes back), subscriptions,
  player with transport bar, quality/codec label and "Up next" row, settings.
  The interface is uniformly English (no multilingual build) and uses
  **Google Sans Flex** (400/500/700, OFL) plus DejaVu Sans Mono for numbers
  and the sign-in code. L1/R1 still switches the sections directly.
- **Sign in with QR code or TV code**: OAuth 2.0 device flow of the YouTube TV
  client. The login screen shows the QR code on the left and the code to type,
  with the verification URL the service returns (shown as `youtube.com/activate`,
  falling back to `google.com/device`), on the right. The refresh token lives in
  `/download0/hui/auth.json`.
- **Codecs individually switchable**: AVC (H.264), VP9 and AV1 in the
  settings; at least one stays active. At equal height AVC wins over VP9 over
  AV1, with codec ceilings **AVC 1080p, VP9 720p, AV1 480p**; the "Maximum
  resolution" setting (720p/1080p/1440p/2160p, default 1080p) caps everything
  below it; formats above a codec ceiling only ever play as a fallback (the
  full pick order is under "Client choice and error messages"). Audio: Opus
  before AAC.
- **Decoding mode**: the "Decoding" setting offers **Software** (default),
  **Auto** and **Hardware**. Auto/Hardware attempt the console decoder
  (Videodec2) for AVC/VP9 and fall back to software automatically at every
  failed step — even the picture import into a GL texture is not proven yet,
  so today every hardware playback visibly falls back to software in the log.
  AV1 stays software and is never selected in Hardware mode.
- **Playback pacing**: video follows the audio clock (frames that are too late
  are dropped), and the picture buffer is capped at 192 MB.
- **Software decoder**: PacBrew-ffmpeg (7.0.1) demuxes and decodes both
  tracks; audio runs through the kit mixer, video as an OpenGL texture.

## Installing on the console

Requires a jailbroken PS5 with a running ELF loader (port 9021) and a
homebrew environment (elfldr/kstuff/etaHEN, ShadowMount+), as described by
ps5upload.

1. Open `PPSA99350.zip` from the `dist/` folder (or from a GitHub release) in
   ps5upload.
2. Choose **`/data/homebrew/`** as the target; if the tool asks for a folder
   name, enter **`PPSA99350`**. ps5upload unpacks the archive into that
   folder, so the app lives under `/data/homebrew/PPSA99350/`. The ZIP is
   **flat**: `eboot.bin`, `sce_sys/`, `sce_module/` and `assets/` sit directly
   at the top level of the archive (no enclosing `PPSA99350/` folder), so the
   folder ps5upload creates is itself the app folder.
3. Start the app in the homebrew launcher. If the launcher scans
   `/user/homebrew` instead, upload the ZIP there — the layout is identical.
4. After an update via ps5upload (or `make deploy`), ShadowMount+ shows title
   and icon updated only after a re-mount or a console restart — it stages
   `param.json` and the start images once.

## Releases and auto-update

The only source is this repository at `https://github.com/haasele/ytps5`: the
git remote (`origin`) points there, there is no second upstream. The fork
builds on the kit
[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) and
its boilerplate
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate),
both GPL-3.0-or-later — the same licence as this project.

**Cutting a release:** the version lives in `sce_sys/param.json` in the
`contentVersion` field (`NN.NNN.NNN`, e.g. `01.000.001`) and is also the git
tag. So set `contentVersion`, commit, `git tag 01.000.001` and
`git push origin 01.000.001`. CI (`.github/workflows/tooling.yml`) builds on
ubuntu-24.04 with the pinned clang-18/ninja toolchain, checks the flat
`dist/PPSA99350.zip` and creates or updates the GitHub release for the tag
with exactly this ZIP and `SHA256SUMS`. Tag = version, asset =
`PPSA99350.zip` — an update needs nothing more.

**In the app:** at startup the app queries the repo's `releases/latest`
endpoint and compares the tag with its own `contentVersion`. If the release is
newer, it downloads `PPSA99350.zip` over the **ELF loader on
`127.0.0.1:9021`** and hands the archive to the bundled `self-updater.elf`;
that checks size and SHA-256, unpacks alongside and swaps the files as soon as
the app closes (the title then starts in the new version). A running ELF
loader (elfldr/etaHEN, port 9021) is required — without it the version check
happens, but downloading and staging report an error.

## Building it yourself

On Linux/WSL with `clang-18`, `ninja`, `make`, `python3`, `curl`, `git`; in
the repo this is enough:

```bash
make deps              # toolchain parts, ps5-payload-sdk, ps5-opengl, PacBrew sysroot
make                   # dist/PPSA99350/ and dist/PPSA99350.zip
make deploy PS5_HOST=<ip>   # optional: straight to the console over FTP (port 2121)
DEPLOY_DRY_RUN=1 make deploy PS5_HOST=192.0.2.1   # only shows the target
```

`make` uses PacBrew modules for libcurl, OpenSSL and ffmpeg
(`PACBREW_PACKAGES` in `.env` or on the command line). The boilerplate's
`APP_*` switches apply unchanged; the app's identity lives entirely in
`sce_sys/param.json` (`titleId`, `conceptId`, `contentId`, `contentVersion`).

Working on a machine without system clang/EGL/curl dev packages: the host
builds (tests, snapshots, smoke) pull headers and libraries from
`.hostdeps/` (curl, GL, EGL, a local EGL vendor JSON); cross-compilation uses
the `PATH` set in `.env` (`.toolchain/bin` with clang-18, lld, ninja,
clang-format).

## Verification without a console

```bash
make test-unit          # 211 host tests (kit + yt layer, sanitizers on)
make test-integration   # tool tests (build, deploy, ZIP mode)
make host-snapshots     # renders every app page as PNG to build/snapshots
make smoke              # LIVE: device code + real search against YouTube (needs internet)
make smoke SMOKE_ARGS=login   # LIVE: sign-in flow including cancel, without a phone
```

`make smoke` is the most honest test: it gets a real device code, runs a real
search query and lists the found videos with their thumbnail URLs. It does not
check playback, because YouTube answers "Sign in to confirm you're not a bot"
on some networks (data-centre IPs) even when signed out — the console signs in
beforehand, so it does not matter there.

`make host-snapshots` renders the app's six pages with sample data (no
network, no decoder) and writes them to `build/snapshots/`; on the console the
built-in tour does the same (see `docs/CONSOLE_VALIDATION.md`).

## Controls

| Key | Effect |
| --- | --- |
| D-pad / left stick | Navigate |
| X | Confirm (Circle when confirm is swapped in the settings) |
| Circle | Back, leave the player |
| L1 / R1 | Switch section (Home, Search, Subscriptions, Settings); ±10 s in the player |
| L2 / R2 | Page through the grid |
| Left | Focus the sidebar (at the left edge of the content) |
| Up/Down (bar) | Select a section (Home → Subscriptions → Settings) |
| X (bar) | Open the section |
| Touchpad | Focus/collapse the sidebar |
| Square | Pin the sidebar open · Login: new code |
| Options | Quit the app |

## Account, guest mode and hardware decode

**Guest access is possible and the default.** The app starts without sign-in:
home, search and settings work, "Subscriptions" asks for the account, the
login screen can be skipped with Circle. Only *playback* requires sign-in on
some networks: YouTube then answers clients that are not signed in with "Sign
in to confirm you're not a bot" — no client (iOS, Android, TV, Web) gets past
it, that is a network/IP decision, not an app limit. The player shows
"Sign-in required" in that case and **X** goes straight to sign-in; afterwards
the video starts by itself.

**Sign-ins persist.** The refresh token lives in `/download0/hui/auth.json` —
that is the title's save area on the console, it survives reboots and every
re-jailbreak (only deleting the title data or the console removes it).
Additionally:

- At startup the app reads `yt-token.json` from the app folder
  (`/app0/yt-token.json`, i.e. e.g. `/data/homebrew/PPSA99350/yt-token.json`)
  if nothing is in the sandbox storage — after a wipe simply put the file back
  (ps5upload/FTP) and the app is signed in.
- On sign-in it writes this copy back if the folder is writable, and reports
  by toast where it lies.
- "Back up account" in the settings writes the copy again at any time.
- Every start refreshes the token quietly, so Google does not let it expire as
  unused.

**Hardware decode:** the PS5 has a hardwired video decoder (Videodec2) that
verifiably decodes AVC/HEVC/VP9 — VP9 even at 4K60. This app links the 26
known entry points as imports at build time (`tooling/stubs/`, 26/26 NIDs
verified in the package), logs at startup how many of them the console binds,
and runs the decoder as a real session (kernel-mapped regions, compute queue,
explicit profile/level, `Reset`, validated `Decode`); the **Decoding** setting
enables it for AVC/VP9, with automatic fallback to software. AV1 has no proof
there and stays software. The complete status, the public references, the
console test and the still-open picture import are in
[docs/HARDWARE_DECODE.md](docs/HARDWARE_DECODE.md).

## Client choice and error messages

YouTube practically accepts signed-in requests only from the TV client. The
app switches automatically: signed out it queries as the web client (sign-out
content), signed in as the TV client, and on a refusal (HTTP 400/401/403/410
or an empty page) falls back to the web client exactly once. Errors name the
client that caused them (`http 400 (TVHTML5)`), and every request appears with
client, status and result count in `yt.log`.

For playback the `/player` chain now runs **VISIONOS → TVHTML5 (tv_samsung,
5.20260707) → TVHTML5 (7.20260707.07.00, Cobalt-UA) → WEB**: YouTube answers
the `tv` client SABR-only now (`streamingData.serverAbrStreamingUrl` without
format URLs, yt-dlp issue #12482), which is why VISIONOS comes first and WEB
last. Only the two TV clients get the OAuth bearer; the `signatureTimestamp`
comes from the player JS. The two URL kinds are scored apart: a plain-URL
format is picked whenever one fits the caps and the codecs' ceilings. When
none does, the best `signatureCipher` format takes over (its over-ceiling
fallback included), because its descrambled URL can be refused with 403 — a
refused ciphered URL still beats no stream, and the service retries past it.
The lowest over-ceiling plain format is the very last resort. If a client's
response is SABR-only or a chosen
format cannot be opened, the app tries the next format of the same response
and then the next client; the unmodified response of the first client lands at
`/download0/hui/yt-player-last.json` on complete failure, and `yt.log` keeps
the format count per client. `make smoke SMOKE_ARGS=clients` shows the result
of every attempt.

## Channel page

Channel cards in the search results open the channel page: avatar, name,
subscription row and the **tabs the channel itself reports** (Home, Videos,
Live, Playlists, Posts …). **Left/Right** switches the tab (L1/R1 stay on the
main sections), X opens a video, Circle goes back to the result list. If there
is no Live tab, the channel is not streaming right now — the tab list comes
from the response, not from a fixed order. `make smoke SMOKE_ARGS=channel`
checks header, tabs and tab switching live.

## Sign-in

The sign-in flow runs as a device flow (QR or TV code). Two things that were
broken in earlier builds are now pinned:

- The flow stays pending: `make smoke SMOKE_ARGS=login` gets a real device
  code, checks that the state stays pending, and that cancelling is answered
  within 200 ms. The test runs without a phone.
- After successful sign-in the app returns **to where the sign-in was
  started** (player → video starts by itself; subscriptions → feed loads) and
  drops a copy of the token (see "Sign-ins persist").

## When playback stalls

The app writes every stage of a video request into its own log file:

- Path on the console: `/download0/hui/dev/yt.log`
  (readable over FTP while the app runs:
  `/mnt/sandbox/PPSA99350_000/download0/hui/dev/yt.log`).
- Lines like `video: request id=…`, `video: player info ok …`,
  `video: selection itag=… VP9 (plain) ceiling=…`, `video: selected itag=…`,
  `video: open begin`, `video: try … itag=… -> …`
  show exactly where it stalls; an open failure names the format and its URL
  kind (`itag=137 1080p AVC (ciphered)`).
- Decode and presentation leave their own traces: `ffmpeg: video resolution
  WxH -> WxH`, `ffmpeg: video buffer acquired/released index=N size=S pool=P
  free=F`, `ffmpeg: seek S.SSs cleared N queued pictures`,
  `ffmpeg: texture upload WxHx4` (first, then once a second) and
  `ffmpeg: audio ring write=N read=N buffered=N`. They name the stage a stalled
  run reached; a line carrying `(defect)` marks a fault the player refuses to
  work around.
- The player also names the running stage on screen
  ("Loading video – Streams/Decoder"), and a request that for 30 s neither
  arrives nor fails is ended by the watchdog ("Playback timed out") instead of
  loading forever.

## Limits and notes

- **Resolution**: software decoder; default limit 1080p. 1440p/2160p can be
  selected but may stutter — that is expected behaviour, not a malfunction
  (hardware decode is prepared, see above).
- **Sign-in**: see above — guest mode without an account, sign-in only for
  playback on "flagged" networks, token survives re-jailbreaks. If no device
  code appears, the TV client's client ID is stale; the app reads it from the
  TV bundle at sign-in, the checked-in value is only a fallback.
- **Fonts**: titles outside Latin/Greek/Cyrillic (e.g. CJK or emoji) are drawn
  as empty boxes; the baked fonts cover the European set.
- **Recommendations**: for signed-in accounts YouTube delivers the home feed;
  without sign-in it stays empty on many networks (the app says so).
- **Legal**: this is an independent homebrew project with no connection to
  Google/YouTube; "YouTube", "PlayStation" and "PS5" belong to their
  respective owners. Licence: GPL-3.0-or-later like the kit
  (see `LICENSE`, `THIRD_PARTY_NOTICES.md`).

## Repository layout

```
src/yt/            network and logic layer, platform-independent and host-tested:
                   http, innertube (TVHTML5/WEB, ad blocking), auth (device flow),
                   qr, codec (stream selection), image, service (worker thread), player
src/app/           screens: app (tabs/tour/chrome), home, search,
                   subscriptions, settings, login, player, feed_view, thumbs
src/platform/ps5/  console layer: display, pad, AudioOut, player (ffmpeg),
                   libcurl transport, libc shims
host/              PC builds: snapshot runner, tests, live smoke, HTTP transport,
                   player stub
```

The kit documentation under `docs/` (kit reference, craft, components, themes,
performance, console validation) still applies unchanged.
