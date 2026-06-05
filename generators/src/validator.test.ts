import { mkdtemp, rm, writeFile, mkdir, readFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { describe, expect, it } from "vitest";
import { emitAll } from "./emitters/index.js";
import { loadSpec } from "./loader.js";
import type { SpecModel } from "./models.js";
import { normalizeId } from "./util.js";
import { validateSpec } from "./validator.js";

function baseSpec(): SpecModel {
  return {
    specRoot: "/tmp/spec",
    config: {},
    version: {},
    payloadTypes: [{ id: 1, value: 1, name: "CONTROL", domain: "protocol", status: "mvp" }],
    controlOpcodes: [{ id: 1, value: 1, name: "OPEN", domain: "control", status: "mvp" }],
    rpcEncodings: [{ id: 2, value: 2, name: "BINARY", domain: "rpc", status: "mvp" }],
    rpcBodyEncodings: [{ id: 1, value: 1, name: "TLV8", domain: "rpc", status: "mvp" }],
    rpcOps: [{ id: 7, value: 7, name: "REQUEST", domain: "rpc", status: "mvp" }],
    streamProfiles: [],
    methods: [{
      id: 0x0902,
      name: "audio.setAlgorithmConfig",
      domain: "audio",
      status: "stable",
      bitOffset: 0,
      rpcOp: "request_response",
      requestSchema: "AudioSetAlgorithmConfigRequest",
      responseSchema: "AudioSetAlgorithmConfigResponse",
      recommendedEncoding: ["binary_tlv"],
      capabilities: ["audio.algorithm"],
      events: ["audio.algorithmConfigChanged"],
      errors: ["SUCCESS"]
    }],
    events: [{
      id: 0x0901,
      name: "audio.algorithmConfigChanged",
      domain: "audio",
      status: "stable",
      bitOffset: 0,
      eventSchema: "AudioAlgorithmConfigChangedEvent",
      trigger: ["audio.setAlgorithmConfig"],
      capabilities: ["audio.algorithm"]
    }],
    errors: [{ id: 0, name: "SUCCESS", domain: "common", status: "stable", retryable: false }],
    capabilities: [
      { id: 0x0901, name: "audio.algorithm", domain: "audio", status: "stable", type: "object", schema: "AudioAlgorithmCapability" }
    ],
    legacyMappings: [{
      legacyProtocol: "axdp_hid",
      legacyCmdValue: 0x42,
      legacyName: "CommonSetNoiseSuppressionLevel",
      axtpMethodId: 0x0902,
      axtpMethodName: "audio.setAlgorithmConfig",
      direction: "request_response",
      statusMapping: { "0x00": "SUCCESS" }
    }],
    schemas: [
      { name: "AudioAlgorithmCapability", type: "object", fields: [] },
      {
        name: "AudioSetAlgorithmConfigRequest",
        type: "object",
        fields: [{ id: 1, name: "config", type: "object", required: true, deprecated: false }]
      },
      {
        name: "AudioSetAlgorithmConfigResponse",
        type: "object",
        fields: [{ id: 1, name: "applyState", type: "enum", required: true, deprecated: false }]
      },
      {
        name: "AudioAlgorithmConfigChangedEvent",
        type: "object",
        fields: [{ id: 1, name: "reason", type: "enum", required: true, deprecated: false }]
      }
    ],
    mvpProfile: {
      methods: [],
      events: [],
      errors: ["SUCCESS"],
      capabilities: []
    }
  };
}

describe("normalizeId", () => {
  it("normalizes decimal and hex ids", () => {
    expect(normalizeId("0x0602", "test")).toBe(0x0602);
    expect(normalizeId(1538, "test")).toBe(0x0602);
  });
});

describe("validateSpec", () => {
  it("accepts a valid spec", () => {
    expect(validateSpec(baseSpec())).toContain("[OK] method_registry.yaml: 1 methods checked");
  });

  it("rejects duplicate method ids", () => {
    const spec = baseSpec();
    spec.methods.push({ ...spec.methods[0], name: "display.duplicate" });
    expect(() => validateSpec(spec)).toThrow(/AXTP-GEN-1002|duplicate methodId/);
  });

  it("rejects non-contiguous source bitOffset values", () => {
    const spec = baseSpec();
    spec.methods[0].bitOffset = 2;
    expect(() => validateSpec(spec)).toThrow(/bitOffset must be contiguous from 0/);
  });

  it("rejects missing schema references", () => {
    const spec = baseSpec();
    spec.methods[0].requestSchema = "MissingSchema";
    expect(() => validateSpec(spec)).toThrow(/missing schema/);
  });

  it("rejects ids outside the Domain Registry range", () => {
    const spec = baseSpec();
    spec.capabilities[0].id = 0x0301;
    expect(() => validateSpec(spec)).toThrow(/Domain Registry/);
  });

  it("rejects reserved references", () => {
    const spec = baseSpec();
    spec.capabilities[0].status = "reserved";
    expect(() => validateSpec(spec)).toThrow(/reserved capability/);
  });

  it("rejects missing MVP items", () => {
    const spec = baseSpec();
    spec.mvpProfile.methods.push("missing.method");
    expect(() => validateSpec(spec)).toThrow(/missing method/);
  });

  it("rejects invalid legacy targets", () => {
    const spec = baseSpec();
    spec.legacyMappings[0].axtpMethodId = 0xffff;
    expect(() => validateSpec(spec)).toThrow(/target method does not exist/);
  });
});

describe("loader", () => {
  it("reports YAML parse failures", async () => {
    const dir = await mkdtemp(path.join(os.tmpdir(), "axtp-gen-invalid-"));
    try {
      await writeFile(path.join(dir, "generator.yaml"), "generator: [");
      await expect(loadSpec(dir)).rejects.toThrow(/Flow sequence|parse/i);
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });
});

describe("emitters", () => {
  it("generates stable output snapshots", async () => {
    const dir = await mkdtemp(path.join(os.tmpdir(), "axtp-gen-out-"));
    try {
      await mkdir(dir, { recursive: true });
      await emitAll(baseSpec(), dir);
      await expect(await readFile(path.join(dir, "cpp", "axtp_ids_generated.h"), "utf8")).toMatchFileSnapshot("./__snapshots__/axtp_ids_generated.h");
      await expect(await readFile(path.join(dir, "docs", "method_registry.generated.md"), "utf8")).toMatchFileSnapshot("./__snapshots__/method_registry.generated.md");
      await expect(await readFile(path.join(dir, "json", "method_registry.generated.json"), "utf8")).toMatchFileSnapshot("./__snapshots__/method_registry.generated.json");
      await expect(await readFile(path.join(dir, "test_vectors", "manifest.json"), "utf8")).toMatchFileSnapshot("./__snapshots__/manifest.json");
      await expect(await readFile(path.join(dir, "ts", "axtp_ids_generated.ts"), "utf8")).toMatchFileSnapshot("./__snapshots__/axtp_ids_generated.ts");
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });
});
