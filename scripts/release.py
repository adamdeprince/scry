#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Create matching source archives and native, tested Goblin Scry packages."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PINS = {
    'freerdp': '955ad49545919c35ed731bc7716b26ddf888833a',
    'libvncserver': '9b54b1ec32731bd23158ca014dc18014db4194c3',
    'openssl': 'c8bd5a57108599ac650bbae77fcabe3109dab2e8',
}
SOURCE_ITEMS = ('CMakeLists.txt', 'README.md', 'LICENSE', 'THIRD_PARTY.md',
                '.gitignore', 'src', 'tests', 'scripts', 'packaging', 'licenses', 'docs')
EXCLUDED = {'.git', '.agents', '.codex', '.claude', '__pycache__', 'AGENTS.md',
            'SKILL.md', 'CLAUDE.md', 'html'}


def run(*command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, **kwargs)


def output(*command, **kwargs):
    return run(*command, text=True, stdout=subprocess.PIPE, **kwargs).stdout.strip()


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        while True:
            block = stream.read(1024 * 1024)  # Streaming I/O chunk, not an input limit.
            if not block:
                break
            result.update(block)
    return result.hexdigest()


def version(root=ROOT):
    return re.search(r'project\(goblin_scry VERSION ([0-9.]+)',
                     (root / 'CMakeLists.txt').read_text()).group(1)


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n')


def source_filter(info):
    parts = Path(info.name).parts
    if any(part in EXCLUDED for part in parts) or info.name.endswith(('.pyc', '.o', '.a', '.dylib')):
        return None
    info.uid = info.gid = 0
    info.uname = info.gname = 'root'
    return info


def archive_metadata(info):
    info.uid = info.gid = 0
    info.uname = info.gname = 'root'
    return info


def source_archive(args):
    dest = args.output.resolve()
    dest.mkdir(parents=True, exist_ok=True)
    archive = dest / ('goblin-scry-' + version() + '-source.tar.gz')
    # Source archives are immutable. A new release version is needed for changes.
    if archive.exists():
        raise RuntimeError('Source archive already exists: ' + str(archive))
    with tempfile.TemporaryDirectory(prefix='scry-source-') as temporary:
        stage = Path(temporary) / ('goblin-scry-' + version())
        stage.mkdir()
        for name in SOURCE_ITEMS:
            source = ROOT / name
            if not source.exists():
                continue
            if source.is_dir():
                shutil.copytree(source, stage / name, ignore=shutil.ignore_patterns(*EXCLUDED, '*.pyc'))
            else:
                shutil.copy2(source, stage / name)
        for name, commit in PINS.items():
            checkout = args.dependencies / name
            if output('git', '-C', checkout, 'rev-parse', 'HEAD') != commit:
                raise RuntimeError('Dependency commit mismatch: ' + name)
            if output('git', '-C', checkout, 'status', '--porcelain'):
                raise RuntimeError('Dependency has unrecorded changes: ' + name)
            vendor = stage / 'vendor' / name
            vendor.mkdir(parents=True)
            with tempfile.TemporaryFile() as stream:
                run('git', '-C', checkout, 'archive', commit, stdout=stream)
                stream.seek(0)
                with tarfile.open(fileobj=stream) as upstream:
                    upstream.extractall(vendor, filter='data')
        files = {str(p.relative_to(stage)): digest(p) for p in sorted(stage.rglob('*')) if p.is_file()}
        write_json(stage / 'SOURCE_MANIFEST.json', {'version': version(), 'dependencies': PINS, 'files': files})
        with tarfile.open(archive, 'w:gz') as tar:
            tar.add(stage, arcname=stage.name, filter=archive_metadata)
    (dest / (archive.name + '.sha256')).write_text(digest(archive) + '  ' + archive.name + '\n')
    print(archive)


