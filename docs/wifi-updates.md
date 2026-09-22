# Wi-Fi updates (in development)

The multiroom branch introduces signed application updates. This is not yet a published OTA release, and the Kitchen speaker must pass migration and rollback tests before the other speakers are upgraded.

## One-time migration

The original partition table has one factory application. `firmware/partitions-ota.csv` adds two OTA slots and OTA metadata while preserving the offsets and sizes of ordinary settings, wake models, and encrypted credentials. The smaller application slot is 0x3f0000 bytes. A normal application-only OTA release cannot update the partition table, bootloader or shared wake models.

`firmware/build.ps1 -OtaLayout` builds this layout without writing to a board. Adding `-Flash -Port COM14` is a USB migration and must only be used after confirming the connected board identity and backing up its data. An existing OTA device must continue using the OTA layout for USB updates; the legacy layout is not a supported downgrade path.

The migration does not provision hardware keys or modify eFuses. Each new board still needs its own initial credential-storage provisioning.

## Release signing

Install development tools with `pip install -e ".[release-tools]"`. The initial maintainer signing private key is stored locally under ignored `secrets/ota-signing.pem`; back it up securely. Only `firmware/main/update_public_key.h` belongs in source or on devices. Other maintainers should generate and pin their own public key for their own fleet.

The envelope contains base64 `payload` and `signature`. ECDSA P-256/SHA-256 authenticates the exact decoded payload bytes. The payload specifies board, partition layout, wake-asset compatibility, monotonically increasing release number, application version, byte length, SHA-256 digest, and an HTTPS image URL. Both slots must accommodate the image. Firmware versions and compiled release counters must agree with the manifest; never reuse a release number for different firmware.

Use `scripts/sign_release.py --help` to create a manifest from the **application image**, never a merged flash backup. The script creates a new output file and does not publish anything. Release bundles must never include NVS, credential partitions, API keys, Wi-Fi passwords or the private signing key.

The default check URL is the repository's latest GitHub release asset named `update.json`. A release must contain that signed manifest and the corresponding versioned application image before devices can use it. Publishing a source commit alone does not create an update.

## Installation behavior

Checking and installing wait until conversation, calibration and playback are idle. Audio ownership excludes simultaneous live connection startup. Downloads use verified HTTPS, including redirects. The application is streamed into the inactive slot; length, digest, app descriptor and ESP image integrity are checked before changing the boot selection. A failed download leaves the running application selected.

On an OTA boot, local startup checks decide whether to accept the new firmware. Internet/API availability must not be part of that decision. A failing candidate rolls back to the prior slot. Physical power-cut, malformed-manifest, image-corruption and failed-boot tests remain required before production rollout.

USB `u` reads update status; `u` followed by byte 1 queues a check, and byte 2 queues installation of a newer signed release. These physical development commands do not return credentials.

## Current validation

- Host tests verify signature rejection, corruption/truncation, replay/downgrade rejection, compatibility fields, and data-partition preservation.
- Firmware compilation and hardware migration/rollback are separate validation gates.
- Multi-speaker update scheduling and management outside the home are tracked in [the multiroom plan](multiroom-plan.md).
