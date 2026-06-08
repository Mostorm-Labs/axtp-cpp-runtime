import { readFile } from "node:fs/promises";
import path from "node:path";
import { GeneratorError } from "./errors.js";
import type { ProtocolModel } from "./protocolModel.js";

export interface ProtocolDocsText {
  streamSpec: string;
  controlSpec: string;
  typesSpec: string;
}

function fail(file: string, entry: string, message: string): never {
  throw new GeneratorError({
    code: "AXTP-GEN-1004",
    file,
    entry,
    message
  });
}

function requirePattern(text: string, pattern: RegExp, file: string, entry: string, message: string): void {
  if (!pattern.test(text)) fail(file, entry, message);
}

function assertYamlStreamHeader(model: ProtocolModel): void {
  const expected = [
    ["streamId", "uint32"],
    ["seqId", "uint32"],
    ["cursor", "uint64"]
  ];
  const fields = model.stream.header.fields;
  if (model.stream.header.size !== 16 || fields.length !== expected.length) {
    fail("protocol/axtp.protocol.yaml", "stream.header", "YAML stream.header must match the 16B STREAM header defined in docs/specs/1-core/07-Stream-Data-Plane.md");
  }
  for (const forbidden of ["seq", "position", "chunkLength", "flags"]) {
    if (fields.some((field) => field.name === forbidden)) {
      fail("protocol/axtp.protocol.yaml", "stream.header", `YAML stream.header must not contain legacy field: ${forbidden}`);
    }
  }
  expected.forEach(([name, type], index) => {
    const field = fields[index];
    if (!field || field.name !== name || field.type !== type) {
      fail("protocol/axtp.protocol.yaml", "stream.header", "YAML stream.header must be streamId:uint32, seqId:uint32, cursor:uint64");
    }
  });
}

function assertYamlControl(model: ProtocolModel): void {
  for (const opcode of ["OPEN", "ACCEPT", "HEARTBEAT", "HEARTBEAT_ACK", "CLOSE", "CLOSE_ACK"]) {
    if (!model.control.requiredOpcodes.includes(opcode)) {
      fail("protocol/axtp.protocol.yaml", "control.requiredOpcodes", `YAML control.requiredOpcodes must include ${opcode}`);
    }
  }
  for (const opcode of ["READY", "ACK", "NACK"]) {
    if (model.control.requiredOpcodes.includes(opcode)) {
      fail("protocol/axtp.protocol.yaml", "control.requiredOpcodes", `${opcode} must not be a Phase 1 required opcode`);
    }
  }
  if (!model.control.optionalOpcodes.includes("READY")) {
    fail("protocol/axtp.protocol.yaml", "control.optionalOpcodes", "YAML control.optionalOpcodes must include READY");
  }
  for (const opcode of ["ACK", "NACK"]) {
    if (!model.control.optionalOpcodes.includes(opcode)) {
      fail("protocol/axtp.protocol.yaml", "control.optionalOpcodes", `YAML control.optionalOpcodes must keep ${opcode} as future optional opcode`);
    }
  }
}

function assertYamlCapability(model: ProtocolModel): void {
  const supportedMethodsResponse = model.schemas.find((schema) => schema.name === "CapabilitySupportedMethodsResponse");
  if (supportedMethodsResponse) {
    const methodMasks = supportedMethodsResponse.fields.find((field) => field.name === "methodMasks");
    if (!methodMasks || methodMasks.derivedFrom !== "methods[].bitOffset") {
      fail("protocol/axtp.protocol.yaml", "CapabilitySupportedMethodsResponse.methodMasks", "methodMasks must derive from methods[].bitOffset");
    }
  }
}

export async function loadProtocolDocs(specRoot: string): Promise<ProtocolDocsText> {
  const docsRoot = path.join(specRoot, "docs", "specs");
  const [streamSpec, controlSpec, typesSpec] = await Promise.all([
    readFile(path.join(docsRoot, "1-core", "07-Stream-Data-Plane.md"), "utf8"),
    readFile(path.join(docsRoot, "1-core", "05-Control-Session.md"), "utf8"),
    readFile(path.join(docsRoot, "3-codec", "02-Capability-Types.md"), "utf8")
  ]);
  return { streamSpec, controlSpec, typesSpec };
}

export function validateProtocolDocsConsistency(model: ProtocolModel, docs: ProtocolDocsText): string[] {
  requirePattern(docs.streamSpec, /STREAM Header[^\n]*16B|16B STREAM Header/, "docs/specs/1-core/07-Stream-Data-Plane.md", "STREAM Header", "stream spec must define a 16B STREAM Header");
  requirePattern(docs.streamSpec, /streamId:uint32/, "docs/specs/1-core/07-Stream-Data-Plane.md", "STREAM Header", "stream spec must define streamId:uint32");
  requirePattern(docs.streamSpec, /seqId:uint32/, "docs/specs/1-core/07-Stream-Data-Plane.md", "STREAM Header", "stream spec must define seqId:uint32");
  requirePattern(docs.streamSpec, /cursor:uint64/, "docs/specs/1-core/07-Stream-Data-Plane.md", "STREAM Header", "stream spec must define cursor:uint64");

  requirePattern(docs.controlSpec, /OPEN[\s\S]*ACCEPT/, "docs/specs/1-core/05-Control-Session.md", "OPEN/ACCEPT", "control spec must define OPEN and ACCEPT");
  requirePattern(docs.controlSpec, /HEARTBEAT[\s\S]*HEARTBEAT_ACK/, "docs/specs/1-core/05-Control-Session.md", "HEARTBEAT", "control spec must define HEARTBEAT and HEARTBEAT_ACK");
  requirePattern(docs.controlSpec, /CLOSE[\s\S]*CLOSE_ACK/, "docs/specs/1-core/05-Control-Session.md", "CLOSE", "control spec must define CLOSE and CLOSE_ACK");
  requirePattern(docs.controlSpec, /READY[\s\S]{0,80}可选/, "docs/specs/1-core/05-Control-Session.md", "READY", "control spec must define READY as optional");
  requirePattern(docs.controlSpec, /默认握手只要求 OPEN \/ ACCEPT/, "docs/specs/1-core/05-Control-Session.md", "READY", "control spec must state that default handshake only requires OPEN / ACCEPT");
  requirePattern(docs.controlSpec, /Phase 1[\s\S]{0,120}不要求 runtime 实现 ACK\/NACK/, "docs/specs/1-core/05-Control-Session.md", "ACK/NACK", "control spec must keep ACK/NACK out of Phase 1");

  assertYamlStreamHeader(model);
  assertYamlControl(model);
  assertYamlCapability(model);

  return [
    "[OK] docs/specs: STREAM header facts checked",
    "[OK] docs/specs: CONTROL Phase 1 opcode facts checked",
    "[OK] docs/specs: optional capability discovery facts checked"
  ];
}
