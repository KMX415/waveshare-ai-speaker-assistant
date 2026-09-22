import base64
import json
import unittest
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric import ec
from home_voice.releases import make_payload, sign_manifest, verify_manifest


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.key = ec.generate_private_key(ec.SECP256R1())
        self.image = b'\xe9' + b'\0' * 127
        self.payload = make_payload(self.image, 'https://example.org/app.bin', '0.7.0', 700)
        self.envelope = sign_manifest(self.payload, self.key)

    def test_valid_image(self):
        self.assertEqual(verify_manifest(self.envelope, self.key.public_key(), image=self.image), self.payload)

    def test_wrong_signer(self):
        with self.assertRaises(InvalidSignature):
            verify_manifest(self.envelope, ec.generate_private_key(ec.SECP256R1()).public_key())

    def test_tamper(self):
        raw = base64.b64decode(self.envelope['payload']).replace(b'0.7.0', b'0.9.0')
        self.envelope['payload'] = base64.b64encode(raw).decode()
        with self.assertRaises(InvalidSignature):
            verify_manifest(self.envelope, self.key.public_key())

    def test_corruption_and_truncation(self):
        for image in (self.image[:-1], b'\xe9' + b'x' * 127):
            with self.assertRaises(ValueError):
                verify_manifest(self.envelope, self.key.public_key(), image=image)

    def test_replay_or_downgrade(self):
        for release in (700, 701):
            with self.assertRaises(ValueError):
                verify_manifest(self.envelope, self.key.public_key(), current_release=release)

    def test_incompatible_payload(self):
        for field, value in [('board','other'), ('layout',2), ('assets','other'),
                             ('size',0x400000), ('release',True), ('url','http://example.org/a'),
                             ('url','https://user:pass@example.org/a'), ('version','a\nb')]:
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                sign_manifest(dict(self.payload, **{field:value}), self.key)

    def test_partition_migration_preserves_data(self):
        from pathlib import Path
        def table(name):
            rows = (line.split(',') for line in Path('firmware', name).read_text().splitlines()
                    if line and not line.startswith('#'))
            return {row[0]: (int(row[3],0), int(row[4],0)) for row in rows}
        old, new = table('partitions.csv'), table('partitions-ota.csv')
        for name in ('nvs','phy_init','model','credentials'):
            self.assertEqual(old[name], new[name])
        ordered = sorted(new.values())
        for (offset,size), (next_offset,_) in zip(ordered, ordered[1:]):
            self.assertLessEqual(offset+size, next_offset)
        self.assertLessEqual(ordered[-1][0]+ordered[-1][1], 0x1000000)
        self.assertEqual(old['factory'], new['ota_0'])
