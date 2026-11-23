# BinBot Voice Protocol v1

BinBot Voice Protocol（BBVP）是设备和 BinBot Voice Server 之间的版本化协议。
它不承诺兼容任何第三方语音助手协议。

## Transport

- Endpoint: `GET /v1/device/ws`
- Control plane: UTF-8 JSON WebSocket text messages
- Audio plane: binary WebSocket messages with a fixed 40-byte network-order header
- A device owns one connection and one active turn at a time
- All identifiers are UUID strings in JSON and 16-byte UUID values in binary headers

The server rejects unknown JSON fields, unsupported protocol versions, invalid state
transitions, oversized payloads, duplicate sequence numbers, and audio for stale turns.

## Handshake

The first message must arrive within the configured handshake timeout:

```json
{
  "type": "hello",
  "protocol_version": 1,
  "device_id": "binbot-001",
  "client_version": "0.1.0",
  "token": "development-token",
  "uplink": {
    "codec": "opus",
    "sample_rate": 16000,
    "channels": 1,
    "frame_ms": 20
  },
  "downlink": {
    "codec": "opus",
    "sample_rate": 24000,
    "channels": 1,
    "frame_ms": 20
  },
  "capabilities": ["turn_cancel", "remote_control_v1"],
  "camera": {
    "transport": "rtsp",
    "port": 554,
    "path": "/live/binbot"
  }
}
```

The server answers with `hello.ack`, including a new `session_id` and the negotiated
audio formats. `camera` is optional. The server derives its host from the WebSocket
peer address and accepts only the advertised port and restricted absolute path; the
device cannot supply an arbitrary camera host. A device must advertise
`remote_control_v1` before the server will send remote commands.

Devices may advertise `server_vad_v1`. If server-side VAD is enabled and the uplink
is 16 kHz mono, `hello.ack.input_endpoint` is `"server_vad_v1"`; otherwise it is
`"client"`. A device must keep its local endpoint behavior until the server
explicitly acknowledges this capability, including after reconnecting.

After negotiation either peer may send `{"type":"ping","nonce":"..."}`. The receiver
answers with `pong` carrying the same nonce. Heartbeats are control messages and do not
change turn state.

## Calendar state

The server pushes `device.calendar` with a calendar revision, a bounded `events`
snapshot and optional `changes` counts (`added`, `updated`, `removed`). Counts
describe actual stored changes, including events outside the device snapshot.
Web creation is reported as `added`; Feishu record changes are reported as
`updated`, and deletion or cancellation as `removed`. Repeated creation requests
return zero changes. Snapshot eviction must not be interpreted as cancellation.
Devices use the first snapshot as a baseline and suppress repeated revisions.

## Turn lifecycle

```text
turn.start -> binary uplink audio* -> audio.end
           -> asr.final -> assistant.delta* -> audio.start
           -> binary downlink audio* -> assistant.final -> audio.end -> state(idle)
```

Start a microphone turn:

```json
{
  "type": "turn.start",
  "turn_id": "0711f972-1d2b-4a18-ab43-6b257b01fc4b",
  "stream_id": "19df1d39-2312-41fe-b491-776b45efd872",
  "wake_word": "你好米雅"
}
```

Finish or cancel it:

```json
{"type":"audio.end","turn_id":"...","stream_id":"..."}
```

With `server_vad_v1`, the device continuously uploads microphone audio and the
server decides when input ends. The server sends:

```json
{"type":"audio.input_ended","session_id":"...","turn_id":"...","stream_id":"...","reason":"speech_ended"}
```

Reasons are `speech_ended`, `no_speech` (initial input deadline), or `max_duration`.
The device stops uploading only when both turn and uplink stream match, and enters
thinking without sending another `audio.end`. A manual `audio.end` remains valid.
Audio already in flight and duplicate `audio.end` for the known completed input
are ignored; unrelated stream IDs remain errors. ASR partial events continue
during capture and the final transcript replaces the partial text.
Changed ASR words count as speech evidence even when acoustic VAD misses quiet
speech. Automatic input ending waits for both acoustic silence and the configured
ASR text grace period; identical text and punctuation-only changes do not extend it.

```json
{"type":"turn.cancel","turn_id":"...","reason":"user_interrupt"}
```

For diagnostics, `text.input` starts a turn without microphone audio:

```json
{"type":"text.input","turn_id":"...","text":"你好，请介绍一下你自己"}
```

## Real-Time Voice Negotiation

`realtime_voice_v1` devices may receive `hello.ack.input_endpoint = "realtime_voice_v1"`.
In this mode one wake/`turn.start` owns a continuous conversation. Microphone audio
continues during `thinking` and `speaking`; local VAD does not terminate capture.
The default server policy is to take turns: the server decodes but discards
microphone frames while generating and playing a reply. It mutes upstream input,
clears queued microphone audio, and ignores overlapping recognition. It restores
input after paced audio has drained plus a 200 ms device-buffer allowance.
After each reply the server sends `state(listening)`, retaining the turn and uplink
stream IDs. `assistant.final` ends an exchange, not the continuous conversation.

