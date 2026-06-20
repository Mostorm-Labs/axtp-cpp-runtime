import path from "node:path";
import type { SpecModel } from "../models.js";
import { hex, sortById, toJsonStable, writeTextFile } from "../util.js";

export async function emitJson(spec: SpecModel, outDir: string): Promise<void> {
  await emitJsonFiles(spec, path.join(outDir, "json"));
}

export async function emitJsonFiles(spec: SpecModel, jsonDir: string): Promise<void> {
  await Promise.all([
    writeTextFile(path.join(jsonDir, "method_registry.generated.json"), toJsonStable({ methods: sortById(spec.methods).map(withIdHex) })),
    writeTextFile(path.join(jsonDir, "event_registry.generated.json"), toJsonStable({ events: sortById(spec.events).map(withIdHex) })),
    writeTextFile(path.join(jsonDir, "error_code.generated.json"), toJsonStable({ errors: sortById(spec.errors).map(withIdHex) })),
    writeTextFile(path.join(jsonDir, "capability_registry.generated.json"), toJsonStable({ capabilities: sortById(spec.capabilities).map(withIdHex) })),
    writeTextFile(path.join(jsonDir, "schema.generated.json"), toJsonStable({ schemas: spec.schemas })),
    writeTextFile(path.join(jsonDir, "legacy_mapping.generated.json"), toJsonStable({ legacyMappings: spec.legacyMappings }))
  ]);
}

function withIdHex<T extends { id: number }>(item: T): T & { idHex: string } {
  return { ...item, idHex: hex(item.id) };
}
