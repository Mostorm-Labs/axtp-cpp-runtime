import path from "node:path";
import type { ProtocolModel } from "../protocolModel.js";
import { toJsonStable, writeTextFile } from "../util.js";

export function toProtocolJson(model: ProtocolModel): Record<string, unknown> {
  return {
    source: "protocol/axtp.protocol.yaml",
    protocol: model.protocol,
    overview: model.overview,
    architecture: model.architecture,
    guide: model.guide,
    wire: model.wire,
    frameProfiles: model.frameProfiles,
    transports: model.transports,
    payloadTypes: model.payloadTypes,
    control: model.control,
    stream: model.stream,
    compatibility: model.compatibility,
    schemas: model.schemas,
    wireExamples: model.wireExamples,
    methods: model.methods,
    events: model.events,
    errors: model.errors,
    profiles: model.profiles
  };
}

export async function emitProtocolJson(model: ProtocolModel, outDir: string): Promise<void> {
  await writeTextFile(path.join(outDir, "protocol.json"), toJsonStable(toProtocolJson(model)));
}
