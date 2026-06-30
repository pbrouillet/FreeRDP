# Webcam redirection (MS-RDPECAM) — enablement, resiliency & log hygiene

This document describes the client-side **webcam / camera redirection** support
carried on the `feature/pbrouillet/copilot-agent` branch: how to build and use
it, how it is wired internally, and the resiliency / logging hardening added so
that webcam stream problems never disrupt the RDP session or flood the log.

> Motivating use case: using a local USB webcam (e.g. inside a Microsoft Teams
> call) on a Windows 365 / AVD Cloud PC through `sdl-freerdp`. The webcam stream
> is **best-effort** — like the video in a call, it is far less important than
> the remote-desktop connection itself. A dropped frame, a transient channel
> hiccup, or a quirky MJPEG frame must therefore degrade the *video*, never the
> *session*, and must not spam the terminal.

The two standalone patches that implement everything described here are:

| Patch | Files touched | Role |
| ----- | ------------- | ---- |
| `rdpecam-resiliency.patch` | `channels/rdpecam/client/*`, `libfreerdp/codec/video.c` | Channel error containment + lenient MJPEG decode |
| `ffmpeg-audio-log.patch`   | `libfreerdp/codec/dsp_ffmpeg.c` | FFmpeg→WLog routing + demote best-effort decoder spam |

They touch **disjoint files** and can be applied in either order; apply **both**
for the complete webcam experience (resiliency + quiet logs).

---

## 1. Building with webcam support

The `[MS-RDPECAM]` camera redirection **client** is `OFF` by default. Enable it
at configure time:

```sh
cmake -GNinja -DCHANNEL_RDPECAM_CLIENT=ON \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING_INTERNAL=ON \
  -DWITH_CLIENT_SDL3=ON \
  -B build -S .
cmake --build build --target sdl-freerdp --parallel
```

### No extra packages are required

Despite the `client/v4l/` subsystem directory name, **`libv4l-dev` is not
needed**. The channel needs only:

- `linux/videodev2.h` — a *kernel* UAPI header shipped by `linux-libc-dev`
  (already present on any normal dev box). `cmake/FindV4L.cmake` probes for this
  header alone.
- `libusb-1.0` (`libusb-1.0-0-dev`) — already present; used by the optional
  UVC/H.264 path (`uvc_h264.c`).

The MJPEG decode + H.264 re-encode path (see §3) is provided by the existing
FFmpeg DSP/codec backend FreeRDP already links; no additional codec packages are
required beyond what the rest of the build uses.

### Verifying the channel is built in

The camera addin is registered as a **static** dynamic-virtual-channel addin in
the generated `build/channels/client/tables.c`:

```c
{ "rdpecam", rdpecam_DVCPluginEntry },
{ "v4l", "", v4l_freerdp_rdpecam_client_subsystem_entry },
```

---

## 2. Using it at runtime

The camera channel is a **dynamic virtual channel** and therefore rides on
`drdynvc`. Load it with the `/rdpecam` switch (or, from an `.rdp` file, the
`camerastoredirect:s:` / `RedirectCameras` setting, handled in
`client/common/file.c`).

```sh
# redirect the default/all cameras
sdl-freerdp /v:<host> /rdpecam ...

# typical full invocation used against the Cloud PC (gateway, AAD, etc. omitted)
sdl-freerdp /v:<host> /rdpecam /sound /microphone ...
```

`/rdpecam` accepts the usual sub-options (`device:`, `encode:`, `quality:`),
which FreeRDP forwards to the addin; see `freerdp_client_channel_args_to_string`
usage in `client/common/file.c`.

### Useful logging knobs

All FFmpeg-originated messages are routed under the `com.freerdp.codec.ffmpeg`
WLog logger (see §4). The camera channel logs under `rdpecam-*` tags.

```sh
# show everything the camera channel and FFmpeg emit (including demoted spam)
WLOG_LEVEL=trace WLOG_FILTER=com.freerdp.codec.ffmpeg sdl-freerdp ...

# default level: webcam best-effort noise is hidden, real errors still print
```

