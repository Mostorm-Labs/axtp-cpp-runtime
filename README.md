# AXTP C++ Runtime

This repository contains the AXTP C++ runtime, SDK, optional transports, and
developer tooling. Protocol facts are generated from the locked AXTP spec; this
repository implements those facts for C++.

## Repository Layout

The public C++ headers are arranged by dependency direction:

```text
support/io -> protocol -> runtime -> sdk/transports/json_rpc -> tools
```

| Path | Purpose |
|---|---|
| `core/include/axtp.hpp` | Main aggregate header for core/runtime users. |
| `core/include/support/io/` | Byte buffers, readers/writers, byte sinks, CRC, text writer, and transport packet boundaries. |
| `core/include/protocol/model/` | Stable protocol value types: bytes, errors, frames, messages, payloads, result wrappers, and protocol enums. |
| `core/include/protocol/generated/` | Generated IDs, registries, schema facts, traits, lookup helpers, and generated version constants. Do not edit by hand. |
| `core/include/protocol/session/` | Control/session helpers, pending call tracking, and stream session state. |
| `core/include/protocol/wire/framed_binary/` | Standard framed binary encoders/decoders and control TLV codec. |
| `core/include/protocol/wire/websocket_json_rpc/` | WebSocket JSON-RPC payload and envelope codecs. |
| `core/include/protocol/wire/` | Wire-mode inbound/outbound processors and payload sink contracts. |
| `core/include/runtime/core/` | `AxtpCore`, `CoreEvent`, and RPC dispatcher. |
| `core/include/runtime/broker/` | `BasicBroker<>`, business routing, middleware, task dispatch, and result queues. |
| `core/include/runtime/endpoint/` | `AxtpEndpoint` glue between core, broker, and transport. |
| `core/include/runtime/transport/` | Transport interface and transport profile contracts. |
| `core/include/runtime/testing/` | Test/mock transport utilities. |
| `sdk/include/sdk/` | Higher-level client/device APIs, call options, endpoint options, typed generated clients, and SDK result/error wrappers. |
| `json-rpc/include/json_rpc/` | Optional JSON-RPC registry loading and WebSocket JSON-RPC adapter. |
| `transports/include/transports/` | Optional transport headers for HID, TCP Boost, WebSocket Boost, WebSocket IX, and WebSocket++/Asio. |
| `transports/src/` | Non-header implementation files for optional transports, currently HID. |
| `tools/axtpctl/` | General AXTP CLI for method/capability inspection, app-ready handshakes, mock calls, and optional TCP/WebSocket/HID transport debugging. |
| `tools/toolkit/` | Internal tool support library shared by CLI and application-style tools. |
| `tools/axtp-mediahost/` | Windows MediaHost application-style integration sample split into app, media protocol, media model, and Win32 render layers. |
| `generators/` | TypeScript generator that consumes the AXTP spec and emits C++ generated headers. |
| `scripts/` | Spec lock, generation, versioning, conformance, and release helper scripts. |
| `tests/` and `conformance/` | Runtime conformance runner sources and runtime conformance profile. |
| `docs/` | Design notes, execution flow, style guide, generator notes, and tool designs. |
| `thirdparty/` | Vendored or bundled dependencies used by local builds. |

## Quickstart

For most application code, start with the SDK target. It brings in the runtime
target and the public include paths.

Minimal `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(axtp_quickstart LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(path/to/axtp-cpp-runtime/sdk axtp-cpp-runtime-sdk)

add_executable(axtp_quickstart main.cpp)
target_link_libraries(axtp_quickstart PRIVATE axtp_sdk)
```

Minimal `main.cpp` using the SDK with the in-memory mock transport:

```cpp
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include "runtime/testing/mock_transport.hpp"
#include "sdk/axtp_sdk_all.hpp"

int main() {
    axtp::sdk::AxtpClient client;
    client.attachTransport(std::make_unique<axtp::MockTransport>());

    client.registerMethod(
        static_cast<std::uint16_t>(axtp::MethodId::AudioGetAlgorithmConfig),
        [](const axtp::RpcPayload&) {
            const std::string body =
                R"({"noiseSuppression":{"enabled":true,"level":3}})";
            return axtp::Bytes(body.begin(), body.end());
        });

    const std::string response = client.callJson("audio.getAlgorithmConfig", "{}");
    std::cout << response << "\n";
}
```

Build it:

```bash
cmake -S . -B build
cmake --build build
```

Use lower-level targets when you need tighter control:

| CMake target | Use when |
|---|---|
| `axtp_core` | You only need protocol types, generated facts, and wire codecs. |
| `axtp_runtime` | You need `AxtpCore`, broker, endpoint glue, and transport interfaces. |
| `axtp_sdk` | You want client/device convenience APIs. |
| `axtp_json_rpc` | You need the optional WebSocket JSON-RPC adapter. Enable `AXTP_BUILD_JSON_RPC`. |
| `axtp_transport_hidapi` | You need the optional HID transport. Enable `AXTP_BUILD_OPTIONAL_TRANSPORTS`. |
| `axtp_transport_tcp_boost`, `axtp_transport_websocket_ix`, `axtp_transport_websocket_boost`, `axtp_transport_websocket_websocketpp` | You need optional TCP/WebSocket transport headers and their third-party dependencies. Enable `AXTP_BUILD_OPTIONAL_TRANSPORTS`. |

If you are embedding only the runtime layer:

```cmake
add_subdirectory(path/to/axtp-cpp-runtime/core axtp-cpp-runtime-core)
target_link_libraries(your_target PRIVATE axtp_runtime)
```

