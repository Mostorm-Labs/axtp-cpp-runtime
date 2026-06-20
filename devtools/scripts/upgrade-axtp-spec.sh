#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 spec/vMAJOR.MINOR.PATCH" >&2
  exit 2
fi

tag="$1"
if [[ "$tag" == "main" || "$tag" == "unreleased" || ! "$tag" =~ ^spec/v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Expected a released AXTP Spec tag, for example spec/v0.3.0" >&2
  exit 2
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
lock="$root/AXTP_SPEC.lock.yaml"
repo="${AXTP_SPEC_REPOSITORY:-https://github.com/Mostorm-Labs/axtp.git}"

commit="$(git ls-remote "$repo" "refs/tags/$tag^{}" | awk 'NR == 1 { print $1 }')"
if [[ -z "$commit" ]]; then
  commit="$(git ls-remote "$repo" "refs/tags/$tag" | awk 'NR == 1 { print $1 }')"
fi

if [[ -z "$commit" ]]; then
  echo "Could not resolve $tag from $repo" >&2
  exit 1
fi

version="${tag#spec/v}"
major="${version%%.*}"
rest="${version#*.}"
minor="${rest%%.*}"
next_minor=$((minor + 1))
compatibility=">=$version <$major.$next_minor.0"
tmp="$lock.tmp"

cat > "$tmp" <<YAML
axtp_spec:
  repository: https://github.com/Mostorm-Labs/axtp
  tag: $tag
  version: $version
  commit: "$commit"
  compatibility: "$compatibility"
  updated_at: "$(date +%F)"
YAML
mv "$tmp" "$lock"

if [[ -d "$root/third_party/axtp-spec/.git" ]]; then
  git -C "$root/third_party/axtp-spec" fetch --tags
  git -C "$root/third_party/axtp-spec" checkout "$tag"
fi

if [[ -f "$root/package.json" ]]; then
  AXTP_RUNTIME_ROOT="$root" \
  AXTP_SPEC_REPOSITORY_METADATA="https://github.com/Mostorm-Labs/axtp" \
  AXTP_SPEC_TAG_METADATA="$tag" \
  AXTP_SPEC_VERSION_METADATA="$version" \
  node --input-type=module <<'NODE'
import { readFile, writeFile } from "node:fs/promises";
import path from "node:path";

const packageJsonPath = path.join(process.env.AXTP_RUNTIME_ROOT, "package.json");
const packageJson = JSON.parse(await readFile(packageJsonPath, "utf8"));
packageJson.axtp = {
  ...(packageJson.axtp ?? {}),
  specRepository: process.env.AXTP_SPEC_REPOSITORY_METADATA,
  specTag: process.env.AXTP_SPEC_TAG_METADATA,
  specVersion: process.env.AXTP_SPEC_VERSION_METADATA
};
await writeFile(packageJsonPath, `${JSON.stringify(packageJson, null, 2)}\n`, "utf8");
NODE
  echo "Updated package.json AXTP spec metadata"
fi

spec_path="${AXTP_SPEC_PATH:-}"
if [[ -z "$spec_path" &&
      ( -d "$root/third_party/axtp-spec/registry" ||
        -d "$root/third_party/axtp-spec/contract/registry" ) ]]; then
  spec_path="$root/third_party/axtp-spec"
fi

if [[ -n "$spec_path" && ( -d "$spec_path/registry" || -d "$spec_path/contract/registry" ) ]]; then
  if [[ -x "$root/devtools/scripts/generate-axtp-artifacts.sh" && -f "$root/devtools/generators/dist/sourceLoader.js" ]]; then
    echo "Regenerating AXTP artifacts from $spec_path"
    AXTP_SPEC_PATH="$spec_path" "$root/devtools/scripts/generate-axtp-artifacts.sh"
  elif [[ -x "$root/devtools/scripts/generate-axtp-artifacts.sh" ]]; then
    echo "Skipping generated artifacts: generator is not built. Run: pnpm --dir devtools/generators build"
  else
    echo "Skipping generated artifacts: devtools/scripts/generate-axtp-artifacts.sh is missing"
  fi
else
  echo "Skipping generated artifacts: no AXTP spec checkout found via AXTP_SPEC_PATH or third_party/axtp-spec"
fi

echo "Updated AXTP_SPEC.lock.yaml to $tag at $commit"
echo "No commit was created."