def extract_source(archive, target):
    with tarfile.open(archive) as tar:
        tar.extractall(target, filter='data')
    roots = list(target.iterdir())
    if len(roots) != 1 or not roots[0].is_dir():
        raise RuntimeError('Expected one source root')
    root = roots[0]
    manifest = json.loads((root / 'SOURCE_MANIFEST.json').read_text())
    if manifest['dependencies'] != PINS:
        raise RuntimeError('Unreviewed dependency versions')
    for name, expected in manifest['files'].items():
        path = (root / name).resolve()
        if not path.is_relative_to(root.resolve()) or digest(path) != expected:
            raise RuntimeError('Source checksum mismatch: ' + name)
    return root


def dylibs(binary):
    return [line.strip().split(' (', 1)[0] for line in output('otool', '-L', binary).splitlines()[1:]]


def relocate_macos(stage, dependency_prefix):
    library_dir = stage / 'lib/goblin-scry'
    library_dir.mkdir(parents=True)
    names = ('libfreerdp3.3.dylib', 'libwinpr3.3.dylib', 'libvncclient.1.dylib')
    for name in names:
        shutil.copy2(dependency_prefix / 'lib' / name, library_dir / name)
    binaries = [stage / 'bin/goblin-scry'] + sorted(library_dir.iterdir())
    allowed = ('/usr/lib/', '/System/Library/', '/opt/homebrew/opt/openssl@3/',
               '/opt/homebrew/opt/jpeg-turbo/')
    for binary in binaries:
        if binary.parent == library_dir:
            run('install_name_tool', '-id', '@rpath/' + binary.name, binary)
        for dep in dylibs(binary):
            if Path(dep).name in names:
                run('install_name_tool', '-change', dep, '@rpath/' + Path(dep).name, binary)
            elif not dep.startswith(allowed):
                raise RuntimeError('Undeclared runtime dependency: ' + dep)
        commands = output('otool', '-l', binary)
        rpaths = re.findall(r'cmd LC_RPATH\n.*?\n\s*path (.*?) \(offset', commands)
        for old in rpaths:
            run('install_name_tool', '-delete_rpath', old, binary)
        relative = '@loader_path' if binary.parent == library_dir else '@executable_path/../lib/goblin-scry'
        run('install_name_tool', '-add_rpath', relative, binary)
        run('codesign', '--force', '--sign', '-', binary)
        run('codesign', '--verify', '--strict', binary)
    return {str(p.relative_to(stage)): dylibs(p) for p in binaries}


def relocate_linux(stage, dependency_prefix):
    library_dir = stage / 'lib/goblin-scry'
    library_dir.mkdir(parents=True)
    names = ['libfreerdp3.so.3', 'libwinpr3.so.3', 'libvncclient.so.1']
    if (dependency_prefix / 'lib/libssl.so.3').is_file():
        names += ['libssl.so.3', 'libcrypto.so.3']
    for name in names:
        candidates = list(dependency_prefix.rglob(name))
        if len(candidates) != 1:
            raise RuntimeError('Cannot locate private library: ' + name)
        shutil.copy2(candidates[0], library_dir / name)
    binaries = [stage / 'bin/goblin-scry'] + sorted(library_dir.iterdir())
    dependencies = {}
    for binary in binaries:
        run('patchelf', '--set-rpath', '$ORIGIN' if binary.parent == library_dir else '$ORIGIN/../lib/goblin-scry', binary)
    # Inspect only after every library has its final runpath, including libcrypto.
    for binary in binaries:
        dependencies[str(binary.relative_to(stage))] = output('ldd', binary)
        if 'not found' in dependencies[str(binary.relative_to(stage))]:
            raise RuntimeError('Unresolved dependencies for ' + str(binary))
        for resolved in re.findall(r'=> (/\S+)', dependencies[str(binary.relative_to(stage))]):
            path = Path(resolved).resolve()
            if not any(path.is_relative_to(base) for base in (stage.resolve(), Path('/usr/lib'), Path('/lib'), Path('/lib64'))):
                raise RuntimeError('Dependency outside package or system library directories: ' + str(path))
    return dependencies


