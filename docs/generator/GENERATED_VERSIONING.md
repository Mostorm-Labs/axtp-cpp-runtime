# AXTP Generated Versioning

Runtime repositories track three separate version concepts:

- AXTP Spec Version comes from the AXTP main repository tag, such as `spec/v0.3.0`.
- Runtime Version comes from this runtime repository, such as `v0.3.1`.
- Generated Artifact Version comes from `generated/axtp_generated_manifest.json` and generated language constants.

AXTP Spec tags use `spec/vX.Y.Z`. Runtime release tags use `vX.Y.Z`.
Runtime patch releases may move independently while staying on the same AXTP
Spec patch.

## Generated Manifest

Every generator run writes:

```text
generated/axtp_generated_manifest.json
```

The manifest records:

- `axtpSpec` from `AXTP_SPEC.lock.yaml`
- `generator` from `generators/package.json` and the current repository commit
- `runtime` from the runtime version source and current repository commit
- `inputs.registryHash`, `inputs.schemasHash`, and `inputs.conformanceHash`

Missing `schemas` or `conformance` directories are recorded as `null`; the
generator does not invent hashes for missing inputs.

## Checks

Use:

```bash
scripts/check-generated-version.sh
scripts/check-runtime-release.sh 0.3.1
```

`check-generated-version.sh` validates lock, manifest, runtime version, and
language constants. `check-runtime-release.sh` adds runtime tag validation and
rejects releases based on `tag: unreleased`.