---

## 3. How webcam capture works (data flow)

```
 V4L2 device (/dev/videoN)
   │  capture thread: cam_v4l_stream_capture_thread()           [v4l/camera_v4l.c]
   │  dequeues V4L2 buffers (often MJPEG or YUYV)
   ▼
 sampleCallback ──► ecam_dev_sample_captured_callback()         [camera_device_main.c]
   │  stores the latest sample on the stream
   ▼
 server SampleRequest ──► ecam_dev_process_sample_request()     [camera_device_main.c]
   │  ▼
   │  ecam_dev_send_pending()                                    [camera_device_main.c ~156]
   │     ▼ ecam_encoder_compress()                               [encoding.c]
   │         freerdp_video_sample_convert()                      [libfreerdp/codec/video.c]
   │           ├─ if input is MJPEG: freerdp_video_decode_mjpeg() (FFmpeg MJPEG decoder)
   │           └─ H.264 encode for transmission
   │     ▼ ecam_channel_write() ──► channel->Write()             [camera_device_enum_main.c ~81]
   ▼
 drdynvc ──► RDP server
```

Inbound control/messages from the server arrive on the DVC callback
`ecam_dev_on_data_received()` (`camera_device_main.c ~739`), which dispatches by
`MessageId` (activate/deactivate stream, sample request, property get/set, etc.).

### Why MJPEG matters

Most USB webcams expose **MJPEG** as their high-resolution/high-FPS format. The
rdpecam path decodes those JPEG frames with FFmpeg (`freerdp_video_decode_mjpeg`
in `libfreerdp/codec/video.c`) and re-encodes to H.264 for the wire. This decode
step is the source of the log flood addressed in §4.

---

## 4. Resiliency & logging hardening

Three independent layers were added. None of them changes the on-wire protocol;
the server still receives correct `CAM` responses where applicable.

### 4.1 Channel error containment (the connection-saver)

**Problem.** `drdynvc`'s non-threaded data path calls
`setChannelError(rdpcontext, error, …)` whenever a channel's data handler
returns a non-`CHANNEL_RC_OK` status (`channels/drdynvc/client/drdynvc_main.c`,
around lines 1802 / 1864 / 2216). `setChannelError` **aborts the entire RDP
session**. So *any* error bubbling out of the camera handler — a single bad
frame, a transient `channel->Write` failure, a short/garbled message — would
drop the user's whole remote desktop.

**Fix** (`channels/rdpecam/client/camera_device_main.c`):

- `ecam_dev_on_data_received()` is now a **full error-containment boundary**.
  The early-argument guards and **any** per-message processing error are logged
  and converted to `CHANNEL_RC_OK`, so they can never reach `setChannelError`:

  ```c
  if (error != CHANNEL_RC_OK)
  {
      WLog_WARN(TAG,
                "contained camera channel error 0x%08" PRIx32
                " (MessageId=0x%02" PRIx8 "); keeping connection alive",
                error, messageId);
      error = CHANNEL_RC_OK;
  }
  return error;
  ```

  (See `camera_device_main.c:821`.) Individual handlers still notify the server
  via `ecam_channel_send_error_response()` where the protocol calls for it; we
  only stop the *client* from killing its own connection.

- `ecam_dev_send_pending()` (`camera_device_main.c ~156`): a failed sample
  `ecam_channel_write()` no longer propagates. It drops that one frame and
  continues:

  ```c
  const UINT werr = ecam_channel_write(dev->ecam, stream->hSampleReqChannel,
                                       CAM_MSG_ID_SampleResponse, output, FALSE);
  if (werr != CHANNEL_RC_OK)
  {
      WLog_WARN(TAG, "Frame dropped: sample channel write failed: %" PRIu32, werr);
      return CHANNEL_RC_OK;
  }
  ```

  (See `camera_device_main.c:203`.)

