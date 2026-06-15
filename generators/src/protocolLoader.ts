import { readFile } from "node:fs/promises";
import path from "node:path";
import YAML from "yaml";
import { GeneratorError } from "./errors.js";
import type {
  CompatibilityDefinition,
  ControlDefinition,
  ErrorDefinition,
  EventDefinition,
  FrameProfile,
  MethodDefinition,
  PayloadType,
  ProfileDefinition,
  ProtocolArchitecture,
  ProtocolGuide,
  ProtocolMetadata,
  ProtocolModel,
  ProtocolOverview,
  SchemaDefinition,
  SchemaField,
  StreamDefinition,
  TransportProfile,
  WireDefinition,
  WireExample,
  WireExampleStep
} from "./protocolModel.js";
import { normalizeId } from "./util.js";

function asArray(value: unknown): any[] {
  return Array.isArray(value) ? value : [];
}

function asStringArray(value: unknown): string[] {
  return asArray(value).map(String);
}

function asObject(value: unknown, context: string): Record<string, any> {
  if (value && typeof value === "object" && !Array.isArray(value)) return value as Record<string, any>;
  throw new GeneratorError({
    code: "AXTP-GEN-1001",
    file: "protocol/axtp.protocol.yaml",
    entry: context,
    message: `${context} must be an object`
  });
}

function optionalNumber(value: unknown): number | undefined {
  return typeof value === "number" ? value : undefined;
}

function mapSchemaField(field: any, schemaName: string): SchemaField {
  return {
    fieldId: normalizeId(field.fieldId, `${schemaName}.${field.name}`),
    name: String(field.name),
    type: String(field.type),
    required: Boolean(field.required),
    min: optionalNumber(field.min),
    max: optionalNumber(field.max),
    maxLength: optionalNumber(field.maxLength),
    deprecated: field.deprecated === undefined ? undefined : Boolean(field.deprecated),
    derivedFrom: field.derivedFrom === undefined ? undefined : String(field.derivedFrom),
    description: field.description === undefined ? undefined : String(field.description)
  };
}

function mapSchemas(raw: any): SchemaDefinition[] {
  // Accept both `schemas:` (new) and `types:` (legacy) as the top-level key
  const rawSchemas = asObject(raw.schemas ?? raw.types, "schemas");
  return Object.entries(rawSchemas).map(([name, item]) => {
    const entry = asObject(item, `schemas.${name}`);
    return {
      name,
      kind: String(entry.kind ?? "object"),
      description: entry.description === undefined ? undefined : String(entry.description),
      fields: asArray(entry.fields).map((field) => mapSchemaField(field, name))
    };
  });
}

function mapMethods(methods: unknown): MethodDefinition[] {
  return asArray(methods).map((item) => ({
    name: String(item.name),
    description: item.description === undefined ? undefined : String(item.description),
    methodId: normalizeId(item.methodId, `methods.${item.name}.methodId`),
    bitOffset: normalizeId(item.bitOffset, `methods.${item.name}.bitOffset`),
    domain: String(item.domain),
    since: String(item.since ?? ""),
    status: item.status ?? "draft",
    request: { type: String(item.request?.type ?? "") },
    response: { type: String(item.response?.type ?? "") },
    encodings: asStringArray(item.encodings),
    capabilities: asStringArray(item.capabilities),
    events: asStringArray(item.events),
    errors: asStringArray(item.errors),
    legacy: item.legacy
  }));
}

function mapEvents(events: unknown): EventDefinition[] {
  return asArray(events).map((item) => ({
    name: String(item.name),
    description: item.description === undefined ? undefined : String(item.description),
    eventId: normalizeId(item.eventId, `events.${item.name}.eventId`),
    bitOffset: normalizeId(item.bitOffset, `events.${item.name}.bitOffset`),
    domain: String(item.domain),
    since: String(item.since ?? ""),
    status: item.status ?? "draft",
    payload: { type: String(item.payload?.type ?? "") },
    severity: item.severity === undefined ? undefined : String(item.severity),
    trigger: asStringArray(item.trigger),
    capabilities: asStringArray(item.capabilities)
  }));
}

function mapErrors(errors: unknown): ErrorDefinition[] {
  return asArray(errors).map((item) => ({
    name: String(item.name),
    code: normalizeId(item.code, `errors.${item.name}.code`),
    category: String(item.category),
    since: item.since === undefined ? undefined : String(item.since),
    status: item.status ?? "draft",
    severity: String(item.severity),
    retryable: Boolean(item.retryable),
    message: String(item.message)
  }));
}

