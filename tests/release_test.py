#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Release gates: incomplete builds, tampering, immutable artifacts and source extraction."""
import importlib.util
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import release

spec = importlib.util.spec_from_file_location('publisher', Path(release.__file__).with_name('publish-repositories.py'))
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def fixture(self):
        (self.root / 'source').mkdir()
        source = self.root / 'source/source.tar.gz'
        source.write_bytes(b'fixture source')
        for target, architecture, kind in [('local', 'arm64', 'homebrew-binary'),
                ('naamah', 'amd64', 'deb'), ('graviton5', 'arm64', 'deb'),
                ('loongson', 'loongarch64', 'deb')]:
            directory = self.root / target
            directory.mkdir()
            binary = directory / 'package'
            binary.write_bytes(target.encode())
            release.write_json(directory / 'build-info.json', {
                'build_host': target, 'version': '0.1.0', 'source': source.name,
                'source_sha256': release.digest(source), 'artifact': binary.name,
                'sha256': release.digest(binary), 'license': 'GPL-3.0-only',
                'format': kind, 'architecture': architecture,
                'tests': {'ctest': 'passed', 'packaged_terminal': 'passed', 'backends': 'passed'}})

    def test_complete_manifest(self):
        self.fixture()
        self.assertEqual(set(publisher.validate(self.root)), set(publisher.TARGETS))

    def test_missing_target(self):
        self.fixture()
        (self.root / 'loongson/build-info.json').unlink()
        with self.assertRaises(FileNotFoundError):
            publisher.validate(self.root)

    def test_binary_tampering(self):
        self.fixture()
        (self.root / 'naamah/package').write_bytes(b'changed')
        with self.assertRaisesRegex(RuntimeError, 'checksum'):
            publisher.validate(self.root)

    def test_failed_test(self):
        self.fixture()
        path = self.root / 'graviton5/build-info.json'
        data = json.loads(path.read_text())
        data['tests']['packaged_terminal'] = 'failed'
        release.write_json(path, data)
        with self.assertRaisesRegex(RuntimeError, 'successful tests'):
            publisher.validate(self.root)

    def test_installation_reports_must_match_release(self):
        self.fixture()
        builds = publisher.validate(self.root)
        for target, info in builds.items():
            release.write_json(self.root / target / 'public-install.json', {
                'build_host': target, 'version': info['version'],
                'source_sha256': info['source_sha256'], 'artifact_sha256': info['sha256'],
                'tests': {'package_manager': 'passed', 'backends': 'passed', 'terminal': 'passed'}})
        self.assertEqual(set(publisher.installation_reports(self.root, builds)), set(builds))
        path = self.root / 'loongson/public-install.json'
        report = json.loads(path.read_text())
        report['tests']['terminal'] = 'failed'
        release.write_json(path, report)
        with self.assertRaisesRegex(RuntimeError, 'verification failed'):
            publisher.installation_reports(self.root, builds)
        report['tests']['terminal'] = 'passed'
        report['artifact_sha256'] = 'different build'
        release.write_json(path, report)
        with self.assertRaisesRegex(RuntimeError, 'different build'):
            publisher.installation_reports(self.root, builds)
        report['artifact_sha256'] = builds['loongson']['sha256']
        release.write_json(path, report)
        (self.root / 'local/public-install.json').unlink()
        self.assertEqual(set(publisher.installation_reports(self.root, builds, ['loongson'])), {'loongson'})
        with self.assertRaises(FileNotFoundError):
            publisher.installation_reports(self.root, builds)

    def test_distinct_loongarch_abi(self):
        self.fixture()
        path = self.root / 'loongson/build-info.json'
        data = json.loads(path.read_text())
        data['architecture'] = 'loong64'
        release.write_json(path, data)
        with self.assertRaisesRegex(RuntimeError, 'Unexpected platform'):
            publisher.validate(self.root)

    def test_published_bytes_are_immutable(self):
        source, target = self.root / 'source', self.root / 'published'
        source.write_bytes(b'first')
        publisher.immutable(source, target)
        publisher.immutable(source, target)
        source.write_bytes(b'second')
        with self.assertRaisesRegex(RuntimeError, 'differs'):
            publisher.immutable(source, target)
        self.assertEqual(target.read_bytes(), b'first')

    def test_source_path_traversal_rejected(self):
        archive = self.root / 'bad.tar'
        with tarfile.open(archive, 'w') as tar:
            info = tarfile.TarInfo('../escaped')
            info.size = 1
            tar.addfile(info, io.BytesIO(b'x'))
        target = self.root / 'unpacked'
        target.mkdir()
        with self.assertRaises(tarfile.FilterError):
            release.extract_source(archive, target)
        self.assertFalse((self.root / 'escaped').exists())


if __name__ == '__main__':
    unittest.main()
