"""Signed application-only OTA manifests. Requires the release-tools extra.

Signing keys are separate from OpenAI credentials and never go on a speaker.
The signature covers the exact UTF-8 payload bytes, not reserialized JSON.
"""
import base64
import hashlib
import json
import re
from urllib.parse import urlsplit

BOARD = "waveshare-esp32-s3-audio"
LAYOUT = 1
ASSETS = "esp-sr-2.1.5-wn9-jarvis-computer"
MAX_IMAGE = 0x3F0000
FIELDS = {"board", "layout", "assets", "release", "version", "size", "sha256", "url"}


def validate_payload(value):
    if not isinstance(value, dict) or set(value) != FIELDS:
        raise ValueError("Unexpected manifest fields")
    if value["board"] != BOARD or type(value["layout"]) is not int or value["layout"] != LAYOUT or value["assets"] != ASSETS:
        raise ValueError("Incompatible board, partition layout or wake assets")
    if type(value["release"]) is not int or not 1 <= value["release"] <= 2**31 - 1:
        raise ValueError("Invalid release counter")
    if type(value["size"]) is not int or not 32 <= value["size"] <= MAX_IMAGE:
        raise ValueError("Image exceeds the smaller OTA slot")
    if not isinstance(value["version"], str) or not re.fullmatch(r"[A-Za-z0-9._-]{1,31}", value["version"]):
        raise ValueError("Invalid version")
    if not isinstance(value["sha256"], str) or not re.fullmatch(r"[a-f0-9]{64}", value["sha256"]):
        raise ValueError("Invalid image digest")
    url = value["url"]
    if not isinstance(url, str) or len(url) > 384 or any(ord(c) <= 32 or ord(c) >= 127 for c in url):
        raise ValueError("Invalid image URL")
    parsed = urlsplit(url)
    if parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password or parsed.fragment:
        raise ValueError("Image URL must be HTTPS without credentials or fragment")
    return value


def make_payload(image: bytes, url: str, version: str, release: int):
    if not image or image[0] != 0xE9:
        raise ValueError("Expected an ESP application image, not a merged flash backup")
    return validate_payload(dict(board=BOARD, layout=LAYOUT, assets=ASSETS,
        release=release, version=version, size=len(image),
        sha256=hashlib.sha256(image).hexdigest(), url=url))


def sign_manifest(payload, private_key):
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec
    if not isinstance(private_key, ec.EllipticCurvePrivateKey) or not isinstance(private_key.curve, ec.SECP256R1):
        raise ValueError("Release key must be ECDSA P-256")
    raw = json.dumps(validate_payload(payload), separators=(",", ":"), ensure_ascii=True).encode()
    signature = private_key.sign(raw, ec.ECDSA(hashes.SHA256()))
    return dict(payload=base64.b64encode(raw).decode(), signature=base64.b64encode(signature).decode())


def verify_manifest(envelope, public_key, *, current_release=0, image=None):
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec
    if not isinstance(envelope, dict) or set(envelope) != {"payload", "signature"}:
        raise ValueError("Invalid signature envelope")
    raw = base64.b64decode(envelope["payload"], validate=True)
    signature = base64.b64decode(envelope["signature"], validate=True)
    if not 1 <= len(raw) <= 1024 or not 1 <= len(signature) <= 80:
        raise ValueError("Oversized manifest")
    if not isinstance(public_key, ec.EllipticCurvePublicKey) or not isinstance(public_key.curve, ec.SECP256R1):
        raise ValueError("Release key must be ECDSA P-256")
    public_key.verify(signature, raw, ec.ECDSA(hashes.SHA256()))
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("Duplicate manifest key")
            result[key] = value
        return result
    payload = validate_payload(json.loads(raw, object_pairs_hook=unique))
    if payload["release"] <= current_release:
        raise ValueError("Release is not newer than the installed firmware")
    if image is not None and (len(image) != payload["size"] or hashlib.sha256(image).hexdigest() != payload["sha256"]):
        raise ValueError("Image size or digest mismatch")
    return payload
