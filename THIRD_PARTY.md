# Licensing and dependencies

Copyright (C) 2026 Adam DePrince. Goblin Scry's original source, documentation,
website, and release scripts are licensed under the GNU General Public License,
version 3 only (`GPL-3.0-only`). See [LICENSE](LICENSE) for the full terms.
The software is provided without warranty.

## Why GPLv3

The complete application links to LibVNCClient for VNC and local X11/Wayland
sessions. LibVNCClient is GPL-2.0-or-later; its upstream licensing guidance
requires linked applications to comply with the GPL. We exercise its permission
to use GPLv3. FreeRDP and WinPR use Apache-2.0, which is compatible with GPLv3.
Therefore the combined application is distributed under GPLv3, rather than
Apache-2.0. The upstream components retain their own copyrights and licenses.

Sources reviewed:

- [LibVNCServer 0.9.15 licensing statement](https://github.com/LibVNC/libvncserver/blob/LibVNCServer-0.9.15/README.md#license)
- [LibVNCClient licensing FAQ](https://github.com/LibVNC/libvncserver#faq)
- [FreeRDP 3.17.2 license](https://github.com/FreeRDP/FreeRDP/blob/3.17.2/LICENSE)
- [Apache/GPL compatibility](https://www.apache.org/licenses/GPL-compatibility)
- [GNU GPL compatibility](https://www.gnu.org/licenses/quick-guide-gplv3.html)

## Release dependencies

| Component | License | Distribution |
| --- | --- | --- |
| FreeRDP / WinPR 3.17.2 | Apache-2.0, with individual permissive notices | Private shared libraries; exact source included in the source archive |
| LibVNCClient 0.9.15 | GPL-2.0-or-later, used under GPLv3 | Private shared library; exact source included in the source archive |
| OpenSSL 3 | Apache-2.0 | Runtime package dependency on macOS/Ubuntu; private OpenSSL 3.6.5 shared libraries on Kylin, with exact source and notices included |
| libjpeg-turbo | BSD-style/IJG/zlib licenses; see upstream LICENSE.md | Runtime package dependency when JPEG is enabled |
| zlib | Zlib | Operating-system/runtime package dependency |
| ICU | MIT/Unicode terms | Runtime package dependency for WinPR character conversion on Linux |
| Zstandard | BSD-3-Clause (dual-license option selected) | Transitive runtime dependency of Ubuntu's OpenSSL package |
| C/C++ runtime and platform frameworks | Platform-specific system licenses | Operating-system/compiler runtime dependencies |

Release builds require OpenSSL **3 or newer**. OpenSSL 1.x has different license
terms and is not accepted by the release scripts. Optional LZO, SASL, FFmpeg,
Opus, JSON, and external codec integrations are disabled in the pinned release
build to keep the runtime dependency set explicit. A custom build that enables
additional libraries needs a separate dependency/license review.

TigerVNC, xrandr, Sway, swaymsg, and wayvnc are separately installed programs
launched by the Linux local-session modes. They are not copied into these
packages. Their own licenses continue to apply.

## Optional HPSS runtime

The HPSS helper uses separately installed iShareScreen, pinned to
`9ab40d3a3151524f954cff9d6239d891197c1705`, under AGPL-3.0-or-later.
[GPLv3 section 13](https://www.gnu.org/licenses/gpl-3.0.html#section13)
permits this combination with Scry's GPLv3 code; the AGPL network-interaction
source provision applies to the combination. The iShareScreen license is
preserved in `licenses/iShareScreen-AGPL-3.0.txt`. This optional runtime is not
bundled by the current release scripts. See [HPSS.md](docs/HPSS.md) for its
installation, pinned source, validation limits, and distribution requirements.

The local decoder test used the following runtime versions. The installer pins
PyAV and iShareScreen and accepts compatible versions of the remaining Python
packages; distributors must record the versions actually installed and retain
their notices and corresponding source where required.

| Component tested | License |
| --- | --- |
| PyAV 19.0.1 | BSD-3-Clause; linked FFmpeg libraries have separate terms |
| NumPy 2.5.4 | BSD-3-Clause, with bundled 0BSD/MIT/Zlib/CC0-1.0 notices |
| cryptography 50.0.2 | Apache-2.0 or BSD-3-Clause; linked OpenSSL has separate terms |
| PyCryptodome 4.0.0 | BSD-2-Clause and Unlicense |
| pyperclip 1.11.0 | BSD-3-Clause |
| cffi 2.1.1 | MIT-0 |
| pycparser 3.11 | BSD-3-Clause |

FFmpeg is normally LGPL-2.1-or-later, with GPL options selected by some builds.
The optional color test uses libx265 encoding; x265 is GPL-2.0-or-later and
compatible with GPLv3. Do not infer the license of an installed FFmpeg binary
from PyAV's BSD license. Audit build flags and preserve the selected FFmpeg and
codec source when redistributing them. Python and system runtimes retain their
own terms. The runtime licenses above permit combination under GPLv3/AGPLv3;
the binary packages without this optional runtime remain as described above.

## Source and notices

Every release must publish the matching `goblin-scry-VERSION-source.tar.gz`
alongside its binary downloads, at no additional charge. It contains the full
Goblin Scry source, build and packaging scripts, pinned FreeRDP and LibVNCServer
sources, OpenSSL 3.6.5 source for the Kylin build, and their original copyright/license notices. The binary packages
install this document, GPLv3, upstream license texts, and the source URL.
Source archives must remain available for as long as the corresponding binaries.

The original upstream license texts are in `licenses/`. File-specific upstream
notices are retained in the corresponding source archive. Dependency versions,
build flags, system dependency inventory, and validation results are recorded
in each package's build manifest. Package-manager dependencies are supplied
separately by Homebrew or the target distribution; their source packages and
license notices remain available through those repositories.
