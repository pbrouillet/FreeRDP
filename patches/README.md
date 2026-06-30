# copilot-agent patches

This directory holds the out-of-tree changes carried on the
`feature/pbrouillet/copilot-agent` branch so they can be reapplied after a
rebase onto a fresh upstream `master`.

## Files

- **`copilot-agent.patch`** — the canonical, **single combined patch** with the
  full set of branch changes (all commits since the base below). Apply with
  `git apply`. This is the recommended reusable artifact.
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
git apply patches/copilot-agent.patch
# review, build, then commit as desired
```

Use `git apply --check patches/copilot-agent.patch` first to confirm it applies
cleanly. If upstream has moved and a hunk no longer applies, use
`git apply --3way patches/copilot-agent.patch` to fall back to a 3-way merge.

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
