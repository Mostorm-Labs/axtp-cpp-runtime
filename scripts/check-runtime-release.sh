#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 X.Y.Z" >&2
  exit 2
fi

tag_version="$1"
if [[ ! "$tag_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-.+][0-9A-Za-z.-]+)?$ ]]; then
  echo "Expected runtime tag version without leading v, for example 0.3.1" >&2
  exit 2
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_name="$(basename "$root")"

"$root/scripts/check-axtp-spec-lock.sh"
node "$root/scripts/axtp-versioning.mjs" release-check --runtime-name "$runtime_name" --tag-version "$tag_version"
