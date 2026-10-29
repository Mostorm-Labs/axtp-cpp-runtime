import { mkdtemp, readFile, readdir, rm } from "node:fs/promises";
import path from "node:path";
import os from "node:os";
import { pathToFileURL, fileURLToPath } from "node:url";
import { resolveSpecInput } from "./axtp-spec-input.mjs";

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),"../..");
await resolveSpecInput(root);
const specRoot=process.env.AXTP_SPEC_PATH || path.join(root,"third_party/axtp-spec");
const {loadProtocolSources}=await import(pathToFileURL(path.join(root,"devtools/generators/dist/sourceLoader.js")));
const {emitCppFiles}=await import(pathToFileURL(path.join(root,"devtools/generators/dist/emitters/cpp.js")));
const directory=await mkdtemp(path.join(os.tmpdir(),"axtp-generated-check-"));
try {
  await emitCppFiles(await loadProtocolSources(specRoot),directory);
  const files=await readdir(directory);
  for(const name of files) {
    const actual=await readFile(path.join(root,"include/core/protocol/generated",name));
    if(!actual.equals(await readFile(path.join(directory,name))))throw new Error(`Generated drift: ${name}`);
  }
  console.log(`Generated drift check: ${files.length} runtime artifacts match frozen input`);
} finally {
  if(path.dirname(path.resolve(directory))!==path.resolve(os.tmpdir()) || !path.basename(directory).startsWith("axtp-generated-check-"))throw new Error("Unsafe cleanup path");
  await rm(directory,{recursive:true,force:true});
}
