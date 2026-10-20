# Local cast-control development input

This branch consumes the adopted, unpublished P0/P1 checkpoint
`e1c4bfcdb0daa12c208a4ecef2126376ce48fe1e`, not a released `spec/v0.18.0`.
`AXTP_SPEC.lock.yaml` remains the formal release lock. Its old spec version
must never be used to label these development artifacts.

The explicit development binding is `AXTP_SPEC.local.json`. The frozen input
must include `.axtp-local-snapshot.json`; every file, size, safe path, absence of
extra files, source identity and the complete SHA256 fingerprint are checked.
Fingerprint: `683a051759b7b82b2c0c80323dd719c99cee0ed02a6d42ee447d54a24751686a`.
It includes the entire protocol/registry/generated facts/vectors/conformance
input. Generation, version checks and conformance use the same input path.

PowerShell, after installing the generator's frozen pnpm dependencies:

```powershell
$env:AXTP_SPEC_PATH='C:/path/to/spec-snapshot'
$env:AXTP_SPEC_SOURCE_MODE='local-development'
pnpm --dir devtools/generators build
node devtools/scripts/generate-runtime.mjs
node devtools/scripts/check-generated-runtime.mjs
node devtools/scripts/axtp-versioning.mjs check --runtime-name axtp-cpp-runtime
node --test devtools/scripts/axtp-spec-input.test.mjs
pnpm --dir devtools/generators test
ctest --test-dir C:/path/to/runtime-build --output-on-failure
$env:CONFORMANCE_BUILD_DIR='C:/path/to/runtime-build'
$env:CONFORMANCE_RESULT_DIR='C:/path/to/development-evidence'
node devtools/scripts/run-conformance.mjs
```

The cross-platform runner uses Node/YAML instead of requiring Ruby. It records
development provenance separately from a released conformance-result schema.
The existing runtime profile covers core, WebSocket JSON-RPC, endpoint relay
and optional events. It does not claim product cast-control conformance.
NearCast tests exercise those product owners and persistence operations.

`release-check` and `axtp-spec-input.mjs --release-only` deliberately reject this
input. A passing development check is not a passing formal release lock check.
Before release, adopt an actually published spec tag/commit through the normal
lock workflow, regenerate, and rerun all downstream checks.

The runtime-owned generator emits protocol IDs, schema facts and generic object
validation/JSON encoding. Decimal uint64 strings remain lossless. The TLV8
object codec covers defined scalar/nested-object layouts and rejects undefined
enum/array layouts rather than inventing bytes. The active control endpoint
uses WebSocket JSON-RPC; no new product policy belongs in this runtime.
