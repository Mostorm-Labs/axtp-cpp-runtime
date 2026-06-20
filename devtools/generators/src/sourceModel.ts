import type { SpecModel } from "./models.js";

export interface ProtocolSourceModel extends SpecModel {
  protocolMeta: Record<string, unknown>;
  sourceFiles: string[];
  profiles: Array<Record<string, unknown>>;
}
