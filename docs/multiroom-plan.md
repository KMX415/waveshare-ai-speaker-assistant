# Multiroom implementation plan

One or more speakers, each operating on USB wall power and home Wi-Fi. No PC, MQTT broker, Home Assistant installation, or always-on hub is required. Each speaker continues ordinary conversations if its peers are offline.

Room labels and household membership are user configuration. Do not hardcode room names, wake-group names, or a three-device topology in firmware, tools, setup defaults, or routing. Use stable device IDs for routing and editable labels for display. Any resource-based peer limit must be explicit and documented; a three-device test setup does not define the product limit.

Validation should include two speakers with overlapping acoustic coverage and a speaker in a separate wake group. Actual room labels and group membership are chosen during setup and may change without rebuilding firmware.

Transport, privacy, replay protection and wake-group contracts are recorded in [household protocol design](household-protocol.md). That protocol is design work until its implementation and physical tests are checked off below.

## Delivery sequence and acceptance criteria

| Phase | Work | Required evidence before calling it complete |
| --- | --- | --- |
| 1. Safe Wi-Fi updates | Signed releases, staged download to an inactive firmware slot, version/status reporting, idle installation, boot health checks and rollback. One USB migration for existing speakers. | Reject unsigned, corrupted, wrong-board and incompatible-model releases; recover from interrupted download and failed boot; preserve credentials and settings. |
| 2. Household membership | Stable device identity, room naming by voice, explicit household pairing, authenticated/encrypted local messages, peer discovery and removal. | Two physical speakers discover and pair; unpaired peers cannot control playback/settings; reboot retains membership; Wi-Fi reconnect recovers. |
| 3. One wake responder | Brief local arbitration using measured wake audio level and deterministic tie breaking. Separate simultaneous requests in different rooms must still work. | Overlapping wake tests on two speakers; isolated speaker still wakes; no recurrent double answers or cross-room starvation. |
| 4. Announcements and intercom | Explicit destination/all rooms; bounded messages, acknowledgement, expiry, duplicate suppression, busy/quiet-room handling; spoken replies to the originating room. Start with spoken messages rather than an open microphone stream. | Deliver once, report missing rooms honestly, prevent echo/rebroadcast loops, preserve ongoing conversations, no playback from unauthenticated requests. |
| 5. Shared household facts | Explicit shared facts separate from existing device memories; versioned updates and tombstones for deletion; room settings stay local. | Offline peer catches up without resurrecting deleted facts; conflicting edits have deterministic resolution; existing private facts are never automatically published. |
| 6. Conversation handoff | User-requested destination and bounded conversation summary, acknowledged before source closes; explicit destination acceptance if occupied. | Destination can answer a follow-up; failed transfer leaves source usable; summaries expire and are not saved as permanent memories. |
| 7. Household controls | Voice room volume/status/update requests; one-at-a-time update scheduling; status page on a paired/authenticated local interface. | Room-targeted controls affect only the intended room; unavailable peers are reported; failed update halts the household rollout. |

## API access

One OpenAI project, preferably an individual restricted API key per speaker. Reusing one key is supported but does not share conversation state. Never copy credentials through discovery, announcements, memory sync, or handoff. Pairing secrets are separate from OpenAI credentials. Concurrent sessions share the account/project limits and incur individual usage.

## Firmware/update design

- Keep existing ordinary NVS, encrypted credential NVS, and wake-model offsets stable during the initial migration.
- Check actual application size against both OTA slots; do not overwrite model assets to make room.
- Pin an update signing public key on the device. Keep the private release-signing key outside Git and outside firmware builds/artifacts.
- Authenticate the manifest and check board, layout, model compatibility, version, image size and image digest before selecting the new boot slot.
- Firmware downloads use HTTPS. Keep serving the existing known-good firmware until validation succeeds.
- Download/install only when audio and calibration are idle. A failed update must not disable ordinary conversations.
- Boot health checks must validate local hardware and required assets. Cloud or internet downtime alone must not cause a rollback loop.
- Store versioned settings with backward compatibility so firmware rollback does not destroy preferences.
- Distinguish application-only updates from bootloader/partition/model migrations. A normal OTA release must never rewrite those shared regions.
- Publishing source to Git does not publish or install a firmware release. First update one speaker, validate it, then update the remaining speakers.

## Local network design

Use explicit household pairing and per-device identities. Discovery only advertises identity/capabilities, never credentials, memories, transcripts or pairing secrets. All control and content messages require authenticated encryption, bounded payloads, replay protection, expiration and delivery IDs. A speaker on a different/isolated guest network cannot be assumed reachable. Discovery must be tested across the actual home mesh.

No speaker is a permanent coordinator. Short-lived coordination must recover when the initiating speaker disappears. Losing local discovery must not block that speaker's independent OpenAI conversations.

Room name, assistant conversational name, built-in speech voice, and local wake phrase are separate concepts. Renaming a room must not change any of the other three.

## Scope of remote control

Initial management is from speakers or an authenticated controller on the home LAN. Speakers can independently fetch signed public releases over the internet. Management from outside the home needs a separately configured VPN or authenticated relay; do not open unauthenticated router ports. A relay is not required for ordinary home operation.

## Validation and rollout status

- [x] Requirements and delivery sequence recorded.
- [x] Signed manifest tooling, rejection tests, and non-overlapping OTA partition layout added.
- [x] Test-speaker settings/credential partitions backed up locally; real reboot persistence test passed.
- [x] OTA-enabled firmware compiled, with exclusive update/audio ownership and local boot checks.
- [ ] Phase 1 implementation and local tests.
- [x] USB migration of the test speaker; encrypted key, voice and microphone preferences retained. On-device signed-manifest checks passed; live voice/tool tests and a real reboot passed.
- [ ] Signed OTA and rollback exercised on hardware.
- [x] The test speaker installed signed release 702 over Wi-Fi; boot health acceptance and a second reboot passed, retaining room identity, encrypted-key availability and audio preferences. Interrupted-download and failed-boot recovery remain open.
- [ ] Phase 2 implementation and local tests.
- [ ] Two-device pairing/discovery validation.
- [ ] Phases 3–7 implementation and simulation tests.
- [ ] Real multiroom acoustic tests on at least two devices.
- [ ] Multiroom soak test, reconnect/power-cycle tests, and documentation.

Software/simulation checks and physical-device checks are separate gates. Do not mark a feature verified from a successful build alone. New physical speakers may be required for the remaining gates; continue independent implementation while they are unavailable.
