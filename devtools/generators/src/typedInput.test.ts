import { describe, expect, it } from "vitest";
import path from "node:path";
import { mkdtemp, readFile } from "node:fs/promises";
import os from "node:os";
import { loadProtocolSources } from "./sourceLoader.js";
import { validateSpec } from "./validator.js";
import { buildProtocolDefinition } from "./protocolBuilder.js";
import { validateProtocolDefinition } from "./protocolValidator.js";
import { toProtocolJson } from "./emitters/protocolJson.js";
import { emitCppFiles } from "./emitters/cpp.js";

const root = process.env.AXTP_SPEC_PATH ?? path.resolve("../../third_party/axtp-spec");
const fields = (source: any, name: string) => source.schemas.find((s: any) => s.name === name).fields;

describe("R2 input-sample-v1 adopted facts", () => {
  it("loads the packed 7-byte contact layout and exact Mouse variants from source", async () => {
    const source: any = await loadProtocolSources(root);
    expect(source.inputStream?.format).toBe("input-sample-v1");
    expect(source.inputStream.touch.recordBytes).toBe(7);
    expect(source.inputStream.touch.fields.map((f: any) => [f.name, f.offset, f.bytes])).toEqual([
      ["contactId", 0, 2], ["phase", 2, 1], ["x", 3, 2], ["y", 5, 2]
    ]);
    expect(source.inputStream.mouse.records.map((r: any) => [r.name, r.kind, r.bytes])).toEqual([
      ["move", 0, 11], ["buttons", 1, 11], ["wheel", 2, 21], ["cancel", 3, 1], ["modifiers", 4, 3]
    ]);
    const model: any = buildProtocolDefinition(source);
    expect((toProtocolJson(model) as any).inputStream).toEqual(source.inputStream);
    expect(model.stream.header.size).toBe(16);
    expect(() => validateSpec(source)).not.toThrow();
    expect(() => validateProtocolDefinition(model)).not.toThrow();
  });

  it("opens on control context and allows both types without mode or per-packet fences", async () => {
    const source = await loadProtocolSources(root);
    const names = fields(source, "InputOpenStreamParams").map((f: any) => f.name);
    expect(names).toEqual(expect.arrayContaining(["sourceBindingId", "format", "inputTypes", "maxDataBytes", "touch", "mouse"]));
    expect(names).not.toEqual(expect.arrayContaining(["preferredMode"]));
    for (const name of ["epoch", "canvasToken", "geometryToken", "maxFrameBytes", "supportedModes", "modeRequirement"])
      expect(names).not.toContain(name);
    expect(fields(source, "InputCloseStreamParams").map((f: any) => f.name)).not.toContain("epoch");
    expect(fields(source, "InputOpenStreamParams").find((f: any) => f.name === "format").enum).toEqual(["input-sample-v1"]);
  });

  it("retires the mode allocation without renumbering unrelated method masks", async () => {
    const source: any = await loadProtocolSources(root);
    expect(source.methods.some((m: any) => m.name === "cast.setTouchbackMode")).toBe(false);
    expect(source.methods.find((m: any) => m.name === "cast.setTouchbackResolutionPreference").bitOffset).toBe(30);
    expect(source.retiredAllocations).toContainEqual({kind:"method", id:0x161e, domain:"cast", bitOffset:29});
    const state = fields(source, "CastTouchbackState");
    expect(state.map((f: any) => f.name)).toContain("acceptedInputTypes");
    expect(state.map((f: any) => f.name)).not.toContain("requestedMode");
    expect(state.find((f: any) => f.name === "revision").id).toBe(14);
  });

  it("rejects layout drift and retired allocation reuse in source and IR", async () => {
    const source: any = await loadProtocolSources(root);
    expect(source.inputStream).toBeDefined();
    source.inputStream.touch.fields[3].offset = 6;
    expect(() => validateSpec(source)).toThrow(/input.*layout/i);
    const fresh: any = await loadProtocolSources(root);
    const model: any = buildProtocolDefinition(fresh);
    model.inputStream.mouse.records[2].bytes = 20;
    expect(() => validateProtocolDefinition(model)).toThrow(/input.*layout/i);
    const collision: any = await loadProtocolSources(root);
    collision.methods.push({...collision.methods.find((m: any) => m.name === "cast.setTouchbackResolutionPreference"), id:0x161e, bitOffset:29, name:"cast.reusedRetiredSlot"});
    expect(() => validateSpec(collision)).toThrow(/retired|duplicate/i);
    const schema = fields(collision, "InputOpenStreamParams");
    schema.push({id:6,name:"reusedField",type:"uint8",required:false,deprecated:false});
    collision.methods.pop();
    expect(() => validateSpec(collision)).toThrow(/retired.*field/i);
  });

  it("publishes a framed WebRTC profile and the minimal common stream-state requirements", async () => {
    const model = buildProtocolDefinition(await loadProtocolSources(root));
    expect(model.transports.find(t => t.name === "AXTP-WEBRTC-DC")).toMatchObject({frameProfile:"STANDARD_FRAME", supportsControl:true, supportsStream:true});
    const profile = model.profiles.find(p => p.name === "AXTP-TYPED-INPUT-V1");
    expect(profile?.requiredMethods).toEqual(expect.arrayContaining(["input.openStream", "input.closeStream", "stream.getState"]));
    expect(profile?.requiredEvents).toContain("stream.stateChanged");
    expect(profile?.requiredMethods).not.toContain("stream.ack");
  });

  it("emits wire constants for C++ without handwritten receiver IDs", async () => {
    const source = await loadProtocolSources(root);
    const dir = await mkdtemp(path.join(os.tmpdir(), "axtp-input-"));
    await emitCppFiles(source, dir);
    const wire = await readFile(path.join(dir, "axtp_input_stream_generated.h"), "utf8").catch(() => "");
    expect(wire).toContain("kTouchRecordBytes = 7");
    expect(wire).toContain("kWheelRecordBytes = 21");
    expect(wire).toContain("kMaxDataBytes = 4096");
    expect(wire).not.toContain("kHeaderBytes = 40");
  });

});
