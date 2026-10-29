import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { execFileSync } from "node:child_process";
import { resolveSpecInput } from "./axtp-spec-input.mjs";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const input = await resolveSpecInput(root);
const specRoot = process.env.AXTP_SPEC_PATH || path.join(root, "third_party/axtp-spec");
const { loadProtocolSources } = await import(pathToFileURL(path.join(root, "devtools/generators/dist/sourceLoader.js")));
const { validateSpec } = await import(pathToFileURL(path.join(root, "devtools/generators/dist/validator.js")));
const { emitCppFiles } = await import(pathToFileURL(path.join(root, "devtools/generators/dist/emitters/cpp.js")));
const spec = await loadProtocolSources(specRoot);
for (const message of validateSpec(spec)) console.log(message);
await emitCppFiles(spec, path.join(root, "include/core/protocol/generated"));
await resolveSpecInput(root);
execFileSync(process.execPath, [path.join(root, "devtools/scripts/axtp-versioning.mjs"), "generate", "--runtime-name", "axtp-cpp-runtime"], {stdio:"inherit"});
console.log(`Generated C++ artifacts from ${input.tag} (${input.commit}), sourceMode=${input.sourceMode}`);
