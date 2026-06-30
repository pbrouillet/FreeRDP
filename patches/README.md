# copilot-agent patches

This directory holds the out-of-tree changes carried on the
`feature/pbrouillet/copilot-agent` branch so they can be reapplied after a
rebase onto a fresh upstream `master`.

## Files

- **`copilot-agent.patch`** — the canonical, **single combined patch** with the
  clipboard / uwac / tooling branch changes (all such commits since the base
  below, but **not** the AAD token-cache change). Apply with `git apply`.
- **`aad-token-cache.patch`** — standalone patch for the **AVD/RDS gateway
  refresh-token caching** feature (touches only `client/common/client.c`). Kept
  separate from the clipboard work; it is independent and can be applied on its
  own or alongside `copilot-agent.patch` (they modify disjoint files).
- **`ffmpeg-audio-log.patch`** — standalone patch that **silences the FFmpeg AAC
  encoder log spam** seen during Teams/headset audio (`Qavg`, `N frames left in
  the queue on closing`, `Input contains (near) NaN/+-Inf`). Touches only
  `libfreerdp/codec/dsp_ffmpeg.c`; independent of the other patches (disjoint
  files). See the section below for details.
- `0001-*.patch` … `0004-*.patch` + `apply.sh` — an older, **partial**
  `git format-patch` / `git am` series covering only the tooling and
  uwac/Wayland-decoration commits. Superseded by `copilot-agent.patch`.

## Base commit

The combined patch is generated against upstream:

```
7a7eea091a5597720e5e1a0a025c9674f0c61098
  Merge pull request #12983 from akallabeth/pdu-tracker-leak
```

## Reapplying after a rebase

```sh
# from the repository root, on a tree at (or rebased onto) the base commit
git checkout 7a7eea091a5597720e5e1a0a025c9674f0c61098
git apply patches/copilot-agent.patch      # clipboard / uwac / tooling
git apply patches/aad-token-cache.patch    # AAD gateway token caching
git apply patches/ffmpeg-audio-log.patch   # FFmpeg audio log spam fix
# review, build, then commit as desired
```

Use `git apply --check patches/<file>.patch` first to confirm each applies
cleanly. If upstream has moved and a hunk no longer applies, use
`git apply --3way patches/<file>.patch` to fall back to a 3-way merge.

## Required build flags (image clipboard)

The guest⇆host **image** clipboard fixes rely on winpr's image
synthesizers, which are **OFF by default**. Without them a host screenshot
published only as `image/png` cannot be decoded/synthesized to `CF_DIB`, and
guest→host image paste silently fails. Configure the build with:

```sh
cmake -GNinja \
  -DWINPR_UTILS_IMAGE_PNG=ON \
  -DWINPR_UTILS_IMAGE_WEBP=ON \
  -DWINPR_UTILS_IMAGE_JPEG=ON \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING_INTERNAL=ON \
  -DWITH_CLIENT_SDL3=ON \
  -B build -S .
```

`libpng-dev`, `libwebp-dev` and `libjpeg-dev` must be installed.

## AVD/RDS gateway token caching (`aad-token-cache.patch`)

Connecting through the AVD/RDS gateway (e.g. a Windows 365 Cloud PC) normally
prompts for interactive browser OAuth **twice per connect** — once for the AVD
broker token and once for the RDS device PoP token. This patch adds an **opt-in**
local cache of the OAuth **refresh token** so subsequent connects are silent.

- Requires `WITH_AAD` (ON by default for the SDL/X11 clients).
- **Enable at runtime** with the environment variable:

  ```sh
  export FREERDP_AAD_TOKEN_CACHE=1
  ```

  When unset, behaviour is unchanged (no disk access, original scopes).
- Cache file: `$XDG_CACHE_HOME/freerdp/aad-token-cache.json` (falls back to
  `~/.cache/freerdp/...`), created with `0600` perms inside a `0700` directory.
  Delete it to force a fresh interactive login.
- Why refresh tokens (not access tokens): the RDS token is a Proof-of-Possession
  token bound to a freshly generated key (`req_cnf`) each connection, so it
  cannot be reused. The refresh token is redeemed silently with
  `grant_type=refresh_token`, re-supplying the new `req_cnf`.

> Security note: the refresh token is a long-lived credential stored in
> plaintext (protected only by `0600` file perms). The feature is therefore
> opt-in. Remove the cache file or unset the env var to disable.

