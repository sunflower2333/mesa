# SPDX-License-Identifier: MIT
"""Synthetic PE/package regressions; never load graphics drivers."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

import stage_bundle as bundle

REVISION = 'a' * 40


# Create only the structural header bytes consumed by the offline validator.
def fake_pe(machine=0xaa64):
    data = bytearray(256)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 128)
    data[128:132] = b'PE\0\0'
    struct.pack_into('<H', data, 132, machine)
    return data


class BundleTests(unittest.TestCase):
    # Isolate every test from real build outputs and installed drivers.
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / 'build'
        self.build.mkdir()
        for name in bundle.DLLS:
            (self.build / name).write_bytes(fake_pe())
        self.probe = self.root / 'probe.exe'
        self.probe.write_bytes(fake_pe())
        self.output = self.root / 'output'
        (self.build / 'freedreno_icd.arm64.json').write_text(json.dumps({
            'ICD': {'api_version': '1.4.999'}}))

    # Produce a candidate using the real staging implementation.
    def stage(self):
        return bundle.stage(self.build, self.probe, self.output, REVISION)

    def test_stage_and_verify(self):
        receipt = self.stage()
        self.assertEqual(receipt, bundle.verify(self.output, REVISION))
        self.assertFalse(receipt['native_freedreno_gallium'])
        self.assertFalse(receipt['gpu_test_executed'])
        self.assertEqual(len(receipt['files']), 8)

    def test_missing_opengl_is_rejected(self):
        (self.build / 'opengl32.dll').unlink()
        with self.assertRaisesRegex(ValueError, 'exactly one'):
            self.stage()

    def test_duplicate_output_is_rejected(self):
        other = self.build / 'old'
        other.mkdir()
        (other / 'opengl32.dll').write_bytes(fake_pe())
        with self.assertRaisesRegex(ValueError, 'exactly one'):
            self.stage()

    def test_wrong_architecture_is_rejected(self):
        for machine in (0x8664, 0x14c, 0xa641):
            with self.subTest(machine=machine):
                (self.build / 'libgallium_wgl.dll').write_bytes(fake_pe(machine))
                with self.assertRaisesRegex(ValueError, 'AA64'):
                    self.stage()

    def test_bad_headers_are_rejected(self):
        for value in (b'', b'MZ', b'NO' + bytes(254)):
            self.probe.write_bytes(value)
            with self.assertRaisesRegex(ValueError, 'DOS header'):
                self.stage()

    def test_invalid_pe_offset_is_rejected(self):
        for offset in (0, 0xffffffff, 254):
            data = fake_pe()
            struct.pack_into('<I', data, 0x3c, offset)
            self.probe.write_bytes(data)
            with self.assertRaisesRegex(ValueError, 'PE header'):
                self.stage()

    def test_output_reuse_is_rejected(self):
        self.stage()
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.stage()

    def test_revision_is_required(self):
        for revision in ('main', 'A' * 40, 'a' * 39):
            with self.assertRaisesRegex(ValueError, 'commit SHA'):
                bundle.stage(self.build, self.probe, self.output, revision)

    def test_changed_file_is_rejected(self):
        self.stage()
        (self.output / 'opengl32.dll').write_bytes(fake_pe() + b'changed')
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            bundle.verify(self.output, REVISION)

    def test_unexpected_driver_is_rejected(self):
        self.stage()
        (self.output / 'viogpu.sys').write_bytes(b'not allowed')
        with self.assertRaisesRegex(ValueError, 'unexpected'):
            bundle.verify(self.output, REVISION)

    def test_wrong_revision_is_rejected(self):
        self.stage()
        with self.assertRaisesRegex(ValueError, 'provenance'):
            bundle.verify(self.output, 'b' * 40)

    def test_fake_native_claim_is_rejected(self):
        receipt = self.stage()
        receipt['native_freedreno_gallium'] = True
        (self.output / 'bundle.json').write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, 'provenance'):
            bundle.verify(self.output, REVISION)

    def test_icd_escape_is_rejected_even_with_matching_hash(self):
        receipt = self.stage()
        path = self.output / 'freedreno_icd.arm64.json'
        icd = json.loads(path.read_text())
        icd['ICD']['library_path'] = '..\\other.dll'
        path.write_text(json.dumps(icd))
        receipt['files'][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
        (self.output / 'bundle.json').write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, 'app-local'):
            bundle.verify(self.output, REVISION)

    def test_bad_api_version_is_rejected(self):
        (self.build / 'freedreno_icd.arm64.json').write_text('{"ICD":{"api_version":"999"}}')
        with self.assertRaisesRegex(ValueError, 'API version'):
            self.stage()

    def test_unexpected_receipt_key_is_rejected(self):
        receipt = self.stage()
        receipt['files']['../escape.dll'] = '0' * 64
        (self.output / 'bundle.json').write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, 'inventory'):
            bundle.verify(self.output, REVISION)


if __name__ == '__main__':
    unittest.main()
