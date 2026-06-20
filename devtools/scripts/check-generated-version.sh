#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
runtime_name="$(basename "$root")"

node "$root/devtools/scripts/axtp-versioning.mjs" check --runtime-name "$runtime_name"
