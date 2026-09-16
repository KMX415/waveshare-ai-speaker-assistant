# Audio device protocol v1

This is our own satellite protocol, not the OpenAI API protocol. The desktop audio
client implements it; the ESP32 USB adapter translates the board's framed USB audio
into this protocol. Standalone ESP32 Wi-Fi firmware connects directly to OpenAI and does not use this legacy bridge protocol.

## Connection

- Endpoint: `/audio` on the host's configured WebSocket port (default 8765).
- Local desktop client: `ws://127.0.0.1:8765/audio`.
- ESP32: `wss://<configured-host>:8765/audio` with certificate validation.
- If configured, HTTP handshake header: `Authorization: Bearer <VOICE_DEVICE_TOKEN>`.
- No credentials in URLs. The OpenAI API key is never sent to the device.
- Native clients omit `Origin`. Browser origins are rejected.
- Only one active device session. A second connection receives an error and closes.

## Start and audio

First send a JSON text message within 10 seconds:

```json
{"type":"start","version":1,"encoding":"pcm_s16le","sample_rate":16000,"channels":1}
```

Wait for the host's JSON response before transmitting microphone data:

```json
{"type":"ready","version":1,"encoding":"pcm_s16le","sample_rate":16000,"channels":1,"mock":false}
```

Then exchange **binary WebSocket messages** containing raw signed little-endian PCM16,
mono, 16 kHz. No WAV header, JSON, base64, or Opus on this device connection.

- Recommended microphone frame: 320 samples / 640 bytes / 20 ms.
- Accepted microphone messages: nonempty even byte counts up to 3200 bytes / 100 ms.
- Pace microphone transmission in real time, including silence, throughout a session.
- Keep capture active during playback. Feed the actual playback reference into the
  ESP32 echo canceller; send its processed microphone channel to the host.
- Speaker messages contain up to 640 bytes. Play them in order at 16 kHz.
- Use short, bounded queues. Silence-fill a temporarily empty playback buffer.
- Do not infer end of speech from packet boundaries. The Live audio stream can include
  silence. There is no fabricated turn/response-done event in this protocol.
- Stop and report a persistent backlog instead of retaining seconds of stale audio.

The host converts raw device PCM to/from the OpenAI Live WebSocket JSON/base64 format.
It sends `session.start`, waits for `session.started`, and only then relays input.
It does not use the older Realtime `input_audio_buffer.commit` / `response.create`
speech-turn loop.

## Status messages

The host may interleave these JSON text messages with audio:

```json
{"type":"transcript","speaker":"user","delta":"Hi"}
{"type":"transcript","speaker":"assistant","delta":"Hello"}
{"type":"notice","message":"Conversation ended after inactivity"}
{"type":"error","message":"Wait for ready before sending audio"}
```

Transcript values are fragments, not finished turns. The desktop hides them unless
explicitly requested. They are not used as local executable commands.
An error is fatal to the current conversation: stop capturing/playing, keep reading
for the terminal close if possible, then return to idle.

## Stop and finalization

Stop microphone capture, clear queued playback, then send:

```json
{"type":"stop"}
```

Keep the socket open until the host sends:

```json
{"type":"closed","finalized":true,"usage":{"seconds":12.5},"input_bytes":400000}
```

`usage` comes from the upstream terminal event. It is cumulative voice usage, not
backend token usage. Do not sum cumulative snapshots. In mock mode it is local elapsed
time and incurs no charge. `input_bytes` is this service's received PCM count.

If final usage cannot be collected within 15 seconds, the host sends:

```json
{"type":"closed","finalized":false}
```

A socket disconnect is not proof of zero charges or confirmed finalization. The host
attempts `session.close` even after a device disconnect. Idle and maximum-duration
limits also close the upstream session. A new conversation needs a new connection
and `start`; protocol v1 does not automatically reconnect or retain history.
