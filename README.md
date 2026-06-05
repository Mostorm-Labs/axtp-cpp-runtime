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

Generated C++ artifacts are written to `core/include/generated/`.

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
