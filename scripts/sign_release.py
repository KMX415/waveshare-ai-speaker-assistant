"""Create a signed application-only update manifest; does not publish anything."""
import argparse
import json
from pathlib import Path
from cryptography.hazmat.primitives.serialization import load_pem_private_key
from home_voice.releases import make_payload, sign_manifest, verify_manifest

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--image', required=True, type=Path)
parser.add_argument('--key', required=True, type=Path, help='Private release key kept outside Git')
parser.add_argument('--url', required=True, help='HTTPS URL of this exact versioned app image')
parser.add_argument('--version', required=True)
parser.add_argument('--release', required=True, type=int, help='Monotonically increasing release number')
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
image = args.image.read_bytes()
key = load_pem_private_key(args.key.read_bytes(), password=None)
payload = make_payload(image, args.url, args.version, args.release)
envelope = sign_manifest(payload, key)
verify_manifest(envelope, key.public_key(), image=image)
with args.output.open('x', encoding='utf-8') as output:
    json.dump(envelope, output, separators=(',', ':'))
    output.write('\n')
print(f'Signed {args.version} ({len(image)} application bytes). Nothing published.')
