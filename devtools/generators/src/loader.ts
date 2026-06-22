import { existsSync } from "node:fs";
import { readFile } from "node:fs/promises";
import path from "node:path";
import YAML from "yaml";
import { GeneratorError } from "./errors.js";
import type {
  Capability,
  CommonRegistryItem,
  DomainRange,
  ErrorCode,
  Event,
  Field,
  LegacyMapping,
  Method,
  Schema,
  SpecModel
} from "./models.js";
import { normalizeId } from "./util.js";

async function loadYamlFile(filePath: string): Promise<any> {
  try {
    const text = await readFile(filePath, "utf8");
    return YAML.parse(text) ?? {};
  } catch (error) {
    throw new GeneratorError({
      code: "AXTP-GEN-1001",
      file: filePath,
      message: error instanceof Error ? error.message : String(error)
    });
  }
}

async function loadOptionalYamlFile(filePath: string): Promise<any> {
  try {
    const text = await readFile(filePath, "utf8");
    return YAML.parse(text) ?? {};
  } catch (error: any) {
    if (error?.code === "ENOENT") return {};
    throw new GeneratorError({
      code: "AXTP-GEN-1001",
      file: filePath,
      message: error instanceof Error ? error.message : String(error)
    });
  }
}

export function resolveContractRoot(specRoot: string): string {
  if (existsSync(path.join(specRoot, "registry"))) return specRoot;
  if (existsSync(path.join(specRoot, "contract", "registry"))) {
    return path.join(specRoot, "contract");
  }
  return specRoot;
}

function resolveGeneratorConfigPath(specRoot: string, contractRoot: string): string {
  const candidates = [
    path.join(specRoot, "tooling", "generators", "generator.yaml"),
    path.join(specRoot, "generators", "generator.yaml"),
    path.join(contractRoot, "generators", "generator.yaml"),
    path.join(contractRoot, "generator.yaml"),
    path.join(specRoot, "generator.yaml")
  ];
  return candidates.find((candidate) => existsSync(candidate)) ?? candidates[candidates.length - 1];
}

function asArray(value: unknown): any[] {
  return Array.isArray(value) ? value : [];
}

function mapCommon(items: any[], file: string): CommonRegistryItem[] {
  return items.map((item) => ({
    id: normalizeId(item.id ?? item.value, `${file}:${item.name}`),
    value: normalizeId(item.id ?? item.value, `${file}:${item.name}`),
    name: String(item.name),
    domain: String(item.domain ?? "protocol"),
    status: item.status ?? "mvp",
    description: item.description,
    since: item.since,
    deprecated: Boolean(item.deprecated)
  }));
}

function mapDomainRanges(items: any[], file: string): DomainRange[] {
  return items.map((item) => ({
    highByte: normalizeId(item.high_byte ?? item.highByte, `${file}:${item.domain}`),
    domain: String(item.domain),
    status: item.status ?? "stable",
    description: item.description,
    since: item.since,
    deprecated: Boolean(item.deprecated)
  }));
}

function mapMethod(item: any, file: string): Method {
  return {
    id: normalizeId(item.id, `${file}:${item.name}`),
    bitOffset: normalizeId(item.bitOffset, `${file}:${item.name}.bitOffset`),
    name: String(item.name),
    domain: String(item.domain),
    status: item.status ?? "mvp",
    description: item.description,
    since: item.since,
    deprecated: Boolean(item.deprecated),
    rpcOp: item.rpc_op ?? item.rpcOp ?? "request_response",
    requestSchema: item.request_schema ?? item.requestSchema,
    responseSchema: item.response_schema ?? item.responseSchema,
    recommendedEncoding: asArray(item.recommended_encoding ?? item.recommendedEncoding).map(String),
    capabilities: asArray(item.capabilities).map(String),
    events: asArray(item.events).map(String),
    errors: asArray(item.errors).map(String),
    legacy: item.legacy
  };
}

function mapEvent(item: any, file: string): Event {
  return {
    id: normalizeId(item.id, `${file}:${item.name}`),
    bitOffset: normalizeId(item.bitOffset, `${file}:${item.name}.bitOffset`),
    name: String(item.name),
    domain: String(item.domain),
    status: item.status ?? "mvp",
    description: item.description,
    since: item.since,
    deprecated: Boolean(item.deprecated),
    eventSchema: item.event_schema ?? item.eventSchema,
    severity: item.severity,
    trigger: asArray(item.trigger).map(String),
    capabilities: asArray(item.capabilities).map(String)
  };
}

