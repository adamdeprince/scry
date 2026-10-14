# Building and deploying Goblin Scry

The source repository is <https://github.com/adamdeprince/scry.git>. It contains
code, licenses, packaging recipes, and these instructions. Do not commit
binaries, build directories, agent instructions, or `html/`. The website lives
in the local `html/` directory and is deployed separately.

## Release layout and prerequisites

Use Python 3.12 or newer for the release scripts (safe tar extraction requires
the `data` filter), CMake 3.20+, a C++17 compiler, pkg-config, OpenSSL 3 development
files, zlib, and libjpeg-turbo. Linux packaging also requires `patchelf`,
`dpkg-dev`, and `dpkg-deb`. On macOS, install the dependency formulas:

```sh
brew install cmake pkgconf openssl@3 jpeg-turbo
```

Linux prerequisites on a recent Debian/Ubuntu host:

```sh
sudo apt install build-essential cmake pkg-config python3 libssl-dev \
  zlib1g-dev libjpeg-dev patchelf dpkg-dev
```

Check host OS, `dpkg --print-architecture`, compiler, Python, and OpenSSL before
building. The requested targets are local Apple Silicon/macOS 26, `naamah`
(amd64), `graviton5` (arm64), and `loongson` (Kylin old-world loongarch64).
Recent sibling-project release records identify these Linux targets, but each
Scry build records the actual host OS and architecture independently. Do not
substitute Debian's new-world `loong64` for Kylin's `loongarch64`.

Older Kylin may need a private newer Python/CMake/compiler/OpenSSL installation.
Set `PATH`, `PKG_CONFIG_PATH`, and `CMAKE_PREFIX_PATH` in the build environment;
never replace the operating system's OpenSSL or C++ runtime. If a non-system
runtime must be bundled, extend the dependency inventory and matching source
archive before publishing. The current scripts stop if dependencies cannot be
resolved; Kylin is not considered verified until its native build passes.

## Create the source snapshot

Increase the version in `CMakeLists.txt` before replacing a public release.
Initialize pinned dependency checkouts with `sh scripts/build-deps.sh` if they
are not already available. The source archiver verifies the exact dependency
commits and refuses dirty checkouts. It includes their complete source and
notices so the GPL binary downloads have matching source available.

```sh
python3 tests/release_test.py
python3 scripts/release.py source
```

This writes `dist/source/goblin-scry-VERSION-source.tar.gz` and its SHA-256 file.
An existing archive is never silently overwritten. All targets must use the
same archive checksum. Keep the source archive while any binary is available.

## Build and test all four native targets

```sh
python3 scripts/release.py build-native \
  --source dist/source/goblin-scry-0.1.0-source.tar.gz \
  --output dist/local --label local >dist/local-build.log 2>&1

python3 scripts/release.py build-hosts \
  --source dist/source/goblin-scry-0.1.0-source.tar.gz
```

`build-hosts` uses the existing SSH aliases and builds on all three hosts. Use
`--host naamah` to retry one target, or `--remote-python /path/to/python3.12`
when the host's default Python is too old. Each host gets an isolated directory
keyed by the source checksum; nothing is installed globally. Build logs and
artifacts return to `dist/HOST/`. `--suite` is available on `build-native` for
OS releases without `VERSION_CODENAME`; the Loongson workflow supplies `kylin`.

Native builds compile the pinned protocol libraries, run all CTest tests, stage
an installation, relocate private shared libraries, and exercise the packaged
executable through the terminal integration test without library-path overrides.
macOS private libraries are ad-hoc signed. Linux dependencies are calculated by
`dpkg-shlibdeps`; missing metadata or libraries are errors. Build manifests
record the source checksum, OS, architecture, compiler, dependencies, tests,
binary checksum, and file size.

The Mac artifact is a prebuilt tar archive consumed by a Homebrew formula,
not a universal macOS binary. The recipe targets arm64/macOS 26 with Homebrew
at `/opt/homebrew` and declares
OpenSSL 3 and libjpeg-turbo dependencies. The Linux artifacts are native `.deb`
packages for the suite/ABI on which they were built.

## Publish Brew and APT

