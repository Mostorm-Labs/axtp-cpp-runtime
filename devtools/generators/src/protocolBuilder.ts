import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import YAML from "yaml";
import { buildSourceDomainByHighByte, type DomainByHighByte } from "./domainRegistry.js";
import type { Capability, ErrorCode, Event, Field, Method, Schema } from "./models.js";
import type { ProtocolModel, TypeDefinition, TypeField } from "./protocolModel.js";
import { loadProtocolDefinitionFromRaw } from "./protocolLoader.js";
import type { ProtocolSourceModel } from "./sourceModel.js";

function protocolStatus(status: string): string {
  return status === "mvp" ? "stable" : status;
}

function emptySchemaNames(schemas: Schema[]): Set<string> {
  return new Set(schemas.filter((schema) => schema.fields.length === 0).map((schema) => schema.name));
}

function schemaRef(name: string | undefined, emptyNames: Set<string>): string {
  if (!name || emptyNames.has(name)) return "Empty";
  return name;
}

function fieldToTypeField(field: Field): TypeField {
  const itemType = field.array?.itemType ?? field.array?.itemSchema ?? field.schema;
  const itemSchema = field.array?.itemSchema ?? field.schema;
  const enumValues = Array.isArray(field.enum)
    ? field.enum.map(String)
    : field.enum === undefined ? undefined : [String(field.enum)];
  return {
    fieldId: field.id,
    name: field.name,
    type: field.type,
    required: field.required,
    min: typeof field.min === "number" ? field.min : undefined,
    max: typeof field.max === "number" ? field.max : undefined,
    maxLength: field.maxLength,
    default: field.default,
    deprecated: field.deprecated || undefined,
    derivedFrom: field.derivedFrom,
    schema: field.schema,
    enumValues,
    repeated: field.repeated,
    array: field.type === "array" || field.repeated ? {
      itemType,
      itemSchema
    } : undefined,
    description: field.description
  };
}

function buildSchemas(schemas: Schema[]): Record<string, Omit<TypeDefinition, "name">> {
  const result: Record<string, Omit<TypeDefinition, "name">> = {
    Empty: { kind: "object", fields: [] }
  };
  for (const schema of schemas) {
    if (schema.fields.length === 0) continue;
    result[schema.name] = {
      kind: schema.type,
      description: schema.description,
      fields: schema.fields.map(fieldToTypeField)
    };
  }
  return result;
}

function methodToProtocol(method: Method, emptyNames: Set<string>): Record<string, unknown> {
  const legacy = method.legacy ? {
    cmdValue: method.legacy.cmd_value ?? method.legacy.cmdValue,
    name: method.legacy.name,
    payloadFormat: method.legacy.payload_format ?? method.legacy.payloadFormat
  } : undefined;
  return {
    name: method.name,
    description: method.description,
    methodId: method.id,
    bitOffset: method.bitOffset,
    domain: method.domain,
    since: method.since ?? "1.0.0",
    status: protocolStatus(method.status),
    request: { type: schemaRef(method.requestSchema, emptyNames) },
    response: { type: schemaRef(method.responseSchema, emptyNames) },
    encodings: method.recommendedEncoding,
    capabilities: method.capabilities,
    events: method.events,
    errors: method.errors,
    ...(legacy?.cmdValue ? { legacy } : {})
  };
}

function eventToProtocol(event: Event, emptyNames: Set<string>): Record<string, unknown> {
  return {
    name: event.name,
    description: event.description,
    eventId: event.id,
    bitOffset: event.bitOffset,
    domain: event.domain,
    since: event.since ?? "1.0.0",
    status: protocolStatus(event.status),
    payload: { type: schemaRef(event.eventSchema, emptyNames) },
    severity: event.severity,
    trigger: event.trigger,
    capabilities: event.capabilities
  };
}

function errorCategory(code: number, domainByHighByte: DomainByHighByte): string {
  if (code <= 0x00ff) return "common";
  const highByte = code >> 8;
  const domain = domainByHighByte.get(highByte);
  if (domain) return domain;
  if (highByte >= 0x70 && highByte <= 0x7e) return "vendor";
  if (highByte === 0x7f) return "legacy";
  return "reserved";
}

function errorSeverity(error: ErrorCode): string {
  if (error.name === "SUCCESS") return "info";
  return error.retryable ? "warning" : "error";
}

function errorToProtocol(error: ErrorCode, domainByHighByte: DomainByHighByte): Record<string, unknown> {
  return {
    name: error.name,
    code: error.id,
    category: error.category ?? error.domain ?? errorCategory(error.id, domainByHighByte),
    since: error.since ?? "1.0.0",
    status: protocolStatus(error.status),
    severity: error.severity ?? errorSeverity(error),
    retryable: error.retryable,
    message: error.message ?? error.description ?? error.name
  };
}

function capabilityToProtocol(capability: Capability): Record<string, unknown> {
  return {
    name: capability.name,
    description: capability.description,
    capabilityId: capability.id,
    domain: capability.domain,
    since: capability.since ?? "1.0.0",
    status: protocolStatus(capability.status),
    type: capability.type,
    schema: capability.schema
  };
}

function defaultProfiles(source: ProtocolSourceModel): Array<Record<string, unknown>> {
  const requiredMethods = source.mvpProfile.methods;
  const requiredEvents = source.mvpProfile.events;
  const requiredErrors = source.mvpProfile.errors;
  return [
    {
      name: "AXTP-MVP",
      since: "1.0.0",
      status: "stable",
      requiredMethods,
      requiredEvents,
      requiredErrors,
      transportProfiles: ["AXTP-USB-HID", "AXTP-TCP", "AXTP-WS-JSON", "AXTP-WS-CLOUD-REVERSE"],
      frameProfiles: ["STANDARD_FRAME"]
    },
    {
      name: "AXTP-MVP-HID",
      since: "1.0.0",
      status: "stable",
      extends: "AXTP-MVP",
      requiredMethods,
      requiredEvents,
      requiredErrors,
      transportProfiles: ["AXTP-USB-HID"],
      frameProfile: "STANDARD_FRAME"
    }
  ];
}

export function buildProtocolDefinitionRaw(source: ProtocolSourceModel): Record<string, unknown> {
  const emptyNames = emptySchemaNames(source.schemas);
  const domainByHighByte = buildSourceDomainByHighByte(source);
  return {
    ...source.protocolMeta,
    schemas: buildSchemas(source.schemas),
    methods: source.methods.map((method) => methodToProtocol(method, emptyNames)),
    events: source.events.map((event) => eventToProtocol(event, emptyNames)),
    errors: source.errors.map((error) => errorToProtocol(error, domainByHighByte)),
    capabilities: source.capabilities.map(capabilityToProtocol),
    profiles: [...defaultProfiles(source), ...source.profiles]
  };
}

export function buildProtocolDefinition(source: ProtocolSourceModel): ProtocolModel {
  const raw = buildProtocolDefinitionRaw(source);
  return loadProtocolDefinitionFromRaw(source.specRoot, path.join(source.specRoot, "protocol", "axtp.protocol.yaml"), raw);
}

export async function writeProtocolDefinition(raw: Record<string, unknown>, outFile: string): Promise<void> {
  await mkdir(path.dirname(outFile), { recursive: true });
  const text = `# @generated by axtp-gen build-protocol. Do not edit manually.\n${YAML.stringify(raw, { lineWidth: 120 })}`;
  await writeFile(outFile, text, "utf8");
}