def make_deb(root, stage, dest, package_version, architecture):
    package = dest / ('goblin-scry_' + package_version + '_' + architecture + '.deb')
    with tempfile.TemporaryDirectory(prefix='scry-deb-') as temporary:
        work = Path(temporary)
        package_root = work / 'package'
        shutil.copytree(stage, package_root / 'usr')
        control = package_root / 'DEBIAN'
        control.mkdir()
        (work / 'debian').mkdir()
        (work / 'debian/control').write_text('Source: goblin-scry\n\nPackage: goblin-scry\nArchitecture: any\nDescription: Graphical desktops in your terminal\n')
        private_shlibs = 'libfreerdp3 3 goblin-scry\nlibwinpr3 3 goblin-scry\nlibvncclient 1 goblin-scry\n'
        if (stage / 'lib/goblin-scry/libssl.so.3').is_file():
            private_shlibs += 'libssl 3 goblin-scry\nlibcrypto 3 goblin-scry\n'
        (work / 'debian/shlibs.local').write_text(private_shlibs)
        lib = package_root / 'usr/lib/goblin-scry'
        binaries = [package_root / 'usr/bin/goblin-scry'] + sorted(lib.iterdir())
        dependencies = output('dpkg-shlibdeps', '-O', '-xgoblin-scry', '-l' + str(lib),
                              *['-e' + str(p) for p in binaries], cwd=work)
        depends = next(line.split('=', 1)[1] for line in dependencies.splitlines() if line.startswith('shlibs:Depends='))
        installed = sum(p.stat().st_size for p in package_root.rglob('*') if p.is_file())
        (control / 'control').write_text(
            'Package: goblin-scry\nVersion: ' + package_version + '\nArchitecture: ' + architecture + '\n'
            'Maintainer: Adam DePrince <adam@goblinreactor.com>\nSection: x11\nPriority: optional\n'
            'Homepage: https://scry.goblinreactor.com/\nInstalled-Size: ' + str((installed + 1023) // 1024) + '\n'
            'Depends: ' + depends + ', ca-certificates\nSuggests: tigervnc-standalone-server, x11-xserver-utils, sway, wayvnc\n'
            'Description: RDP, VNC, X11 and Wayland desktops inside a terminal\n'
            ' Renders graphical sessions through Kitty graphics with keyboard and mouse input.\n')
        copyright_file = package_root / 'usr/share/doc/goblin-scry/copyright'
        copyright_file.write_text((root / 'THIRD_PARTY.md').read_text() + '\n' + (root / 'LICENSE').read_text())
        run('dpkg-deb', '--root-owner-group', '--build', package_root, package)
        unpacked = work / 'unpacked'
        run('dpkg-deb', '-x', package, unpacked)
        run(unpacked / 'usr/bin/goblin-scry', '--version')
    return package, depends + ', ca-certificates'


def build_native(args):
    source = args.source.resolve()
    expected = args.sha256 or (source.parent / (source.name + '.sha256')).read_text().split()[0]
    if digest(source) != expected:
        raise RuntimeError('Source archive checksum mismatch')
    dest = args.output.resolve()
    dest.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='goblin-scry-build-', dir=args.work_dir))
    print('Build directory:', work, flush=True)
    extracted = work / 'source'
    extracted.mkdir()
    root = extract_source(source, extracted)
    release_version = version(root)
    system, machine = platform.system(), platform.machine()
    if system == 'Darwin' and (machine != 'arm64' or platform.mac_ver()[0].split('.')[0] != '26'):
        raise RuntimeError('The current Homebrew recipe targets Apple Silicon on macOS 26; add a tested recipe for this target')
    env = os.environ.copy()
    env['GOBLIN_DEPENDENCY_PREFIX'] = str(work / 'deps')
    env['GOBLIN_DEPENDENCY_WORK'] = str(work / 'deps-work')
    env.setdefault('BUILD_JOBS', str(os.cpu_count() or 1))
    if system == 'Darwin':
        env['PKG_CONFIG_PATH'] = '/opt/homebrew/opt/openssl@3/lib/pkgconfig:/opt/homebrew/opt/jpeg-turbo/lib/pkgconfig:' + env.get('PKG_CONFIG_PATH', '')
        env['CMAKE_PREFIX_PATH'] = '/opt/homebrew/opt/openssl@3;/opt/homebrew/opt/jpeg-turbo;' + env.get('CMAKE_PREFIX_PATH', '')
    run('sh', root / 'scripts/build-deps.sh', env=env)
    prefix = work / 'deps'
    pkgdirs = sorted(str(p) for p in prefix.rglob('pkgconfig') if p.is_dir())
    env['PKG_CONFIG_PATH'] = os.pathsep.join(pkgdirs + [env.get('PKG_CONFIG_PATH', '')])
    build = work / 'build'
    run('cmake', '-S', root, '-B', build, '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_INSTALL_LIBDIR=lib', '-DCMAKE_INSTALL_PREFIX=/usr',
        '-DPython3_EXECUTABLE=' + sys.executable, env=env)
    run('cmake', '--build', build, '--parallel', env['BUILD_JOBS'], env=env)
    run('ctest', '--test-dir', build, '--output-on-failure', env=env)
    stage = work / 'stage'
    run('cmake', '--install', build, '--prefix', stage, env=env)
    libraries = relocate_macos(stage, prefix) if system == 'Darwin' else relocate_linux(stage, prefix)
    clean_env = env.copy()
    clean_env.pop('DYLD_LIBRARY_PATH', None)
    clean_env.pop('LD_LIBRARY_PATH', None)
    run(sys.executable, root / 'tests/terminal_test.py', stage / 'bin/goblin-scry', env=clean_env)
    help_text = output(stage / 'bin/goblin-scry', '--help', env=clean_env)
    if 'FreeRDP3 LibVNCClient' not in help_text:
        raise RuntimeError('A required protocol backend is missing')
    doc = stage / 'share/doc/goblin-scry'
    doc.mkdir(parents=True, exist_ok=True)
    host_os = platform.platform()
    suite = None
    if system == 'Linux':
        release = dict(line.split('=', 1) for line in Path('/etc/os-release').read_text().splitlines() if '=' in line)
        release = {k: v.strip('"') for k, v in release.items()}
        host_os = release.get('PRETTY_NAME', host_os)
        suite = args.suite or release.get('VERSION_CODENAME')
        if not suite or not re.fullmatch('[a-z][a-z0-9]*', suite):
            raise RuntimeError('Pass --suite for a distribution without VERSION_CODENAME')
    manifest = {'project': 'goblin-scry', 'version': release_version, 'build_host': args.label,
                'os': host_os, 'machine': machine, 'suite': suite, 'source': source.name,
                'source_sha256': expected, 'license': 'GPL-3.0-only', 'libraries': libraries,
                'tests': {'ctest': 'passed', 'packaged_terminal': 'passed', 'backends': 'passed'},
                'compiler': output('c++', '--version'),
                'openssl': output('pkg-config', '--modversion', 'openssl', env=env),
                'bundled_openssl': (prefix / 'lib/libssl.so.3').is_file()}
    write_json(doc / 'build-info.json', manifest)
    base = 'brew' if system == 'Darwin' else 'apt'
    (doc / 'SOURCE.txt').write_text('Complete matching source (including bundled libraries):\nhttps://' + base + '.goblinreactor.com/sources/' + source.name + '\nSHA-256: ' + expected + '\n')
    if system == 'Darwin':
        package = dest / ('goblin-scry-' + release_version + '-macos26-arm64.tar.gz')
        with tarfile.open(package, 'w:gz') as tar:
            for child in sorted(stage.iterdir()):
                tar.add(child, arcname=child.name, filter=archive_metadata)
        formula = (root / 'packaging/homebrew/goblin-scry.rb.in').read_text()
        for key, value in {'VERSION': release_version, 'ARCHIVE': package.name, 'SHA256': digest(package)}.items():
            formula = formula.replace('@' + key + '@', value)
        (dest / 'goblin-scry.rb').write_text(formula)
        run('ruby', '-c', dest / 'goblin-scry.rb')
        manifest.update(format='homebrew-binary', architecture='arm64', dependencies=['openssl@3', 'jpeg-turbo'])
    else:
        architecture = output('dpkg', '--print-architecture')
        package_version = release_version + '-1+' + suite + '1'
        package, depends = make_deb(root, stage, dest, package_version, architecture)
        manifest.update(format='deb', architecture=architecture, package_version=package_version, depends=depends)
    manifest.update(artifact=package.name, sha256=digest(package), size=package.stat().st_size)
    write_json(dest / 'build-info.json', manifest)
    (dest / (package.name + '.sha256')).write_text(digest(package) + '  ' + package.name + '\n')
    print('Packaged:', package, flush=True)


