# Reproducing the public HDR example

The website example shows a clean Chrome window displaying a public HDR image.
A window-only macOS capture becomes a linear RGBA binary16 frame, which runs
through Scry's actual renderer in Silkie. A second HDR window capture records
Silkie's output. This demonstrates the renderer, not an authenticated HPSS
connection. Keep that distinction in the website caption.

Only use an isolated browser profile and public sample content. Select windows
by both process ID and window ID. The helper never captures the desktop or
other windows. Inspect its SDR PNG before publishing any capture. Keep the
website, browser profile, images, app bundle, and binary frames out of Git.

## Make the public HDR image

Use an HDRI-enabled ImageMagick, libavif's `avifenc`, and Python with NumPy.
The example uses Poly Haven's **Kloppenheim 06 Pure Sky** by Greg Zaal and Jarod
Guest, available under CC0: <https://polyhaven.com/a/kloppenheim_06_puresky>.
Download its 2K Radiance `.hdr` file, then run:

```sh
python3 scripts/make-hdr-example.py kloppenheim_06_puresky_2k.hdr sky.scryhdr
python3 scripts/encode-hdr-avif.py sky.scryhdr sky-hdr.avif
```

The conversion retains floating-point samples. The AVIF uses 10-bit 4:4:4
BT.2020/PQ, CICP 9/16/9, with 203 cd/m² reference white. It is not an SDR image
with HDR labels added. The JSON sidecar records dimensions, maximum light
values, and the number of pixels above SDR white.

The separately deployed `html/hdr-example/` page displays this AVIF, credits
its source, and links Silkie. Open it in Chrome with a new dedicated
`--user-data-dir`, `--no-first-run`, `--disable-sync`, and `--disable-extensions`.
Never use the user's normal browser profile for this demonstration. Keep the
browser's tab and address bar visible in the capture.

## Capture one browser window

macOS 26 and the current command-line SDK are required for the capture helper:

```sh
python3 scripts/build-hdr-capture.py
open -gna "$PWD/release-work/Scry HDR Capture.app" --args \
  --report "$PWD/release-work/capture.log" permission
```

Allow **Scry HDR Capture** in System Settings → Privacy & Security → Screen &
System Audio Recording. Building an ad-hoc signed replacement may invalidate
that permission; remove and re-add the app if macOS reports a code requirement
mismatch. Do not work around a permission denial by capturing the full display.

List only the dedicated Chrome process's windows, then capture the selected ID:

```sh
open -gna "$PWD/release-work/Scry HDR Capture.app" --args \
  --report "$PWD/release-work/windows.log" list CHROME_PID
open -gna "$PWD/release-work/Scry HDR Capture.app" --args \
  --report "$PWD/release-work/capture.log" \
  capture CHROME_PID WINDOW_ID "$PWD/release-work/browser.scryhdr"
```

`open` returns before capture completes. Read the report and verify that the
output exists before proceeding. The helper requests HDR through
ScreenCaptureKit, converts it to linear extended sRGB binary16, and writes an
SDR PNG alongside it when the screenshot API supplies one.

## Render in Silkie and export

```sh
cmake --build build --target scry-hdr-preview --parallel
# Run this inside a fresh Silkie window configured with hdr always:
build/scry-hdr-preview release-work/browser.scryhdr
```

`hdr auto` can also work on an HDR-capable display. The preview requires Silkie's
`f=64` response and reuses Scry's real terminal renderer. It makes no HPSS
connection. Size the terminal to contain the complete browser frame, leaving
room for its window title. Ctrl+] exits the preview.

Capture only that Silkie process and window using the same helper, then encode:

```sh
python3 scripts/encode-hdr-avif.py release-work/silkie.scryhdr \
  html/images/scry-silkie-browser-hdr.avif
avifdec --info html/images/scry-silkie-browser-hdr.avif
# With PyAV and NumPy installed (for example, the HPSS virtual environment):
python3 scripts/verify-hdr-avif.py html/images/scry-silkie-browser-hdr.avif
```

Verify 10-bit depth, BT.2020/PQ metadata, and decoded highlight values above SDR
white. Inspect the SDR preview for private content, clipping, and readable
browser chrome. Use the AVIF as the website's primary picture, with the SDR PNG
as its fallback. Describe the source/runtime requirements and link
<https://silkie.goblinreactor.com/>.

The encoder rejects frames with no luminance above SDR white. macOS can capture
an SDR rendering while a browser or display is changing HDR state; wait for the
window to finish rendering and recapture in that case. Merely receiving 16-bit
pixels does not prove the capture contains HDR highlights. For the stream-based
capture API, explicitly select HDR canonical range and linear RGBA half-float;
the preset alone did not preserve HDR in this environment.

## Deploy

Follow [DEPLOYMENT.md](DEPLOYMENT.md). The Scry nginx route corrects the storage
origin's older AVIF MIME type to `image/avif` for successful AVIF responses.
Other content types and error responses keep the origin's type. Upload assets
before the page that references them, and verify the public asset's Content-Type
and checksum. Use a new filename or versioned URL when replacing a cached asset.
