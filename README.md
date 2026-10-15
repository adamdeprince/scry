# Goblin Scry

Graphical desktops in your terminal. [Homepage](https://scry.goblinreactor.com/)
· [Source](https://github.com/adamdeprince/scry)

RDP, VNC, and local X11/Wayland desktops inside a terminal that speaks Kitty graphics, including
Goblin View. The client displays remote pixels at their original size by default,
with integer magnification available for Retina and other high-density displays.
It uses one graphic element per character cell. Only cells whose pixels changed
are sent again.

The RDP backend uses FreeRDP 3 and software GDI. The VNC backend uses
LibVNCClient, including its Apple Remote Desktop (ARD, security type 30)
username/password authentication. Apple Screen Sharing can use that account
authentication without enabling a separate legacy VNC password.

## License

Copyright (C) 2026 Adam DePrince. Goblin Scry is licensed under the GNU General
Public License, version 3 only (`GPL-3.0-only`). See [LICENSE](LICENSE) and
[THIRD_PARTY.md](THIRD_PARTY.md). LibVNCClient's GPL-2.0-or-later terms require
GPL distribution of the linked application; GPLv3 is compatible with FreeRDP's
Apache-2.0 license.

## Packages and releases

Native release targets are Apple Silicon/macOS 26, Ubuntu 26.04 amd64 and arm64,
and Kylin V10 old-world LoongArch. Install a release listed in the repositories:

```sh
brew tap goblinreactor/tap https://brew.goblinreactor.com/homebrew-tap.git
brew install goblinreactor/tap/goblin-scry
```

Linux users should configure the signed repository at
[apt.goblinreactor.com](https://apt.goblinreactor.com/) for their OS release,
then run `sudo apt install goblin-scry`. Use a package matching the distribution
and architecture; Kylin old-world `loongarch64` is distinct from Debian `loong64`.

[Deployment instructions](docs/DEPLOYMENT.md) cover native builds, matching
source archives, checksums, Brew/APT publication, website uploads, nginx,
rollback, and public installation checks. The `html/` website and all release
artifacts are deliberately excluded from the source repository.

## Build

Requires a C++17 compiler, CMake, pkg-config, FreeRDP 3, and LibVNCClient 0.9.14
or newer. LibVNCClient needs a crypto backend for ARD authentication.

On macOS, dependencies can be installed with Homebrew:

```sh
brew install cmake pkg-config freerdp libvncserver
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

On Debian/Ubuntu with FreeRDP 3 packages:

```sh
sudo apt install build-essential cmake pkg-config freerdp3-dev libvncserver-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

You can also build pinned copies of the protocol libraries into this repository:

```sh
sh scripts/build-deps.sh
PKG_CONFIG_PATH="$PWD/build/deps/lib/pkgconfig" cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

That script requires Git, CMake, OpenSSL development files, and zlib; JPEG
support is included when available. It installs under `build/deps`, without
changing system packages. If OpenSSL is installed outside the standard search
paths, pass its prefix in `CMAKE_PREFIX_PATH` (for example `/opt/homebrew`).

Either protocol backend can be omitted with `-DGOBLIN_WITH_RDP=OFF` or
`-DGOBLIN_WITH_VNC=OFF`. With both disabled, the demo and renderer tests still
build. Use `-DBUILD_TESTING=OFF` to omit tests and the Python requirement.

## Run

```sh
./build/goblin-scry demo --size 1280x720
./build/goblin-scry rdp windows.example --username adam --size 1920x1080
./build/goblin-scry vnc mac.example --username adam
./build/goblin-scry vnc linux.example --port 5901
./build/goblin-scry vnc mac.example --username adam --magnification 2
./build/goblin-scry rdp windows.example --username adam --magnification 2
./build/goblin-scry x11 -- xterm
./build/goblin-scry x11 -- startxfce4
./build/goblin-scry wayland -- foot
```

Passwords are requested with terminal echo disabled, or read from
`GOBLIN_PASSWORD`. Passwords are not accepted as command-line arguments.
`--domain` sets an RDP domain; `--view-only` disables remote input. RDP
certificates are verified by default; `--insecure` explicitly bypasses that
verification for a connection. ARD authentication protects the credential
exchange; it does not encrypt subsequent desktop traffic by itself.

### Local X11 and Wayland servers

`x11` starts a new TigerVNC X server (`Xvnc` or `Xtigervnc`), with an automatically
allocated X display and a private authentication cookie. It requires `xrandr`
for live resolution changes. `wayland` starts a separate headless Sway compositor
and connects to it through wayvnc. It requires `sway`, `swaymsg`, and `wayvnc` with
Unix socket support. Both modes require the LibVNCClient backend and are intended
for Linux systems with those runtime programs installed.

For example, on Debian/Ubuntu, install the runtime programs and example terminals:

```sh
sudo apt install tigervnc-standalone-server x11-xserver-utils xterm sway wayvnc foot
```

An optional command after `--` runs inside the new display, with its original
arguments and the appropriate `DISPLAY`/`XAUTHORITY` or
`WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR`/`SWAYSOCK` environment. Without a command, the
server starts with an empty desktop. The Wayland mode uses a minimal Sway
configuration with one output, no borders or bar, and Xwayland disabled.
The X11 mode can run a window manager or a complete desktop using that command.
Closing the command exits the session; Ctrl+] and termination signals also stop
the servers and any remaining processes in the launched command's process group.

Local desktops use the full drawable **terminal text-area pixel resolution**.
Putting the terminal into fullscreen therefore gives the server that fullscreen
resolution. Exact pixel reports take precedence over columns × cell width and
rows × cell height, which are the fallback when pixel geometry is unavailable.
With `--magnification N`, the server dimensions are divided by N, rounding down,
so magnified content fits the same area. TigerVNC's RandR minimum is 32×32 pixels.
`--size` and `--port` are rejected in local modes: geometry follows the terminal,
and the VNC connection uses a Unix socket in a private temporary directory.
There is no password prompt or TCP VNC listener.

SIGWINCH, XTWINOPS geometry replies, and in-band resize reports update the
underlying RandR output or Sway headless output. Font-size changes refresh
geometry as well. Geometry reports are collected briefly before resizing, and
the server's framebuffer-size notification rebuilds the tile grid. Local modes
do not request a terminal window resize. Startup or resize failures restore the
terminal and stop the local server processes; child output is captured separately
and included in error messages when needed.

**Ctrl+] exits.** Ctrl+C goes to the remote session. Common navigation keys,
F1–F12, Unicode text, and Shift/Alt/Ctrl/Super are supported. Kitty keyboard
events preserve press, repeat, and release; terminals without that protocol
produce ordinary key taps. SGR mouse input supports hover, dragging, three
buttons (including simultaneous presses), modifier clicks, and vertical and
horizontal scrolling. Pixel mouse mode is enabled only after a successful
DECRQM 1016 capability report; otherwise cell coordinates map to cell centers.
Both coordinate paths account for magnification before sending remote pointer
positions. Button releases preserve other held buttons, and shutdown releases
any remaining buttons. Extra mouse buttons are ignored.

`--magnification N` accepts a positive integer (default **1**). For example,
`--magnification 2` displays every remote pixel as a 2×2 block, including the
remote cursor. The remote resolution stays the same. Larger factors need more
terminal cells and images, and may reach `--max-tiles` or clip to the window.

## Geometry and Goblin View

The terminal is queried for cell pixels with `CSI 16 t`, cell counts with
`CSI 18 t`, and text-area pixels with `CSI 14 t`. A reported cell size takes
precedence over the pixel-to-cell ratio. If the terminal supplies no pixel
geometry, use `--cell-size WIDTHxHEIGHT`.

After the VNC server announces its framebuffer, or RDP completes desktop-size
negotiation, the client requests this terminal size:

```text
columns = ceil(remote_width  * magnification / cell_width)
rows    = ceil(remote_height * magnification / cell_height)
ESC [ 8 ; rows ; columns t
```

The request is emitted before image data. For example, 1920×1080 remote pixels
with 9×18 cells requests **214 columns × 60 rows** at the default factor, or
**427 columns × 120 rows** with `--magnification 2`. Partial edge cells are padded
with black, keeping every image exactly one cell. Magnification repeats source
pixels without interpolation, even across cell boundaries. Server
resolution changes and terminal font-size changes rebuild the grid and request
the corresponding size again. SIGWINCH and in-band resize reports (mode 2048)
refresh geometry.

Goblin View's `src/term.cc` already accepts this resize request and relays the
host cell size. Its current 4,096-image/placement limit needs increasing for
larger desktops: the example above requires 12,840 placements and images.
The Goblin View capacity change is being handled separately. `--max-tiles`
can bound allocations to a terminal's image budget; the client default is
262,144. Exceeding it produces an error instead of evicting images silently.

A terminal may cap or ignore the requested size. The client draws visible
cells at the selected magnification and retains damage for clipped cells. Enlarge the
terminal window to expose the rest; there is currently no scrolling viewport
or automatic remote-resolution change when the terminal is resized in `rdp` or
`vnc` mode. Local `x11` and `wayland` desktops follow the terminal resolution.

For Goblin Skiff, use matching current client and server builds and open a fresh
session after installing them. Existing sessions retain the programs they started
with. Put `--` before the hostname when passing goblin-scry options through Skiff:

```sh
goblin-skiff -- HOST goblin-scry demo --size 128x96
goblin-skiff -- HOST goblin-scry wayland -- foot
```

`tests/transport_test.py` provides an isolated terminal emulator for small-window
checks, including decoding the transmitted pixels and checking every placement.
It can compare direct execution, SSH, or Skiff; `--local` uses a local Skiff UDP
session. These checks do not launch a GUI terminal.

## Rendering

Both backends decode into a four-byte-per-pixel framebuffer. RDP's GDI damage
regions and VNC's update rectangles mark candidate cells. The renderer reads
only those cells and compares their RGB pixels with the last displayed values.
An identical update produces no terminal output. Cursor movement in RDP
damages the old and new cursor areas; VNC requests a server-rendered cursor.

Each cell owns a stable Kitty image ID and placement ID. Changed cells use
direct RGB transmission (`a=T,f=24,c=1,r=1,C=1,q=2`) at an explicit cursor
position; Base64 payloads are split into chunks of at most 4,096 bytes. Frames
use synchronized output (mode 2026). Resize and shutdown delete the client's
own image IDs. The alternate screen, keyboard stack, mouse modes, cursor
visibility, and termios settings are restored on normal exit, errors, and
caught termination signals.

The initial desktop and geometry changes require a complete visible paint;
ordinary remote updates transmit changed cells only. No desktop-size Kitty
image is used. The demo changes one pixel every 300 milliseconds after its
initial paint, making the incremental output easy to inspect.

## Verification and current scope

```sh
ctest --test-dir build --output-on-failure

cmake -S . -B build-sanitize -DGOBLIN_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-sanitize -j
ctest --test-dir build-sanitize --output-on-failure
```

Core tests cover damage clipping, byte order, unchanged cells, partial edges,
cursor movement, magnified damage and cursors, chunking, image cleanup, capacity
limits, and fragmented input.
Pseudo-terminal tests independently decode Kitty output and check sizing,
font changes, magnified pixels, incremental updates, and terminal restoration. The VNC adapter
is exercised against a local RFB peer over a socket pair, including an ARD
Diffie–Hellman/AES credential exchange, raw updates, DesktopSize, keyboard,
mouse hover/drag/release, modifier clicks, scrolling, magnified pixel and cell
coordinates, view-only mode, and authentication failure. The ARD test uses the OpenSSL command-line
tool to decrypt and verify credentials.

Local-mode integration tests launch isolated substitute runtime processes with
a real RFB socket connection. They check exact initial pixel dimensions, native
resize commands, SIGWINCH and in-band changes, magnification, view-only input,
application environments, authentication isolation, startup/resize failure,
and process/socket cleanup. These tests use pseudo-terminals and do not launch
Kitty. Real server startup, application launch, growth, shrinkage, font changes,
and cleanup have also passed on Ubuntu 26.04 with TigerVNC 1.15.0, Sway 1.11,
and wayvnc 0.9.1. With those runtime programs, xterm, and foot installed, run:

```sh
python3 tests/local_runtime_test.py build/goblin-scry
```

The RDP adapter has been compiled against FreeRDP 3.17.2; a real RDP-server
session has not yet been tested. This first version uses the classic software
GDI path, with RDPGFX disabled. Clipboard, audio, file transfer, reconnection,
RDP gateways, Apple session selection, and other proprietary Apple extensions
are not implemented. Apple support currently means ARD account authentication
and the ordinary RFB desktop/input path.

Protocol references: [Kitty graphics](https://sw.kovidgoyal.net/kitty/graphics-protocol/),
[Kitty keyboard](https://sw.kovidgoyal.net/kitty/keyboard-protocol/),
[XTerm mouse tracking](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-Mouse-Tracking),
[FreeRDP](https://github.com/FreeRDP/FreeRDP), and
[LibVNCClient](https://github.com/LibVNC/libvncserver).
