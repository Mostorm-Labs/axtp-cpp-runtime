#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 spec/vMAJOR.MINOR.PATCH" >&2
  exit 2
fi

tag="$1"
if [[ "$tag" == "main" || ! "$tag" =~ ^spec/v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Expected a released AXTP Spec tag, for example spec/v0.1.0" >&2
  exit 2
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
lock="$root/AXTP_SPEC.lock.yaml"
repo="${AXTP_SPEC_REPOSITORY:-https://github.com/Mostorm-Labs/axtp.git}"
commit="$(git ls-remote --tags "$repo" "refs/tags/$tag" | awk 'NR == 1 { print $1 }')"

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

echo "Updated AXTP_SPEC.lock.yaml to $tag at $commit"
echo "No commit was created."
