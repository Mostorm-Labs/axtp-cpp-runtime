#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

spec_path="${AXTP_SPEC_PATH:-}"
if [[ -z "$spec_path" ]]; then
  if [[ -d "$root/third_party/axtp-spec" ]]; then
    spec_path="$root/third_party/axtp-spec"
  elif [[ -d "$root/.axtp-spec" ]]; then
    spec_path="$root/.axtp-spec"
  fi
fi

conformance_dir=""
if [[ -n "$spec_path" ]]; then
  if [[ -f "$spec_path/docs/conformance/manifest.yaml" ]]; then
    conformance_dir="$spec_path/docs/conformance"
  elif [[ -f "$spec_path/conformance/manifest.yaml" ]]; then
    conformance_dir="$spec_path/conformance"
  fi
fi

if [[ -z "$spec_path" || -z "$conformance_dir" ]]; then
  echo "AXTP conformance manifest not found. Set AXTP_SPEC_PATH or checkout third_party/axtp-spec." >&2
  exit 2
fi

profile_path="$root/conformance/runtime-profile.yaml"
if [[ ! -f "$profile_path" ]]; then
  echo "Missing runtime conformance profile: $profile_path" >&2
  exit 2
fi

build_dir="${CONFORMANCE_BUILD_DIR:-$root/build/conformance}"
result_dir="$root/conformance-results"
result_path="$result_dir/result.json"

cmake -S "$root/core" -B "$build_dir" \
  -DAXTP_BUILD_JSON_RPC=ON \
  -DAXTP_BUILD_OPTIONAL_TRANSPORTS=OFF \
  -DAXTP_CORE_BUILD_TESTS=OFF \
  -DAXTP_BUILD_CONFORMANCE=ON
cmake --build "$build_dir" --target axtp_conformance_runner

mkdir -p "$result_dir"
"$build_dir/axtp_conformance_runner" "$spec_path" "$result_path" "$profile_path"

node - "$conformance_dir/schemas/conformance-result.schema.json" "$result_path" <<'NODE'
const fs = require("node:fs");
const [schemaPath, resultPath] = process.argv.slice(2);
JSON.parse(fs.readFileSync(schemaPath, "utf8"));
const result = JSON.parse(fs.readFileSync(resultPath, "utf8"));
const errors = [];
for (const key of ["runtime", "runtimeVersion", "specTag", "summary", "cases"]) {
  if (!(key in result)) errors.push(`missing ${key}`);
}
if (!/^spec\/v[0-9]+\.[0-9]+\.[0-9]+$/.test(result.specTag || "")) {
  errors.push(`invalid specTag ${result.specTag}`);
}
if (!Array.isArray(result.cases)) errors.push("cases must be an array");
for (const [key, value] of Object.entries(result.summary || {})) {
  if (["total", "passed", "failed", "skipped", "unsupported"].includes(key) && (!Number.isInteger(value) || value < 0)) {
    errors.push(`summary.${key} must be a non-negative integer`);
  }
}
for (const item of result.cases || []) {
  if (typeof item.id !== "string" || item.id.length === 0) errors.push("case id must be a non-empty string");
  if (!["passed", "failed", "skipped", "unsupported"].includes(item.status)) {
    errors.push(`case ${item.id || "<unknown>"} has invalid status ${item.status}`);
  }
}
if (errors.length > 0) {
  console.error(`Invalid conformance result ${resultPath}`);
  for (const error of errors) console.error(`- ${error}`);
  process.exit(1);
}
NODE

echo "AXTP conformance result: $result_path"
