<!-- This file was automatically generated. Do not edit directly! -->

# AXTP Protocol

## Main Table of Contents

- [Overview](#overview)
- [Protocol Framework](#protocol-framework)
- [Supported Connection Profiles](#supported-connection-profiles)
- [Design Goals / Non-Goals](#design-goals--non-goals)
- [Connection Lifecycle](#connection-lifecycle)
- [Capability Discovery](#capability-discovery)
- [Methods](#methods)
  - [audio Methods](#audio-methods)
  - [device Methods](#device-methods)
  - [firmware Methods](#firmware-methods)
  - [network Methods](#network-methods)
  - [video Methods](#video-methods)
- [Events](#events)
  - [audio Events](#audio-events)
  - [firmware Events](#firmware-events)
  - [network Events](#network-events)
  - [video Events](#video-events)
- [Additional Types](#additional-types)
- [Errors Reference](#errors-reference)
- [Profiles Reference](#profiles-reference)

## Implemented Domains

| Domain | Methods | Events |
| ---- | ---- | ---- |
| audio | 9 | 4 |
| device | 1 | 0 |
| firmware | 4 | 2 |
| network | 18 | 8 |
| video | 6 | 3 |

## Overview

AXTP is a transport-independent device communication protocol for CONTROL, RPC and STREAM payloads across Standard Framed transports, plus a formal WebSocket Unframed JSON RPC profile. Phase 1 requires the STREAM data plane for audio/video media flow profiles.

| Property | Value |
| ---- | ---- |
| Protocol | AXTP |
| Version | 1.0.0 |
| Spec Version | 1 |
| Registry Version | 1.0.0 |
| Status | rc1 |
| Wire Byte Order | big-endian / network |
| Wire Integer Encoding | unsigned and signed multi-byte integers use Big-Endian / network byte order |
| CRC Byte Order | big-endian |

## Protocol Framework

AXTP v1 has two formal integration paths:

- **Standard Framed**: uses the 12-byte Standard Frame header, CONTROL OPEN/ACCEPT, HEARTBEAT/CLOSE, RPC, STREAM, fragmentation and CRC16. ACK/NACK reliability is future/profile-level work.
- **WebSocket Unframed JSON**: uses the JSON `sid`/`op`/`d` envelope directly over WebSocket. It is RPC-only and does not carry CONTROL or STREAM payloads.

| Path | Transports | Frame | RPC Encodings | CONTROL | STREAM |
| ---- | ---- | ---- | ---- | ---- | ---- |
| Standard Framed | AXTP-USB-HID<br>AXTP-TCP | STANDARD_FRAME | `JSON`, `CBOR`, `MSGPACK`, `JSON_BINARY` | Yes | Yes |
| WebSocket Unframed JSON | AXTP-WS-JSON<br>AXTP-WS-CLOUD-REVERSE | None | `JSON` | No | No |

Compact/HID-64/BLE/UART framing is a low-bandwidth degradation path, not an AXTP v1 Core requirement. See `docs/specs/1-core/08-Low-Bandwidth-Degradation.md` for that path.

## Design Goals / Non-Goals

### Goals

- Provide one unified protocol model for control, request/response RPC and audio/video stream transfer.
- Make Standard Frame the AXTP v1 Core binary path for USB HID High Speed and TCP.
- Support WebSocket Unframed JSON as the formal RPC-only integration path.
- Keep full dynamic capability modeling optional outside AXTP v1 Core.

### Non-Goals

- Full dynamic UI capability modeling is not required in v1.
- Compact/HID-64/BLE/UART low-bandwidth framing is not required by AXTP v1 Core.
- STREAM is not carried over WebSocket Unframed JSON.
- Header profile negotiation is not performed dynamically in v1.

## Connection Lifecycle

| Step | From | To | Status | Description |
| ---- | ---- | ---- | ---- | ---- |
| OPEN | Client | Server | - | Open an AXTP logical session and declare runtime limits. |
| ACCEPT | Server | Client | - | Accept the session and return final runtime parameters. |
| Hello | Server | Client | - | Announce RPC session rules, protocol version and authentication requirements. |
| Identify | Client | Server | - | Submit client identity and optional authentication data. |
| Identified | Server | Client | - | Confirm that the RPC session is ready. |
| Load Adopted Registry | Client | Server | - | Use the generated protocol registry to select adopted business methods for the current product. |

### Optional Lifecycle Extensions

| Step | From | To | Status | Description |
| ---- | ---- | ---- | ---- | ---- |
| READY | - | - | optional | Reserved for transports that need an explicit client acknowledgement after ACCEPT; not required by AXTP v1 Core. |

## Supported Connection Profiles

The current protocol definition exposes the connection profiles that are intended for AXTP v1 readers and SDKs.

| Profile | Family | Mode | Frame | RPC Encodings | CONTROL | STREAM | Notes |
| ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- |
| AXTP-USB-HID | usb-hid | standard-framed | STANDARD_FRAME | `JSON`, `CBOR`, `MSGPACK`, `JSON_BINARY` | Yes | Yes | USB HID High Speed or large-report HID transport using Standard Frame. |
| AXTP-TCP | tcp | standard-framed | STANDARD_FRAME | `JSON`, `CBOR`, `MSGPACK`, `JSON_BINARY` | Yes | Yes | TCP byte stream transport using Standard Frame magic and length parsing. |
| AXTP-WS-JSON | websocket | unframed-json | None | `JSON` | No | No | Formal RPC-only WebSocket JSON profile using the sid/op/d envelope. |
| AXTP-WS-CLOUD-REVERSE | websocket | unframed-json-cloud-reverse | None | `JSON` | No | No | Device initiates the WebSocket connection but remains the Logical Server. |

### Role Matrix

| Profile | Physical Client | Physical Server | Logical Client | Logical Server | Hello Sender |
| ---- | ---- | ---- | ---- | ---- | ---- |
| AXTP-USB-HID | Host / App | USB HID Device | Host / App | Device | Device |
| AXTP-TCP | App / PC | Device | App / PC | Device | Device |
| AXTP-WS-JSON | App / Cloud | Device / Gateway | App / Cloud | Device | Device |
| AXTP-WS-CLOUD-REVERSE | Device | Cloud | Cloud | Device | Device |

**Logical Server sends Hello.** This is true even when the device is the Physical Client in `AXTP-WS-CLOUD-REVERSE`.

### Cloud Reverse Connection

In `AXTP-WS-CLOUD-REVERSE`, the device initiates the WebSocket connection to the cloud endpoint, but the device remains the Logical Server:

```text
Physical Client: Device    Physical Server: Cloud
Logical Client:  Cloud     Logical Server:  Device

  Device opens the WebSocket connection to the cloud endpoint.
  No CONTROL OPEN or Standard Frame is used in this profile.
  Device remains the Logical Server and sends Hello after the WebSocket is established.
  Cloud identifies as the Logical Client and then issues JSON RPC requests.
```

The key invariant: **the Logical Server sends Hello** after the WebSocket is established.

### WebSocket Unframed JSON

This profile is a formal RPC-only path. It skips the Frame and CONTROL layers, uses JSON `sid`/`op`/`d`, and does not carry STREAM data.

- Open the WebSocket connection.
- Wait for Hello from the Logical Server.
- Send Identify using the JSON sid/op/d envelope.
- Wait for Identified.
- Load generated protocol registry for the current product build.
- Start JSON RPC requests and receive JSON events.

| WebSocket Unframed JSON | Standard Framed AXTP |
| --- | --- |
| WebSocket Upgrade | Transport connect + CONTROL OPEN/ACCEPT |
| Hello (op=0) | RPC Hello |
| Identify (op=2) | RPC Identify |
| Identified (op=3) | RPC Identified |
| REQUEST (op=7) | RPC Request |
| REQUEST_RESPONSE (op=8) | RPC RequestResponse |
| EVENT (op=6) | RPC Event |
| WebSocket Close | CONTROL CLOSE or transport close |
| Not supported | STREAM |

## Payload Types

Every Standard Framed AXTP Frame carries exactly one payload. WebSocket Unframed JSON skips this layer and carries only RPC JSON envelopes.

| Type | ID | Header Size | When to Use |
| ---- | ---- | ---- | ---- |
| `CONTROL` | 0x01 | 5B | Logical session control payload. |
| `RPC` | 0x02 | 1B | RPC payload starts with rpcEncoding; JSON_BINARY then carries the fixed binary envelope. |
| `STREAM` | 0x03 | 16B | Chunk-oriented data plane payload. |

## Generated Method Index

The generated registry groups methods by domain. Each method keeps a stable `bitOffset` within its domain for generated indexes, test vectors, and any adopted runtime discovery method.

| Domain | Methods |
| ---- | ---- |
| audio | 1: audio.getAlgorithmConfig<br>2: audio.setAlgorithmConfig<br>0: audio.getAlgorithmCapabilities<br>3: audio.resetAlgorithmConfig<br>4: audio.getStreamCapabilities<br>5: audio.openStream<br>6: audio.closeStream<br>7: audio.getStreamState<br>8: audio.getStreamSourceState |
| device | 0: device.getInfo |
| firmware | 0: firmware.getUpdateCapabilities<br>1: firmware.beginUpdate<br>3: firmware.getUpdateState<br>2: firmware.finishUpdate |
| network | 2: network.getIpConfig<br>3: network.setIpConfig<br>5: network.getWifiConfig<br>6: network.setWifiConfig<br>7: network.scanWifi<br>8: network.connectWifi<br>9: network.disconnectWifi<br>10: network.getWifiState<br>12: network.getApConfig<br>13: network.setApConfig<br>15: network.startAp<br>16: network.stopAp<br>14: network.getApState<br>0: network.getInterfaces<br>1: network.getInterfaceInfo<br>4: network.getWifiCapabilities<br>11: network.getApCapabilities<br>17: network.getApClients |
| video | 1: video.openStream<br>2: video.closeStream<br>3: video.getStreamState<br>0: video.getStreamCapabilities<br>4: video.getStreamSourceState<br>5: video.requestKeyFrame |

# Methods

## audio Methods

### Methods in this domain

- [audio.getAlgorithmConfig](#audiogetalgorithmconfig)
- [audio.setAlgorithmConfig](#audiosetalgorithmconfig)
- [audio.getAlgorithmCapabilities](#audiogetalgorithmcapabilities)
- [audio.resetAlgorithmConfig](#audioresetalgorithmconfig)
- [audio.getStreamCapabilities](#audiogetstreamcapabilities)
- [audio.openStream](#audioopenstream)
- [audio.closeStream](#audioclosestream)
- [audio.getStreamState](#audiogetstreamstate)
- [audio.getStreamSourceState](#audiogetstreamsourcestate)

---

### audio.getAlgorithmConfig

Return the current effective configuration for supported audio algorithm objects.

- Method ID: `0x0901`
- Domain: `audio`
- bitOffset: `1`
- Status: `stable`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `audio.algorithm`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `INTERNAL_ERROR`

#### Request Fields

Type: `AudioGetAlgorithmConfigRequest`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?items | Bytes | 0x01 | Optional JSON array of algorithm object names; omit to query all supported algorithms. | maxLength=128 | Omit if not used. |

#### Response Fields

Type: `AudioAlgorithmConfig`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?noiseSuppression | AudioNoiseSuppressionConfig | 0x01 | Noise suppression configuration. | None | Omit if not used. |
| ?echoCancellation | AudioEchoCancellationConfig | 0x02 | Echo cancellation configuration. | None | Omit if not used. |
| ?autoGainControl | AudioAutoGainControlConfig | 0x03 | Automatic gain control configuration. | None | Omit if not used. |
| ?beamforming | AudioBeamformingConfig | 0x04 | Beamforming configuration. | None | Omit if not used. |
| ?dereverberation | AudioDereverberationConfig | 0x05 | Dereverberation configuration. | None | Omit if not used. |
| ?voiceActivityDetection | AudioVoiceActivityDetectionConfig | 0x06 | Voice activity detection configuration. | None | Omit if not used. |
| ?directionOfArrival | AudioDirectionOfArrivalConfig | 0x07 | Direction of arrival configuration. | None | Omit if not used. |
| ?howlingSuppression | AudioHowlingSuppressionConfig | 0x08 | Howling suppression configuration. | None | Omit if not used. |

---

### audio.setAlgorithmConfig

Partially update one or more audio algorithm configuration objects atomically.

- Method ID: `0x0902`
- Domain: `audio`
- bitOffset: `2`
- Status: `stable`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `audio.algorithm`
- Possible Events: `audio.algorithmConfigChanged`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `OUT_OF_RANGE`, `INVALID_STATE`, `BUSY`, `PERMISSION_DENIED`, `INTERNAL_ERROR`

#### Request Fields

Type: `AudioSetAlgorithmConfigRequest`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| config | AudioAlgorithmConfig | 0x01 | Partial configuration keyed by algorithm object name. | None | N/A |

#### Response Fields

Type: `AudioSetAlgorithmConfigResponse`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| applyState | Enum | 0x01 | Apply state; values are applied or pending_restart. | None | N/A |
| requiresAudioRestart | Boolean | 0x02 | Whether the change requires restarting the audio link or rebuilding the audio pipeline. | None | N/A |
| config | AudioAlgorithmConfig | 0x03 | Final effective configuration for the algorithms affected by this operation. | None | N/A |

---

### audio.getAlgorithmCapabilities

Return supported audio algorithm objects, fields, defaults, ranges, units, and update policy.

- Method ID: `0x090D`
- Domain: `audio`
- bitOffset: `0`
- Status: `stable`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `audio.algorithm`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `INTERNAL_ERROR`

#### Request Fields

Type: `AudioGetAlgorithmCapabilitiesRequest`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?items | Bytes | 0x01 | Optional JSON array of algorithm object names; omit to query all supported algorithms. | maxLength=128 | Omit if not used. |

#### Response Fields

Type: `AudioGetAlgorithmCapabilitiesResponse`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| capability | String | 0x01 | Fixed capability name audio.algorithm. | maxLength=32 | N/A |
| updatePolicy | AudioAlgorithmUpdatePolicy | 0x02 | Update and atomicity policy for set and reset operations. | None | N/A |
| algorithms | AudioAlgorithmCapabilities | 0x03 | Capability descriptors keyed by algorithm object name. | None | N/A |

---

### audio.resetAlgorithmConfig

Reset all, selected, or selected-field audio algorithm configuration to declared default values.

- Method ID: `0x090E`
- Domain: `audio`
- bitOffset: `3`
- Status: `stable`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `audio.algorithm`
- Possible Events: `audio.algorithmConfigChanged`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `OUT_OF_RANGE`, `INVALID_STATE`, `BUSY`, `PERMISSION_DENIED`, `INTERNAL_ERROR`

#### Request Fields

Type: `AudioResetAlgorithmConfigRequest`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| items | Bytes | 0x01 | JSON reset selector: the string all, an array of algorithm object names, or a map from algorithm names to field-name arrays. | maxLength=256 | N/A |

#### Response Fields

Type: `AudioSetAlgorithmConfigResponse`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| applyState | Enum | 0x01 | Apply state; values are applied or pending_restart. | None | N/A |
| requiresAudioRestart | Boolean | 0x02 | Whether the change requires restarting the audio link or rebuilding the audio pipeline. | None | N/A |
| config | AudioAlgorithmConfig | 0x03 | Final effective configuration for the algorithms affected by this operation. | None | N/A |

---

### audio.getStreamCapabilities

Return real-time audio stream sources, codecs, stream profiles, and open-mode support.

- Method ID: `0x090F`
- Domain: `audio`
- bitOffset: `4`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `audio.stream`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `PERMISSION_DENIED`, `UNAVAILABLE`

#### Request Fields

Type: `AudioGetStreamCapabilitiesParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?source | String | 0x01 | Optional audio source identifier; omit to query all visible sources. | maxLength=128 | Omit if not used. |
| ?includeRuntimeState | Boolean | 0x02 | Whether to include current source runtime state. | None | Omit if not used. |

#### Response Fields

Type: `AudioStreamCapabilities`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| capability | String | 0x01 | Fixed capability name audio.stream. | maxLength=32 | N/A |
| sources | Bytes | 0x02 | JSON array of AudioStreamSource objects. | maxLength=8192 | N/A |
| streamProfiles | Bytes | 0x03 | JSON array of supported stream profiles, normally media.audio. | maxLength=512 | N/A |
| openModes | Bytes | 0x04 | JSON array of supported open modes, such as producer_open and receiver_pull. | maxLength=512 | N/A |
| peerRoles | Bytes | 0x05 | JSON array of peer roles, such as receiver and transmitter. | maxLength=512 | N/A |
| supportsSourceStateEvent | Boolean | 0x06 | Whether audio.streamSourceStateChanged is supported. | None | N/A |
| supportsSyncGroup | Boolean | 0x07 | Whether audio streams can share a synchronization group with video streams. | None | N/A |
| flowControlManagedByRuntime | Boolean | 0x08 | Whether normal applications can rely on runtime-managed STREAM flow control. | None | N/A |
| ?aacTransportFormats | Bytes | 0x09 | Optional JSON array of AAC transport format strings; exact supported set remains product-confirmed. | maxLength=512 | Omit if not used. |

---

### audio.openStream

Open a real-time audio STREAM and return the negotiated streamId and media metadata.

- Method ID: `0x0910`
- Domain: `audio`
- bitOffset: `5`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `audio.stream`
- Possible Events: `audio.streamStateChanged`, `audio.streamSourceStateChanged`
- Possible Errors: `SUCCESS`, `INVALID_ARGUMENT`, `BUSY`, `RESOURCE_EXHAUSTED`, `MEDIA_SOURCE_NOT_FOUND`, `MEDIA_SOURCE_UNAVAILABLE`, `MEDIA_CODEC_UNSUPPORTED`, `MEDIA_STREAM_START_FAILED`

#### Request Fields

Type: `AudioOpenStreamParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| source | String | 0x01 | Audio source identifier. | maxLength=128 | N/A |
| peerRole | Enum | 0x02 | Requested peer media role; values include receiver and transmitter. | None | N/A |
| codec | Enum | 0x03 | Requested audio codec, such as aac, opus, or pcm. | None | N/A |
| ?transportFormat | Enum | 0x04 | Optional codec transport format, such as adts, latm, or raw_aac. | None | Omit if not used. |
| ?sampleRate | UInt32 | 0x05 | Requested sample rate in Hz. | None | Omit if not used. |
| ?channels | UInt8 | 0x06 | Requested channel count. | None | Omit if not used. |
| ?sampleFormat | Enum | 0x07 | Requested sample format. | None | Omit if not used. |
| ?chunkDurationMs | UInt32 | 0x08 | Preferred chunk duration in milliseconds. | None | Omit if not used. |
| ?streamProfile | String | 0x09 | STREAM profile name. | maxLength=64 | Omit if not used. |
| ?cursorUnit | Enum | 0x0A | STREAM cursor unit, such as timestampUs or sampleIndex. | None | Omit if not used. |
| ?syncGroupId | String | 0x0B | Optional synchronization group identifier. | maxLength=128 | Omit if not used. |
| ?castSessionId | String | 0x0C | Optional cast session identifier. | maxLength=128 | Omit if not used. |
| ?clockDomain | String | 0x0D | Source media clock domain. | maxLength=128 | Omit if not used. |
| ?receiverClockDomain | String | 0x0E | Receiver clock domain. | maxLength=128 | Omit if not used. |
| ?maxDataSize | UInt32 | 0x0F | Preferred maximum STREAM payload data size. | None | Omit if not used. |

#### Response Fields

Type: `AudioOpenStreamResult`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| streamId | UInt32 | 0x01 | STREAM data plane stream identifier. | None | N/A |
| state | Enum | 0x02 | Initial state, normally opening or streaming. | None | N/A |
| source | String | 0x03 | Bound source identifier. | maxLength=128 | N/A |
| peerRole | Enum | 0x04 | Confirmed peer media role. | None | N/A |
| codec | Enum | 0x05 | Negotiated codec. | None | N/A |
| ?transportFormat | Enum | 0x06 | Negotiated transport format. | None | Omit if not used. |
| sampleRate | UInt32 | 0x07 | Negotiated sample rate in Hz. | None | N/A |
| channels | UInt8 | 0x08 | Negotiated channel count. | None | N/A |
| ?sampleFormat | Enum | 0x09 | Negotiated sample format. | None | Omit if not used. |
| streamProfile | String | 0x0A | Normalized stream profile. | maxLength=64 | N/A |
| cursorUnit | Enum | 0x0B | STREAM cursor unit. | None | N/A |
| ?syncGroupId | String | 0x0C | Synchronization group identifier. | maxLength=128 | Omit if not used. |
| ?castSessionId | String | 0x0D | Cast session identifier. | maxLength=128 | Omit if not used. |
| ?clockDomain | String | 0x0E | Source media clock domain. | maxLength=128 | Omit if not used. |
| ?receiverClockDomain | String | 0x0F | Receiver clock domain. | maxLength=128 | Omit if not used. |
| ?maxDataSize | UInt32 | 0x10 | Negotiated maximum STREAM payload data size. | None | Omit if not used. |

---

### audio.closeStream

Close a previously opened audio STREAM.

- Method ID: `0x0911`
- Domain: `audio`
- bitOffset: `6`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `audio.stream`
- Possible Events: `audio.streamStateChanged`
- Possible Errors: `SUCCESS`, `STREAM_NOT_FOUND`, `STREAM_CLOSED`, `INVALID_STATE`, `MEDIA_STREAM_STOP_FAILED`

#### Request Fields

Type: `AudioCloseStreamParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| streamId | UInt32 | 0x01 | STREAM data plane stream identifier. | None | N/A |
| ?peerRole | Enum | 0x02 | Peer role in this stream. | None | Omit if not used. |
| ?reason | Enum | 0x03 | Close reason. | None | Omit if not used. |
| ?finalCursor | UInt64 | 0x04 | Last processed cursor value. | None | Omit if not used. |

#### Response Fields

Type: `AudioCloseStreamResult`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| streamId | UInt32 | 0x01 | Closed stream identifier. | None | N/A |
| state | Enum | 0x02 | Close state, such as closing, closed, or failed. | None | N/A |
| ?reason | Enum | 0x03 | Final close reason. | None | Omit if not used. |
| ?alreadyClosed | Boolean | 0x04 | Whether the stream was already terminal before this request. | None | Omit if not used. |

---

### audio.getStreamState

Return runtime state for an opened audio stream.

- Method ID: `0x0912`
- Domain: `audio`
- bitOffset: `7`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `audio.stream`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `STREAM_NOT_FOUND`, `PERMISSION_DENIED`, `UNAVAILABLE`

#### Request Fields

Type: `AudioGetStreamStateParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| streamId | UInt32 | 0x01 | STREAM data plane stream identifier. | None | N/A |

#### Response Fields

Type: `AudioStreamState`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| streamId | UInt32 | 0x01 | STREAM data plane stream identifier. | None | N/A |
| state | Enum | 0x02 | Stream state, such as opening, streaming, closing, closed, or failed. | None | N/A |
| source | String | 0x03 | Bound audio source. | maxLength=128 | N/A |
| ?peerRole | Enum | 0x04 | Peer media role. | None | Omit if not used. |
| ?codec | Enum | 0x05 | Negotiated audio codec. | None | Omit if not used. |
| ?streamProfile | String | 0x06 | Stream profile. | maxLength=64 | Omit if not used. |
| ?syncGroupId | String | 0x07 | Synchronization group identifier. | maxLength=128 | Omit if not used. |
| ?cursorUnit | Enum | 0x08 | STREAM cursor unit. | None | Omit if not used. |
| ?lastCursor | UInt64 | 0x09 | Last known cursor value. | None | Omit if not used. |
| ?failureReason | Enum | 0x0A | Failure reason when state is failed. | None | Omit if not used. |

---

### audio.getStreamSourceState

Return availability and receiving state for an audio stream source.

- Method ID: `0x0913`
- Domain: `audio`
- bitOffset: `8`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `audio.stream`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `MEDIA_SOURCE_NOT_FOUND`, `UNAVAILABLE`

#### Request Fields

Type: `AudioGetStreamSourceStateParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| source | String | 0x01 | Audio source identifier. | maxLength=128 | N/A |

#### Response Fields

Type: `AudioStreamSourceState`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| source | String | 0x01 | Audio source identifier. | maxLength=128 | N/A |
| ?mediaKind | Enum | 0x02 | Media kind, normally audio. | None | Omit if not used. |
| state | Enum | 0x03 | Source state, such as unavailable, available, receiving, stopped, or failed. | None | N/A |
| ?available | Boolean | 0x04 | Whether the source is available for openStream. | None | Omit if not used. |
| ?activeStreamId | UInt32 | 0x05 | Active downstream stream id, if any. | None | Omit if not used. |
| ?lastOpenRejectedReason | Enum | 0x06 | Last open rejection reason. | None | Omit if not used. |

---

## device Methods

### Methods in this domain

- [device.getInfo](#devicegetinfo)

---

### device.getInfo

Return the current endpoint main device identity, product, hardware, OS, software, AXTP runtime, and optional capability summary.

- Method ID: `0x0101`
- Domain: `device`
- bitOffset: `0`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `device.info`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `PERMISSION_DENIED`, `INTERNAL_ERROR`

#### Request Fields

Type: `GetDeviceInfoParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?includeCapabilitySummary | Boolean | 0x01 | Whether to include the lightweight DeviceCapabilitySummary block. | None | Omit if not used. |

#### Response Fields

Type: `DeviceInfo`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| identity | DeviceIdentity | 0x01 | Stable device identity fields. | None | N/A |
| product | DeviceProduct | 0x02 | Brand, product type, model, and display information. | None | N/A |
| ?hardware | DeviceHardware | 0x03 | Hardware summary. | None | Omit if not used. |
| ?os | DeviceOs | 0x04 | Operating system summary. | None | Omit if not used. |
| ?software | DeviceSoftware | 0x05 | Installed or hosted software component summary. | None | Omit if not used. |
| ?runtime | DeviceAxtpRuntime | 0x06 | AXTP runtime summary. | None | Omit if not used. |
| ?capability | DeviceCapabilitySummary | 0x07 | Lightweight modeling summary; not a complete capability registry. | None | Omit if not used. |

---

## firmware Methods

### Methods in this domain

- [firmware.getUpdateCapabilities](#firmwaregetupdatecapabilities)
- [firmware.beginUpdate](#firmwarebeginupdate)
- [firmware.getUpdateState](#firmwaregetupdatestate)
- [firmware.finishUpdate](#firmwarefinishupdate)

---

### firmware.getUpdateCapabilities

Return P0 firmware update capability and upload constraints.

- Method ID: `0x0401`
- Domain: `firmware`
- bitOffset: `0`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `firmware.update`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_STATE`, `FW_DEVICE_NOT_READY`

#### Request Fields

Type: `Empty`

No fields.

#### Response Fields

Type: `FirmwareUpdateCapabilities`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| supported | Boolean | 0x01 | Whether firmware.update P0 is supported. | None | N/A |
| supportsMultiFile | Boolean | 0x02 | Whether manifest may contain multiple files. | None | N/A |
| streamLayout | Enum | 0x03 | P0 stream layout, currently file. | None | N/A |
| hashAlgorithm | Enum | 0x04 | P0 hash algorithm, currently md5. | None | N/A |
| autoReboot | Boolean | 0x05 | Whether the device automatically reboots after installation. | None | N/A |
| ?maxChunkSize | UInt32 | 0x06 | Maximum STREAM data chunk size supported by the device. | None | Omit if not used. |
| ?devicePolicyVersion | String | 0x07 | Optional device policy version used by host tooling. | maxLength=64 | Omit if not used. |

---

### firmware.beginUpdate

Create a firmware update session, accept the manifest, and bind file IDs to STREAM streamIds.

- Method ID: `0x0402`
- Domain: `firmware`
- bitOffset: `1`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `firmware.update`
- Possible Events: `firmware.updateStateChanged`, `firmware.updateProgressReported`
- Possible Errors: `SUCCESS`, `INVALID_ARGUMENT`, `BUSY`, `FW_VERSION_UNSUPPORTED`, `FW_STORAGE_NOT_ENOUGH`, `FW_DEVICE_NOT_READY`

#### Request Fields

Type: `BeginUpdateParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| manifest | FirmwareUpdateManifest | 0x01 | Minimal firmware update manifest. | None | N/A |

#### Response Fields

Type: `BeginUpdateResult`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| updateSessionId | String | 0x01 | Firmware update session identifier. | maxLength=128 | N/A |
| state | Enum | 0x02 | State after begin, normally receiving. | None | N/A |
| streams | Bytes | 0x03 | JSON array of FirmwareUpdateStreamBinding objects. | maxLength=4096 | N/A |
| ?chunkSize | UInt32 | 0x04 | Recommended STREAM chunk size. | None | Omit if not used. |

---

### firmware.getUpdateState

Return current firmware update state for UI refresh, reconnect, or event-loss recovery.

- Method ID: `0x0408`
- Domain: `firmware`
- bitOffset: `3`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `firmware.update`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_FOUND`, `FW_TRANSFER_NOT_STARTED`

#### Request Fields

Type: `GetUpdateStateParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| updateSessionId | String | 0x01 | Firmware update session identifier. | maxLength=128 | N/A |

#### Response Fields

Type: `FirmwareUpdateState`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| updateSessionId | String | 0x01 | Firmware update session identifier. | maxLength=128 | N/A |
| state | Enum | 0x02 | State, such as idle, receiving, verifying, installing, rebooting, confirmed, or failed. | None | N/A |
| ?progress | UInt8 | 0x03 | Overall progress percentage. | min=0, max=100 | Omit if not used. |
| ?currentFileId | String | 0x04 | Current file identifier, if file-level progress is available. | maxLength=128 | Omit if not used. |
| ?error | FirmwareUpdateErrorInfo | 0x05 | Error details when state is failed. | None | Omit if not used. |

---

### firmware.finishUpdate

Tell the device that upload is complete and hand off verification, install, and reboot to the device.

- Method ID: `0x040B`
- Domain: `firmware`
- bitOffset: `2`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `firmware.update`
- Possible Events: `firmware.updateStateChanged`, `firmware.updateProgressReported`
- Possible Errors: `SUCCESS`, `INVALID_STATE`, `STREAM_CHUNK_MISSING`, `FW_SIZE_MISMATCH`, `FW_DEVICE_NOT_READY`, `BUSY`

#### Request Fields

Type: `FinishUpdateParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| updateSessionId | String | 0x01 | Firmware update session identifier. | maxLength=128 | N/A |

#### Response Fields

Type: `FinishUpdateResult`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| updateSessionId | String | 0x01 | Firmware update session identifier. | maxLength=128 | N/A |
| accepted | Boolean | 0x02 | Whether the device accepted the finish handoff. | None | N/A |
| state | Enum | 0x03 | State after finish, normally verifying or failed. | None | N/A |

---

## network Methods

### Methods in this domain

- [network.getIpConfig](#networkgetipconfig)
- [network.setIpConfig](#networksetipconfig)
- [network.getWifiConfig](#networkgetwificonfig)
- [network.setWifiConfig](#networksetwificonfig)
- [network.scanWifi](#networkscanwifi)
- [network.connectWifi](#networkconnectwifi)
- [network.disconnectWifi](#networkdisconnectwifi)
- [network.getWifiState](#networkgetwifistate)
- [network.getApConfig](#networkgetapconfig)
- [network.setApConfig](#networksetapconfig)
- [network.startAp](#networkstartap)
- [network.stopAp](#networkstopap)
- [network.getApState](#networkgetapstate)
- [network.getInterfaces](#networkgetinterfaces)
- [network.getInterfaceInfo](#networkgetinterfaceinfo)
- [network.getWifiCapabilities](#networkgetwificapabilities)
- [network.getApCapabilities](#networkgetapcapabilities)
- [network.getApClients](#networkgetapclients)

---

### network.getIpConfig

Return IP configuration for a network interface.

- Method ID: `0x0E02`
- Domain: `network`
- bitOffset: `2`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.ip`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `NOT_FOUND`, `PERMISSION_DENIED`, `UNAVAILABLE`

#### Request Fields

Type: `NetworkGetIpConfigParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Interface identifier; omitted means default primary interface. | maxLength=64 | Omit if not used. |
| ?family | Enum | 0x02 | IP family; candidate values include ipv4 and ipv6. | None | Omit if not used. |

#### Response Fields

Type: `NetworkIpConfig`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| interfaceId | String | 0x01 | Interface identifier. | maxLength=64 | N/A |
| ?family | Enum | 0x02 | IP family; candidate values include ipv4 and ipv6. | None | Omit if not used. |
| mode | Enum | 0x03 | IP mode; candidate values include dhcp, static, disabled, link_local, and unknown. | None | N/A |
| ?address | String | 0x04 | IP address. | maxLength=64 | Omit if not used. |
| ?prefixLength | UInt8 | 0x05 | Network prefix length. | min=0, max=128 | Omit if not used. |
| ?gateway | String | 0x06 | Default gateway. | maxLength=64 | Omit if not used. |
| ?dns | Bytes | 0x07 | JSON array of DNS server addresses. | maxLength=1024 | Omit if not used. |

---

### network.setIpConfig

Set DHCP/static/disabled IP configuration for a network interface.

- Method ID: `0x0E03`
- Domain: `network`
- bitOffset: `3`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.ip`
- Possible Events: `network.ipConfigChanged`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `OUT_OF_RANGE`, `INVALID_STATE`, `BUSY`, `PERMISSION_DENIED`

#### Request Fields

Type: `NetworkSetIpConfigParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Interface identifier. | maxLength=64 | Omit if not used. |
| ?family | Enum | 0x02 | IP family. | None | Omit if not used. |
| config | NetworkIpConfig | 0x03 | Target IP configuration. | None | N/A |
| ?applyPolicy | Enum | 0x04 | Apply policy; candidate values include immediate and pending_restart. | None | Omit if not used. |

#### Response Fields

Type: `NetworkSetIpConfigResult`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| config | NetworkIpConfig | 0x01 | Applied or pending IP configuration. | None | N/A |
| applyState | Enum | 0x02 | Apply state; candidate values include applied, pending_restart, and failed. | None | N/A |

---

### network.getWifiConfig

Return saved Wi-Fi profile summaries without plaintext credentials.

- Method ID: `0x0E04`
- Domain: `network`
- bitOffset: `5`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.wifi`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `NOT_FOUND`, `PERMISSION_DENIED`, `UNAVAILABLE`

#### Request Fields

Type: `NetworkGetWifiConfigParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi station interface identifier. | maxLength=64 | Omit if not used. |
| ?includeProfiles | Boolean | 0x02 | Whether to include saved profile summaries. | None | Omit if not used. |

#### Response Fields

Type: `NetworkWifiConfig`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi station interface identifier. | maxLength=64 | Omit if not used. |
| ?profiles | Bytes | 0x02 | JSON array of NetworkWifiProfile summaries. Plaintext credentials must not be returned. | maxLength=8192 | Omit if not used. |
| ?defaultProfileId | String | 0x03 | Default profile identifier. | maxLength=128 | Omit if not used. |

---

### network.setWifiConfig

Create or update a Wi-Fi station profile.

- Method ID: `0x0E05`
- Domain: `network`
- bitOffset: `6`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.wifi`
- Possible Events: `network.wifiConfigChanged`, `network.wifiStateChanged`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `OUT_OF_RANGE`, `INVALID_STATE`, `BUSY`, `PERMISSION_DENIED`

#### Request Fields

Type: `NetworkSetWifiConfigParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi station interface identifier. | maxLength=64 | Omit if not used. |
| profile | NetworkWifiProfile | 0x02 | Profile to create or update. | None | N/A |
| ?replaceExisting | Boolean | 0x03 | Whether an existing matching profile may be replaced. | None | Omit if not used. |
| ?makeDefault | Boolean | 0x04 | Whether to make this the default profile. | None | Omit if not used. |
| ?connectAfterSave | Boolean | 0x05 | Whether to start connection after saving. | None | Omit if not used. |

#### Response Fields

Type: `NetworkSetWifiConfigResult`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| profileId | String | 0x01 | Accepted or assigned profile identifier. | maxLength=128 | N/A |
| ?config | NetworkWifiConfig | 0x02 | Updated profile summary. | None | Omit if not used. |
| ?connectStarted | Boolean | 0x03 | Whether connection was started after saving. | None | Omit if not used. |

---

### network.scanWifi

Scan visible Wi-Fi access points.

- Method ID: `0x0E06`
- Domain: `network`
- bitOffset: `7`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.wifi`
- Possible Events: `network.wifiScanResultReported`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `BUSY`, `TIMEOUT`, `UNAVAILABLE`

#### Request Fields

Type: `NetworkScanWifiParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi station interface identifier. | maxLength=64 | Omit if not used. |
| ?ssidFilter | String | 0x02 | Optional SSID filter. | maxLength=64 | Omit if not used. |
| ?timeoutMs | UInt32 | 0x03 | Scan timeout in milliseconds. | None | Omit if not used. |

#### Response Fields

Type: `NetworkScanWifiResult`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?scanId | String | 0x01 | Asynchronous scan identifier. | maxLength=128 | Omit if not used. |
| ?results | Bytes | 0x02 | JSON array of NetworkWifiScanResult objects. | maxLength=16384 | Omit if not used. |
| ?complete | Boolean | 0x03 | Whether returned results are complete. | None | Omit if not used. |

---

### network.connectWifi

Connect to a saved Wi-Fi profile or an inline profile.

- Method ID: `0x0E07`
- Domain: `network`
- bitOffset: `8`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.wifi`
- Possible Events: `network.wifiStateChanged`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `NOT_FOUND`, `INVALID_STATE`, `BUSY`, `TIMEOUT`, `PERMISSION_DENIED`

#### Request Fields

Type: `NetworkConnectWifiParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi station interface identifier. | maxLength=64 | Omit if not used. |
| ?profileId | String | 0x02 | Saved profile identifier. | maxLength=128 | Omit if not used. |
| ?profile | NetworkWifiProfile | 0x03 | Inline profile to connect with. | None | Omit if not used. |
| ?timeoutMs | UInt32 | 0x04 | Connection timeout in milliseconds. | None | Omit if not used. |

#### Response Fields

Type: `NetworkWifiActionResult`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| accepted | Boolean | 0x01 | Whether the action was accepted. | None | N/A |
| state | NetworkWifiState | 0x02 | Current or target Wi-Fi state after accepting the action. | None | N/A |

---

### network.disconnectWifi

Disconnect the current Wi-Fi station connection.

- Method ID: `0x0E08`
- Domain: `network`
- bitOffset: `9`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.wifi`
- Possible Events: `network.wifiStateChanged`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `INVALID_STATE`, `BUSY`, `PERMISSION_DENIED`

#### Request Fields

Type: `NetworkDisconnectWifiParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi station interface identifier. | maxLength=64 | Omit if not used. |
| ?reason | Enum | 0x02 | Disconnect reason; candidate values include user_request, profile_changed, shutdown, and unknown. | None | Omit if not used. |

#### Response Fields

Type: `NetworkWifiActionResult`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| accepted | Boolean | 0x01 | Whether the action was accepted. | None | N/A |
| state | NetworkWifiState | 0x02 | Current or target Wi-Fi state after accepting the action. | None | N/A |

---

### network.getWifiState

Return current Wi-Fi station association, authentication, and connection state.

- Method ID: `0x0E09`
- Domain: `network`
- bitOffset: `10`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.wifi`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `NOT_FOUND`, `PERMISSION_DENIED`, `UNAVAILABLE`

#### Request Fields

Type: `NetworkGetWifiStateParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi station interface identifier. | maxLength=64 | Omit if not used. |

#### Response Fields

Type: `NetworkWifiState`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi station interface identifier. | maxLength=64 | Omit if not used. |
| state | Enum | 0x02 | State; candidate values include disabled, disconnected, scanning, authenticating, associating, connected, failed, and unknown. | None | N/A |
| ?profileId | String | 0x03 | Active profile identifier. | maxLength=128 | Omit if not used. |
| ?ssid | String | 0x04 | Active SSID. | maxLength=64 | Omit if not used. |
| ?rssi | Int32 | 0x05 | Received signal strength indicator in dBm. | None | Omit if not used. |
| ?ipReady | Boolean | 0x06 | Whether IP configuration is ready. | None | Omit if not used. |
| ?failureReason | Enum | 0x07 | Failure reason, if state is failed. | None | Omit if not used. |

---

### network.getApConfig

Return Wi-Fi AP configuration without exposing plaintext credentials unless explicitly allowed by policy.

- Method ID: `0x0E0A`
- Domain: `network`
- bitOffset: `12`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.ap`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `PERMISSION_DENIED`, `UNAVAILABLE`

#### Request Fields

Type: `NetworkGetApConfigParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi AP interface identifier. | maxLength=64 | Omit if not used. |

#### Response Fields

Type: `NetworkApConfig`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi AP interface identifier. | maxLength=64 | Omit if not used. |
| ?enabled | Boolean | 0x02 | Whether AP should be enabled by configuration. | None | Omit if not used. |
| ssid | String | 0x03 | AP SSID. | maxLength=64 | N/A |
| ?hidden | Boolean | 0x04 | Whether SSID broadcast is hidden. | None | Omit if not used. |
| ?band | Enum | 0x05 | AP band. | None | Omit if not used. |
| ?channel | UInt16 | 0x06 | AP channel. | None | Omit if not used. |
| securityType | Enum | 0x07 | AP security type. | None | N/A |
| ?credential | NetworkCredential | 0x08 | Credential descriptor; plaintext must not be returned unless policy explicitly allows it. | None | Omit if not used. |
| ?maxClients | UInt16 | 0x09 | Maximum client count. | None | Omit if not used. |

---

### network.setApConfig

Partially update Wi-Fi AP configuration.

- Method ID: `0x0E0B`
- Domain: `network`
- bitOffset: `13`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.ap`
- Possible Events: `network.apConfigChanged`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `OUT_OF_RANGE`, `INVALID_STATE`, `BUSY`, `PERMISSION_DENIED`

#### Request Fields

Type: `NetworkSetApConfigParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi AP interface identifier. | maxLength=64 | Omit if not used. |
| config | NetworkApConfig | 0x02 | AP configuration patch or target configuration. | None | N/A |

#### Response Fields

Type: `NetworkSetApConfigResult`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| config | NetworkApConfig | 0x01 | Applied or pending AP configuration. | None | N/A |
| applyState | Enum | 0x02 | Apply state; candidate values include applied, pending_restart, and failed. | None | N/A |

---

### network.startAp

Start the Wi-Fi AP role.

- Method ID: `0x0E0C`
- Domain: `network`
- bitOffset: `15`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.ap`
- Possible Events: `network.apStateChanged`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `INVALID_STATE`, `BUSY`, `PERMISSION_DENIED`

#### Request Fields

Type: `NetworkApActionParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi AP interface identifier. | maxLength=64 | Omit if not used. |
| ?reason | Enum | 0x02 | Action reason. | None | Omit if not used. |

#### Response Fields

Type: `NetworkApActionResult`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| accepted | Boolean | 0x01 | Whether the action was accepted. | None | N/A |
| state | NetworkApState | 0x02 | Current or target AP state. | None | N/A |

---

### network.stopAp

Stop the Wi-Fi AP role.

- Method ID: `0x0E0D`
- Domain: `network`
- bitOffset: `16`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.ap`
- Possible Events: `network.apStateChanged`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `INVALID_STATE`, `BUSY`, `PERMISSION_DENIED`

#### Request Fields

Type: `NetworkApActionParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi AP interface identifier. | maxLength=64 | Omit if not used. |
| ?reason | Enum | 0x02 | Action reason. | None | Omit if not used. |

#### Response Fields

Type: `NetworkApActionResult`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| accepted | Boolean | 0x01 | Whether the action was accepted. | None | N/A |
| state | NetworkApState | 0x02 | Current or target AP state. | None | N/A |

---

### network.getApState

Return runtime state for the device Wi-Fi AP role.

- Method ID: `0x0E0E`
- Domain: `network`
- bitOffset: `14`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.ap`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `PERMISSION_DENIED`, `UNAVAILABLE`

#### Request Fields

Type: `NetworkGetApConfigParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi AP interface identifier. | maxLength=64 | Omit if not used. |

#### Response Fields

Type: `NetworkApState`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi AP interface identifier. | maxLength=64 | Omit if not used. |
| enabled | Boolean | 0x02 | Whether AP is currently enabled. | None | N/A |
| state | Enum | 0x03 | AP state; candidate values include disabled, starting, enabled, stopping, failed, and unknown. | None | N/A |
| ?ssid | String | 0x04 | Active AP SSID. | maxLength=64 | Omit if not used. |
| ?clientCount | UInt16 | 0x05 | Current associated client count. | None | Omit if not used. |
| ?failureReason | Enum | 0x06 | Failure reason when state is failed. | None | Omit if not used. |

---

### network.getInterfaces

Return visible network interfaces and default interface identifiers.

- Method ID: `0x0E10`
- Domain: `network`
- bitOffset: `0`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.interface`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `PERMISSION_DENIED`, `UNAVAILABLE`

#### Request Fields

Type: `NetworkGetInterfacesParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?includeDisabled | Boolean | 0x01 | Whether disabled interfaces should be included. | None | Omit if not used. |

#### Response Fields

Type: `NetworkInterfaces`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| interfaces | Bytes | 0x01 | JSON array of NetworkInterfaceSummary objects. | maxLength=8192 | N/A |
| ?defaults | NetworkDefaultInterfaceIds | 0x02 | Default interface identifiers for common roles. | None | Omit if not used. |

---

### network.getInterfaceInfo

Return detailed information for one network interface.

- Method ID: `0x0E11`
- Domain: `network`
- bitOffset: `1`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.interface`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `NOT_FOUND`, `PERMISSION_DENIED`, `UNAVAILABLE`

#### Request Fields

Type: `NetworkGetInterfaceInfoParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| interfaceId | String | 0x01 | Interface identifier. | maxLength=64 | N/A |

#### Response Fields

Type: `NetworkInterfaceInfo`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| interfaceId | String | 0x01 | Interface identifier. | maxLength=64 | N/A |
| type | Enum | 0x02 | Interface type. | None | N/A |
| ?macAddress | String | 0x03 | Interface MAC address, if available and permitted. | maxLength=32 | Omit if not used. |
| ?state | NetworkInterfaceState | 0x04 | Current interface state. | None | Omit if not used. |
| ?supportsIpConfig | Boolean | 0x05 | Whether this interface can be used with network.ip. | None | Omit if not used. |

---

### network.getWifiCapabilities

Return Wi-Fi station capability, including security types, bands, scanning, and credential import modes.

- Method ID: `0x0E12`
- Domain: `network`
- bitOffset: `4`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.wifi`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `NOT_FOUND`, `PERMISSION_DENIED`, `UNAVAILABLE`

#### Request Fields

Type: `NetworkGetWifiCapabilitiesParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi station interface identifier. | maxLength=64 | Omit if not used. |

#### Response Fields

Type: `NetworkWifiCapabilities`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| capability | String | 0x01 | Fixed capability name network.wifi. | maxLength=32 | N/A |
| securityTypes | Bytes | 0x02 | JSON array of supported security type strings. | maxLength=1024 | N/A |
| ?bands | Bytes | 0x03 | JSON array of supported Wi-Fi bands. | maxLength=512 | Omit if not used. |
| credentialImportModes | Bytes | 0x04 | JSON array of supported credential import modes such as passphrase, pairing_token, and opaque_ref. | maxLength=512 | N/A |
| savedProfilesSupported | Boolean | 0x05 | Whether saved profiles are supported. | None | N/A |
| scanSupported | Boolean | 0x06 | Whether Wi-Fi scanning is supported. | None | N/A |
| ?autoConnectSupported | Boolean | 0x07 | Whether profiles can auto-connect. | None | Omit if not used. |

---

### network.getApCapabilities

Return Wi-Fi AP capability, including supported bands, security types, channel ranges, and credential export policy.

- Method ID: `0x0E13`
- Domain: `network`
- bitOffset: `11`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.ap`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `PERMISSION_DENIED`, `UNAVAILABLE`

#### Request Fields

Type: `NetworkGetApCapabilitiesParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi AP interface identifier. | maxLength=64 | Omit if not used. |

#### Response Fields

Type: `NetworkApCapabilities`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| capability | String | 0x01 | Fixed capability name network.ap. | maxLength=32 | N/A |
| securityTypes | Bytes | 0x02 | JSON array of supported security types. | maxLength=1024 | N/A |
| ?bands | Bytes | 0x03 | JSON array of supported bands. | maxLength=512 | Omit if not used. |
| ?credentialExportModes | Bytes | 0x04 | JSON array of credential export modes. | maxLength=512 | Omit if not used. |
| ?clientsSupported | Boolean | 0x05 | Whether client list query and client change events are supported. | None | Omit if not used. |

---

### network.getApClients

Return clients currently associated with the Wi-Fi AP.

- Method ID: `0x0E14`
- Domain: `network`
- bitOffset: `17`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `network.ap`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `PERMISSION_DENIED`, `UNAVAILABLE`

#### Request Fields

Type: `NetworkGetApConfigParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi AP interface identifier. | maxLength=64 | Omit if not used. |

#### Response Fields

Type: `NetworkApClients`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| clients | Bytes | 0x01 | JSON array of NetworkApClientInfo objects. | maxLength=16384 | N/A |

---

## video Methods

### Methods in this domain

- [video.openStream](#videoopenstream)
- [video.closeStream](#videoclosestream)
- [video.getStreamState](#videogetstreamstate)
- [video.getStreamCapabilities](#videogetstreamcapabilities)
- [video.getStreamSourceState](#videogetstreamsourcestate)
- [video.requestKeyFrame](#videorequestkeyframe)

---

### video.openStream

Open a real-time video STREAM and return the negotiated streamId and media metadata.

- Method ID: `0x080B`
- Domain: `video`
- bitOffset: `1`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `video.stream`
- Possible Events: `video.streamStateChanged`, `video.streamSourceStateChanged`
- Possible Errors: `SUCCESS`, `INVALID_ARGUMENT`, `BUSY`, `RESOURCE_EXHAUSTED`, `MEDIA_SOURCE_NOT_FOUND`, `MEDIA_SOURCE_UNAVAILABLE`, `MEDIA_CODEC_UNSUPPORTED`, `MEDIA_RESOLUTION_UNSUPPORTED`, `MEDIA_FRAMERATE_UNSUPPORTED`, `MEDIA_STREAM_START_FAILED`

#### Request Fields

Type: `VideoOpenStreamParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| source | String | 0x01 | Video source identifier. | maxLength=128 | N/A |
| peerRole | Enum | 0x02 | Requested peer media role; values include receiver and transmitter. | None | N/A |
| codec | Enum | 0x03 | Requested video codec, such as h264, h265, mjpeg, or raw. | None | N/A |
| ?width | UInt32 | 0x04 | Requested frame width in pixels. | None | Omit if not used. |
| ?height | UInt32 | 0x05 | Requested frame height in pixels. | None | Omit if not used. |
| ?frameRate | UInt32 | 0x06 | Requested frame rate. | None | Omit if not used. |
| ?bitrateKbps | UInt32 | 0x07 | Requested bitrate in kbps. | None | Omit if not used. |
| ?streamProfile | String | 0x08 | STREAM profile name. | maxLength=64 | Omit if not used. |
| ?cursorUnit | Enum | 0x09 | STREAM cursor unit, such as timestampUs or frameIndex. | None | Omit if not used. |
| ?syncGroupId | String | 0x0A | Optional synchronization group identifier. | maxLength=128 | Omit if not used. |
| ?castSessionId | String | 0x0B | Optional cast session identifier. | maxLength=128 | Omit if not used. |
| ?clockDomain | String | 0x0C | Source media clock domain. | maxLength=128 | Omit if not used. |
| ?maxDataSize | UInt32 | 0x0D | Preferred maximum STREAM payload data size. | None | Omit if not used. |

#### Response Fields

Type: `VideoOpenStreamResult`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| streamId | UInt32 | 0x01 | STREAM data plane stream identifier. | None | N/A |
| state | Enum | 0x02 | Initial state, normally opening or streaming. | None | N/A |
| source | String | 0x03 | Bound source identifier. | maxLength=128 | N/A |
| peerRole | Enum | 0x04 | Confirmed peer media role. | None | N/A |
| codec | Enum | 0x05 | Negotiated codec. | None | N/A |
| ?width | UInt32 | 0x06 | Negotiated frame width. | None | Omit if not used. |
| ?height | UInt32 | 0x07 | Negotiated frame height. | None | Omit if not used. |
| ?frameRate | UInt32 | 0x08 | Negotiated frame rate. | None | Omit if not used. |
| ?bitrateKbps | UInt32 | 0x09 | Negotiated bitrate in kbps. | None | Omit if not used. |
| streamProfile | String | 0x0A | Normalized stream profile. | maxLength=64 | N/A |
| cursorUnit | Enum | 0x0B | STREAM cursor unit. | None | N/A |
| ?syncGroupId | String | 0x0C | Synchronization group identifier. | maxLength=128 | Omit if not used. |
| ?maxDataSize | UInt32 | 0x0D | Negotiated maximum STREAM payload data size. | None | Omit if not used. |

---

### video.closeStream

Close a previously opened video STREAM.

- Method ID: `0x080C`
- Domain: `video`
- bitOffset: `2`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `video.stream`
- Possible Events: `video.streamStateChanged`
- Possible Errors: `SUCCESS`, `STREAM_NOT_FOUND`, `STREAM_CLOSED`, `INVALID_STATE`, `MEDIA_STREAM_STOP_FAILED`

#### Request Fields

Type: `VideoCloseStreamParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| streamId | UInt32 | 0x01 | STREAM data plane stream identifier. | None | N/A |
| ?peerRole | Enum | 0x02 | Peer role in this stream. | None | Omit if not used. |
| ?reason | Enum | 0x03 | Close reason. | None | Omit if not used. |
| ?finalCursor | UInt64 | 0x04 | Last processed cursor value. | None | Omit if not used. |

#### Response Fields

Type: `VideoCloseStreamResult`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| streamId | UInt32 | 0x01 | Closed stream identifier. | None | N/A |
| state | Enum | 0x02 | Close state, such as closing, closed, or failed. | None | N/A |
| ?reason | Enum | 0x03 | Final close reason. | None | Omit if not used. |
| ?alreadyClosed | Boolean | 0x04 | Whether the stream was already terminal before this request. | None | Omit if not used. |

---

### video.getStreamState

Return runtime state for an opened video stream.

- Method ID: `0x080D`
- Domain: `video`
- bitOffset: `3`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `video.stream`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `STREAM_NOT_FOUND`, `PERMISSION_DENIED`, `UNAVAILABLE`

#### Request Fields

Type: `VideoGetStreamStateParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| streamId | UInt32 | 0x01 | STREAM data plane stream identifier. | None | N/A |

#### Response Fields

Type: `VideoStreamState`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| streamId | UInt32 | 0x01 | STREAM data plane stream identifier. | None | N/A |
| state | Enum | 0x02 | Stream state, such as opening, streaming, closing, closed, or failed. | None | N/A |
| source | String | 0x03 | Bound video source. | maxLength=128 | N/A |
| ?peerRole | Enum | 0x04 | Peer media role. | None | Omit if not used. |
| ?codec | Enum | 0x05 | Negotiated video codec. | None | Omit if not used. |
| ?streamProfile | String | 0x06 | Stream profile. | maxLength=64 | Omit if not used. |
| ?syncGroupId | String | 0x07 | Synchronization group identifier. | maxLength=128 | Omit if not used. |
| ?cursorUnit | Enum | 0x08 | STREAM cursor unit. | None | Omit if not used. |
| ?lastCursor | UInt64 | 0x09 | Last known cursor value. | None | Omit if not used. |
| ?keyFrameRequested | Boolean | 0x0A | Whether a key frame has been requested and is pending. | None | Omit if not used. |
| ?failureReason | Enum | 0x0B | Failure reason when state is failed. | None | Omit if not used. |

---

### video.getStreamCapabilities

Return video stream sources, codecs, profiles, and open-mode support.

- Method ID: `0x0812`
- Domain: `video`
- bitOffset: `0`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `video.stream`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `PERMISSION_DENIED`, `UNAVAILABLE`

#### Request Fields

Type: `VideoGetStreamCapabilitiesParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?source | String | 0x01 | Optional video source identifier; omit to query all visible sources. | maxLength=128 | Omit if not used. |
| ?includeRuntimeState | Boolean | 0x02 | Whether to include current source runtime state. | None | Omit if not used. |

#### Response Fields

Type: `VideoStreamCapabilities`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| capability | String | 0x01 | Fixed capability name video.stream. | maxLength=32 | N/A |
| sources | Bytes | 0x02 | JSON array of VideoStreamSource objects. | maxLength=8192 | N/A |
| streamProfiles | Bytes | 0x03 | JSON array of supported stream profiles, normally media.video. | maxLength=512 | N/A |
| openModes | Bytes | 0x04 | JSON array of supported open modes, such as producer_open and receiver_pull. | maxLength=512 | N/A |
| peerRoles | Bytes | 0x05 | JSON array of peer roles, such as receiver and transmitter. | maxLength=512 | N/A |
| supportsSourceStateEvent | Boolean | 0x06 | Whether video.streamSourceStateChanged is supported. | None | N/A |
| supportsSyncGroup | Boolean | 0x07 | Whether video streams can share a synchronization group with audio streams. | None | N/A |
| flowControlManagedByRuntime | Boolean | 0x08 | Whether normal applications can rely on runtime-managed STREAM flow control. | None | N/A |

---

### video.getStreamSourceState

Return availability and receiving state for a video stream source.

- Method ID: `0x0813`
- Domain: `video`
- bitOffset: `4`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `video.stream`
- Possible Events: `None`
- Possible Errors: `SUCCESS`, `NOT_SUPPORTED`, `INVALID_ARGUMENT`, `MEDIA_SOURCE_NOT_FOUND`, `UNAVAILABLE`

#### Request Fields

Type: `VideoGetStreamSourceStateParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| source | String | 0x01 | Video source identifier. | maxLength=128 | N/A |

#### Response Fields

Type: `VideoStreamSourceState`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| source | String | 0x01 | Video source identifier. | maxLength=128 | N/A |
| ?mediaKind | Enum | 0x02 | Media kind, normally video. | None | Omit if not used. |
| state | Enum | 0x03 | Source state, such as unavailable, available, receiving, stopped, or failed. | None | N/A |
| ?available | Boolean | 0x04 | Whether the source is available for openStream. | None | Omit if not used. |
| ?activeStreamId | UInt32 | 0x05 | Active downstream stream id, if any. | None | Omit if not used. |

---

### video.requestKeyFrame

Request an encoder key frame for an active video stream.

- Method ID: `0x0814`
- Domain: `video`
- bitOffset: `5`
- Status: `draft`
- Added in v1.0.0
- Encodings: `json`, `tlv`
- Required Capabilities: `video.stream`
- Possible Events: `video.streamStateChanged`
- Possible Errors: `SUCCESS`, `STREAM_NOT_FOUND`, `INVALID_STATE`, `MEDIA_STREAM_START_FAILED`, `PERMISSION_DENIED`

#### Request Fields

Type: `VideoRequestKeyFrameParams`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| streamId | UInt32 | 0x01 | STREAM data plane stream identifier. | None | N/A |
| ?reason | Enum | 0x02 | Request reason. | None | Omit if not used. |

#### Response Fields

Type: `VideoRequestKeyFrameResult`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| accepted | Boolean | 0x01 | Whether the request was accepted. | None | N/A |
| ?state | VideoStreamState | 0x02 | Current or updated stream state. | None | Omit if not used. |

---

# Events

## audio Events

### Events in this domain

- [audio.algorithmConfigChanged](#audioalgorithmconfigchanged)
- [audio.streamStateChanged](#audiostreamstatechanged)
- [audio.streamSourceStateChanged](#audiostreamsourcestatechanged)
- [audio.streamStatsReported](#audiostreamstatsreported)

---

### audio.algorithmConfigChanged

Emitted when audio algorithm configuration changes after set, reset, profile, restore, factory reset, or device policy changes.

- Event ID: `0x0901`
- Domain: `audio`
- bitOffset: `0`
- Status: `stable`
- Severity: `info`
- Added in v1.0.0
- Trigger: `audio.setAlgorithmConfig`, `audio.resetAlgorithmConfig`, `profile changed`, `factory reset`, `restore config`, `device policy`
- Required Capabilities: `audio.algorithm`

#### Payload Fields

Type: `AudioAlgorithmConfigChangedEvent`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| reason | Enum | 0x01 | Change reason; values include user_request, reset_to_default, factory_reset, profile_changed, device_policy, restore_config, and unknown. | None | N/A |
| applyState | Enum | 0x02 | Apply state; values are applied or pending_restart. | None | N/A |
| requiresAudioRestart | Boolean | 0x03 | Whether the change requires restarting the audio link or rebuilding the audio pipeline. | None | N/A |
| config | AudioAlgorithmConfig | 0x04 | Changed or affected algorithm configuration values. | None | N/A |
| ?changedFields | Bytes | 0x05 | Optional JSON array of changed field paths such as noiseSuppression.level. | maxLength=256 | Omit if not used. |

---

### audio.streamStateChanged

Emitted when an audio stream enters opening, streaming, closed, or failed state.

- Event ID: `0x0902`
- Domain: `audio`
- bitOffset: `1`
- Status: `draft`
- Severity: `info`
- Added in v1.0.0
- Trigger: `audio.openStream`, `audio.closeStream`, `source disconnected`, `stream failure`
- Required Capabilities: `audio.stream`

#### Payload Fields

Type: `AudioStreamStateChangedEvent`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| streamId | UInt32 | 0x01 | STREAM data plane stream identifier. | None | N/A |
| state | Enum | 0x02 | New stream state. | None | N/A |
| source | String | 0x03 | Bound audio source. | maxLength=128 | N/A |
| ?reason | Enum | 0x04 | State change reason. | None | Omit if not used. |
| ?stats | AudioStreamStats | 0x05 | Optional bounded stream statistics. | None | Omit if not used. |

---

### audio.streamSourceStateChanged

Emitted when an audio stream source availability or receiving state changes.

- Event ID: `0x0903`
- Domain: `audio`
- bitOffset: `2`
- Status: `draft`
- Severity: `info`
- Added in v1.0.0
- Trigger: `upstream source available`, `upstream source receiving`, `upstream source stopped`
- Required Capabilities: `audio.stream`

#### Payload Fields

Type: `AudioStreamSourceStateChangedEvent`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| source | String | 0x01 | Audio source identifier. | maxLength=128 | N/A |
| ?mediaKind | Enum | 0x02 | Media kind, normally audio. | None | Omit if not used. |
| state | Enum | 0x03 | New source state. | None | N/A |
| ?reason | Enum | 0x04 | Source state change reason. | None | Omit if not used. |
| ?activeStreamId | UInt32 | 0x05 | Active downstream stream id, if any. | None | Omit if not used. |

---

### audio.streamStatsReported

Emitted with bounded runtime statistics for an audio stream.

- Event ID: `0x0904`
- Domain: `audio`
- bitOffset: `3`
- Status: `draft`
- Severity: `info`
- Added in v1.0.0
- Trigger: `stream statistics interval`, `diagnostic sampling`
- Required Capabilities: `audio.stream`

#### Payload Fields

Type: `AudioStreamStatsReportedEvent`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| streamId | UInt32 | 0x01 | STREAM data plane stream identifier. | None | N/A |
| stats | AudioStreamStats | 0x02 | Bounded stream statistics. | None | N/A |

---

## firmware Events

### Events in this domain

- [firmware.updateProgressReported](#firmwareupdateprogressreported)
- [firmware.updateStateChanged](#firmwareupdatestatechanged)

---

### firmware.updateProgressReported

Emitted when firmware receiving, verification, or install progress changes.

- Event ID: `0x0402`
- Domain: `firmware`
- bitOffset: `0`
- Status: `draft`
- Severity: `info`
- Added in v1.0.0
- Trigger: `STREAM upload progress`, `firmware.finishUpdate`, `internal verify`, `internal install`
- Required Capabilities: `firmware.update`

#### Payload Fields

Type: `FirmwareUpdateProgressEvent`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| updateSessionId | String | 0x01 | Firmware update session identifier. | maxLength=128 | N/A |
| state | Enum | 0x02 | Current firmware update state. | None | N/A |
| ?progress | UInt8 | 0x03 | Overall progress percentage. | min=0, max=100 | Omit if not used. |
| ?fileId | String | 0x04 | Current file identifier. | maxLength=128 | Omit if not used. |

---

### firmware.updateStateChanged

Emitted when firmware update state changes or fails.

- Event ID: `0x0403`
- Domain: `firmware`
- bitOffset: `1`
- Status: `draft`
- Severity: `info`
- Added in v1.0.0
- Trigger: `firmware.beginUpdate`, `firmware.finishUpdate`, `verify complete`, `install complete`, `rebooting`, `failure`
- Required Capabilities: `firmware.update`

#### Payload Fields

Type: `FirmwareUpdateStateChangedEvent`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| updateSessionId | String | 0x01 | Firmware update session identifier. | maxLength=128 | N/A |
| state | Enum | 0x02 | New firmware update state. | None | N/A |
| ?error | FirmwareUpdateErrorInfo | 0x03 | Error details when state is failed. | None | Omit if not used. |

---

## network Events

### Events in this domain

- [network.interfaceStateChanged](#networkinterfacestatechanged)
- [network.ipConfigChanged](#networkipconfigchanged)
- [network.wifiConfigChanged](#networkwificonfigchanged)
- [network.wifiStateChanged](#networkwifistatechanged)
- [network.wifiScanResultReported](#networkwifiscanresultreported)
- [network.apConfigChanged](#networkapconfigchanged)
- [network.apStateChanged](#networkapstatechanged)
- [network.apClientChanged](#networkapclientchanged)

---

### network.interfaceStateChanged

Emitted when network interface administrative or link state changes.

- Event ID: `0x0E01`
- Domain: `network`
- bitOffset: `0`
- Status: `draft`
- Severity: `info`
- Added in v1.0.0
- Trigger: `link state change`, `admin state change`, `interface availability change`
- Required Capabilities: `network.interface`

#### Payload Fields

Type: `NetworkInterfaceStateChangedEvent`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| interfaceId | String | 0x01 | Interface identifier. | maxLength=64 | N/A |
| state | NetworkInterfaceState | 0x02 | New interface state. | None | N/A |
| ?previousState | NetworkInterfaceState | 0x03 | Previous interface state. | None | Omit if not used. |
| ?reason | Enum | 0x04 | Change reason. | None | Omit if not used. |

---

### network.ipConfigChanged

Emitted when IP configuration changes for an interface.

- Event ID: `0x0E02`
- Domain: `network`
- bitOffset: `1`
- Status: `draft`
- Severity: `info`
- Added in v1.0.0
- Trigger: `network.setIpConfig`, `DHCP renew`, `device policy`
- Required Capabilities: `network.ip`

#### Payload Fields

Type: `NetworkIpConfigChangedEvent`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| interfaceId | String | 0x01 | Interface identifier. | maxLength=64 | N/A |
| ?family | Enum | 0x02 | IP family. | None | Omit if not used. |
| config | NetworkIpConfig | 0x03 | New IP configuration. | None | N/A |
| ?previousConfig | NetworkIpConfig | 0x04 | Previous IP configuration. | None | Omit if not used. |
| ?reason | Enum | 0x05 | Change reason. | None | Omit if not used. |

---

### network.wifiConfigChanged

Emitted when saved Wi-Fi profile configuration changes.

- Event ID: `0x0E03`
- Domain: `network`
- bitOffset: `2`
- Status: `draft`
- Severity: `info`
- Added in v1.0.0
- Trigger: `network.setWifiConfig`, `device policy`
- Required Capabilities: `network.wifi`

#### Payload Fields

Type: `NetworkWifiConfigChangedEvent`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi station interface identifier. | maxLength=64 | Omit if not used. |
| config | NetworkWifiConfig | 0x02 | New Wi-Fi configuration summary. | None | N/A |
| ?changedFields | Bytes | 0x03 | JSON array of changed field paths. | maxLength=1024 | Omit if not used. |
| ?reason | Enum | 0x04 | Change reason. | None | Omit if not used. |

---

### network.wifiStateChanged

Emitted when Wi-Fi station association, authentication, connection, or failure state changes.

- Event ID: `0x0E04`
- Domain: `network`
- bitOffset: `3`
- Status: `draft`
- Severity: `info`
- Added in v1.0.0
- Trigger: `network.setWifiConfig`, `network.connectWifi`, `network.disconnectWifi`, `local Wi-Fi state change`
- Required Capabilities: `network.wifi`

#### Payload Fields

Type: `NetworkWifiStateChangedEvent`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| state | NetworkWifiState | 0x01 | New Wi-Fi station state. | None | N/A |
| ?previousState | NetworkWifiState | 0x02 | Previous state. | None | Omit if not used. |
| ?reason | Enum | 0x03 | Change reason. | None | Omit if not used. |

---

### network.wifiScanResultReported

Emitted for asynchronous Wi-Fi scan results or scan completion.

- Event ID: `0x0E05`
- Domain: `network`
- bitOffset: `4`
- Status: `draft`
- Severity: `info`
- Added in v1.0.0
- Trigger: `network.scanWifi`
- Required Capabilities: `network.wifi`

#### Payload Fields

Type: `NetworkWifiScanResultReportedEvent`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?scanId | String | 0x01 | Scan identifier. | maxLength=128 | Omit if not used. |
| ?results | Bytes | 0x02 | JSON array of NetworkWifiScanResult objects. | maxLength=16384 | Omit if not used. |
| complete | Boolean | 0x03 | Whether this event completes the scan. | None | N/A |

---

### network.apConfigChanged

Emitted when Wi-Fi AP configuration changes.

- Event ID: `0x0E06`
- Domain: `network`
- bitOffset: `5`
- Status: `draft`
- Severity: `info`
- Added in v1.0.0
- Trigger: `network.setApConfig`, `device policy`
- Required Capabilities: `network.ap`

#### Payload Fields

Type: `NetworkApConfigChangedEvent`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceId | String | 0x01 | Wi-Fi AP interface identifier. | maxLength=64 | Omit if not used. |
| config | NetworkApConfig | 0x02 | New AP configuration. | None | N/A |
| ?changedFields | Bytes | 0x03 | JSON array of changed field paths. | maxLength=1024 | Omit if not used. |
| ?reason | Enum | 0x04 | Change reason. | None | Omit if not used. |

---

### network.apStateChanged

Emitted when Wi-Fi AP runtime state changes.

- Event ID: `0x0E07`
- Domain: `network`
- bitOffset: `6`
- Status: `draft`
- Severity: `info`
- Added in v1.0.0
- Trigger: `network.startAp`, `network.stopAp`, `local AP state change`
- Required Capabilities: `network.ap`

#### Payload Fields

Type: `NetworkApStateChangedEvent`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| state | NetworkApState | 0x01 | New AP state. | None | N/A |
| ?previousState | NetworkApState | 0x02 | Previous AP state. | None | Omit if not used. |
| ?reason | Enum | 0x03 | Change reason. | None | Omit if not used. |

---

### network.apClientChanged

Emitted when a client joins or leaves the Wi-Fi AP.

- Event ID: `0x0E08`
- Domain: `network`
- bitOffset: `7`
- Status: `draft`
- Severity: `info`
- Added in v1.0.0
- Trigger: `AP client join`, `AP client leave`
- Required Capabilities: `network.ap`

#### Payload Fields

Type: `NetworkApClientChangedEvent`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| change | Enum | 0x01 | Client change type; candidate values include joined, left, and updated. | None | N/A |
| client | NetworkApClientInfo | 0x02 | Client summary. | None | N/A |
| ?reason | Enum | 0x03 | Change reason. | None | Omit if not used. |

---

## video Events

### Events in this domain

- [video.streamStateChanged](#videostreamstatechanged)
- [video.streamSourceStateChanged](#videostreamsourcestatechanged)
- [video.streamStatsReported](#videostreamstatsreported)

---

### video.streamStateChanged

Emitted when a video stream enters opening, streaming, closed, failed, or keyframe-related state.

- Event ID: `0x0806`
- Domain: `video`
- bitOffset: `0`
- Status: `draft`
- Severity: `info`
- Added in v1.0.0
- Trigger: `video.openStream`, `video.closeStream`, `video.requestKeyFrame`, `source disconnected`, `stream failure`
- Required Capabilities: `video.stream`

#### Payload Fields

Type: `VideoStreamStateChangedEvent`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| streamId | UInt32 | 0x01 | STREAM data plane stream identifier. | None | N/A |
| state | Enum | 0x02 | New stream state. | None | N/A |
| source | String | 0x03 | Bound video source. | maxLength=128 | N/A |
| ?reason | Enum | 0x04 | State change reason. | None | Omit if not used. |
| ?stats | VideoStreamStats | 0x05 | Optional bounded stream statistics. | None | Omit if not used. |

---

### video.streamSourceStateChanged

Emitted when a video stream source availability or receiving state changes.

- Event ID: `0x0807`
- Domain: `video`
- bitOffset: `1`
- Status: `draft`
- Severity: `info`
- Added in v1.0.0
- Trigger: `source available`, `source receiving`, `source stopped`
- Required Capabilities: `video.stream`

#### Payload Fields

Type: `VideoStreamSourceStateChangedEvent`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| source | String | 0x01 | Video source identifier. | maxLength=128 | N/A |
| ?mediaKind | Enum | 0x02 | Media kind, normally video. | None | Omit if not used. |
| state | Enum | 0x03 | New source state. | None | N/A |
| ?reason | Enum | 0x04 | Source state change reason. | None | Omit if not used. |
| ?activeStreamId | UInt32 | 0x05 | Active downstream stream id, if any. | None | Omit if not used. |

---

### video.streamStatsReported

Emitted with bounded runtime statistics for a video stream.

- Event ID: `0x0808`
- Domain: `video`
- bitOffset: `2`
- Status: `draft`
- Severity: `info`
- Added in v1.0.0
- Trigger: `stream statistics interval`, `diagnostic sampling`
- Required Capabilities: `video.stream`

#### Payload Fields

Type: `VideoStreamStatsReportedEvent`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| streamId | UInt32 | 0x01 | STREAM data plane stream identifier. | None | N/A |
| stats | VideoStreamStats | 0x02 | Bounded stream statistics. | None | N/A |

---

# Additional Types

## AudioAlgorithmCapabilities

Capability descriptors for audio algorithm objects.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?noiseSuppression | AudioNoiseSuppressionCapabilities | 0x01 | Noise suppression capability descriptor. | None | Omit if not used. |
| ?echoCancellation | AudioEchoCancellationCapabilities | 0x02 | Echo cancellation capability descriptor. | None | Omit if not used. |
| ?autoGainControl | AudioAutoGainControlCapabilities | 0x03 | Automatic gain control capability descriptor. | None | Omit if not used. |
| ?beamforming | AudioBeamformingCapabilities | 0x04 | Beamforming capability descriptor. | None | Omit if not used. |
| ?dereverberation | AudioDereverberationCapabilities | 0x05 | Dereverberation capability descriptor. | None | Omit if not used. |
| ?voiceActivityDetection | AudioVoiceActivityDetectionCapabilities | 0x06 | Voice activity detection capability descriptor. | None | Omit if not used. |
| ?directionOfArrival | AudioDirectionOfArrivalCapabilities | 0x07 | Direction of arrival capability descriptor. | None | Omit if not used. |
| ?howlingSuppression | AudioHowlingSuppressionCapabilities | 0x08 | Howling suppression capability descriptor. | None | Omit if not used. |

---

## AudioAlgorithmCapability

Device-level audio.algorithm capability summary.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?configSchemaVersion | String | 0x01 | Version of the audio algorithm configuration schema exposed by the device. | maxLength=16 | Omit if not used. |
| updatePolicy | AudioAlgorithmUpdatePolicy | 0x02 | Update and atomicity policy for set and reset operations. | None | N/A |
| ?supportedAlgorithms | Bytes | 0x03 | Optional compact list of supported algorithm object names; JSON implementations expose names as an array of strings. | maxLength=64 | Omit if not used. |

---

## AudioAlgorithmPropertyCapability

Descriptor for one algorithm configuration property.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| type | Enum | 0x01 | Property type; values include boolean, enum, uint8, uint16, uint32, int32, float, string, object, and array. | None | N/A |
| ?defaultBool | Boolean | 0x02 | Boolean default value when type is boolean. | None | Omit if not used. |
| ?defaultEnum | String | 0x03 | Enum default value when type is enum. | maxLength=32 | Omit if not used. |
| ?defaultInt32 | Int32 | 0x04 | Numeric default value for integer-backed properties. | None | Omit if not used. |
| ?min | Int32 | 0x05 | Inclusive numeric minimum. | None | Omit if not used. |
| ?max | Int32 | 0x06 | Inclusive numeric maximum. | None | Omit if not used. |
| ?step | Int32 | 0x07 | Numeric step size. | None | Omit if not used. |
| ?values | Bytes | 0x08 | Optional JSON array of enum values. | maxLength=128 | Omit if not used. |
| ?unit | String | 0x09 | Unit such as ms, dB, or degree. | maxLength=16 | Omit if not used. |
| ?requiresAudioRestart | Boolean | 0x0A | Whether modifying this field requires restarting the audio link or rebuilding the audio pipeline. | None | Omit if not used. |

---

## AudioAlgorithmUpdatePolicy

Audio algorithm update and atomicity policy.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| partialUpdateSupported | Boolean | 0x01 | Whether clients may send only the fields they want to modify. | None | N/A |
| multiAlgorithmUpdateSupported | Boolean | 0x02 | Whether one request may update multiple algorithm objects. | None | N/A |
| atomicUpdateSupported | Boolean | 0x03 | Whether set and reset operations are applied atomically. | None | N/A |

---

## AudioAutoGainControlCapabilities

Automatic gain control supported fields.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| supported | Boolean | 0x01 | Whether the device supports autoGainControl. | None | N/A |
| ?displayName | String | 0x02 | UI-readable display name. | maxLength=64 | Omit if not used. |
| ?enabled | AudioAlgorithmPropertyCapability | 0x03 | enabled property descriptor. | None | Omit if not used. |
| ?targetLevelDb | AudioAlgorithmPropertyCapability | 0x04 | targetLevelDb property descriptor. | None | Omit if not used. |
| ?maxGainDb | AudioAlgorithmPropertyCapability | 0x05 | maxGainDb property descriptor. | None | Omit if not used. |
| ?attackTimeMs | AudioAlgorithmPropertyCapability | 0x06 | attackTimeMs property descriptor. | None | Omit if not used. |
| ?releaseTimeMs | AudioAlgorithmPropertyCapability | 0x07 | releaseTimeMs property descriptor. | None | Omit if not used. |

---

## AudioAutoGainControlConfig

Automatic gain control configuration object.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?enabled | Boolean | 0x01 | Whether automatic gain control is enabled. | None | Omit if not used. |
| ?targetLevelDb | Int32 | 0x02 | Target output level in dB. | min=-36, max=-6 | Omit if not used. |
| ?maxGainDb | UInt8 | 0x03 | Maximum gain in dB. | min=0, max=36 | Omit if not used. |
| ?attackTimeMs | UInt32 | 0x04 | Gain attack time in milliseconds. | min=1, max=1000 | Omit if not used. |
| ?releaseTimeMs | UInt32 | 0x05 | Gain release time in milliseconds. | min=10, max=5000 | Omit if not used. |

---

## AudioBeamformingCapabilities

Beamforming supported fields.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| supported | Boolean | 0x01 | Whether the device supports beamforming. | None | N/A |
| ?displayName | String | 0x02 | UI-readable display name. | maxLength=64 | Omit if not used. |
| ?enabled | AudioAlgorithmPropertyCapability | 0x03 | enabled property descriptor. | None | Omit if not used. |
| ?lookDirectionDeg | AudioAlgorithmPropertyCapability | 0x05 | lookDirectionDeg property descriptor. | None | Omit if not used. |
| ?beamWidthDeg | AudioAlgorithmPropertyCapability | 0x06 | beamWidthDeg property descriptor. | None | Omit if not used. |

---

## AudioBeamformingConfig

Beamforming configuration object.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?enabled | Boolean | 0x01 | Whether beamforming is enabled. | None | Omit if not used. |
| ?lookDirectionDeg | Int32 | 0x03 | Fixed beam look direction in degrees. | min=-180, max=180 | Omit if not used. |
| ?beamWidthDeg | UInt32 | 0x04 | Beam width in degrees. | min=10, max=180 | Omit if not used. |

---

## AudioDereverberationCapabilities

Dereverberation supported fields.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| supported | Boolean | 0x01 | Whether the device supports dereverberation. | None | N/A |
| ?displayName | String | 0x02 | UI-readable display name. | maxLength=64 | Omit if not used. |
| ?enabled | AudioAlgorithmPropertyCapability | 0x03 | enabled property descriptor. | None | Omit if not used. |
| ?level | AudioAlgorithmPropertyCapability | 0x05 | level property descriptor. | None | Omit if not used. |

---

## AudioDereverberationConfig

Dereverberation configuration object.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?enabled | Boolean | 0x01 | Whether dereverberation is enabled. | None | Omit if not used. |
| ?level | UInt8 | 0x03 | Dereverberation strength. | min=0, max=3 | Omit if not used. |

---

## AudioDirectionOfArrivalCapabilities

Direction of arrival supported fields.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| supported | Boolean | 0x01 | Whether the device supports directionOfArrival. | None | N/A |
| ?displayName | String | 0x02 | UI-readable display name. | maxLength=64 | Omit if not used. |
| ?enabled | AudioAlgorithmPropertyCapability | 0x03 | enabled property descriptor. | None | Omit if not used. |
| ?reportingEnabled | AudioAlgorithmPropertyCapability | 0x04 | reportingEnabled property descriptor. | None | Omit if not used. |
| ?reportIntervalMs | AudioAlgorithmPropertyCapability | 0x05 | reportIntervalMs property descriptor. | None | Omit if not used. |
| ?smoothingMs | AudioAlgorithmPropertyCapability | 0x06 | smoothingMs property descriptor. | None | Omit if not used. |

---

## AudioDirectionOfArrivalConfig

Direction of arrival configuration object.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?enabled | Boolean | 0x01 | Whether direction of arrival estimation is enabled. | None | Omit if not used. |
| ?reportingEnabled | Boolean | 0x02 | Whether DOA or beam result reporting is enabled by this configuration. | None | Omit if not used. |
| ?reportIntervalMs | UInt32 | 0x03 | Result report interval in milliseconds. | min=20, max=5000 | Omit if not used. |
| ?smoothingMs | UInt32 | 0x04 | Smoothing window in milliseconds. | min=0, max=5000 | Omit if not used. |

---

## AudioEchoCancellationCapabilities

Echo cancellation supported fields.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| supported | Boolean | 0x01 | Whether the device supports echoCancellation. | None | N/A |
| ?displayName | String | 0x02 | UI-readable display name. | maxLength=64 | Omit if not used. |
| ?enabled | AudioAlgorithmPropertyCapability | 0x03 | enabled property descriptor. | None | Omit if not used. |
| ?tailLengthMs | AudioAlgorithmPropertyCapability | 0x05 | tailLengthMs property descriptor. | None | Omit if not used. |
| ?nlpLevel | AudioAlgorithmPropertyCapability | 0x06 | nlpLevel property descriptor. | None | Omit if not used. |

---

## AudioEchoCancellationConfig

Echo cancellation configuration object.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?enabled | Boolean | 0x01 | Whether echo cancellation is enabled. | None | Omit if not used. |
| ?tailLengthMs | UInt32 | 0x03 | Echo tail length in milliseconds; modifying it may require restarting the audio link. | min=64, max=512 | Omit if not used. |
| ?nlpLevel | UInt8 | 0x04 | Non-linear processing strength. | min=0, max=3 | Omit if not used. |

---

## AudioHowlingSuppressionCapabilities

Howling suppression supported fields.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| supported | Boolean | 0x01 | Whether the device supports howlingSuppression. | None | N/A |
| ?displayName | String | 0x02 | UI-readable display name. | maxLength=64 | Omit if not used. |
| ?enabled | AudioAlgorithmPropertyCapability | 0x03 | enabled property descriptor. | None | Omit if not used. |
| ?level | AudioAlgorithmPropertyCapability | 0x05 | level property descriptor. | None | Omit if not used. |

---

## AudioHowlingSuppressionConfig

Howling suppression configuration object.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?enabled | Boolean | 0x01 | Whether howling suppression is enabled. | None | Omit if not used. |
| ?level | UInt8 | 0x03 | Howling suppression strength. | min=0, max=3 | Omit if not used. |

---

## AudioNoiseSuppressionCapabilities

Noise suppression supported fields.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| supported | Boolean | 0x01 | Whether the device supports noiseSuppression. | None | N/A |
| ?displayName | String | 0x02 | UI-readable display name. | maxLength=64 | Omit if not used. |
| ?enabled | AudioAlgorithmPropertyCapability | 0x03 | enabled property descriptor. | None | Omit if not used. |
| ?level | AudioAlgorithmPropertyCapability | 0x05 | level property descriptor. | None | Omit if not used. |

---

## AudioNoiseSuppressionConfig

Noise suppression configuration object.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?enabled | Boolean | 0x01 | Whether noise suppression is enabled. | None | Omit if not used. |
| ?level | UInt8 | 0x03 | Suppression strength. | min=0, max=3 | Omit if not used. |

---

## AudioStreamSource

One real-time audio stream source.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| source | String | 0x01 | Source identifier such as wireless_cast_audio. | maxLength=128 | N/A |
| ?displayName | String | 0x02 | User-visible source name. | maxLength=128 | Omit if not used. |
| codecs | Bytes | 0x03 | JSON array of supported audio codecs. | maxLength=512 | N/A |
| ?sampleRates | Bytes | 0x04 | JSON array of supported sample rates in Hz. | maxLength=512 | Omit if not used. |
| ?channels | Bytes | 0x05 | JSON array of supported channel counts. | maxLength=256 | Omit if not used. |
| ?state | Enum | 0x06 | Runtime source state, such as available, receiving, stopped, or unavailable. | None | Omit if not used. |

---

## AudioStreamStats

Bounded runtime statistics for an audio stream.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?packets | UInt64 | 0x01 | Number of STREAM packets observed. | None | Omit if not used. |
| ?bytes | UInt64 | 0x02 | Number of STREAM payload bytes observed. | None | Omit if not used. |
| ?droppedPackets | UInt64 | 0x03 | Number of dropped packets. | None | Omit if not used. |
| ?jitterMs | UInt32 | 0x04 | Estimated jitter in milliseconds. | None | Omit if not used. |

---

## AudioVoiceActivityDetectionCapabilities

Voice activity detection supported fields.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| supported | Boolean | 0x01 | Whether the device supports voiceActivityDetection. | None | N/A |
| ?displayName | String | 0x02 | UI-readable display name. | maxLength=64 | Omit if not used. |
| ?enabled | AudioAlgorithmPropertyCapability | 0x03 | enabled property descriptor. | None | Omit if not used. |
| ?sensitivity | AudioAlgorithmPropertyCapability | 0x04 | sensitivity property descriptor. | None | Omit if not used. |
| ?hangoverMs | AudioAlgorithmPropertyCapability | 0x05 | hangoverMs property descriptor. | None | Omit if not used. |

---

## AudioVoiceActivityDetectionConfig

Voice activity detection configuration object.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?enabled | Boolean | 0x01 | Whether voice activity detection is enabled. | None | Omit if not used. |
| ?sensitivity | UInt8 | 0x02 | Detection sensitivity. | min=0, max=3 | Omit if not used. |
| ?hangoverMs | UInt32 | 0x03 | Speech-end hangover time in milliseconds. | min=0, max=2000 | Omit if not used. |

---

## ControlAcceptBody

Kind: `object`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| sessionId | UInt32 | 0x01 | - | None | N/A |
| protocolVersion | UInt8 | 0x02 | - | min=1, max=15 | N/A |
| ?reservedHeaderProfile | UInt8 | 0x03 | - | min=1, max=2, deprecated | Omit if not used. |
| maxFrameSize | UInt16 | 0x04 | - | min=1, max=65535 | N/A |
| mtu | UInt16 | 0x06 | - | min=1, max=65535 | N/A |
| supportedPayloadTypes | Bitmap | 0x07 | - | None | N/A |
| heartbeatIntervalMs | UInt32 | 0x0A | - | min=500, max=60000 | N/A |
| ackMode | UInt8 | 0x0B | - | min=0, max=4 | N/A |
| selectedRpcEncoding | UInt8 | 0x1E | - | min=1, max=4 | N/A |

---

## ControlOpenBody

Kind: `object`

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| protocolVersion | UInt8 | 0x02 | - | min=1, max=15 | N/A |
| ?reservedHeaderProfile | UInt8 | 0x03 | - | min=1, max=2, deprecated | Omit if not used. |
| maxFrameSize | UInt16 | 0x04 | - | min=1, max=65535 | N/A |
| mtu | UInt16 | 0x06 | - | min=1, max=65535 | N/A |
| supportedPayloadTypes | Bitmap | 0x07 | - | None | N/A |
| supportedRpcEncodings | Bitmap | 0x08 | - | None | N/A |
| heartbeatIntervalMs | UInt32 | 0x0A | - | min=500, max=60000 | N/A |
| ackMode | UInt8 | 0x0B | - | min=0, max=4 | N/A |

---

## DeviceAxtpRuntime

AXTP runtime summary.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?axtpRuntime | String | 0x01 | AXTP runtime implementation name. | maxLength=128 | Omit if not used. |
| ?axtpRuntimeVersion | String | 0x02 | AXTP runtime implementation version. | maxLength=64 | Omit if not used. |
| ?hostAppId | String | 0x03 | Host application identifier. | maxLength=64 | Omit if not used. |

---

## DeviceCapabilitySummary

Lightweight capability modeling summary returned by device.getInfo.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?domains | Bytes | 0x01 | JSON array of domain names represented by the device. | maxLength=1024 | Omit if not used. |
| ?features | Bytes | 0x02 | JSON array of domain.feature names represented by the device. | maxLength=4096 | Omit if not used. |
| ?profiles | Bytes | 0x03 | JSON array of profile names or product profile hints. | maxLength=1024 | Omit if not used. |

---

## DeviceHardware

Hardware summary.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?revision | String | 0x01 | Hardware revision. | maxLength=32 | Omit if not used. |
| ?cpuArch | Enum | 0x02 | CPU architecture; candidate values include x86_64, arm64, armv7, riscv64, and unknown. | None | Omit if not used. |
| ?memoryBytes | UInt64 | 0x03 | Physical memory capacity in bytes. | None | Omit if not used. |

---

## DeviceIdentity

Stable identity fields for the current main device.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| deviceId | String | 0x01 | Stable AXTP or business device identifier. | maxLength=128 | N/A |
| ?serialNumber | String | 0x02 | Vendor serial number; may be omitted by permission policy. | maxLength=128 | Omit if not used. |
| ?vendorId | String | 0x03 | Vendor identifier. | maxLength=64 | Omit if not used. |
| ?productId | String | 0x04 | Product identifier. | maxLength=64 | Omit if not used. |

---

## DeviceInfoCapability

Capability descriptor for device.info.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| readOnly | Boolean | 0x01 | device.info currently exposes only read-only information. | None | N/A |
| ?supportsCapabilitySummary | Boolean | 0x02 | Whether device.getInfo can include DeviceCapabilitySummary. | None | Omit if not used. |
| ?identityMerged | Boolean | 0x03 | Whether device.identity has been merged into device.info. | None | Omit if not used. |

---

## DeviceOs

Operating system summary.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| type | Enum | 0x01 | OS type; candidate values include windows, android, linux, rtos, and unknown. | None | N/A |
| ?name | String | 0x02 | OS display name. | maxLength=128 | Omit if not used. |
| ?version | String | 0x03 | OS version string. | maxLength=64 | Omit if not used. |
| ?arch | Enum | 0x04 | OS architecture; candidate values include x86_64, arm64, armv7, riscv64, and unknown. | None | Omit if not used. |

---

## DeviceProduct

Product and user-visible model information.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?brand | String | 0x01 | Product brand. | maxLength=64 | Omit if not used. |
| productType | Enum | 0x02 | Product type; candidate values include windowsDevice, androidDevice, embeddedDevice, rtosDevice, cameraDevice, displayDevice, and unknown. | None | N/A |
| model | String | 0x03 | Hardware or whole-product model. | maxLength=128 | N/A |
| ?displayName | String | 0x04 | User-visible display name; this feature exposes it as read-only. | maxLength=128 | Omit if not used. |

---

## DeviceSoftware

Software component summary.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?components | Bytes | 0x01 | JSON array of SoftwareComponent objects. | maxLength=4096 | Omit if not used. |

---

## FirmwareUpdateErrorInfo

Firmware update error details.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| error | Enum | 0x01 | Candidate or adopted error name. | None | N/A |
| ?message | String | 0x02 | Developer-facing error message. | maxLength=256 | Omit if not used. |
| ?fileId | String | 0x03 | Related file identifier, if applicable. | maxLength=128 | Omit if not used. |

---

## FirmwareUpdateFile

One file in the firmware update manifest.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| fileId | String | 0x01 | Manifest-scoped file identifier. | maxLength=128 | N/A |
| ?target | String | 0x02 | Device-defined target component or partition. | maxLength=128 | Omit if not used. |
| size | UInt64 | 0x03 | File size in bytes. | None | N/A |
| md5 | String | 0x04 | File md5 digest as lowercase hexadecimal. | maxLength=32 | N/A |

---

## FirmwareUpdateManifest

Minimal firmware update manifest.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?packageId | String | 0x01 | Firmware package identifier. | maxLength=128 | Omit if not used. |
| ?version | String | 0x02 | Target firmware version string. | maxLength=64 | Omit if not used. |
| files | Bytes | 0x03 | JSON array of FirmwareUpdateFile objects. | maxLength=16384 | N/A |
| ?devicePolicyVersion | String | 0x04 | Optional policy version used to interpret the package. | maxLength=64 | Omit if not used. |

---

## FirmwareUpdateStreamBinding

Binding between a manifest file and a STREAM streamId.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| fileId | String | 0x01 | Manifest-scoped file identifier. | maxLength=128 | N/A |
| streamId | UInt32 | 0x02 | STREAM data plane stream identifier. | None | N/A |

---

## NetworkApClientInfo

One AP client summary.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| clientId | String | 0x01 | Client identifier. | maxLength=128 | N/A |
| ?macAddress | String | 0x02 | Client MAC address, if available and permitted. | maxLength=32 | Omit if not used. |
| ?displayName | String | 0x03 | Client display name. | maxLength=128 | Omit if not used. |
| ?rssi | Int32 | 0x04 | Client RSSI in dBm. | None | Omit if not used. |
| ?connectedSeconds | UInt32 | 0x05 | Connection age in seconds. | None | Omit if not used. |

---

## NetworkCredential

Credential descriptor or secret reference.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| type | Enum | 0x01 | Credential type; candidate values include passphrase, pairing_token, opaque_ref, and none. | None | N/A |
| ?secretRef | String | 0x02 | Opaque reference to sensitive credential material. | maxLength=256 | Omit if not used. |
| ?expiresInSeconds | UInt32 | 0x03 | Relative validity lifetime for ephemeral credentials. | None | Omit if not used. |

---

## NetworkDefaultInterfaceIds

Default network interface identifiers by role.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?primary | String | 0x01 | Primary interface. | maxLength=64 | Omit if not used. |
| ?wifiSta | String | 0x02 | Default Wi-Fi station interface. | maxLength=64 | Omit if not used. |
| ?wifiAp | String | 0x03 | Default Wi-Fi AP interface. | maxLength=64 | Omit if not used. |

---

## NetworkInterfaceCapability

Capability descriptor for network.interface.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?interfaceTypes | Bytes | 0x01 | JSON array of supported interface type strings. | maxLength=512 | Omit if not used. |
| ?supportsStateEvent | Boolean | 0x02 | Whether network.interfaceStateChanged is supported. | None | Omit if not used. |

---

## NetworkInterfaceState

Network interface administrative and link state.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?admin | Enum | 0x01 | Administrative state; candidate values include up, down, disabled, and unknown. | None | Omit if not used. |
| ?link | Enum | 0x02 | Link state; candidate values include up, down, dormant, unknown. | None | Omit if not used. |
| ?speedMbps | UInt32 | 0x03 | Link speed in Mbps. | None | Omit if not used. |

---

## NetworkInterfaceSummary

Summary of one network interface.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| interfaceId | String | 0x01 | Interface identifier. | maxLength=64 | N/A |
| type | Enum | 0x02 | Interface type; candidate values include ethernet, wifi, cellular, usb, virtual, and unknown. | None | N/A |
| ?displayName | String | 0x03 | User-visible interface name. | maxLength=128 | Omit if not used. |
| ?state | NetworkInterfaceState | 0x04 | Current interface state. | None | Omit if not used. |

---

## NetworkIpCapability

Capability descriptor for network.ip.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?families | Bytes | 0x01 | JSON array of supported IP families. | maxLength=256 | Omit if not used. |
| ?modes | Bytes | 0x02 | JSON array of supported IP modes. | maxLength=512 | Omit if not used. |
| ?applyPolicies | Bytes | 0x03 | JSON array of supported apply policies. | maxLength=512 | Omit if not used. |

---

## NetworkWifiProfile

Wi-Fi profile object used for station connection.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?profileId | String | 0x01 | Profile identifier. | maxLength=128 | Omit if not used. |
| ssid | String | 0x02 | Wi-Fi SSID. | maxLength=64 | N/A |
| ?bssid | String | 0x03 | Optional AP BSSID. | maxLength=32 | Omit if not used. |
| securityType | Enum | 0x04 | Security type, such as open, wpa2_psk, or wpa3_sae. | None | N/A |
| ?credential | NetworkCredential | 0x05 | Credential descriptor or secret reference. Responses must not expose plaintext secrets. | None | Omit if not used. |
| ?source | Enum | 0x06 | Profile source; candidate values include manual, pairing, migrated, and device_policy. | None | Omit if not used. |
| ?persist | Boolean | 0x07 | Whether the profile should be persisted. This remains policy-controlled. | None | Omit if not used. |

---

## NetworkWifiScanResult

One Wi-Fi scan result.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ssid | String | 0x01 | SSID. | maxLength=64 | N/A |
| ?bssid | String | 0x02 | BSSID. | maxLength=32 | Omit if not used. |
| ?band | Enum | 0x03 | Wi-Fi band. | None | Omit if not used. |
| ?channel | UInt16 | 0x04 | Channel number. | None | Omit if not used. |
| ?rssi | Int32 | 0x05 | RSSI in dBm. | None | Omit if not used. |
| ?securityType | Enum | 0x06 | Security type. | None | Omit if not used. |

---

## SoftwareComponent

One software component running on or hosted by the device.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| id | String | 0x01 | Component identifier. | maxLength=64 | N/A |
| ?name | String | 0x02 | Component display name. | maxLength=128 | Omit if not used. |
| ?version | String | 0x03 | Component version. | maxLength=64 | Omit if not used. |
| ?role | Enum | 0x04 | Component role, such as axtpHost, launcher, signagePlayer, agent, or unknown. | None | Omit if not used. |

---

## VideoStreamSource

One real-time video stream source.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| source | String | 0x01 | Source identifier such as wireless_cast_video. | maxLength=128 | N/A |
| ?displayName | String | 0x02 | User-visible source name. | maxLength=128 | Omit if not used. |
| codecs | Bytes | 0x03 | JSON array of supported video codecs. | maxLength=512 | N/A |
| ?resolutions | Bytes | 0x04 | JSON array of supported resolution descriptors. | maxLength=1024 | Omit if not used. |
| ?frameRates | Bytes | 0x05 | JSON array of supported frame rates. | maxLength=512 | Omit if not used. |
| ?state | Enum | 0x06 | Runtime source state, such as available, receiving, stopped, or unavailable. | None | Omit if not used. |

---

## VideoStreamStats

Bounded runtime statistics for a video stream.

| Name | Type | Field ID | Description | Value Restrictions | ?Default Behavior |
| ---- | :---: | :---: | ---- | :---: | ---- |
| ?frames | UInt64 | 0x01 | Number of frames observed. | None | Omit if not used. |
| ?bytes | UInt64 | 0x02 | Number of STREAM payload bytes observed. | None | Omit if not used. |
| ?droppedFrames | UInt64 | 0x03 | Number of dropped frames. | None | Omit if not used. |
| ?bitrateKbps | UInt32 | 0x04 | Estimated bitrate in kbps. | None | Omit if not used. |

---

# Errors Reference

| Code | Name | Category | Severity | Retryable | Status | Message |
| ---- | ---- | ---- | ---- | ---- | ---- | ---- |
| 0x0000 | SUCCESS | common | info | No | stable | Operation completed successfully. |
| 0x0001 | UNKNOWN_ERROR | common | error | No | stable | Unknown error. |
| 0x0002 | NOT_IMPLEMENTED | common | error | No | stable | Feature is not implemented. |
| 0x0003 | NOT_SUPPORTED | common | error | No | stable | Feature is not supported by the current device or mode. |
| 0x0004 | INVALID_STATE | common | error | No | stable | Operation is not allowed in the current state. |
| 0x0005 | BUSY | common | warning | Yes | stable | Device or resource is busy. |
| 0x0006 | TIMEOUT | common | warning | Yes | stable | Operation timed out. |
| 0x0007 | CANCELED | common | error | No | stable | Operation was canceled. |
| 0x0008 | RESOURCE_EXHAUSTED | common | warning | Yes | stable | Resource is exhausted. |
| 0x0009 | PERMISSION_DENIED | common | error | No | stable | Permission denied. |
| 0x000A | INVALID_ARGUMENT | common | error | No | stable | Argument is invalid. |
| 0x000B | OUT_OF_RANGE | common | error | No | stable | Argument is out of range. |
| 0x000C | NOT_FOUND | common | error | No | stable | Resource was not found. |
| 0x000D | ALREADY_EXISTS | common | error | No | draft | Resource already exists. |
| 0x000E | INTERNAL_ERROR | common | error | No | stable | Internal error. |
| 0x000F | UNAVAILABLE | common | warning | Yes | draft | Service is temporarily unavailable. |
| 0x0011 | FRAME_MAGIC_INVALID | frame | error | No | stable | Frame magic is invalid. |
| 0x0012 | FRAME_VERSION_UNSUPPORTED | frame | error | No | stable | Frame version is not supported. |
| 0x0013 | FRAME_HEADER_INVALID | frame | error | No | stable | Frame header is invalid. |
| 0x0014 | FRAME_LENGTH_INVALID | frame | error | No | stable | Frame payload length or total length is invalid. |
| 0x0015 | FRAME_PAYLOAD_TYPE_INVALID | frame | error | No | stable | Frame payload type is invalid. |
| 0x0016 | FRAME_CRC_ERROR | frame | warning | Yes | stable | Frame CRC check failed. |
| 0x0017 | FRAME_FRAGMENT_INVALID | frame | error | No | stable | Frame fragment metadata is invalid. |
| 0x0018 | FRAME_FRAGMENT_MISSING | frame | warning | Yes | stable | One or more frame fragments are missing. |
| 0x0019 | FRAME_REASSEMBLY_TIMEOUT | frame | warning | Yes | stable | Frame reassembly timed out. |
| 0x001A | FRAME_TOO_LARGE | frame | error | No | stable | Frame exceeds the negotiated maximum size. |
| 0x001B | TRANSPORT_MTU_EXCEEDED | frame | error | No | stable | Transport MTU was exceeded. |
| 0x001C | TRANSPORT_WRITE_FAILED | frame | warning | Yes | draft | Transport write failed. |
| 0x001D | TRANSPORT_READ_FAILED | frame | warning | Yes | draft | Transport read failed. |
| 0x001E | TRANSPORT_DISCONNECTED | frame | warning | Yes | stable | Transport disconnected. |
| 0x0021 | CONTROL_OPCODE_INVALID | control | error | No | stable | Control opcode is invalid. |
| 0x0022 | CONTROL_PAYLOAD_INVALID | control | error | No | stable | Control payload is invalid. |
| 0x0023 | RESERVED_CONTROL_BODY_ENCODING_UNSUPPORTED | control | error | No | reserved | Historical control body encoding negotiation error. AXTP v1 implementations must not emit it. |
| 0x0024 | CONTROL_OPEN_REQUIRED | control | error | No | stable | Session has not completed CONTROL OPEN. |
| 0x0025 | CONTROL_OPEN_REJECTED | control | error | No | stable | Control OPEN was rejected. |
| 0x0026 | RESERVED_CONTROL_PROFILE_UNSUPPORTED | control | error | No | reserved | Historical header profile negotiation error. AXTP v1 implementations must not emit it. |
| 0x0027 | CONTROL_NEGOTIATION_FAILED | control | error | No | stable | Control negotiation failed. |
| 0x0028 | CONTROL_SESSION_INVALID | control | error | No | stable | SessionId is invalid. |
| 0x0029 | CONTROL_SESSION_EXPIRED | control | error | No | stable | Session has expired. |
| 0x002A | CONTROL_RESUME_FAILED | control | error | No | stable | Session resume failed. |
| 0x002B | CONTROL_ACK_TARGET_INVALID | control | error | No | stable | ACK/NACK target type is invalid. |
| 0x002C | CONTROL_WINDOW_EXCEEDED | control | warning | Yes | stable | Flow-control window was exceeded. |
| 0x002D | CONTROL_HEARTBEAT_TIMEOUT | control | warning | Yes | stable | Control heartbeat timed out. |
| 0x0031 | RPC_ENCODING_UNSUPPORTED | rpc | error | No | stable | RPC encoding is not supported. |
| 0x0032 | RPC_OP_INVALID | rpc | error | No | stable | RPC operation is invalid. |
| 0x0033 | RPC_PAYLOAD_INVALID | rpc | error | No | stable | RPC payload is invalid. |
| 0x0034 | RPC_BODY_ENCODING_UNSUPPORTED | rpc | error | No | stable | RPC body encoding is not supported. |
| 0x0035 | RPC_BODY_DECODE_FAILED | rpc | error | No | stable | RPC body decoding failed. |
| 0x0036 | RPC_METHOD_NOT_FOUND | rpc | error | No | stable | MethodId or method name is not registered. |
| 0x0037 | RPC_METHOD_NOT_SUPPORTED | rpc | error | No | stable | Method exists but is not supported by the current device. |
| 0x0038 | RPC_METHOD_DISABLED | rpc | error | No | draft | Method is disabled. |
| 0x0039 | RPC_REQUEST_ID_INVALID | rpc | error | No | stable | RPC requestId is invalid. |
| 0x003A | RPC_PARAM_MISSING | rpc | error | No | stable | Required RPC parameter is missing. |
| 0x003B | RPC_PARAM_INVALID | rpc | error | No | stable | RPC parameters are invalid. |
| 0x003C | RPC_PARAM_OUT_OF_RANGE | rpc | error | No | stable | RPC parameter is out of range. |
| 0x003D | RPC_EXECUTION_FAILED | rpc | error | No | stable | RPC method execution failed. |
| 0x003E | RPC_RESPONSE_TIMEOUT | rpc | warning | Yes | stable | RPC response timed out. |
| 0x003F | RPC_BATCH_UNSUPPORTED | rpc | error | No | draft | RPC batch is not supported. |
| 0x0040 | RPC_BATCH_PARTIAL_FAILED | rpc | error | No | draft | One or more RPC batch items failed. |
| 0x0101 | DEVICE_INFO_UNAVAILABLE | device | warning | Yes | stable | Device information is unavailable. |
| 0x0102 | DEVICE_REBOOT_FAILED | device | error | No | draft | Device reboot failed. |
| 0x0103 | DEVICE_FACTORY_RESET_FAILED | device | error | No | draft | Device factory reset failed. |
| 0x0104 | DEVICE_LOW_POWER | device | warning | Yes | draft | Device power is low. |
| 0x0105 | DEVICE_OVER_TEMPERATURE | device | warning | Yes | draft | Device temperature is too high. |
| 0x0106 | DEVICE_STORAGE_FULL | device | error | No | stable | Device storage is full. |
| 0x0107 | DEVICE_MODE_CONFLICT | device | error | No | stable | Device mode conflicts with the requested operation. |
| 0x0108 | DEVICE_RESOURCE_BUSY | device | warning | Yes | stable | Device resource is busy. |
| 0x0109 | DEVICE_HARDWARE_FAILURE | device | error | No | draft | Device hardware failure. |
| 0x0201 | CAPABILITY_NOT_FOUND | capability | error | No | stable | Capability does not exist. |
| 0x0202 | CAPABILITY_DOMAIN_NOT_FOUND | capability | error | No | stable | Capability domain does not exist. |
| 0x0203 | CAPABILITY_METHOD_UNSUPPORTED | capability | error | No | stable | Method capability is not supported. |
| 0x0204 | CAPABILITY_EVENT_UNSUPPORTED | capability | error | No | stable | Event capability is not supported. |
| 0x0205 | CAPABILITY_STREAM_UNSUPPORTED | capability | error | No | stable | Stream capability is not supported. |
| 0x0206 | CAPABILITY_ENCODING_UNSUPPORTED | capability | error | No | stable | Encoding capability is not supported. |
| 0x0207 | CAPABILITY_NEGOTIATION_FAILED | capability | error | No | stable | Business capability negotiation failed. |
| 0x0208 | CAPABILITY_LIMIT_EXCEEDED | capability | error | No | stable | Capability limit was exceeded. |
| 0x0401 | FW_IMAGE_INVALID | firmware | error | No | stable | Firmware image is invalid. |
| 0x0402 | FW_IMAGE_TYPE_UNSUPPORTED | firmware | error | No | stable | Firmware image type is not supported. |
| 0x0403 | FW_VERSION_UNSUPPORTED | firmware | error | No | stable | Firmware version is not supported. |
| 0x0404 | FW_VERSION_TOO_OLD | firmware | error | No | draft | Firmware version is too old. |
| 0x0405 | FW_TRANSFER_NOT_STARTED | firmware | error | No | stable | Firmware transfer has not started. |
| 0x0406 | FW_TRANSFER_ALREADY_STARTED | firmware | error | No | draft | Firmware transfer has already started. |
| 0x0407 | FW_CHUNK_INVALID | firmware | error | No | stable | Firmware chunk is invalid. |
| 0x0408 | FW_CHUNK_CRC_ERROR | firmware | warning | Yes | stable | Firmware chunk CRC failed. |
| 0x0409 | FW_SIZE_MISMATCH | firmware | error | No | stable | Firmware size does not match the declared size. |
| 0x040A | FW_HASH_MISMATCH | firmware | error | No | stable | Firmware hash does not match the declared verification value. |
| 0x040B | FW_VERIFY_FAILED | firmware | error | No | stable | Firmware verification failed. |
| 0x040C | FW_APPLY_FAILED | firmware | error | No | stable | Firmware apply failed. |
| 0x040D | FW_ROLLBACK_FAILED | firmware | error | No | draft | Firmware rollback failed. |
| 0x040E | FW_STORAGE_NOT_ENOUGH | firmware | error | No | stable | Not enough storage for firmware update. |
| 0x040F | FW_DEVICE_NOT_READY | firmware | warning | Yes | stable | Device is not ready for firmware update. |
| 0x0410 | FW_REBOOT_REQUIRED | firmware | error | No | draft | Reboot is required before continuing. |
| 0x0501 | STREAM_NOT_FOUND | stream | error | No | stable | Stream context does not exist. |
| 0x0502 | STREAM_TIMEOUT | stream | warning | Yes | stable | Stream timed out. |
| 0x0503 | STREAM_CRC_ERROR | stream | warning | Yes | stable | Stream chunk CRC check failed. |
| 0x0504 | STREAM_PAYLOAD_INVALID | stream | error | No | stable | Stream payload is invalid. |
| 0x0505 | STREAM_ID_INVALID | stream | error | No | stable | StreamId is invalid. |
| 0x0506 | STREAM_NOT_OPEN | stream | error | No | stable | Stream is not open. |
| 0x0507 | STREAM_ALREADY_OPEN | stream | error | No | draft | Stream is already open. |
| 0x0508 | STREAM_SEQ_INVALID | stream | error | No | stable | Stream seqId is invalid. |
| 0x0509 | STREAM_SEQ_DUPLICATED | stream | error | No | draft | Stream seqId is duplicated. |
| 0x050A | STREAM_CHUNK_MISSING | stream | warning | Yes | stable | Stream chunk is missing. |
| 0x050B | STREAM_OFFSET_INVALID | stream | error | No | stable | Stream cursor or offset is invalid. |
| 0x050C | STREAM_WINDOW_FULL | stream | warning | Yes | stable | Stream receive window is full. |
| 0x050D | STREAM_BACKPRESSURE | stream | warning | Yes | draft | Stream receiver reported backpressure. |
| 0x050E | STREAM_RESUME_UNSUPPORTED | stream | error | No | stable | Stream resume is not supported. |
| 0x050F | STREAM_RESUME_FAILED | stream | error | No | stable | Stream resume failed. |
| 0x0510 | STREAM_CLOSED | stream | error | No | stable | Stream is closed. |
| 0x0511 | STREAM_TRANSFER_ABORTED | stream | error | No | stable | Stream transfer was aborted. |
| 0x0801 | MEDIA_SOURCE_NOT_FOUND | video | error | No | stable | Requested media source does not exist. |
| 0x0802 | MEDIA_SOURCE_UNAVAILABLE | video | warning | Yes | stable | Requested media source is currently unavailable. |
| 0x0803 | MEDIA_CODEC_UNSUPPORTED | video | error | No | stable | Requested media codec or sample format is unsupported. |
| 0x0804 | MEDIA_RESOLUTION_UNSUPPORTED | video | error | No | stable | Requested video resolution is unsupported. |
| 0x0805 | MEDIA_FRAMERATE_UNSUPPORTED | video | error | No | stable | Requested video frame rate is unsupported. |
| 0x0806 | MEDIA_BITRATE_UNSUPPORTED | video | error | No | draft | Requested media bitrate is unsupported. |
| 0x0807 | MEDIA_STREAM_START_FAILED | video | warning | Yes | stable | Device failed to start the requested media stream. |
| 0x0808 | MEDIA_STREAM_STOP_FAILED | video | warning | Yes | draft | Device failed to stop the media stream. |
| 0x0809 | MEDIA_FRAME_DROPPED | video | warning | Yes | draft | Media frame was dropped. |
| 0x080B | MEDIA_VIDEO_SIGNAL_LOST | video | warning | Yes | draft | Video signal was lost. |
| 0x090A | MEDIA_AUDIO_DEVICE_NOT_FOUND | audio | error | No | draft | Audio device was not found. |
| 0x1001 | FILE_NOT_FOUND | file | error | No | stable | File does not exist. |
| 0x1002 | FILE_ALREADY_EXISTS | file | error | No | draft | File already exists. |
| 0x1003 | FILE_PERMISSION_DENIED | file | error | No | stable | File permission denied. |
| 0x1004 | FILE_PATH_INVALID | file | error | No | stable | File path is invalid. |
| 0x1005 | FILE_TYPE_UNSUPPORTED | file | error | No | stable | File type is not supported. |
| 0x1006 | FILE_TOO_LARGE | file | error | No | stable | File is too large. |
| 0x1007 | FILE_READ_FAILED | file | warning | Yes | stable | File read failed. |
| 0x1008 | FILE_WRITE_FAILED | file | warning | Yes | stable | File write failed. |
| 0x1009 | FILE_DELETE_FAILED | file | error | No | draft | File delete failed. |
| 0x100A | FILE_TRANSFER_FAILED | file | warning | Yes | stable | File transfer failed. |
| 0x100B | FILE_VERIFY_FAILED | file | error | No | stable | File verification failed. |
| 0x100C | FILE_STORAGE_FULL | file | error | No | stable | File storage is full. |
| 0x1201 | DIAG_TEST_NOT_FOUND | diagnostic | error | No | draft | Diagnostic test was not found. |
| 0x1202 | DIAG_TEST_UNSUPPORTED | diagnostic | error | No | draft | Diagnostic test is not supported. |
| 0x1203 | DIAG_TEST_RUNNING | diagnostic | error | No | draft | Diagnostic test is already running. |
| 0x1204 | DIAG_TEST_FAILED | diagnostic | error | No | draft | Diagnostic test failed. |
| 0x1205 | DIAG_METRIC_UNAVAILABLE | diagnostic | warning | Yes | draft | Diagnostic metric is unavailable. |
| 0x1206 | DIAG_LOOPBACK_FAILED | diagnostic | error | No | draft | Diagnostic loopback failed. |
| 0x1401 | SEC_AUTH_REQUIRED | auth | error | No | draft | Authentication is required. |
| 0x1402 | SEC_AUTH_FAILED | auth | error | No | draft | Authentication failed. |
| 0x1403 | SEC_PERMISSION_DENIED | auth | error | No | draft | Security permission denied. |
| 0x1404 | SEC_ENCRYPTION_REQUIRED | auth | error | No | draft | Encryption is required. |
| 0x1405 | SEC_DECRYPT_FAILED | auth | error | No | draft | Decryption failed. |
| 0x1406 | SEC_SIGNATURE_INVALID | auth | error | No | draft | Signature is invalid. |
| 0x1407 | SEC_CERT_INVALID | auth | error | No | draft | Certificate is invalid. |
| 0x1408 | SEC_TOKEN_EXPIRED | auth | error | No | draft | Security token expired. |
| 0x7F01 | LEGACY_CMD_UNMAPPED | legacy | error | No | stable | Legacy CmdValue is not mapped to an AXTP method. |
| 0x7F02 | LEGACY_STATUS_UNMAPPED | legacy | error | No | stable | Legacy status is not mapped to an AXTP ErrorCode. |
| 0x7F03 | LEGACY_PAYLOAD_INVALID | legacy | error | No | stable | Legacy payload is invalid. |
| 0x7F04 | LEGACY_PAYLOAD_TOO_SHORT | legacy | error | No | stable | Legacy payload is too short. |
| 0x7F05 | LEGACY_PAYLOAD_TOO_LONG | legacy | error | No | stable | Legacy payload is too long. |
| 0x7F06 | LEGACY_FIELD_UNSUPPORTED | legacy | error | No | stable | Legacy field cannot be adapted. |
| 0x7F07 | LEGACY_CAPABILITY_CONFLICT | legacy | error | No | stable | Legacy capability conflicts with AXTP capability. |
| 0x7F08 | LEGACY_RESPONSE_TIMEOUT | legacy | warning | Yes | stable | Legacy response timed out. |

# Profiles Reference

## AXTP-MVP

- Status: `stable`
- Added in v1.0.0
- Extends: `-`
- Required Methods: `None`
- Required Events: `None`
- Required Errors: `SUCCESS`, `RPC_METHOD_NOT_FOUND`, `RPC_PARAM_INVALID`, `STREAM_NOT_FOUND`, `STREAM_CRC_ERROR`, `BUSY`
- Notes: -

---

## AXTP-MVP-HID

- Status: `stable`
- Added in v1.0.0
- Extends: `AXTP-MVP`
- Required Methods: `None`
- Required Events: `None`
- Required Errors: `SUCCESS`, `RPC_METHOD_NOT_FOUND`, `RPC_PARAM_INVALID`, `STREAM_NOT_FOUND`, `STREAM_CRC_ERROR`, `BUSY`
- Notes: -

---
