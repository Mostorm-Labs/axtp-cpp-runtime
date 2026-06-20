import { readdir, readFile } from "node:fs/promises";
import { existsSync } from "node:fs";
import path from "node:path";
import YAML from "yaml";
import { GeneratorError } from "./errors.js";
import { loadSpec, resolveContractRoot } from "./loader.js";
import type { ProtocolSourceModel } from "./sourceModel.js";
import type { Capability, ErrorCode, Event, Field, LegacyMapping, Method, Schema } from "./models.js";
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

async function listYamlFiles(dir: string): Promise<string[]> {
  if (!existsSync(dir)) return [];
  const entries = await readdir(dir, { withFileTypes: true });
  const files: string[] = [];
  for (const entry of entries) {
    const fullPath = path.join(dir, entry.name);
    if (entry.isDirectory()) files.push(...await listYamlFiles(fullPath));
    if (entry.isFile() && /\.ya?ml$/i.test(entry.name)) files.push(fullPath);
  }
  return files.sort();
}

function asArray(value: unknown): any[] {
  return Array.isArray(value) ? value : [];
}

function mapMethod(item: any, file: string, defaultDomain: string): Method {
  return {
    id: normalizeId(item.id, `${file}:${item.name}`),
    bitOffset: normalizeId(item.bitOffset, `${file}:${item.name}.bitOffset`),
    name: String(item.name),
    domain: String(item.domain ?? defaultDomain),
    status: item.status ?? "draft",
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

function mapEvent(item: any, file: string, defaultDomain: string): Event {
  return {
    id: normalizeId(item.id, `${file}:${item.name}`),
    bitOffset: normalizeId(item.bitOffset, `${file}:${item.name}.bitOffset`),
    name: String(item.name),
    domain: String(item.domain ?? defaultDomain),
    status: item.status ?? "draft",
    description: item.description,
    since: item.since,
    deprecated: Boolean(item.deprecated),
    eventSchema: item.event_schema ?? item.eventSchema,
    severity: item.severity,
    trigger: asArray(item.trigger).map(String),
    capabilities: asArray(item.capabilities).map(String)
  };
}

function mapErrorCode(item: any, file: string, defaultDomain: string): ErrorCode {
  return {
    id: normalizeId(item.id, `${file}:${item.name}`),
    name: String(item.name),
    domain: String(item.domain ?? defaultDomain),
    status: item.status ?? "draft",
    description: item.description,
    since: item.since,
    deprecated: Boolean(item.deprecated),
    category: item.category,
    severity: item.severity,
    message: item.message,
    retryable: Boolean(item.retryable)
  };
}

function mapCapability(item: any, file: string, defaultDomain: string): Capability {
  return {
    id: normalizeId(item.id, `${file}:${item.name}`),
    name: String(item.name),
    domain: String(item.domain ?? defaultDomain),
    status: item.status ?? "draft",
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
    legacyCmdValue: normalizeId(item.legacy_cmd_value ?? item.legacyCmdValue, `${file}:${item.legacy_name ?? item.legacyName}`),
    legacyName: item.legacy_name ?? item.legacyName,
    axtpMethodId: normalizeId(item.axtp_method_id ?? item.axtpMethodId, `${file}:${item.legacy_name ?? item.legacyName}`),
    axtpMethodName: item.axtp_method_name ?? item.axtpMethodName,
    direction: item.direction,
    statusMapping
  };
}

function mapField(item: any, file: string, schemaName: string): Field {
  const array = item.array === undefined ? undefined : {
    itemType: item.array.item_type ?? item.array.itemType,
    itemSchema: item.array.item_schema ?? item.array.itemSchema
  };
  return {
    id: normalizeId(item.id ?? item.field_id ?? item.fieldId, `${file}:${schemaName}.${item.name}`),
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
    array,
    derivedFrom: item.derived_from ?? item.derivedFrom,
    description: item.description
  };
}

function mapSchemas(doc: any, file: string): Schema[] {
  const schemas = doc.types ?? doc.schemas ?? {};
  return Object.entries(schemas).map(([name, raw]: [string, any]) => ({
    name,
    type: raw.type ?? raw.kind ?? "object",
    description: raw.description,
    fields: asArray(raw.fields).map((field) => mapField(field, file, name))
  }));
}

export async function loadProtocolSources(specRoot: string): Promise<ProtocolSourceModel> {
  const contractRoot = resolveContractRoot(specRoot);
  const legacyDomainFiles = await listYamlFiles(path.join(contractRoot, "domains"));
  if (legacyDomainFiles.length > 0) {
    throw new GeneratorError({
      code: "AXTP-GEN-1004",
      file: "domains/",
      message: `top-level domains/ is deprecated; move YAML files to registry/domains/. Found: ${legacyDomainFiles.map((file) => path.relative(contractRoot, file)).join(", ")}`
    });
  }
  const spec = await loadSpec(specRoot);
  const protocolMetaPath = path.join(contractRoot, "registry", "core", "protocol_meta.yaml");
  const protocolMeta = await loadYamlFile(protocolMetaPath);
  const domainFiles = await listYamlFiles(path.join(contractRoot, "registry", "domains"));
  const sourceFiles = [protocolMetaPath, ...domainFiles];
  const profiles: Array<Record<string, unknown>> = [];

  for (const file of domainFiles) {
    const doc = await loadYamlFile(file);
    const defaultDomain = String(doc.domain ?? path.basename(path.dirname(file)));
    spec.methods.push(...asArray(doc.methods).map((item) => mapMethod(item, file, defaultDomain)));
    spec.events.push(...asArray(doc.events).map((item) => mapEvent(item, file, defaultDomain)));
    spec.errors.push(...asArray(doc.errors).map((item) => mapErrorCode(item, file, defaultDomain)));
    spec.capabilities.push(...asArray(doc.capabilities).map((item) => mapCapability(item, file, defaultDomain)));
    spec.legacyMappings.push(...asArray(doc.legacyMappings ?? doc.legacy_mappings).map((item) => mapLegacy(item, file)));
    spec.schemas.push(...mapSchemas(doc, file));
    profiles.push(...asArray(doc.profiles));
  }

  return { ...spec, protocolMeta, sourceFiles, profiles };
}
