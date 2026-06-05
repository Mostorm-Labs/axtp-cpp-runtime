#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
spec="${AXTP_SPEC_PATH:-$root/third_party/axtp-spec}"

if [[ ! -d "$spec/registry" ]]; then
  echo "AXTP_SPEC_PATH must point to an AXTP spec checkout with registry/." >&2
  echo "Current value: $spec" >&2
  exit 1
fi

if [[ ! -f "$root/generators/dist/sourceLoader.js" ]]; then
  echo "Generator is not built. Run: pnpm --dir generators build" >&2
  exit 1
fi

AXTP_RUNTIME_ROOT="$root" AXTP_SPEC_ROOT="$spec" node --input-type=module <<'NODE'
import path from "node:path";
import { pathToFileURL } from "node:url";

const root = process.env.AXTP_RUNTIME_ROOT;
const specRoot = process.env.AXTP_SPEC_ROOT;

const { loadProtocolSources } = await import(pathToFileURL(path.join(root, "generators/dist/sourceLoader.js")).href);
const { validateSpec } = await import(pathToFileURL(path.join(root, "generators/dist/validator.js")).href);
const { emitCppFiles } = await import(pathToFileURL(path.join(root, "generators/dist/emitters/cpp.js")).href);

const spec = await loadProtocolSources(specRoot);
for (const message of validateSpec(spec)) console.log(message);

const outDir = path.join(root, "core/include/generated");
await emitCppFiles(spec, outDir);
console.log(`[OK] generated C++ artifacts: ${outDir}`);
NODE

AXTP_SPEC_PATH="$spec" node "$root/scripts/axtp-versioning.mjs" generate --runtime-name axtp-cpp-runtime
