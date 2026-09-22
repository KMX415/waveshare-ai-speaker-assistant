# Household protocol design (not implemented on speakers yet)

This document fixes the remaining integration contracts before transport code is added. It is not a claim that discovery or intercom is already working.

## Initial household

Kitchen and Living Room share the `common-area` wake group. Bedroom has its own wake group. Arbitration is scoped to a wake group, not the whole house: two independent calls in Bedroom and Kitchen should both work. BOOT remains a deliberate local override. Each speaker has a stable hardware ID and a separately editable room label; duplicate labels must be reported as ambiguous rather than silently routed.

## Pairing and transport

Initial provisioning can use the physical USB setup connection while each board is flashed. It must not introduce a computer dependency during normal operation. Household credentials are generated independently of OpenAI keys and stored in the board's encrypted credential partition. A future phone pairing flow must use an established authenticated key exchange; do not invent a six-digit-code encryption scheme vulnerable to offline guessing.

Only paired household members may send control or content messages. Discovery carries identity, room and capabilities; it never carries an OpenAI key or personal memory. Use authenticated encryption, sender identity, boot generation, sequence number, unique operation ID and expiration. Nonces must remain unique across power loss. Control replay protection must survive receiver reboots; a RAM-only sequence cache is insufficient. Removing a member requires revoking its access, including rotating shared group credentials if group keys are used.

Start with bounded local messages, not continuous audio streaming. Local discovery must be tested across the actual mesh; guest-network isolation is an expected unreachable-peer condition, not a reason to expose an unauthenticated internet endpoint. A household must continue to support independent conversations if discovery fails.

## Wake arbitration

Capture a short wake-audio measurement before automatic digital gain, normalize for hardware gain, and compare only recent candidates in the same wake group. Allow a small bounded collection window and deterministic ID tie-breaking. Repeated/reordered packets must not start additional paid sessions. Bound suppression time so a lost peer cannot indefinitely prevent a local wake. The actual selection window and score require Kitchen/Living Room acoustic testing; microphone loudness alone is not guaranteed to identify the nearest person.

## Message operations

| Operation | Required behavior |
| --- | --- |
| Presence | Room, stable ID, capabilities, installed version, idle/busy state; expire stale peers. |
| Announcement | Destination room or explicit all; bounded text; delivery ID and deadline; at-most-once playback; report busy/offline/expired recipients. |
| Reply | Explicit user request referencing a prior announcement's sender; never create an autonomous assistant-to-assistant loop. |
| Shared fact | Separate shared store, explicit user intent, logical revision plus writer ID, deterministic conflict resolution, durable deletion tombstones. |
| Handoff | Destination and bounded temporary summary; acknowledge storage before ending source conversation; expire unused context. |
| Control | Explicit named target and bounded operation (volume, status, update); acknowledge actual outcome, not just queuing. |
| Update rollout | One member at a time; wait for the new version and healthy boot; halt on failure; list unreachable members. |

An accepted message is not necessarily spoken. Track accepted, playing, delivered, expired and rejected outcomes. Tools must not claim delivery or an applied setting merely because a packet entered an outbound queue.

## Microphone privacy

A remotely triggered announcement must be playback-only. Reusing the normal conversation startup unchanged would stream the destination room's microphone to OpenAI, which is unacceptable for this flow. A playback-only session must suppress microphone upload from its first frame and close after the announcement. Intercom replies require local speech activation; do not turn this into remote microphone monitoring.

A handoff stores a temporary summary on the destination; local wake/acceptance starts listening there. Keep the source usable if the destination rejects or fails to acknowledge. Summaries are context, not permanent memories or executable instructions.

## Memory boundaries

Existing local memories are not automatically copied to other speakers. Users explicitly save household facts into a separate store. Room volume, microphone calibration and room identity remain local. Deletion tombstones survive offline peers rejoining; missing data must never be interpreted as permission to resurrect a deleted fact. Surface conflicts and storage limits; never silently discard facts to make space.

## Testing gates

Simulate loss, duplicates, reordering, delayed acknowledgements, receiver and sender restart, expired messages, unauthorized senders and overlapping wake candidates. Then test on two physical speakers, followed by all three rooms. Test announcements with the destination microphone monitored locally to prove no input is uploaded. Run a longer conversation during peer discovery to check RAM, upload latency and audio continuity before enabling household transport by default.
