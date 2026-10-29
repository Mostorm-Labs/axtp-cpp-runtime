// Cross-platform, source-verified conformance entry point. Development evidence
// is explicitly separate from released conformance-result schema validation.
import {execFileSync} from "node:child_process";
import {readFile,writeFile,mkdir} from "node:fs/promises";
import {createHash} from "node:crypto";
import path from "node:path";
import {fileURLToPath} from "node:url";
import {resolveSpecInput} from "./axtp-spec-input.mjs";
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),"../..");
const input=await resolveSpecInput(root);
const spec=process.env.AXTP_SPEC_PATH || path.join(root,"third_party/axtp-spec");
const build=path.resolve(process.env.CONFORMANCE_BUILD_DIR || path.join(root,"build/conformance"));
const resultDir=path.resolve(process.env.CONFORMANCE_RESULT_DIR || path.join(root,"build/conformance-results"));
const profile=path.join(root,"devtools/conformance/runtime-profile.yaml");
const executable=path.join(build,`axtp_conformance_runner${process.platform==="win32"?".exe":""}`);
const run=(file,args)=>execFileSync(file,args,{cwd:root,stdio:"inherit"});
if(!process.env.CONFORMANCE_SKIP_BUILD) {
  run("cmake",["-S",root,"-B",build,"-DAXTP_BUILD_JSON_RPC=ON","-DAXTP_CPP_RUNTIME_BUILD_CONFORMANCE=ON"]);
  run("cmake",["--build",build,"--target","axtp_conformance_runner"]);
}
await mkdir(resultDir,{recursive:true});
const report=path.join(resultDir,"result.json");
run(executable,[spec,report,profile]);
await resolveSpecInput(root);
const bytes=await readFile(report),result=JSON.parse(bytes);
if(result.specTag!==input.tag || !Array.isArray(result.cases) || !result.summary || result.cases.some(c=>c.requirement==="required" && c.status!=="passed"))
  throw new Error("Conformance report does not match verified input or required profile");
if(input.sourceMode==="release" && !/^spec\/v\d+\.\d+\.\d+$/.test(result.specTag))throw new Error("Invalid released result tag");
await writeFile(path.join(resultDir,"source-verification.json"),JSON.stringify({
  schemaVersion:1,source:input,profile,resultSha256:createHash("sha256").update(bytes).digest("hex"),
  developmentSnapshotVerified:input.sourceMode==="local-development-snapshot",
  formalReleaseLockVerified:input.sourceMode==="release",
  releasedResultSchemaApplicable:input.sourceMode==="release",summary:result.summary
},null,2)+"\n");
console.log(`Verified ${input.sourceMode} conformance: ${report}`);