## AXTP Spec Compatibility

This runtime implements AXTP Spec from the AXTP main specification repository.

See `AXTP_SPEC.lock.yaml` for:

- AXTP Spec repository
- Spec tag
- Spec version
- Source commit
- Compatibility range

Runtime code must not redefine AXTP protocol semantics. Protocol documents,
registries, schemas, business domains, business flows, and conformance cases are
maintained in the AXTP spec repository.

## AXTP Spec Dependency

Use `AXTP_SPEC_PATH` to point local tooling to a checked out AXTP spec
repository:

```bash
export AXTP_SPEC_PATH=/path/to/axtp
```

The checkout should match the tag and commit recorded in
`AXTP_SPEC.lock.yaml`. Do not depend on the `main` branch for reproducible
runtime builds.

C++ integrations may also use a local checkout at:

```text
third_party/axtp-spec
```

If `third_party/axtp-spec` is used, check it out to the locked tag or commit.

## Build And Test

Core runtime:

```bash
cmake -S core -B build/core
cmake --build build/core
ctest --test-dir build/core --output-on-failure
```

SDK:

```bash
cmake -S sdk -B build/sdk
cmake --build build/sdk
ctest --test-dir build/sdk --output-on-failure
```

CLI:

```bash
cmake -S tools/axtpctl -B build/axtpctl
cmake --build build/axtpctl
ctest --test-dir build/axtpctl --output-on-failure
```

The `axtpctl` target is the single protocol CLI. Use `axtpctl -t hid ...` for
real HID device diagnostics; CLI defaults to HID VID/PID `0x0581:0x2581` and
usagePage `0x81`. `--usage-page 0` disables the default usagePage filter. HID
mock/socket simulation is not provided.

## Documentation

- [Runtime patterns](docs/AXTP_CPP_RUNTIME_PATTERNS.md)
- [Execution flow](docs/AXTP_CPP_EXECUTION_FLOW.md)
- [Core API design](docs/AXTP_CORE_API_DESIGN.md)
- [SDK API design](docs/AXTP_SDK_API_DESIGN.md)
- [axtpctl command design](docs/AXTPCTL_COMMAND_DESIGN.md)
- [C++ style guide](docs/AXTP_CPP_STYLE.md)

## Spec Lock Checks

```bash
scripts/check-axtp-spec-lock.sh
```

## AXTP Spec Upgrade

This runtime follows AXTP Spec via `AXTP_SPEC.lock.yaml`.

To upgrade:

```bash
scripts/upgrade-axtp-spec.sh spec/v0.3.0
scripts/check-axtp-spec-lock.sh
```

After upgrading, run generator checks, CMake/CTest, and the conformance runner
before merging.

## Conformance

Conformance cases are owned by the AXTP spec repository. Point the runner at the
locked spec checkout and run:

```bash
AXTP_SPEC_PATH=/path/to/axtp scripts/run-conformance.sh
```

The runner writes `conformance-results/result.json`. Required failures exit
nonzero. Optional cases are reported as skipped or passed unless
`CONFORMANCE_STRICT_OPTIONAL=true`; upgrade PR workflows may temporarily use
`CONFORMANCE_ALLOW_INCOMPLETE=true`.

## Automated AXTP Spec Upgrade

This repository is automatically upgraded when the AXTP Spec repository publishes a tag like `spec/vX.Y.Z`.

Automation flow:

1. Receive `axtp_spec_released` repository dispatch.
2. Update `AXTP_SPEC.lock.yaml`.
3. Set runtime/tool version to `X.Y.Z`.
4. Generate code and `generated/axtp_generated_manifest.json`.
5. Open an Upgrade PR.
6. Auto-merge the PR after checks pass.
7. Create tag `vX.Y.Z`.
8. Create a GitHub Release.

AXTP Spec tag: `spec/vX.Y.Z`

Runtime/tool tag: `vX.Y.Z`

Repository settings must allow GitHub Actions to create PRs, enable auto-merge, create tags, and create releases. Configure `AXTP_RUNTIME_AUTOMATION_TOKEN` when PR-created-by-actions workflows must trigger downstream pull_request checks.

## Local Generator

This repository maintains its own generator under `generators/`.

```bash
export AXTP_SPEC_PATH=/path/to/axtp
pnpm --dir generators install
pnpm --dir generators build
pnpm --dir generators test
pnpm --dir generators generate:runtime
```

Generated C++ artifacts are written to `core/include/protocol/generated/`.

To move to a later released spec tag:

```bash
scripts/upgrade-axtp-spec.sh spec/v0.1.0
```

## Versioning

This repository keeps AXTP Spec, runtime, and generated artifact versions
separate:

- AXTP Spec tags use `spec/vX.Y.Z` and are recorded in `AXTP_SPEC.lock.yaml`.
- Runtime releases use `vX.Y.Z`.
- Generated artifact metadata is recorded in `generated/axtp_generated_manifest.json`.

Use `scripts/check-generated-version.sh` to verify that the lock file,
generated manifest, runtime version, and generated constants are aligned.

See `docs/generator/GENERATED_VERSIONING.md` for generator versioning details.

## Release

Runtime releases are created from runtime tags:

- Runtime tags: `vX.Y.Z`
- AXTP Spec tags: `spec/vX.Y.Z`

AXTP Spec updates create automated upgrade PRs. After checks pass, the PR is auto-merged; the main branch workflow then creates the matching `vX.Y.Z` runtime/tool tag, and that tag triggers the GitHub Release.

Each release records runtime version, AXTP Spec tag, AXTP Spec commit, generator
version, and the generated manifest.
