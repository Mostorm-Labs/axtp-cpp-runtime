# AXTP C++ Runtime

This repository contains the AXTP C++ runtime, SDK, optional transports, and
`axtpctl` tooling extracted from the AXTP specification repository.

The runtime layout is intentionally kept as copied:

```text
core/
json-rpc/
sdk/
thirdparty/
tools/
transports/
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

## Spec Lock Checks

```bash
scripts/check-axtp-spec-lock.sh
```

## Local Generator

This repository maintains its own generator under `generators/`.

```bash
export AXTP_SPEC_PATH=/path/to/axtp
pnpm --dir generators install
pnpm --dir generators build
pnpm --dir generators test
pnpm --dir generators generate:runtime
```

Generated C++ artifacts are written to `core/include/generated/`.

To move to a later released spec tag:

```bash
scripts/upgrade-axtp-spec.sh spec/v0.1.0
```