The server sends `input.started` with the active `turn_id` and a fresh UUID
`message_id` on each confirmed utterance. Overlapping speech does not generate
`input.started` under the default policy. When the server explicitly enables
`providers.seeduplex.allow_interruption`, during playback native speech-start
detection alone does not confirm an interruption: ASR must identify a stop request
or speech distinct from the current assistant reply. Playback echo is ignored,
including any response it induces, while the original buffered audio continues.
Devices clear playback immediately on `input.started` and
use `message_id` to group user/assistant subtitles without changing uplink IDs or
audio sequence numbers. `audio.cancel` clears playback for a canceled response.
`audio.start` always identifies a fresh downlink Opus stream. `turn.cancelled`
terminates capture, including inactivity, exit intent and explicit cancellation.
Manual `audio.end` commits and mutes input; `text.input` continues to use the
original text-model pipeline. Servers configured for real-time voice reject
devices missing this capability with an explicit upgrade error.

## Binary audio header

All integer fields are unsigned and encoded in network byte order.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Magic: ASCII `BBV1` |
| 4 | 1 | Protocol version: `1` |
| 5 | 1 | Kind: `1` uplink Opus, `2` downlink Opus |
| 6 | 2 | Flags, reserved in v1 |
| 8 | 16 | Stream UUID |
| 24 | 4 | Sequence number, starting at zero |
| 28 | 8 | Monotonic timestamp in milliseconds |
| 36 | 4 | Payload size |

The payload follows immediately. V1 limits one encoded audio payload to 4096 bytes.
Within a stream, sequence starts at zero and increments by one. Timestamp is the capture
or playback monotonic time for the first frame and then increments by the negotiated
`frame_ms`; duplicate, skipped, decreasing, or stale stream frames are protocol errors.

The language-neutral example in
`contracts/v1/vectors/uplink-audio-frame.hex` covers the complete header and payload.
Both the Python transport tests and device C tests decode and encode this vector.

## Server events

The server may emit:

- `state`: `idle`, `listening`, `thinking`, `speaking`, `error`
- `asr.partial`, `asr.final`
- `assistant.delta`, `assistant.final`
- `assistant.playback`: optional real-time voice reading cursor, carrying the
  current `stream_id`, an already-spoken text prefix in `text`, and
  `estimated: true`. This never replaces the full transcript. Clients should
  stop following it when speech is interrupted or the stream changes. The
  cursor uses PCM duration and punctuation weights because Seeduplex does not
  supply word timestamps; it is not a precise subtitle alignment.
- `audio.start`, `audio.end`
- `turn.cancelled`
- `device.command` when `remote_control_v1` was negotiated
- `camera.publish` when `camera_publish_v1` was negotiated
- `pong`
- `error`

Every turn-scoped event carries `session_id` and `turn_id`. A device must discard
events whose turn does not match its current turn.

## Cloud camera publishing

A device advertises `camera_publish_v1` only when its local stream-copy
publisher and private cloud destination are configured. After `hello.ack`,
the server may send a publishing lease independently of voice turns:

```json
{"type":"camera.publish","lease_seconds":45}
```

The device starts one publisher or renews the existing publisher. Positive
leases must be integers from 5 through 90 seconds. A zero lease stops upload;
expiry also stops it without another server message. The device uses a
monotonic clock and retries publisher failures only while its lease is valid.
The destination and credential come from private device configuration, never
from the control event. No command result is required for these periodic
leases. Local camera capture and snapshots remain independent of cloud upload.

The Console renews active device leases every 10 seconds. Viewer sessions
expire after 35 seconds without a heartbeat; the final viewer leaving adds a
30-second stop delay. Each authenticated viewer negotiates its own WebRTC
connection while sharing the device's single RTSP/TCP publisher.

## Device state and remote control

A device that advertises `remote_control_v1` sends its complete state after connecting,
after every state-changing command, and periodically while connected. `revision`
increases when the state content changes; a periodic duplicate may keep the same value,
and the counter may restart after the device reconnects:

```json
{
  "type": "device.state",
  "revision": 7,
  "todos": [
    {"text": "整理桌面", "done": false}
  ],
  "music": {
    "track_index": 0,
    "position_seconds": 18,
    "active": true,
    "playing": true
  },
  "display": {
    "gif": "welcome.gif"
  }
}
```

The server sends a command with a unique ID:

```json
{
  "type": "device.command",
  "command_id": "2f56930d-0d03-4436-a541-53f27434ea55",
  "name": "todo.add",
  "payload": {"text": "整理桌面"}
}
```

Supported names and payloads are:

| Name | Payload |
|---|---|
| `voice.wake` | `{}` |
| `music.play`, `music.pause`, `music.stop`, `music.previous`, `music.next` | `{}` |
| `music.seek` | `{"position_seconds": 0..86400}` |
| `music.select` | `{"track_index": 0..255}` |
| `todo.add` | `{"text": "..."}` |
| `todo.edit` | `{"index": 0..7, "text": "..."}` |
| `todo.set_done` | `{"index": 0..7, "done": true}` |
| `todo.delete` | `{"index": 0..7}` |
| `display.gif` | `{"asset_id": "...", "asset_path": "/v1/device/assets/....gif"}` |

The device must acknowledge every accepted command. `success` reports execution, not
merely receipt; `message` is an optional human-readable result:

```json
{
  "type": "device.command.result",
  "command_id": "2f56930d-0d03-4436-a541-53f27434ea55",
  "success": true,
  "message": ""
}
```

## Compatibility rules

- Additive optional JSON fields are introduced only in a new negotiated minor feature.
- Breaking field or binary changes require a new `protocol_version`.
- Unknown message types are errors in v1.
- Audio parameters are negotiated during `hello`; they are never inferred from a packet.
