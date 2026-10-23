# Hardware video decode (Videodec2) — status and path

This page records what is **proven** on the PS5, what is **built** in this app
and what is still missing for the full integration. It is deliberately
conservative: nothing counts as possible that has not run on a console.

## What is proven

The PS5 decodes video not on the shader cores but in the fixed-function block
**Videodec2**. Console-validated measurements are in the research project
[ps5-hardware-video-decoding-research](https://github.com/blackbearreloaded/ps5-hardware-video-decoding-research)
(H.264 High, HEVC Main/Main10, VP9 Profile 0/2; 1080p–4K; zero-copy of the
decoder surface into AGC; VP9 4K60 stable at 59.95 fps). **AV1 is not
established there** — there is no proof for AV1 so far, so it stays on
software (ffmpeg) with the resolution cap.

For this app the target order is therefore clear: **VP9 first**, then
AVC/HEVC; AV1 plays no role in the hardware path.

## What is built in this app

The argument layouts are no longer a reconstruction. The session in
`src/platform/ps5/videodec.cpp` follows the sequence publicly verified on
hardware and names its own sources:

- **EVO-PLAYER-PS5** (`sainsaji`),
  `projects/evoplayer/media/include/sce/sce_videodec2.h` — struct layouts,
  codec/level constants and the memory typing.
- **`docs/evo-pro/videodec2-abi.md`** — the exact bring-up order and the
  protection table per region (hardware-verified on PS5 12.70 on 2026-09-01:
  1080p H.264 IDR up to an NV12 frame, every call `rc=0`).
- Both in turn build on **blackbearreloaded/ProsperoLight**, the Moonlight
  client that decodes on the VCN block.

What is built from that:

- **Import chain unchanged:** `tooling/stubs/libSceVideodec2.c` remains the
  stub with all 26 publicly known entry points; `make` links it in via
  `APP_IMPORT_STUBS`, all 26 NIDs are in `dist/PPSA99350/eboot.bin`.
- **Kernel-mapped regions (type 12):** the CPU workspace lies in **named
  flexible memory** (protection `0x03`), all remaining regions (compute, gpu,
  cpu_gpu, frame pool) are mapped with
  `sceKernelAllocateDirectMemory(…, type 12, alignment 0x4000)` +
  `sceKernelMapDirectMemory` — each with its own protection
  (`0x33` CPU r/w + GPU, `0x32` GPU).
- **Compute queue first:** `QueryComputeMemoryInfo` → map the size →
  `AllocateComputeQueue` with `resource_type = Compute`; the handle then goes
  into the decoder configuration.
- **Explicit profile/level per codec:** AVC `profile_idc 100` (High) and VP9
  profile 0, level x10 per resolution (AVC 51/52, VP9 41/51), geometry rounded
  to 16 (1080p as 1920×1088), `pipeline_depth = 1`, `cpu_affinity 0x3F`,
  `cpu_priority 700`.
- **`Reset` after `CreateDecoder`:** the session reports itself ready only
  after that.
- **Validated `Decode`:** a picture counts as a success only at
  `out.valid && !out.error && picture_count > 0`, and the returned surface must
  be an exact member of the caller's own frame pool; if the first AU at depth 1
  yields none, `Flush` follows.
- **Every step logs:** every phase writes exactly one line with name, return
  value and import count; the **first failure names the step** in the error
  message, and the session never calls a null import.
- **Startup probe stays:** `videodec_probe.cpp` writes
  `[YTV] videodec2 imports: <n>/26 reachable` at startup — the first answer to
  "is libSceVideodec2 loadable for a homebrew title?".
- **`videodec_supports()` = AVC + VP9:** only for these two is there a
  console-proven path; AV1 stays without proof on software. (HEVC has a proven
  path, but `yt::Codec` does not carry HEVC.)

**Presentation** remains unproven: the session describes a decoded picture but
does not bind it itself. Whether a decoder surface can be imported as a GL
texture (Mesa interop, `mesa_glinterop.h`, use unconfirmed) is still open —
the player treats exactly this missing import as a hardware failure and falls
back to software.

## What a console run shows

With `Decoding: Auto` or `Hardware` and an AVC/VP9 stream the run writes the
following into `yt.log` in order:

1. **Startup:** `[YTV] videodec2 imports: <n>/26 reachable`. `26/26` means:
   the library is loadable for homebrew and the NID chain is correct; `0/26`
   means: no symbol bound, and the session then refuses every step *by name*
   instead of calling a null pointer.
2. **Compute queue:** the line `[YTV] videodec2 compute queue: <ptr> (<bytes>
   bytes)` — before it `QueryComputeMemoryInfo`, mapping the compute region
   and `AllocateComputeQueue` (`resource_type = Compute`).
3. **The four direct-memory regions with `type=12`:** `region compute: …
   type=12 prot=0x33`, `region gpu: … type=12 prot=0x32`, `region cpu_gpu: …
   type=12 prot=0x33`, `region frame: … type=12 prot=0x32` (plus `region cpu:
   … prot=0x03` — the CPU workspace in flexible memory).
4. **`CreateDecoder` / `Reset`:** `[YTV] videodec2 sceVideodec2CreateDecoder:
   0x00000000 …` and `… sceVideodec2Reset: 0x00000000 …`; every step logs its
   return value, a negative one names the step in the error message.
5. **Either a decoded picture or the first failed step:** `Decode` counts as
   success only at `valid && !error && picture_count > 0`. In this build the
   still-missing picture import (decoder surface → GL texture) stops every
   hardware video; the log then shows `ffmpeg: hardware decode unavailable:
   hardware picture import unavailable, using software`, and playback runs in
   software. A hardware picture only counts as "green" once that line is
   absent.

## Why software decode is the default

The default is the new setting **`Decoding: Software`** (a byte in the save
format in `src/core/settings.cpp`; `Auto` and `Hardware` are opt-in and read
along). Reasons:

- The argument layouts are still being validated on the console: the session
  follows the published, hardware-verified sequence, but whether *this app*
  gets the regions with the firmware's rights and sizes is only decided by a
  console run. Until then the default stays the path that carries everywhere
  today.
- The picture import (see above) is not proven yet, so the hardware path puts
  no picture on the screen today; the automatic fallback level is software.
- AV1 has no hardware path: software is mandatory there anyway, and in
  `Hardware` mode AV1 is no longer selected at all.
- The software path (ffmpeg → GL texture) carries AVC/VP9 at 1080p today.
  Every failed hardware step is logged, and `open()` never fails on the
  hardware path — software takes over.

## Expected gain (from the research measurements)

| Codec | Resolution | Software (today) | Hardware (proven) |
| --- | --- | --- | --- |
| H.264 | 1080p60 | holds | 2.27 ms decode, 60.00 fps |
| VP9 | 1080p60 | holds | 5.53 ms decode, 173 fps batch |
| VP9 | 4K60 | borderline | 59.95 fps stable, 16.7 ms AU→flip |
| HEVC | 4K60 | borderline | 60.36 fps, 5.46 ms decode |
| AV1 | any | software | no proof |

## Experimental read-back path (Decoding: Hardware)

Since 01.000.160 the player can show what the decoder produces without a GL
import: with **Player > Decoding > Hardware** and an 8-bit VP9 stream
(profile 0), packets go through ffmpeg's `vp9_superframe_split` filter to the
Videodec2 session, the frame pool is mapped CPU-readable (`0x33`, as
`evo_vdec_native.c` maps every region), and each picture is read straight from
the slot as NV12 (luma rows derived from the slot size) and converted and
scaled by the same threaded swscale step as a software picture. AVC (picture
reordering at depth one is unproven), AV1 and 10-bit/HDR VP9 stay on software.

Any decode failure before the first picture, or three in a row after it,
switches the stream back to software at the current position. `yt.log` names
each step: `hardware decode ON/not used/OFF`, the first picture's geometry
(`pitch`, `format`, `size`, `luma rows`), and every 5 s the pictures decoded
and the average read-back + convert time. If the picture comes out scrambled
the surface is tiled, and the zero-copy path is the way forward.

## Console results (1.0.161)

- Videodec2 loads and decodes 4K VP9 (itag 313 at 30 fps, 315 at 60 fps):
  read back plus the downscale to 1080p takes about 2 ms a picture.
- The frame drops come from the texture upload, not the decoder: one
  1920x1080 RGBA `glTexImage2D` into a new texture costs a steady 32 ms on
  ps5-opengl, for software and hardware pictures alike.
- 1.0.162 measures four upload methods on the first video
  (`gfx::VideoUploader`, logged as `upload:` lines) and keeps the fastest;
  Developer > Picture upload forces one.
