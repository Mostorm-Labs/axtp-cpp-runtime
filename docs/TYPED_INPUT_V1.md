# Typed Input v1 development binding

This branch consumes AXTP commit `48144141775da6c35e579d2498d88c0b0449ed61` through
the explicit unpublished snapshot `0.18.0-dev.feac6fa2a4b9`. Its complete SHA-256 is
`feac6fa2a4b9c927708c077746166259ffeb7d5f06dc6972b29f1fc30954c2d0`.
`AXTP_SPEC.lock.yaml` remains the released-spec lock; this is not a new release.

The canonical business/flow and wire rules are in the same spec input:
`workspace/business/nearcast-touchback.md`,
`workspace/flows/nearcast-touchback-na20-nt10.md`,
`workspace/protocol/input/input.stream.md`,
`workspace/protocol/cast/cast.touchback.md`, and `specs/48-input-stream.md`.
Removed draft method/mask/field allocations are not reusable. No legacy input
layout is accepted by these codecs.

## Runtime surface

- `model/input_sample.hpp`: typed Touch/Mouse samples, negotiated limits and
  logical validation state; these are protocol models, not native OS contacts.
- `wire/input_sample_codec.hpp`: packed big-endian input data encoding/decoding
  and whole-sample transition validation. Decode and transition failure leave
  their output unchanged. The host separately accounts for native submission.
- `wire/input_frame_codec.hpp`: full Standard Frame/CRC16 and bounded ordered
  reassembly before STREAM/input parsing. One complete frame is supplied per
  consume call; transport/session dispatch and authentication remain outside.
- `generated/axtp_input_stream_generated.h`: constants emitted from adopted
  `input_stream` source facts; never hand-edit offsets or widths here.

Touch is type byte plus 7 bytes per contact. One packet is one complete sample,
including stationary active contacts and this sample's Up/Cancel. Mouse is one
explicit Move/Buttons/Wheel/Cancel/Modifiers record. No epoch, input header,
CRC32, device ID or geometry token is added to data. Standard STREAM16 supplies
streamId/seqId/cursor; the host binds it to the authenticated immutable context.

Only the host knows allowed input types, real native capacities, source geometry
and ownership. Codec defaults are structural bounds and must not be advertised
as native capabilities. The host validates session/target, strict sequence and
cursor, local generation, permission and output policy before native execution.
Native cleanup, product gate, timeout/queue policy and OS injection stay above
Runtime; `typed-input` is therefore not claimed as a fully implemented native
lifecycle conformance level by this generic Runtime.

## Generation and validation

Set `AXTP_SPEC_SOURCE_MODE=local-development` and `AXTP_SPEC_PATH` to the exact
restored snapshot. `devtools/scripts/generate-runtime.mjs` verifies its manifest,
emits headers with the Runtime-owned generator, and copies the formal vector
corpus to `tests/fixtures/input_stream.json`. The fixture is generated input.

`input_sample_codec_test` runs the same 25 valid frames and 13 rejected traces as
the JS protocol oracle and tests CRC, ordered fragments and small frame budgets.
The normal CTest suite, generated-version check and explicit local-binding test
remain required. These are protocol evidence, not proof of native Touch delivery.