function mapErrorCode(item: any, file: string): ErrorCode {
  return {
    id: normalizeId(item.id, `${file}:${item.name}`),
    name: String(item.name),
    domain: String(item.domain),
    status: item.status ?? "mvp",
    description: item.description,
    since: item.since,
    deprecated: Boolean(item.deprecated),
    category: item.category,
    severity: item.severity,
    message: item.message,
    retryable: Boolean(item.retryable)
  };
}

function mapCapability(item: any, file: string): Capability {
  return {
    id: normalizeId(item.id, `${file}:${item.name}`),
    name: String(item.name),
    domain: String(item.domain),
    status: item.status ?? "mvp",
    description: item.description,
    since: item.since,
    deprecated: Boolean(item.deprecated),
    type: item.type,
    schema: item.schema
  };
}

function mapLegacy(item: any, file: string): LegacyMapping {
  const rawStatus = item.status_mapping ?? item.statusMapping ?? {};
  const statusMapping: Record<string, string> = {};
  for (const [key, value] of Object.entries(rawStatus)) {
    statusMapping[String(key)] = String(value);
  }
  return {
    legacyProtocol: item.legacy_protocol ?? item.legacyProtocol,
    legacyCmdValue: normalizeId(item.legacy_cmd_value ?? item.legacyCmdValue, `${file}:${item.legacy_name}`),
    legacyName: item.legacy_name ?? item.legacyName,
    axtpMethodId: normalizeId(item.axtp_method_id ?? item.axtpMethodId, `${file}:${item.legacy_name}`),
    axtpMethodName: item.axtp_method_name ?? item.axtpMethodName,
    direction: item.direction,
    statusMapping
  };
}

function mapField(item: any, file: string, schemaName: string): Field {
  return {
    id: normalizeId(item.id, `${file}:${schemaName}.${item.name}`),
    name: String(item.name),
    type: String(item.type),
    required: Boolean(item.required),
    deprecated: Boolean(item.deprecated),
    min: item.min,
    max: item.max,
    maxLength: item.max_length ?? item.maxLength,
    default: item.default,
    schema: item.schema,
    enum: item.enum,
    repeated: item.repeated,
    derivedFrom: item.derived_from ?? item.derivedFrom,
    description: item.description
  };
}

function mapSchemas(doc: any, file: string): Schema[] {
  const schemas = doc.schemas ?? {};
  return Object.entries(schemas).map(([name, raw]: [string, any]) => ({
    name,
    type: raw.type ?? "object",
    description: raw.description,
    fields: asArray(raw.fields).map((field) => mapField(field, file, name))
  }));
}

