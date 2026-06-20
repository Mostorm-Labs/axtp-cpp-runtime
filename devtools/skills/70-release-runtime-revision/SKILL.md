---
name: release-runtime-revision
description: Release or prepare an AXTP runtime-only revision from a runtime repository without changing the locked AXTP Spec. Use when the user asks to bump, publish, tag, or prepare a runtime/tool/package revision such as v0.8.4.1, to ship implementation, packaging, tool, or documentation fixes while staying on the current AXTP_SPEC.lock.yaml version. Do not use for AXTP Spec releases or protocol fact changes.
---

# Release Runtime Revision

Prepare a runtime-only revision for the AXTP Spec version already locked by this repository. Runtime tags use `vX.Y.Z.R`: `X.Y.Z` is owned by the AXTP Spec repo, and `R` is owned by this runtime repo.

## Boundaries

- Do not change `AXTP_SPEC.lock.yaml`.
- Do not change generated protocol facts except version metadata produced by the generator.
- Do not use this workflow for protocol semantics, registry, schema, method, event, error, or capability changes.
- If the AXTP Spec repo publishes `spec/vX.Y.Z`, use the spec upgrade workflow instead; it resets the runtime version to `X.Y.Z.0`.
- If the current version is three-part `X.Y.Z`, treat it as revision `0` and prepare `X.Y.Z.1`.

## Preferred CI Path

Use the manual GitHub workflow:

```bash
gh workflow run release-runtime-revision.yml \
  -f reason="Describe the runtime-only change"
```

Optionally pin the next version:

```bash
gh workflow run release-runtime-revision.yml \
  -f reason="Describe the runtime-only change" \
  -f target_version="X.Y.Z.R"
```

The workflow opens a PR that bumps only the runtime revision and regenerated version metadata. After that PR merges, the existing auto-release workflow creates and pushes tag `vX.Y.Z.R`.

## Local Preparation Path

Work from the runtime repository root. Ensure the locked spec checkout is available:

```bash
export AXTP_SPEC_PATH=/path/to/axtp
```

Then run:

```bash
corepack enable
corepack prepare pnpm@11.3.0 --activate
bash devtools/scripts/prepare-runtime-revision.sh
```

For an explicit target:

```bash
bash devtools/scripts/prepare-runtime-revision.sh --target-version X.Y.Z.R
```

Review the diff. Expected files are runtime version sources, `generated/axtp_generated_manifest.json`, and generated language version constants. `AXTP_SPEC.lock.yaml` must remain unchanged.

## Validation

Run:

```bash
devtools/scripts/check-axtp-spec-lock.sh
devtools/scripts/check-generated-version.sh
devtools/scripts/check-runtime-release.sh X.Y.Z.R
git diff --check
```

Run repository build and test commands appropriate for the runtime before merging the PR. The release tag must be created by the existing auto-release workflow after merge, not by this skill directly.
