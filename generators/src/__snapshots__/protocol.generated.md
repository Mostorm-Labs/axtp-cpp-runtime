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
- [Events](#events)
  - [audio Events](#audio-events)
- [Additional Types](#additional-types)
- [Errors Reference](#errors-reference)
- [Profiles Reference](#profiles-reference)

## Implemented Domains

| Domain | Methods | Events |
| ---- | ---- | ---- |
| audio | 4 | 1 |

## Overview

AXTP is a transport-independent device communication protocol for CONTROL, RPC and STREAM payloads across Standard Framed transports, plus a formal WebSocket Unframed JSON RPC profile.

| Property | Value |
| ---- | ---- |
| Protocol | AXTP |
| Version | 1.0.0 |
| Spec Version | 1 |
| Registry Version | 1.0.0 |
| Status | rc1 |

## Protocol Framework

AXTP v1 has two formal integration paths:

- **Standard Framed**: uses the 12-byte Standard Frame header, CONTROL OPEN/ACCEPT, RPC, STREAM, fragmentation, CRC16, and optional ACK/NACK.
- **WebSocket Unframed JSON**: uses the JSON `sid`/`op`/`d` envelope directly over WebSocket. It is RPC-only and does not carry CONTROL or STREAM payloads.

| Path | Transports | Frame | RPC Encodings | CONTROL | STREAM |
| ---- | ---- | ---- | ---- | ---- | ---- |
| Standard Framed | AXTP-USB-HID<br>AXTP-TCP | STANDARD_FRAME | `TLV`, `JSON`, `RAW` | Yes | Yes |
| WebSocket Unframed JSON | AXTP-WS-JSON<br>AXTP-WS-CLOUD-REVERSE | None | `JSON` | No | No |

Compact/HID-64/BLE/UART framing is a low-bandwidth degradation path, not an AXTP v1 Core requirement. See `docs/specs/18-AXTP-Low-Bandwidth-Degradation.md` for that path.

## Design Goals / Non-Goals

### Goals

- Provide one unified protocol model for control, request/response RPC and stream transfer.
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
| AXTP-USB-HID | usb-hid | standard-framed | STANDARD_FRAME | `TLV`, `JSON`, `RAW` | Yes | Yes | USB HID High Speed or large-report HID transport using Standard Frame. |
| AXTP-TCP | tcp | standard-framed | STANDARD_FRAME | `TLV`, `JSON`, `RAW` | Yes | Yes | TCP byte stream transport using Standard Frame magic and length parsing. |
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
| `RPC` | 0x02 | 11B | Binary request, response, event and error payload. |
| `STREAM` | 0x03 | 16B | Chunk-oriented data plane payload. |

## Generated Method Index

The generated registry groups methods by domain. Each method keeps a stable `bitOffset` within its domain for generated indexes, test vectors, and any adopted runtime discovery method.

| Domain | Methods |
| ---- | ---- |
| audio | 1: audio.getAlgorithmConfig<br>2: audio.setAlgorithmConfig<br>0: audio.getAlgorithmCapabilities<br>3: audio.resetAlgorithmConfig |

# Methods

## audio Methods

### Methods in this domain

- [audio.getAlgorithmConfig](#audiogetalgorithmconfig)
- [audio.setAlgorithmConfig](#audiosetalgorithmconfig)
- [audio.getAlgorithmCapabilities](#audiogetalgorithmcapabilities)
- [audio.resetAlgorithmConfig](#audioresetalgorithmconfig)

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

# Events

## audio Events

### Events in this domain

- [audio.algorithmConfigChanged](#audioalgorithmconfigchanged)

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

## Empty

Kind: `object`

No fields.

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