export async function loadSpec(specRoot: string): Promise<SpecModel> {
  const contractRoot = resolveContractRoot(specRoot);
  const registryDir = path.join(contractRoot, "registry");
  const schemaDir = path.join(registryDir, "schema");
  const configPath = resolveGeneratorConfigPath(specRoot, contractRoot);
  const config = await loadYamlFile(configPath);
  const version = await loadYamlFile(path.join(registryDir, "version.yaml"));

  const [
    payloadTypeDoc,
    controlOpcodeDoc,
    rpcEncodingDoc,
    rpcBodyEncodingDoc,
    rpcOpDoc,
    streamProfileDoc,
    domainRegistryDoc,
    methodDoc,
    eventDoc,
    errorDoc,
    capabilityDoc,
    legacyDoc,
    mvpDoc,
    commonSchemaDoc,
    controlSchemaDoc,
    deviceSchemaDoc,
    capabilitySchemaDoc,
    displaySchemaDoc,
    firmwareSchemaDoc,
    streamSchemaDoc,
    eventSchemaDoc,
    sessionSchemaDoc
  ] = await Promise.all([
    loadYamlFile(path.join(registryDir, "core", "payload_type.yaml")),
    loadYamlFile(path.join(registryDir, "core", "control_opcode.yaml")),
    loadYamlFile(path.join(registryDir, "core", "rpc_encoding.yaml")),
    loadYamlFile(path.join(registryDir, "core", "rpc_body_encoding.yaml")),
    loadYamlFile(path.join(registryDir, "core", "rpc_op.yaml")),
    loadOptionalYamlFile(path.join(registryDir, "core", "stream_profile.yaml")),
    loadOptionalYamlFile(path.join(registryDir, "core", "domain_registry.yaml")),
    loadOptionalYamlFile(path.join(registryDir, "method", "method_registry.yaml")),
    loadOptionalYamlFile(path.join(registryDir, "event", "event_registry.yaml")),
    loadYamlFile(path.join(registryDir, "error", "error_code.yaml")),
    loadYamlFile(path.join(registryDir, "capability", "capability_registry.yaml")),
    loadOptionalYamlFile(path.join(registryDir, "legacy", "legacy_mapping.yaml")),
    loadYamlFile(path.join(registryDir, "capability", "mvp_profile.yaml")),
    loadYamlFile(path.join(schemaDir, "common_fields.yaml")),
    loadYamlFile(path.join(schemaDir, "control_schema.yaml")),
    loadOptionalYamlFile(path.join(schemaDir, "device_schema.yaml")),
    loadOptionalYamlFile(path.join(schemaDir, "capability_schema.yaml")),
    loadOptionalYamlFile(path.join(schemaDir, "display_schema.yaml")),
    loadOptionalYamlFile(path.join(schemaDir, "firmware_schema.yaml")),
    loadOptionalYamlFile(path.join(schemaDir, "stream_schema.yaml")),
    loadYamlFile(path.join(schemaDir, "event_schema.yaml")),
    loadYamlFile(path.join(schemaDir, "session_schema.yaml"))
  ]);

  return {
    specRoot,
    config,
    version,
    payloadTypes: mapCommon(asArray(payloadTypeDoc.payload_types), "payload_type.yaml"),
    controlOpcodes: mapCommon(asArray(controlOpcodeDoc.control_opcodes), "control_opcode.yaml"),
    rpcEncodings: mapCommon(asArray(rpcEncodingDoc.rpc_encodings), "rpc_encoding.yaml"),
    rpcBodyEncodings: mapCommon(asArray(rpcBodyEncodingDoc.rpc_body_encodings), "rpc_body_encoding.yaml"),
    rpcOps: mapCommon(asArray(rpcOpDoc.rpc_ops), "rpc_op.yaml"),
    streamProfiles: mapCommon(asArray(streamProfileDoc.stream_profiles), "stream_profile.yaml"),
    domainRegistry: mapDomainRanges(asArray(domainRegistryDoc.domains), "domain_registry.yaml"),
    methods: asArray(methodDoc.methods).map((item) => mapMethod(item, "method_registry.yaml")),
    events: asArray(eventDoc.events).map((item) => mapEvent(item, "event_registry.yaml")),
    errors: asArray(errorDoc.errors).map((item) => mapErrorCode(item, "error_code.yaml")),
    capabilities: asArray(capabilityDoc.capabilities).map((item) => mapCapability(item, "capability_registry.yaml")),
    legacyMappings: asArray(legacyDoc.legacy_mappings).map((item) => mapLegacy(item, "legacy_mapping.yaml")),
    schemas: [
      ...mapSchemas(commonSchemaDoc, "common_fields.yaml"),
      ...mapSchemas(controlSchemaDoc, "control_schema.yaml"),
      ...mapSchemas(deviceSchemaDoc, "device_schema.yaml"),
      ...mapSchemas(capabilitySchemaDoc, "capability_schema.yaml"),
      ...mapSchemas(displaySchemaDoc, "display_schema.yaml"),
      ...mapSchemas(firmwareSchemaDoc, "firmware_schema.yaml"),
      ...mapSchemas(streamSchemaDoc, "stream_schema.yaml"),
      ...mapSchemas(eventSchemaDoc, "event_schema.yaml"),
      ...mapSchemas(sessionSchemaDoc, "session_schema.yaml")
    ],
    mvpProfile: {
      methods: asArray(mvpDoc.mvp?.methods).map(String),
      events: asArray(mvpDoc.mvp?.events).map(String),
      errors: asArray(mvpDoc.mvp?.errors).map(String),
      capabilities: asArray(mvpDoc.mvp?.capabilities).map(String)
    }
  };
}