function mapProfiles(profiles: unknown): ProfileDefinition[] {
  return asArray(profiles).map((item) => ({
    name: String(item.name),
    since: String(item.since ?? ""),
    status: item.status ?? "draft",
    extends: item.extends === undefined ? undefined : String(item.extends),
    requiredMethods: asStringArray(item.requiredMethods),
    requiredEvents: asStringArray(item.requiredEvents),
    requiredTypes: asStringArray(item.requiredTypes),
    requiredErrors: asStringArray(item.requiredErrors),
    transportProfiles: asStringArray(item.transportProfiles),
    frameProfile: item.frameProfile === undefined ? undefined : String(item.frameProfile),
    frameProfiles: asStringArray(item.frameProfiles),
    notes: item.notes === undefined ? undefined : String(item.notes)
  }));
}

function mapProtocol(raw: any): ProtocolMetadata {
  const item = asObject(raw, "protocol");
  return {
    name: String(item.name),
    version: String(item.version),
    specVersion: normalizeId(item.specVersion, "protocol.specVersion"),
    registryVersion: String(item.registryVersion),
    status: item.status === undefined ? undefined : String(item.status)
  };
}

function mapOverview(raw: any): ProtocolOverview {
  const item = asObject(raw, "overview");
  return {
    title: String(item.title),
    summary: String(item.summary ?? "").trim(),
    goals: asStringArray(item.goals),
    nonGoals: asStringArray(item.nonGoals)
  };
}

function mapArchitecture(raw: any): ProtocolArchitecture {
  const item = asObject(raw, "architecture");
  return {
    layers: asArray(item.layers).map((layer) => ({
      name: String(layer.name),
      description: String(layer.description)
    })),
    lifecycle: asArray(item.lifecycle).map((step) => ({
      step: String(step.step),
      from: step.from === undefined ? undefined : String(step.from),
      to: step.to === undefined ? undefined : String(step.to),
      status: step.status === undefined ? undefined : String(step.status),
      description: String(step.description)
    })),
    optionalLifecycleExtensions: asArray(item.optionalLifecycleExtensions).map((step) => ({
      step: String(step.step),
      from: step.from === undefined ? undefined : String(step.from),
      to: step.to === undefined ? undefined : String(step.to),
      status: step.status === undefined ? undefined : String(step.status),
      description: String(step.description)
    }))
  };
}

function mapGuide(raw: any): ProtocolGuide {
  const item = asObject(raw ?? {}, "guide");
  return {
    quickStart: asArray(item.quickStart).map((guide) => ({
      title: String(guide.title),
      steps: asStringArray(guide.steps)
    }))
  };
}

function mapWire(raw: any): WireDefinition {
  const item = asObject(raw, "wire");
  return {
    byteOrder: String(item.byteOrder),
    byteOrderAlias: item.byteOrderAlias === undefined ? undefined : String(item.byteOrderAlias),
    integerEncoding: String(item.integerEncoding),
    crcByteOrder: String(item.crcByteOrder),
    scope: item.scope === undefined ? undefined : String(item.scope)
  };
}

function mapFrameProfiles(value: unknown): FrameProfile[] {
  return asArray(value).map((item) => ({
    name: String(item.name),
    magic: item.magic,
    l1: String(item.l1),
    l2: String(item.l2),
    supportsMixing: item.supportsMixing === undefined ? undefined : Boolean(item.supportsMixing)
  }));
}

function mapTransports(value: unknown): TransportProfile[] {
  return asArray(value).map((item) => ({
    name: String(item.name),
    family: String(item.family),
    mode: item.mode === undefined ? undefined : String(item.mode),
    frameProfile: String(item.frameProfile),
    production: Boolean(item.production),
    maxFrameSize: optionalNumber(item.maxFrameSize),
    rpcEncodings: item.rpcEncodings === undefined ? undefined : asStringArray(item.rpcEncodings),
    supportsControl: item.supportsControl === undefined ? undefined : Boolean(item.supportsControl),
    supportsStream: item.supportsStream === undefined ? undefined : Boolean(item.supportsStream),
    physicalClient: item.physicalClient === undefined ? undefined : String(item.physicalClient),
    physicalServer: item.physicalServer === undefined ? undefined : String(item.physicalServer),
    logicalClient: item.logicalClient === undefined ? undefined : String(item.logicalClient),
    logicalServer: item.logicalServer === undefined ? undefined : String(item.logicalServer),
    helloSender: item.helloSender === undefined ? undefined : String(item.helloSender),
    usage: item.usage === undefined ? undefined : String(item.usage),
    notes: item.notes === undefined ? undefined : String(item.notes)
  }));
}