## What the patch contains (high level)

- **Clipboard reliability (SDL3 + Wayland)**: text and image paste in both
  directions.
  - host→guest: read the host clipboard under whichever mime is actually
    offered (text and image), instead of hardcoded `text/plain;charset=utf-8`
    / `image/bmp`.
  - guest→host: request an image format the guest actually announced; prefer
    the guest's native compressed format (e.g. `PNG`) over the
    server-synthesized `CF_DIB`; store named server formats under their own id;
    and try every announced image format until one delivers (the Windows guest
    can answer `CB_RESPONSE_FAIL` for a given format intermittently).
  - SDL3: fixed a double-pop crash on the format-data-response path.
  - Wayland: do not tear down the RDP session on a local cliprdr conversion
    failure.
- **uwac**: libdecor support for Wayland client-side window decorations and
  related input-crash fixes; clipboard waits dispatched on a private queue.
- **Tooling / CI**: copilot instructions, client build workflows, cmake
  preloads and feature-flag options.

## FFmpeg audio log spam (`ffmpeg-audio-log.patch`)

When using a headset (e.g. Jabra) for audio in a Teams call inside the RDP
session, the AAC encoder on the microphone/`audin` path floods the terminal:

```
[aac @ 0x...] Qavg: 65536.000
[aac @ 0x...] 4 frames left in the queue on closing
[aac @ 0x...] Input contains (near) NaN/+-Inf
```

Two root causes, both fixed in `libfreerdp/codec/dsp_ffmpeg.c`:

- FreeRDP installed **no FFmpeg log handler**, so FFmpeg wrote straight to
  `stderr` at its default `INFO` level. The patch installs a process-global
  `av_log` callback (once, via `InitOnceExecuteOnce`) that routes FFmpeg
  messages through **WLog** under the `com.freerdp.codec.ffmpeg` logger. Level
  mapping: `PANIC/FATAL/ERROR → WLOG_ERROR`, `WARNING → WLOG_WARN`,
  `INFO → WLOG_DEBUG` (hidden at the default level), `VERBOSE/DEBUG/TRACE →
  WLOG_TRACE`. The noisy `Qavg` lines are `INFO`, so they no longer print by
  default but remain recoverable with `WLOG_LEVEL=trace` (optionally
  `WLOG_FILTER=com.freerdp.codec.ffmpeg`).
- The encoder was **freed without draining**, which is exactly the
  `N frames left in the queue on closing` warning. Since Teams/headset audio
  triggers frequent format resets (each closes and re-opens the encoder), the
  patch drains the encoder (send a NULL flush frame, receive packets to EOF and
  discard them) in `ffmpeg_close_context` before `avcodec_free_context`.

### `Input contains (near) NaN/+-Inf`

This one is logged by the AAC encoder itself at **`AV_LOG_ERROR`**, so routing
to WLog alone does not hide it. FFmpeg 8's `aacenc.c` rejects a frame when any
MDCT coefficient fails `fabs(coeff) < 1E16` — that guard fires on true NaN,
`±Inf`, **and finite near-infinite garbage** (hence "(near)"). FreeRDP already
treats the resulting `EINVAL` as benign, so audio keeps working; the line was
pure noise.

The previous sanitizer in `ffmpeg_encode_frame` only neutralized **exact**
`isnan`/`isinf`, letting a finite-but-enormous sample (e.g. `1e30`, often a sign
of a sample-format mismatch) slip through. The patch:

- clamps every float-PCM sample to `±FFMPEG_PCM_CLAMP_LIMIT` (= `8.0`, ~+18 dBFS
  — far above real audio, far below the `1e16` encoder limit) and maps `NaN → 0`.
  `±Inf` is caught by the same magnitude clamp. Normal/loud audio is untouched;
  the encoder can no longer reach `1e16`, so the error stops at the source.
- emits a **rate-limited** `WLog_WARN` (under `com.freerdp.dsp.ffmpeg`, first
  occurrence then every 256th) reporting the **actual offending value**,
  channel/sample index, and the input `AUDIO_FORMAT`
  (`wFormatTag`/`wBitsPerSample`/`nChannels`/`nSamplesPerSec`) — useful to tell a
  transient glitch from a persistent upstream format mismatch.

No new build flags are required (audio AAC support comes from the existing
FFmpeg DSP backend). The `av_log` callback is process-global, so it also
captures the h264/image FFmpeg backends once any DSP context exists.
