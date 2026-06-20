#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
Usage: prepare-runtime-revision.sh [--target-version X.Y.Z.R]

Prepare a runtime-only release revision for the AXTP Spec version locked in
AXTP_SPEC.lock.yaml. If --target-version is omitted, increment the current
runtime revision by one.
USAGE
}

target_version=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --target-version)
      if [[ $# -lt 2 ]]; then
        usage
        exit 2
      fi
      target_version="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unexpected argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
runtime_name="$(basename "$root")"
tool_scripts="devtools/scripts"
generator_dir="devtools/generators"
spec_path="${AXTP_SPEC_PATH:-$root/third_party/axtp-spec}"

read_lock_field() {
  local field="$1"
  awk -v field="$field" '
    $1 == field ":" {
      value = $0
      sub("^[[:space:]]*" field ":[[:space:]]*", "", value)
      gsub(/^"/, "", value)
      gsub(/"$/, "", value)
      print value
      found = 1
      exit
    }
    END { if (!found) exit 1 }
  ' "$root/AXTP_SPEC.lock.yaml"
}

parse_version() {
  local version="$1"
  local prefix="$2"
  if [[ ! "$version" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)(\.([0-9]+))?$ ]]; then
    echo "$prefix must match MAJOR.MINOR.PATCH or MAJOR.MINOR.PATCH.REVISION: $version" >&2
    exit 2
  fi
}

write_output() {
  local key="$1"
  local value="$2"
  if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    printf '%s=%s\n' "$key" "$value" >> "$GITHUB_OUTPUT"
  fi
}

spec_tag="$(read_lock_field tag)"
spec_version="$(read_lock_field version)"
current_version="$("$root/$tool_scripts/get-runtime-version.sh")"

parse_version "$current_version" "Current runtime version"
current_spec_version="${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.${BASH_REMATCH[3]}"
current_revision="${BASH_REMATCH[5]-}"
current_revision="${current_revision:-0}"

if [[ "$current_spec_version" != "$spec_version" ]]; then
  echo "Current runtime version $current_version does not match locked AXTP Spec version $spec_version" >&2
  exit 2
fi

if [[ -z "$target_version" ]]; then
  target_version="$spec_version.$((current_revision + 1))"
fi

parse_version "$target_version" "Target runtime version"
target_spec_version="${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.${BASH_REMATCH[3]}"
target_revision="${BASH_REMATCH[5]-}"

if [[ "$target_spec_version" != "$spec_version" ]]; then
  echo "Target runtime version $target_version must stay on locked AXTP Spec version $spec_version" >&2
  exit 2
fi
if [[ -z "$target_revision" || "$target_revision" == "0" ]]; then
  echo "Runtime-only releases must use a positive fourth revision field: $target_version" >&2
  exit 2
fi
if (( target_revision <= current_revision )); then
  echo "Target runtime revision $target_revision must be greater than current revision $current_revision" >&2
  exit 2
fi
if [[ ! -d "$spec_path" ]]; then
  echo "AXTP spec checkout not found at $spec_path. Set AXTP_SPEC_PATH or checkout third_party/axtp-spec." >&2
  exit 2
fi

spec_lock_hash_before="$(git -C "$root" hash-object AXTP_SPEC.lock.yaml)"

"$root/$tool_scripts/set-runtime-version.sh" "$target_version"

pnpm --dir "$root/$generator_dir" install --frozen-lockfile
pnpm --dir "$root/$generator_dir" build
AXTP_SPEC_PATH="$spec_path" pnpm --dir "$root/$generator_dir" generate:runtime

"$root/$tool_scripts/check-axtp-spec-lock.sh"
"$root/$tool_scripts/check-generated-version.sh"
"$root/$tool_scripts/check-runtime-release.sh" "$target_version"

spec_lock_hash_after="$(git -C "$root" hash-object AXTP_SPEC.lock.yaml)"
if [[ "$spec_lock_hash_after" != "$spec_lock_hash_before" ]]; then
  echo "AXTP_SPEC.lock.yaml changed during a runtime-only revision release" >&2
  exit 1
fi

write_output runtime_name "$runtime_name"
write_output spec_tag "$spec_tag"
write_output spec_version "$spec_version"
write_output previous_version "$current_version"
write_output target_version "$target_version"

cat <<EOF
Prepared $runtime_name runtime revision:
  AXTP Spec: $spec_tag ($spec_version)
  Previous runtime version: $current_version
  Target runtime version: $target_version
EOF