function mapWireExamples(value: unknown): WireExample[] {
  return asArray(value).map((item) => ({
    title: String(item.title),
    transport: String(item.transport),
    frameProfile: String(item.frameProfile),
    description: String(item.description ?? ""),
    steps: asArray(item.steps).map((step: any): WireExampleStep => ({
      direction: String(step.direction),
      label: String(step.label),
      asciiLayout: String(step.asciiLayout ?? ""),
      hexBytes: String(step.hexBytes ?? ""),
      fieldAnnotations: asStringArray(step.fieldAnnotations)
    }))
  }));
}

function mapPayloadTypes(value: unknown): PayloadType[] {
  return asArray(value).map((item) => ({
    name: String(item.name),
    id: normalizeId(item.id, `payloadTypes.${item.name}.id`),
    headerBytes: normalizeId(item.headerBytes, `payloadTypes.${item.name}.headerBytes`),
    description: String(item.description),
    selectionRule: item.selectionRule === undefined ? undefined : String(item.selectionRule),
    headerFields: item.headerFields === undefined ? undefined : asArray(item.headerFields).map((f: any) => ({
      name: String(f.name),
      type: String(f.type),
      bytes: typeof f.bytes === "number" ? f.bytes : String(f.bytes),
      description: String(f.description)
    }))
  }));
}

function mapControl(value: unknown): ControlDefinition {
  const item = asObject(value, "control");
  return {
    requiredOpcodes: asStringArray(item.requiredOpcodes),
    optionalOpcodes: asStringArray(item.optionalOpcodes),
    reservedOpcodes: asStringArray(item.reservedOpcodes),
    rules: asStringArray(item.rules)
  };
}

function mapStream(value: unknown): StreamDefinition {
  const item = asObject(value, "stream");
  const header = asObject(item.header, "stream.header");
  return {
    header: {
      name: String(header.name),
      size: normalizeId(header.size, "stream.header.size"),
      fields: asArray(header.fields).map((field) => ({
        name: String(field.name),
        type: String(field.type)
      }))
    },
    rules: asStringArray(item.rules)
  };
}

function mapCompatibility(value: unknown): CompatibilityDefinition {
  const item = asObject(value, "compatibility");
  return {
    legacySources: asStringArray(item.legacySources),
    rules: asStringArray(item.rules)
  };
}

export async function loadProtocolDefinition(specRoot: string): Promise<ProtocolModel> {
  const sourcePath = path.join(specRoot, "protocol", "axtp.protocol.yaml");
  let raw: any;
  try {
    raw = YAML.parse(await readFile(sourcePath, "utf8")) ?? {};
  } catch (error) {
    throw new GeneratorError({
      code: "AXTP-GEN-1001",
      file: sourcePath,
      message: error instanceof Error ? error.message : String(error)
    });
  }

  return loadProtocolDefinitionFromRaw(specRoot, sourcePath, raw);
}

export function loadProtocolDefinitionFromRaw(specRoot: string, sourcePath: string, raw: any): ProtocolModel {
  return {
    specRoot,
    sourcePath,
    protocol: mapProtocol(raw.protocol),
    overview: mapOverview(raw.overview),
    architecture: mapArchitecture(raw.architecture),
    guide: mapGuide(raw.guide),
    wire: mapWire(raw.wire),
    frameProfiles: mapFrameProfiles(raw.frameProfiles),
    transports: mapTransports(raw.transports),
    payloadTypes: mapPayloadTypes(raw.payloadTypes),
    control: mapControl(raw.control),
    stream: mapStream(raw.stream),
    compatibility: mapCompatibility(raw.compatibility),
    schemas: mapSchemas(raw),
    wireExamples: mapWireExamples(raw.wire_examples ?? raw.wireExamples),
    methods: mapMethods(raw.methods),
    events: mapEvents(raw.events),
    errors: mapErrors(raw.errors),
    profiles: mapProfiles(raw.profiles),
    raw
  };
}
