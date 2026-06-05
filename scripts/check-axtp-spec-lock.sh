#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
lock="$root/AXTP_SPEC.lock.yaml"

if [[ ! -f "$lock" ]]; then
  echo "Missing AXTP_SPEC.lock.yaml" >&2
  exit 1
fi

require_field() {
  local field="$1"
  local value
  value="$(awk -v key="$field" '
    $0 ~ "^[[:space:]]*" key "[[:space:]]*:" {
      value = $0
      sub("^[[:space:]]*" key "[[:space:]]*:[[:space:]]*", "", value)
      gsub(/^"/, "", value)
      gsub(/"$/, "", value)
      print value
      found = 1
      exit
    }
    END { if (!found) exit 1 }
  ' "$lock")"
  if [[ -z "$value" ]]; then
    echo "Missing axtp_spec.$field in AXTP_SPEC.lock.yaml" >&2
    exit 1
  fi
  printf '%s' "$value"
}

repository="$(require_field repository)"
tag="$(require_field tag)"
version="$(require_field version)"
commit="$(require_field commit)"
compatibility="$(require_field compatibility)"

if [[ "$tag" == "main" ]]; then
  echo "AXTP Spec lock must not use main as a runtime dependency" >&2
  exit 1
fi

if [[ "$tag" != "unreleased" && ! "$tag" =~ ^spec/v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "AXTP Spec tag must be unreleased or spec/vMAJOR.MINOR.PATCH" >&2
  exit 1
fi

echo "AXTP Spec lock"
echo "  repository: $repository"
echo "  tag: $tag"
echo "  version: $version"
echo "  commit: $commit"
echo "  compatibility: $compatibility"