```sh
python3 scripts/publish-repositories.py --stage dist --check
python3 scripts/deploy.py packages
```

Publication refuses missing builds, failed tests, changed artifact bytes, or
different source snapshots. Uploads use the rsync daemon destinations
`hail::distribution/brew.goblinreactor.com` and
`hail::distribution/apt.goblinreactor.com`. Because `hail` can be only an SSH
alias, the script resolves its configured hostname using `ssh -G` for rsync.

The repository publisher runs on hail and uses the existing private
`/home/adam/.local/share/goblin-apt` administration directory. Its signing key,
GnuPG home, reprepro database, and credentials stay on hail. The public mount
is `/mnt/distribution`. Both publisher locks are taken before changes.

The publisher backs up the repository metadata and tap head, adds immutable
artifacts and matching source, commits only the Scry formula to the existing
tap, runs `git update-server-info`, imports the `.deb` files through reprepro,
and verifies signed APT indexes. It merges Scry entries into the existing
catalog without dropping other packages, then signs and verifies the catalog
using the existing archive key:

```text
B7B6 1E8C B85A 9270 5303 64F2 3273 CB62 402F C793
```

The publisher never creates an unsigned replacement repository, rewrites the
tap history, or deletes old packages. Repository configuration must already
contain the target suites and architectures; inspect it on hail before the
first release. Backups are under the upload stage's `before-publication-*`.
If publication fails midway, fix the cause and rerun with the same immutable
artifacts. Restore metadata/database/tap head from the backup only when no
other publication has occurred; hold the same locks while restoring.

After publishing, verify public installation on each target:

```sh
brew tap goblinreactor/tap https://brew.goblinreactor.com/homebrew-tap.git
brew update
brew install goblinreactor/tap/goblin-scry
brew test goblinreactor/tap/goblin-scry

# On each Linux host, after configuring the signed repository for its own suite:
sudo apt update
sudo apt install goblin-scry
goblin-scry --version
goblin-scry --help
```

Use <https://apt.goblinreactor.com/> for signed repository setup. Keep TLS and
signature checking enabled. The published manifest initially says
`public_install: pending`; native package tests do not imply that the public
repository installation has passed. Record actual public install results in
the release report before declaring the release fully verified.

## Website and nginx

The website files are deliberately not in Git. Keep a separate backup of
`html/`; a fresh source clone alone does not contain the website. Review the
page's release availability before deployment, so pending builds are never
advertised as published downloads.

```sh
# First deployment: add the origin virtual host, then upload/verify the website.
python3 scripts/deploy.py nginx
python3 scripts/deploy.py website
```

The nginx configuration in `packaging/nginx/scry.goblinreactor.com.conf` maps
Host `scry.goblinreactor.com` to
`http://192.168.1.11:8888/scry.goblinreactor.com/`. It follows the existing
Goblin Skiff origin arrangement. The public CDN/TLS configuration is managed
separately; this script does not create DNS records or certificates.

`nginx` uploads the reviewed config and installer through SSH, saves the
previous configuration under `/var/tmp/goblin-scry-nginx-backup.*`, validates
with `nginx -t`, and reloads only after validation. It restores the previous
configuration on a validation/reload failure. If an existing Scry vhost has
TLS settings, it stops rather than discarding those settings; merge the proxy
location into the existing vhost and validate it.

`website` uploads assets first and `index.html` last through
`hail::distribution/scry.goblinreactor.com`, without deleting unrelated files.
It checks the uploaded index checksum, the backend path, the local nginx Host
route, and public HTTPS. If DNS or CDN configuration is missing, it reports
the failed verification without claiming the site is live.

## Commit and push source

```sh
git add .gitignore CMakeLists.txt README.md LICENSE THIRD_PARTY.md \
  src tests scripts packaging licenses docs
git diff --cached --stat
git diff --cached --name-only
python3 scripts/check-repository.py
git commit -m 'License Goblin Scry and add native release and deployment tooling'
git push -u origin main
```

Review the staged filenames: no `html/`, binary artifacts, vendored build
trees, private signing material, or agent instructions belong in the source
repository. If the remote already contains commits, fetch and integrate its
history first; do not force-push over it.
