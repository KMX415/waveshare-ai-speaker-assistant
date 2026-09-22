# OTA preview: 0.7.0-dev3 (release 702)

Development prerelease for the Waveshare ESP32-S3-AUDIO-Board. This is an application-only OTA test, **not a first-time USB installation bundle**. It requires the migrated two-slot partition layout and matching maintainer public verification key.

Changes:

- Signed firmware manifests and HTTPS downloads to an inactive application slot.
- Image size, digest, board/layout/asset compatibility and application-descriptor checks before boot selection.
- Boot health validation with ESP-IDF rollback support.
- Update checks/installation queued until audio is idle, with an amber maintenance indicator.
- Permanent room identity separate from assistant name, wake phrase and weather location.
- Spoken firmware status/update requests and room naming.
- Fix GitHub asset redirects whose request URLs exceed the HTTP client's default transmit buffer; wait for a Wi-Fi address before checking and report HTTP diagnostics.

Validation before publication: 44 host tests passed; migration on the Kitchen speaker preserved its encrypted API key and preferences; hardware signature checks passed; live conversation/tool checks and actual reboot persistence passed. An unavailable release check exited safely.

Hardware result after publication: Kitchen downloaded release 702 over Wi-Fi, booted it, completed local health acceptance and retained the new release through a second reboot. Its room identity, encrypted-key availability, voice and microphone preferences were preserved.

Power-cut and failed-boot rollback tests, cross-speaker pairing, wake arbitration, intercom, shared memory, handoff and household update scheduling are **not yet validated or released**.

Assets: `update.json` (signed manifest) and `home-voice-0.7.0-dev3.bin` (application image). No credentials, settings backups, private signing keys, partition tables or wake-model images are included. Normal stable update checks do not select this prerelease; the physical USB development interface explicitly selects the preview channel. The versioned release preserves the matching source; the `ota-preview` release serves the current preview manifest. Release 701 is superseded because its updater cannot follow long GitHub asset redirects.