def build_hosts(args):
    source = args.source.resolve()
    expected = digest(source)
    if not re.fullmatch('[0-9a-f]{64}', expected):
        raise RuntimeError('Invalid source hash')
    remote = '/tmp/goblin-scry-release-' + expected
    settings = json.loads((ROOT / 'packaging/build-hosts.json').read_text())
    # Only fixed defaults/explicit CLI host names are accepted; no shell interpolation.
    def build(host):
        if not re.fullmatch('[a-zA-Z0-9_.@-]+', host) or host.startswith('-'):
            raise RuntimeError('Invalid host name')
        config = settings.get(host, {})
        destination = config.get('ssh_host', host)
        run('ssh', '-S', 'none', destination, shlex.join(['mkdir', '-p', remote]))
        run('scp', '-o', 'ControlPath=none', source, str(source) + '.sha256', __file__, destination + ':' + remote + '/')
        environment = config.get('environment', {})
        command = ['env'] + [key + '=' + value for key, value in environment.items()]
        command += [args.remote_python or config.get('python', 'python3'), remote + '/release.py', 'build-native',
                   '--source', remote + '/' + source.name, '--sha256', expected,
                   '--output', remote + '/artifacts', '--label', host]
        if host == 'loongson':
            command += ['--suite', 'kylin']
        dest = args.output / host
        dest.mkdir(parents=True, exist_ok=True)
        with (dest / 'build.log').open('w') as log:
            run('ssh', '-S', 'none', destination, shlex.join(command), stdout=log, stderr=subprocess.STDOUT)
        run('rsync', '-a', '-e', 'ssh -S none', destination + ':' + remote + '/artifacts/', str(dest) + '/')
        return host
    hosts = args.host or ['naamah', 'graviton5', 'loongson']
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)) as executor:
        futures = {executor.submit(build, host): host for host in hosts}
        failures = []
        for future in concurrent.futures.as_completed(futures):
            try:
                print('Built:', future.result())
            except Exception as error:
                failures.append(futures[future])
                print('FAILED:', futures[future], error, file=sys.stderr)
        if failures:
            raise RuntimeError('Native builds failed: ' + ', '.join(failures))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    source = commands.add_parser('source')
    source.add_argument('--output', type=Path, default=ROOT / 'dist/source')
    source.add_argument('--dependencies', type=Path, default=ROOT / 'build/deps-work')
    source.set_defaults(action=source_archive)
    native = commands.add_parser('build-native')
    native.add_argument('--source', type=Path, required=True)
    native.add_argument('--sha256')
    native.add_argument('--output', type=Path, required=True)
    native.add_argument('--work-dir', type=Path)
    native.add_argument('--label', default='local')
    native.add_argument('--suite')
    native.set_defaults(action=build_native)
    hosts = commands.add_parser('build-hosts')
    hosts.add_argument('--source', type=Path, required=True)
    hosts.add_argument('--output', type=Path, default=ROOT / 'dist')
    hosts.add_argument('--host', action='append')
    hosts.add_argument('--remote-python')
    hosts.set_defaults(action=build_hosts)
    args = parser.parse_args()
    args.action(args)


if __name__ == '__main__':
    main()