- `v4l/camera_v4l.c` (`cam_v4l_stream_capture_thread`, ~432): per-frame
  `sampleCallback` failures are downgraded `WLog_ERR → WLog_WARN`
  (`camera_v4l.c:483`). The capture thread already tolerates them and keeps
  capturing; this just removes spam.

> Note on non-droppable formats: `mediaSupportDrops()`
> (`camera_device_main.c:145`) returns `FALSE` for H.264 (cannot drop) and
> `TRUE` otherwise. The non-droppable path makes the capture side block-wait
> (bounded `SleepEx`, ~16–100 ms) until the pending sample is flushed, which is
> already resilient.

### 4.2 Lenient MJPEG decoding (fewer needless frame drops)

**Problem.** The MJPEG decoder in `libfreerdp/codec/video.c` was created with
`err_recognition |= AV_EF_EXPLODE`, which promotes minor, recoverable glitches
to **fatal** decode failures. With imperfect webcam frames this needlessly
dropped otherwise-usable frames and produced `avcodec_receive_frame failed`
errors.

**Fix** (`libfreerdp/codec/video.c:156`):

```c
context->mjpegDecoder->err_recognition = 0;
```

The decoder now conceals minor glitches and keeps emitting frames. Genuinely
undecodable input is still rejected via the `avcodec_send_packet` /
`avcodec_receive_frame` return codes and dropped without tearing down the
stream.

> This is a real resiliency improvement, **but it does not silence** the
> `unable to decode APP fields` flood — see §4.3 for why.

### 4.3 Silencing the `unable to decode APP fields` flood

**Symptom** (seen continuously during a webcam call):

```
[ERROR][com.freerdp.codec.ffmpeg] - [ffmpeg_log_callback]: unable to decode APP fields: Invalid data found when processing input
```

**Root cause.** Webcams embed vendor-specific **APP markers**
(`APP0`/`APP1`/`APP4`, …) in every JPEG frame. FFmpeg's `mjpegdec.c` cannot
fully parse some of them and logs this line at `AV_LOG_ERROR` — then **continues
decoding the frame anyway**. Crucially, in FFmpeg 8.0 the log call is
**unconditional** (`libavcodec/mjpegdec.c:2438`):

```c
} else if (start_code >= APP0 && start_code <= APP15) {
    if ((ret = mjpeg_decode_app(s)) < 0)
        av_log(avctx, AV_LOG_ERROR, "unable to decode APP fields: %s\n",
               av_err2str(ret));
```

It is **not gated by `err_recognition`**, which is why the §4.2 change (dropping
`AV_EF_EXPLODE`) reduced frame drops but could not stop the log spam.

**Fix** — make the process-global FFmpeg→WLog callback **codec-aware**
(`libfreerdp/codec/dsp_ffmpeg.c`). The callback already routes all FFmpeg
messages through WLog (see the `ffmpeg-audio-log.patch` section in
`README.md`). We added a helper that recognises messages originating from a
*best-effort* decoder and demotes their sub-`INFO` chatter to `WLOG_TRACE`:

```c
/* dsp_ffmpeg.c:89 */
static BOOL ffmpeg_log_is_best_effort_decoder(void* avcl)
{
    if (!avcl)
        return FALSE;
    const AVClass* const cls = *(const AVClass* const*)avcl;
    if (cls != avcodec_get_class())
        return FALSE;
    const AVCodecContext* const ctx = (const AVCodecContext*)avcl;
    switch (ctx->codec_id)
    {
        case AV_CODEC_ID_MJPEG:
            return TRUE;
        default:
            return FALSE;
    }
}

/* dsp_ffmpeg.c:122, inside ffmpeg_log_callback() */
if ((level < AV_LOG_INFO) && ffmpeg_log_is_best_effort_decoder(avcl))
    level = AV_LOG_VERBOSE;   /* -> WLOG_TRACE */
```

Why this is safe:

- The `avcl` contract matches FFmpeg's own `av_log_default_callback`: a non-NULL
  `avcl` points to a struct whose first member is a `const AVClass*`. Reading it
  and comparing against `avcodec_get_class()` reliably identifies an
  `AVCodecContext`; the `codec_id` then distinguishes MJPEG from H.264/audio.
