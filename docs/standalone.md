# Standalone runtime and diagnostics

The board connects directly over Wi-Fi to OpenAI. The Python setup UI and original
desktop bridge are optional tools, not runtime requirements. Start with the
[first-time guide](getting-started.md) for installation.

## Conversation lifecycle

WakeNet detects Jarvis or Computer on echo-cancelled 16 kHz audio locally. Detection
acknowledges with a flash and tone, then starts Wi-Fi/TLS setup. Wait for the
session-ready tone before talking. BOOT can also start a session.

“Goodbye, Jarvis” is matched only against input transcript fragments. The matcher
ignores case/punctuation, accepts “good bye,” joins streamed fragments, waits 600 ms
for the last word, and expires fragments after a three-second gap. Assistant output
cannot trigger it. Quoting the command can. BOOT is the offline fallback.

Closing waits for final session usage, then returns to local wake listening after a
three-second cooldown. Limits are 60 seconds inactivity and 10 minutes total.
Both transcript updates and nonempty output audio reset the idle timer, so a long
spoken answer does not count as silence. Idle and total-time shutdown reasons remain
visible in device status after the provider confirms closure.
The activity clock is initialized before marking a session ready, and idle checks
allow an activity timestamp to be newer than the loop's sampled time.
Wi-Fi disconnects schedule a reconnect after five seconds; a failed voice session
does not automatically open another paid session.
At startup, Wi-Fi scans the saved network across channels and explicitly selects
the strongest discovered matching access point for this boot. The choice is kept
in RAM, not saved with credentials; powering on in a new room scans again.
The upload report includes candidate count and selected signal strength without
revealing network names or access-point addresses. This is not continuous roaming.
Wi-Fi saved through setup uses the SDK's all-channel, strongest-signal preference.
Voice sockets retain the SDK's connection-time socket timeouts; the separate
WebSocket send timeout is not an end-to-end deadline for shutdown.
The external WebSocket transport forwards control frames to the client, matching
the SDK's internal transport configuration. Otherwise PONG replies are consumed
below the client and its 120-second heartbeat watchdog can end a healthy session
about 130 seconds after connection. USB upload diagnostics count received PONGs.
TCP/TLS error numbers are read only for TCP errors; the SDK leaves those fields
uninitialized for heartbeat timeouts and other non-TCP events.

## Audio and memory

Capture/playback queues are bounded.
Microphone upload combines up to four 16 ms AEC chunks into each WebSocket message,
reducing TLS/message overhead. A 64-frame PSRAM queue holds about one second of
capture during brief stalls. If it fills, capture discards the oldest queued audio
and keeps listening instead of immediately ending the session. This can lose speech
during congestion; transport/send failures still stop the session. USB `U` and the
PC status endpoint report dropped audio duration, in-flight upload time, queue depth,
and Wi-Fi signal strength without exposing network credentials.
The upload report also includes uptime and the ESP-IDF reset-reason number
(4: panic, 5/6/7: watchdog, 9: brownout). While PC setup is connected, its status
retains recognized USB panic categories and backtrace addresses across a device
restart. It does not retain arbitrary console lines or audio. This capture is
in PC memory and clears when PC setup restarts; device failure counters clear on boot.
Upload buffers also live in PSRAM, keeping the networking task stack small.
Playback now primes with 160 ms of audio (or a 160 ms maximum wait for a short clip),
with a 3.2-second PSRAM queue capacity to absorb delivery bursts without immediately
blocking the response decoder. The initial playback threshold remains 160 ms;
capacity is a bound, not a mandatory playback delay. A final partial PCM frame
is padded after 240 ms without more data, avoiding premature padding between bursts.
I2S DMA blocks match the application's 20 ms audio frames for steadier writes.
Acknowledgment tones pause dequeueing rather than discarding incoming speech.
The tradeoff is a small extra playback delay. USB `B` reports queue depth/peak and
short refill gaps, incoming chunk sizes/timing, partial-frame padding, and slow
speaker writes, plus full-queue enqueue waits; ordinary brief pauses can also
increment the gap counter.

Queued audio, decoded speaker samples, and response
messages use PSRAM; queue control structures remain internal. General allocations above
1 KiB prefer PSRAM, with 64 KiB reserved for internal-only allocations. TLS uses PSRAM.

The wake worker releases its model before TLS startup and reloads after the conversation.
The main task uses an 8 KiB stack. WebSocket transmit locking is separate from receive,
and incoming responses are processed on a worker. LED refresh uses a low-priority RMT DMA
task; the speaker task only publishes the current audio level.
The 16-message response queue applies up to 250 ms of backpressure when full,
allowing its decoder worker to run during network bursts. A sustained backlog still
ends the session rather than silently dropping speech. Failure records distinguish
response allocation failure from queue exhaustion and retain queue peak/wait counts.

## Setup pages

The board serves HTTP through its password-protected setup AP at 192.168.4.1.
Both IPv4 and IPv4-mapped IPv6 clients are handled. Host/origin guards protect settings
requests; ordinary home-LAN access is refused. It is not an automatic captive portal.

The optional computer page at 127.0.0.1:8766 communicates over USB. Secrets are not
read back from device storage. Both pages expose wake selection, sensitivity,
speaker volume, and a temporary 60-second detection-only mode. Test mode blocks
live-session starts and expires automatically. Postal code is stored but unused.

## USB diagnostics

Packets begin with ASCII `HV`, then one byte for packet type and two little-endian bytes
for payload length (maximum 640). Type 1 is a command; type 2 is PCM; type 3 is JSON status.

| Command | Purpose |
|:--|:--|
| I | Firmware identity, audio settings, and device status |
| N | Connection status, key-present booleans, final usage |
| D | Audio queue/send timing, transport errors, internal heap |
| B | Playback buffering, incoming audio timing, and speaker-write counters |
| U | Upload stalls/loss, Wi-Fi signal, uptime, and reset reason |
| F | Previous failure message and queue/memory snapshot, retained across retries until reboot |
| Q | Wake settings, readiness, detection/gap counters |
| J | Hangup matcher self-tests and spoken hangup counter |
| L | LED refresh and audio-meter counters |
| T | Local speaker tone |
| H | Device portal checks; expected pass mask 15 |
| G / Z | Start / stop a paid standalone session |
| C | Enable setup AP and return its private credentials locally |

Do not publish raw diagnostic streams: `C` deliberately returns setup credentials.
Ordinary status commands do not return API keys or transcripts.

The setup pages also show the previous failure separately from the current session
status. Starting another conversation does not clear it. The snapshot records the
first failure of each session and lives in RAM, so restarting/power loss clears it.
It also records session duration, WebSocket close code, SDK event/error type, and
socket/TLS error numbers. A later disconnect callback enriches the same record when
the SDK supplies details after its initial error. Raw server close reasons are not saved.

## Validation

- 37 host tests cover protocol handling, audio, readiness, authentication, setup guards, provisioning confirmation, and sanitized crash capture.
- On-device hangup tests cover fragmented words, punctuation/case, boundaries, and stale data.
- Three consecutive live sessions connected and finalized after the memory changes.
- Spoken wake activation, replies, and spoken hangup were confirmed on hardware.
- Saved key/settings persisted across restarts and firmware updates.
- LED refresh and audio response were checked through hardware counters and user feedback.

These are development checks, not an endurance certification. Long-duration use,
interruption quality across rooms, and other board revisions need broader testing.
