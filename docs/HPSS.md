# Apple High Performance Screen Sharing

The `hpss` backend uses the reverse-engineered
[iShareScreen transport](https://github.com/renegadelink/iShareScreen/tree/9ab40d3a3151524f954cff9d6239d891197c1705),
pinned to commit `9ab40d3a3151524f954cff9d6239d891197c1705`. It is experimental:
transport integration, native-depth HEVC decoding, and terminal output have
software tests; an authenticated session against Apple's server still needs
validation. The published 0.1.0 packages predate this backend; build current
source to use it.

## Install and connect

Build Scry normally. HPSS has no additional C++ build dependency. Its optional
runtime requires Python 3.10 or newer, Git, and the Python dependencies installed
by this script:

```sh
python3 scripts/setup-hpss.py "$HOME/.local/share/goblin-scry/hpss-venv"
export GOBLIN_HPSS_PYTHON="$HOME/.local/share/goblin-scry/hpss-venv/bin/python3"
build/goblin-scry hpss localhost --username adam --size 1280x720 --hdr auto
```

The password prompt hides input. `GOBLIN_PASSWORD` is also supported for existing
credential workflows. Credentials pass to the helper through a private socket;
they are not included in its arguments or environment. Exit with Ctrl+].

Enable Screen Sharing on the host in System Settings → General → Sharing and
allow the chosen login account. The host must support Apple's High Performance
mode. The upstream transport uses TCP 5900 for negotiation and UDP media ports
5900/5901. `--port` changes the TCP port. A successful ordinary VNC connection
does not establish that HPSS works.

The transport requests a virtual display with its default privacy curtain,
which can blank the host's physical display while connected. This also applies
to localhost testing. Use an HDR-capable Silkie terminal with `hdr yes` enabled
for a live HDR check. Enabling HDR in Silkie is separate from enabling Screen
Sharing on macOS.

The installer omits iShareScreen's GUI/audio dependencies and installs its pinned
source without dependency resolution. PyAV 19.0.1 exposes the frame color metadata
used by Scry. On platforms without Python wheels, including some LoongArch
systems, these packages need native builds and their development prerequisites.
Do not substitute an 8-bit decoder to make an installation succeed. A native
Scry build alone does not verify that the optional Python runtime is available.

An installed Scry looks for `libexec/goblin-scry/hpss.py` next to its prefix,
then the helper in its build source tree. Set `GOBLIN_HPSS_HELPER` to override
that path. `GOBLIN_HPSS_PYTHON` selects Python; otherwise a
`libexec/goblin-scry/hpss-venv/bin/python3` installation is preferred over
`python3` on PATH. The installed documentation also includes the setup script.

## Precision and HDR

Every HPSS framebuffer uses four 16-bit channels: little-endian IEEE binary16
RGBA in linear extended sRGB, with opaque alpha. Native HEVC samples are read
before iShareScreen's normal 8-bit display conversion. Float calculations apply
range normalization, transfer functions, and color-primary conversion before
packing the framebuffer. Binary16 preserves source precision without claiming
that an 8-bit or 10-bit source becomes a 16-bit source capture.

`--hdr auto` queries the terminal for Silkie's Kitty `f=64` format. A confirmed
response enables HDR negotiation with the Mac and sends binary16 pixels to the
terminal, preserving negative wide-gamut components and highlights above 1.0.
`--hdr on` requires that response and fails clearly if it is absent.
`--hdr off` requests SDR but still uses the 16-bit HPSS framebuffer.

The [Silkie HDR protocol](https://silkie.goblinreactor.com/) defines 1.0 as SDR
white. PQ samples use 203 cd/m² diffuse white; HLG uses the BT.2100 reference
1000 cd/m² display and system gamma 1.2. Supported primaries are BT.709,
BT.2020, and Display P3. HDR frames with missing or unsupported color metadata
fail rather than guessing a transfer function. Unspecified SDR metadata uses
BT.709. Software HEVC decoding is selected to retain native samples and their
metadata consistently across platforms.

Silkie handles its display's current HDR headroom. Scry does not use per-image
tone mapping, because each character cell is a separate image and independent
curves could produce seams. Without `f=64`, output uses a shared desktop-wide
SDR tone curve and converts to 8-bit only at the terminal output boundary.
Cursor blending happens in linear light before that final conversion.

The requested HPSS resolution remains fixed when the terminal resizes. Audio,
clipboard sharing, reconnect, and multiple monitor selection are not exposed.
Keyboard and pointer input, view-only mode, cursor updates, and cleanup are
implemented. The adapter consumes the upstream decoder's latest frame for each
video tile; it is a desktop viewer, not a lossless video recorder.

## Tests and live validation

```sh
ctest --test-dir build --output-on-failure
"$GOBLIN_HPSS_PYTHON" tests/hpss_color_test.py
```

CTest uses a substitute transport over the real private socket and a terminal
emulator. It checks fragmented protocol replies, HDR negotiation, exact half
values, SDR output, keyboard/pointer forwarding, hundreds of frame updates,
malformed frames, authentication errors, process cleanup, and terminal
restoration. The color test also encodes and decodes a real 10-bit HEVC frame,
checks its CICP metadata, and verifies that adjacent 10-bit samples survive.
It requires a PyAV/FFmpeg build with the libx265 encoder for that test only.

For live validation, connect from Silkie with `--hdr on`, display a known HDR
ramp on the Mac, check highlights and smooth gradients, exercise keyboard and
pointer input, then exit and confirm terminal and host-display restoration.
Repeat with `--hdr off`. Record host macOS, Silkie version, decoder pixel
format/metadata, and results. Software fixtures are not a substitute for this
server and display test.

## Licensing and distribution

Scry's original code remains GPL-3.0-only. iShareScreen is AGPL-3.0-or-later;
its unchanged license is in `licenses/iShareScreen-AGPL-3.0.txt`.
[GPLv3 section 13](https://www.gnu.org/licenses/gpl-3.0.html#section13) explicitly
permits combination with AGPLv3, and AGPL section 13's remote-interaction
source requirements apply to that combination. Original component licenses
continue to apply; do not describe a bundled HPSS runtime as GPL-only.

The current Brew/APT release process does not bundle this optional runtime.
Users install it separately from the pinned upstream source. Before distributing
a combined runtime, retain all dependency notices and provide its complete
corresponding source, exact dependency versions, patches, and build/install
scripts alongside the download. If the AGPL network-source provision applies to
your deployment, provide the required source offer to its remote users. Review
the selected FFmpeg build's license too: optional codecs can select GPL terms.
See [THIRD_PARTY.md](../THIRD_PARTY.md) for the dependency inventory.
