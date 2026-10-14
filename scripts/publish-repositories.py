#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Run on hail to add a verified Scry release to the existing Brew/APT repositories."""
import argparse
import contextlib
import datetime
import fcntl
import html
import json
import os
from pathlib import Path
import shutil
import tempfile

from release import digest, output, run, write_json

KEY = 'B7B61E8CB85A9270530364F23273CB62402FC793'
TARGETS = ('local', 'naamah', 'graviton5', 'loongson')


def immutable(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if digest(source) != digest(destination):
            raise RuntimeError('Published artifact differs: ' + str(destination))
        return
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as stream:
        temporary = Path(stream.name)
        with source.open('rb') as content:
            shutil.copyfileobj(content, stream)
    temporary.chmod(0o644)
    temporary.replace(destination)


def validate(stage):
    builds = {}
    for target in TARGETS:
        info = json.loads((stage / target / 'build-info.json').read_text())
        if info['build_host'] != target or info['license'] != 'GPL-3.0-only':
            raise RuntimeError('Incorrect build identity: ' + target)
        if any(info['tests'].get(name) != 'passed' for name in ('ctest', 'packaged_terminal', 'backends')):
            raise RuntimeError('Missing successful tests: ' + target)
        source = stage / 'source' / info['source']
        binary = stage / target / info['artifact']
        if digest(source) != info['source_sha256'] or digest(binary) != info['sha256']:
            raise RuntimeError('Artifact checksum mismatch: ' + target)
        builds[target] = info
    if len({p['source_sha256'] for p in builds.values()}) != 1:
        raise RuntimeError('Targets were built from different sources')
    expected = {'local': ('homebrew-binary', 'arm64'), 'naamah': ('deb', 'amd64'),
                'graviton5': ('deb', 'arm64'), 'loongson': ('deb', 'loongarch64')}
    for target, fields in expected.items():
        if (builds[target]['format'], builds[target]['architecture']) != fields:
            raise RuntimeError('Unexpected platform; inspect and update target mapping: ' + target)
    return builds


def artifact(site, path):
    relative = path.relative_to(site).as_posix()
    return {'path': relative, 'url': 'https://' + site.name + '/' + relative,
            'filename': path.name, 'size': path.stat().st_size, 'sha256': digest(path)}


def publish(args):
    stage, admin, distribution = args.stage.resolve(), args.admin.resolve(), args.distribution.resolve()
    builds = validate(stage)
    release_version = builds['local']['version']
    if args.check:
        print('All four native build manifests and source/binary checksums passed.')
        return
    with contextlib.ExitStack() as stack:
        # Use the same locks as the existing Goblin publishers.
        for name in ('apt', 'brew'):
            lock = stack.enter_context((admin / (name + '-publish.lock')).open('a'))
            fcntl.flock(lock, fcntl.LOCK_EX)
        backup = stage / ('before-publication-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ'))
        backup.mkdir()
        for name in ('apt', 'brew'):
            site = distribution / (name + '.goblinreactor.com')
            for filename in ('packages.json', 'packages.json.asc', 'README.md', 'README.txt', 'index.html'):
                (backup / name).mkdir(exist_ok=True)
                shutil.copy2(site / filename, backup / name / filename)
        shutil.copytree(admin / 'reprepro/db', backup / 'apt/db')
        shutil.copytree(admin / 'reprepro/conf', backup / 'apt/conf')
        shutil.copytree(distribution / 'apt.goblinreactor.com/dists', backup / 'apt/dists')
        tap = distribution / 'brew.goblinreactor.com/homebrew-tap.git'
        git = ['git', '-c', 'safe.directory=' + str(tap)]
        (backup / 'brew/tap-parent.txt').write_text(output(*git, '--git-dir', tap, 'rev-parse', 'refs/heads/main') + '\n')
        env = dict(os.environ, GNUPGHOME=str(admin / 'gnupg'))
        # Signer must exist before changing repository metadata.
        run('gpg', '--homedir', admin / 'gnupg', '--batch', '--list-secret-keys', KEY, stdout=open(os.devnull, 'w'))
        for name in ('apt', 'brew'):
            site = distribution / (name + '.goblinreactor.com')
            source = stage / 'source' / builds['local']['source']
            immutable(source, site / 'sources' / source.name)
        brew = distribution / 'brew.goblinreactor.com'
        release_path = Path('releases') / ('goblin-scry-' + release_version)
        formula = stage / 'local/goblin-scry.rb'
        if builds['local']['sha256'] not in formula.read_text():
            raise RuntimeError('Formula checksum does not match binary archive')
        immutable(stage / 'local' / builds['local']['artifact'], brew / release_path / builds['local']['artifact'])
        immutable(formula, brew / release_path / formula.name)
        with tempfile.TemporaryDirectory(prefix='scry-tap-', dir=stage) as temporary:
            checkout = Path(temporary) / 'tap'
            run(*git, 'clone', tap, checkout)
            (checkout / 'Formula').mkdir(exist_ok=True)
            shutil.copy2(formula, checkout / 'Formula/goblin-scry.rb')
            run('git', '-C', checkout, 'add', 'Formula/goblin-scry.rb')
            if output('git', '-C', checkout, 'diff', '--cached', '--name-only'):
                run('git', '-C', checkout, '-c', 'user.name=Adam DePrince',
                    '-c', 'user.email=adam@goblinreactor.com', 'commit', '-m', 'Release Goblin Scry ' + release_version)
            parent = output(*git, '--git-dir', tap, 'rev-parse', 'refs/heads/main')
            run('git', '-C', checkout, 'merge-base', '--is-ancestor', parent, 'HEAD')
            run(*git, '--git-dir', tap, 'fetch', '--no-tags', checkout, 'refs/heads/main:refs/heads/main')
        run(*git, '--git-dir', tap, 'update-server-info')
        apt = distribution / 'apt.goblinreactor.com'
        suites = set()
        for target in TARGETS[1:]:
            info = builds[target]
            package = stage / target / info['artifact']
            for field, value in [('Package', 'goblin-scry'), ('Version', info['package_version']), ('Architecture', info['architecture'])]:
                if output('dpkg-deb', '-f', package, field) != value:
                    raise RuntimeError('Incorrect Debian package metadata: ' + field)
            destination = apt / 'pool/main/g/goblin-scry' / package.name
            if destination.exists() and digest(destination) != info['sha256']:
                raise RuntimeError('Refusing to replace existing package bytes')
            run('reprepro', '--keepunreferencedfiles', '-b', admin / 'reprepro', '--export=never',
                'includedeb', info['suite'], package, env=env)
            suites.add(info['suite'])
        run('reprepro', '--keepunreferencedfiles', '-b', admin / 'reprepro', 'check', *sorted(suites), env=env)
        run('reprepro', '--keepunreferencedfiles', '-b', admin / 'reprepro', 'export', *sorted(suites), env=env)
        for suite in suites:
            run('gpgv', '--keyring', apt / 'goblinreactor-archive-keyring.gpg', apt / 'dists' / suite / 'InRelease')
        for name, targets in [('brew', ('local',)), ('apt', TARGETS[1:])]:
            site = distribution / (name + '.goblinreactor.com')
            catalog = json.loads((site / 'packages.json').read_text())
            others = [p for p in catalog['packages'] if p['name'] != 'goblin-scry']
            retained = [p for p in catalog['packages'] if p['name'] == 'goblin-scry' and
                        (p.get('suite'), p.get('architecture')) not in {(builds[t]['suite'], builds[t]['architecture']) for t in targets}]
            records = []
            files = [site / 'sources' / builds['local']['source']]
            for target in targets:
                info = builds[target]
                binary = site / (release_path if name == 'brew' else Path('pool/main/g/goblin-scry')) / info['artifact']
                if digest(binary) != info['sha256']:
                    raise RuntimeError('Published package checksum mismatch')
                report = site / release_path / target / 'build-info.json'
                immutable(stage / target / 'build-info.json', report)
                files += [binary, report]
                record = dict(info, name='goblin-scry', source='https://' + site.name + '/sources/' + info['source'],
                              artifacts=[artifact(site, binary), artifact(site, report)], verification={'native_build': 'passed', 'public_install': 'pending'})
                records.append(record)
            catalog['packages'] = records + retained + others
            if name == 'brew':
                catalog['tap_commit'] = output(*git, '--git-dir', tap, 'rev-parse', 'refs/heads/main')
                files.append(site / release_path / 'goblin-scry.rb')
            summary = '<section id="goblin-scry"><h2>Goblin Scry</h2><p>RDP, VNC, X11 and Wayland desktops in your terminal. GPLv3. <a href="https://scry.goblinreactor.com/">Documentation and installation</a>.</p><p>Version ' + html.escape(release_version) + '.</p></section>\n'
            page = (site / 'index.html').read_text()
            import re
            if '<section id="goblin-scry">' in page:
                page = re.sub(r'<section id="goblin-scry">.*?</section>\s*', lambda _: summary, page, flags=re.S)
            elif '</main>' in page:
                page = page.replace('</main>', summary + '</main>', 1)
            else:
                raise RuntimeError('Cannot locate repository homepage insertion point')
            (site / 'index.html').write_text(page)
            note = '\n<!-- goblin-scry:start -->\n## Goblin Scry\n\nGraphical desktops in your terminal. Version ' + release_version + '. [Documentation and installation](https://scry.goblinreactor.com/). GPLv3.\n<!-- goblin-scry:end -->\n'
            for filename in ('README.md', 'README.txt'):
                path = site / filename
                text = re.sub(r'\n<!-- goblin-scry:start -->.*?<!-- goblin-scry:end -->\n', '', path.read_text(), flags=re.S)
                path.write_text(text.rstrip() + '\n' + note)
                files.append(path)
            inventory = {a['path']: a for a in catalog.get('artifacts', []) if 'path' in a}
            legacy = [a for a in catalog.get('artifacts', []) if 'path' not in a]
            if name == 'apt':
                files += [p for p in (site / 'dists').rglob('*') if p.is_file()]
            for path in files:
                inventory[path.relative_to(site).as_posix()] = artifact(site, path)
            catalog['artifacts'] = legacy + sorted(inventory.values(), key=lambda a: a['path'])
            catalog['updated'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
            candidate = stage / (name + '-packages.json')
            write_json(candidate, catalog)
            signature = candidate.with_suffix('.json.asc')
            run('gpg', '--homedir', admin / 'gnupg', '--batch', '--yes', '--local-user', KEY,
                '--armor', '--detach-sign', '--output', signature, candidate)
            run('gpgv', '--keyring', site / 'goblinreactor-archive-keyring.gpg', signature, candidate)
            for src, dst in [(candidate, site / 'packages.json'), (signature, site / 'packages.json.asc')]:
                temporary = dst.with_name(dst.name + '.scry-new')
                shutil.copy2(src, temporary)
                temporary.replace(dst)
        print('Published packages and signed indexes. Backup:', backup)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', type=Path, required=True)
    parser.add_argument('--admin', type=Path, default=Path('/home/adam/.local/share/goblin-apt'))
    parser.add_argument('--distribution', type=Path, default=Path('/mnt/distribution'))
    parser.add_argument('--check', action='store_true')
    publish(parser.parse_args())
