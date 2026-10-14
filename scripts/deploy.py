#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Deploy Scry packages, html/, and its nginx origin route through hail."""
import argparse
import importlib.util
import json
from pathlib import Path
import shlex
import shutil
import tempfile

from release import ROOT, digest, output, run


def rsync_host(host):
    # SSH aliases are not necessarily DNS names usable by the rsync daemon.
    config = output('ssh', '-G', host)
    return next(line.split(None, 1)[1] for line in config.splitlines() if line.startswith('hostname '))


def destination(host, domain, path=''):
    return rsync_host(host) + '::distribution/' + domain + '/' + path


def packages(args):
    spec = importlib.util.spec_from_file_location('publisher', ROOT / 'scripts/publish-repositories.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    builds = module.validate(args.dist)
    identity = 'goblin-scry-' + builds['local']['version'] + '-' + builds['local']['source_sha256']
    remote_relative = '.incoming/' + identity + '/'
    remote = '/mnt/distribution/apt.goblinreactor.com/' + remote_relative
    run('ssh', args.host, shlex.join(['mkdir', '-p', remote,
        '/mnt/distribution/brew.goblinreactor.com/' + remote_relative + 'local']))
    with tempfile.TemporaryDirectory(prefix='scry-publish-') as temporary:
        stage = Path(temporary)
        shutil.copytree(args.dist / 'source', stage / 'source')
        for target in module.TARGETS:
            shutil.copytree(args.dist / target, stage / target)
        for name in ('release.py', 'publish-repositories.py'):
            shutil.copy2(ROOT / 'scripts' / name, stage / name)
        # Keep uploads off the indexed release paths until the publisher validates them.
        run('rsync', '-a', '--checksum', str(stage) + '/', destination(args.host, 'apt.goblinreactor.com', remote_relative))
        run('rsync', '-a', '--checksum', str(stage / 'local') + '/', destination(args.host, 'brew.goblinreactor.com', remote_relative + 'local/'))
        run('ssh', args.host, shlex.join(['python3', remote + 'publish-repositories.py', '--stage', remote]))


def website(args):
    index = ROOT / 'html/index.html'
    if not index.is_file():
        raise RuntimeError('html/index.html is missing. Website files are intentionally kept outside Git.')
    if 'https://scry.goblinreactor.com/' not in index.read_text():
        raise RuntimeError('Homepage canonical URL is missing')
    digest_before = digest(index)
    # Additive upload, assets first. No --delete on the shared distribution server.
    run('rsync', '-a', '--checksum', '--exclude=index.html', str(ROOT / 'html') + '/',
        destination(args.host, 'scry.goblinreactor.com'))
    run('rsync', '-a', '--checksum', index, destination(args.host, 'scry.goblinreactor.com'))
    expected = shlex.quote(digest_before)
    command = "test \"$(sha256sum /mnt/distribution/scry.goblinreactor.com/index.html | cut -d' ' -f1)\" = " + expected
    run('ssh', args.host, command)
    for url in ('http://192.168.1.11:8888/scry.goblinreactor.com/', 'http://127.0.0.1/'):
        command = ['curl', '--fail', '--silent', '--show-error', '-H', 'Host: scry.goblinreactor.com', url]
        text = output('ssh', args.host, shlex.join(command))
        if 'Goblin Scry' not in text:
            raise RuntimeError('Unexpected origin response: ' + url)
    text = output('curl', '--fail', '--silent', '--show-error', 'https://scry.goblinreactor.com/')
    if 'Goblin Scry' not in text:
        raise RuntimeError('Unexpected public HTTPS response')
    print('Verified website files, backend route, nginx Host route, and public HTTPS.')


def nginx(args):
    config = ROOT / 'packaging/nginx/scry.goblinreactor.com.conf'
    remote = '/tmp/goblin-scry-nginx-' + digest(config)
    run('ssh', args.host, shlex.join(['mkdir', '-p', remote]))
    run('scp', config, ROOT / 'scripts/install-nginx.sh', args.host + ':' + remote + '/')
    run('ssh', args.host, shlex.join(['sudo', '-n', 'sh', remote + '/install-nginx.sh', remote + '/' + config.name]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('packages', 'website', 'nginx'))
    parser.add_argument('--host', default='hail')
    parser.add_argument('--dist', type=Path, default=ROOT / 'dist')
    args = parser.parse_args()
    {'packages': packages, 'website': website, 'nginx': nginx}[args.action](args)


if __name__ == '__main__':
    main()
