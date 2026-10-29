import test from "node:test";
import assert from "node:assert/strict";
import { mkdtemp, mkdir, writeFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { createHash } from "node:crypto";
import { resolveSpecInput } from "./axtp-spec-input.mjs";

test("explicit local binding verifies bytes and release checks remain strict", async () => {
  const root = await mkdtemp(path.join(tmpdir(), "axtp-input-"));
  try {
    const spec = path.join(root,"spec"); await mkdir(spec);
    const data = "frozen protocol\n";
    const sha = createHash("sha256").update(data).digest("hex");
    const fingerprint = createHash("sha256").update(`input.yaml\t${sha}\n`).digest("hex");
    const binding = {sourceMode:"local-development-snapshot",published:false,repository:"https://github.com/Mostorm-Labs/axtp",baselineCommit:"a".repeat(40),snapshotSha256:fingerprint,version:`0.18.0-dev.${fingerprint.slice(0,12)}`};
    const manifest = {...binding,schemaVersion:1,files:[{path:"input.yaml",sha256:sha,size:Buffer.byteLength(data)}]};
    await writeFile(path.join(spec,"input.yaml"),data);
    await writeFile(path.join(spec,".axtp-local-snapshot.json"),JSON.stringify(manifest));
    await writeFile(path.join(root,"AXTP_SPEC.local.json"),JSON.stringify(binding));
    const env={AXTP_SPEC_SOURCE_MODE:"local-development",AXTP_SPEC_PATH:spec};
    assert.equal((await resolveSpecInput(root,{env})).snapshotSha256,fingerprint);
    await assert.rejects(resolveSpecInput(root,{env,releaseOnly:true}),/Release verification rejects/);
    await assert.rejects(resolveSpecInput(root,{env:{AXTP_SPEC_SOURCE_MODE:"local-development"}}),/explicit AXTP_SPEC_PATH/);
    await writeFile(path.join(spec,"input.yaml"),"changed");
    await assert.rejects(resolveSpecInput(root,{env}),/bytes changed/);
    await writeFile(path.join(spec,"input.yaml"),data);
    await writeFile(path.join(spec,"extra.yaml"),"injected");
    await assert.rejects(resolveSpecInput(root,{env}),/Unmanifested/);
    await rm(path.join(spec,"extra.yaml"));
    await writeFile(path.join(root,"AXTP_SPEC.local.json"),JSON.stringify({...binding,snapshotSha256:"b".repeat(64)}));
    await assert.rejects(resolveSpecInput(root,{env}),/identity mismatch/);
    await writeFile(path.join(root,"AXTP_SPEC.lock.yaml"),'axtp_spec:\n  repository: https://github.com/Mostorm-Labs/axtp\n  tag: unreleased\n  version: 0.18.0\n  commit: '+"a".repeat(40)+'\n  compatibility: ">=0.18.0 <0.19.0"\n');
    await assert.rejects(resolveSpecInput(root,{env:{}}),/exact released/);
  } finally {
    assert.equal(path.dirname(path.resolve(root)), path.resolve(tmpdir()));
    assert.ok(path.basename(root).startsWith("axtp-input-"));
    await rm(root,{recursive:true,force:true});
  }
});