- Only **best-effort decoders** (currently MJPEG) are affected. Audio (AAC) and
  H.264 messages are untouched and still map ERROR→`WLOG_ERROR`.
- The messages are **hidden, not lost**: `WLOG_LEVEL=trace` brings them back.
- Our own higher-level diagnostics (e.g. `video.c`'s
  `avcodec_receive_frame failed` via `WLog_Print`) are emitted directly, not via
  FFmpeg, so real decode failures still surface at `WLOG_ERROR`.

To add another best-effort decoder later (e.g. if VP8/VP9 webcams appear), add
its `AV_CODEC_ID_*` to the `switch` in `ffmpeg_log_is_best_effort_decoder`.

---

## 5. Why the fix is split across two patches

The `unable to decode APP fields` line is emitted by FFmpeg itself, so the only
place to suppress it is inside the **av_log callback** — which lives in
`dsp_ffmpeg.c` and was introduced by `ffmpeg-audio-log.patch`. Putting the MJPEG
demotion there keeps the two patches **file-disjoint**:

- `ffmpeg-audio-log.patch` → `libfreerdp/codec/dsp_ffmpeg.c` (callback + demotion)
- `rdpecam-resiliency.patch` → `channels/rdpecam/client/*` + `libfreerdp/codec/video.c`

Functionally, the complete webcam experience needs **both**:

| Want | Needs |
| ---- | ----- |
| Webcam works at all | `-DCHANNEL_RDPECAM_CLIENT=ON` build flag |
| A webcam glitch can't drop the RDP session | `rdpecam-resiliency.patch` (§4.1) |
| Fewer needlessly-dropped frames | `rdpecam-resiliency.patch` (§4.2) |
| No `unable to decode APP fields` flood | `ffmpeg-audio-log.patch` (§4.3) |

---

## 6. Testing checklist

1. Configure with `-DCHANNEL_RDPECAM_CLIENT=ON`, build `sdl-freerdp`
   (warning-clean for the rdpecam files and `video.c`/`dsp_ffmpeg.c`).
2. Confirm registration: `grep -n rdpecam build/channels/client/tables.c`.
3. Connect with `/rdpecam`, open a video app (Teams/Camera) on the remote side,
   and verify the local webcam image appears.
4. **Resiliency:** during streaming, the connection must survive transient
   webcam issues (unplug/replug, mode changes). Look for contained warnings
   (`Frame dropped: …`, `contained camera channel error …`) but **no session
   teardown**.
5. **Log hygiene:** at the default WLog level the terminal must be free of
   `unable to decode APP fields` (and the AAC `Qavg` / `NaN/Inf` lines covered by
   `ffmpeg-audio-log.patch`). They must reappear under `WLOG_LEVEL=trace`.

---

## 7. File / symbol reference

| Location | What |
| -------- | ---- |
| `channels/rdpecam/client/camera_device_main.c:156` | `ecam_dev_send_pending` — drop frame on write failure |
| `channels/rdpecam/client/camera_device_main.c:739` | `ecam_dev_on_data_received` — error-containment boundary |
| `channels/rdpecam/client/camera_device_main.c:821` | contained-error log + `CHANNEL_RC_OK` return |
| `channels/rdpecam/client/v4l/camera_v4l.c:483` | `sampleCallback` failure `WLog_ERR → WLog_WARN` |
| `channels/rdpecam/client/encoding.c` | `ecam_encoder_compress` → `freerdp_video_sample_convert` |
| `libfreerdp/codec/video.c:156` | MJPEG decoder `err_recognition = 0` |
| `libfreerdp/codec/dsp_ffmpeg.c:89` | `ffmpeg_log_is_best_effort_decoder` helper |
| `libfreerdp/codec/dsp_ffmpeg.c:122` | demotion in `ffmpeg_log_callback` |
| `channels/drdynvc/client/drdynvc_main.c:1802/1864/2216` | `setChannelError` (the abort risk being contained) |
