import { GeneratorError } from "./errors.js";
import { buildSourceDomainByHighByte, type DomainByHighByte } from "./domainRegistry.js";
import type { Capability, ErrorCode, Event, Method, Schema, SpecModel } from "./models.js";
import { hex } from "./util.js";

function assertUniqueIds<T extends { id: number; name: string }>(items: T[], label: string, file: string): void {
  const seen = new Map<number, string>();
  for (const item of items) {
    const existing = seen.get(item.id);
    if (existing) {
      throw new GeneratorError({
        code: "AXTP-GEN-1002",
        file,
        entry: item.name,
        field: "id",
        message: `duplicate ${label} ${hex(item.id)}: ${existing} / ${item.name}`
      });
    }
    seen.set(item.id, item.name);
  }
}

function assertUniqueNames<T extends { name: string }>(items: T[], label: string, file: string): void {
  const seen = new Set<string>();
  for (const item of items) {
    if (seen.has(item.name)) {
      throw new GeneratorError({
        code: "AXTP-GEN-1002",
        file,
        entry: item.name,
        field: "name",
        message: `duplicate ${label} name: ${item.name}`
      });
    }
    seen.add(item.name);
  }
}

function assertSchemaFields(schema: Schema): void {
  const ids = new Map<number, string>();
  const names = new Set<string>();
  for (const field of schema.fields) {
    const existing = ids.get(field.id);
    if (existing) {
      throw new GeneratorError({
        code: "AXTP-GEN-1002",
        file: "schema/*.yaml",
        entry: schema.name,
        field: field.name,
        message: `duplicate fieldId ${hex(field.id, 2)}: ${existing} / ${field.name}`
      });
    }
    if (names.has(field.name)) {
      throw new GeneratorError({
        code: "AXTP-GEN-1002",
        file: "schema/*.yaml",
        entry: schema.name,
        field: field.name,
        message: `duplicate field name: ${field.name}`
      });
    }
    ids.set(field.id, field.name);
    names.add(field.name);
  }
}

function assertDomainBitOffsets<T extends { name: string; domain: string; bitOffset: number }>(items: T[], label: string, file: string): void {
  const byDomain = new Map<string, T[]>();
  for (const item of items) {
    if (!Number.isInteger(item.bitOffset) || item.bitOffset < 0) {
      throw new GeneratorError({
        code: "AXTP-GEN-1004",
        file,
        entry: item.name,
        field: "bitOffset",
        message: `${label} bitOffset is required and must be a non-negative integer`
      });
    }
    const list = byDomain.get(item.domain) ?? [];
    list.push(item);
    byDomain.set(item.domain, list);
  }
  for (const [domain, itemsInDomain] of byDomain) {
    const seen = new Map<number, string>();
    for (const item of itemsInDomain) {
      const existing = seen.get(item.bitOffset);
      if (existing) {
        throw new GeneratorError({
          code: "AXTP-GEN-1002",
          file,
          entry: item.name,
          field: "bitOffset",
          message: `duplicate ${label} bitOffset in domain ${domain}: ${item.bitOffset} (${existing} / ${item.name})`
        });
      }
      seen.set(item.bitOffset, item.name);
    }
    const bits = [...seen.keys()].sort((a, b) => a - b);
    bits.forEach((bit, index) => {
      if (bit !== index) {
        throw new GeneratorError({
          code: "AXTP-GEN-1004",
          file,
          entry: domain,
          field: "bitOffset",
          message: `${label} bitOffset must be contiguous from 0 in domain ${domain}: ${bits.join(",")}`
        });
      }
    });
  }
}

function assertRegistryDomainRanges<T extends { id: number; name: string; domain: string }>(
  items: T[],
  label: string,
  file: string,
  zeroByteDomains: string[],
  domainByHighByte: DomainByHighByte
): void {
  const zeroByte = new Set(zeroByteDomains);
  for (const item of items) {
    const highByte = item.id >> 8;
    const expected = domainByHighByte.get(highByte);
    if (expected) {
      if (item.domain !== expected) {
        throw new GeneratorError({
          code: "AXTP-GEN-1004",
          file,
          entry: item.name,
          field: "id",
          message: `${label} high byte must match Domain Registry: ${hex(highByte, 2)} = ${expected}`
        });
      }
      continue;
    }
    if (highByte === 0x00 && zeroByte.has(item.domain)) continue;
    if (highByte >= 0x70 && highByte <= 0x7e && item.domain === "vendor") continue;
    if (highByte === 0x7f && item.domain === "legacy") continue;
    throw new GeneratorError({
      code: "AXTP-GEN-1004",
      file,
      entry: item.name,
      field: "id",
      message: `${label} id ${hex(item.id)} is outside the Domain Registry ranges`
    });
  }
}

function assertKnownSchema(schemaName: string | undefined, schemas: Set<string>, entry: string, field: string): void {
  if (!schemaName) return;
  if (!schemas.has(schemaName)) {
    throw new GeneratorError({
      code: "AXTP-GEN-1004",
      file: "registry/*.yaml",
      entry,
      field,
      message: `missing schema: ${schemaName}`
    });
  }
}

function assertKnownFieldItemType(itemType: string | undefined, schemas: Set<string>, entry: string, field: string): void {
  if (!itemType) return;
  const builtins = new Set(["bool", "uint8", "uint16", "uint32", "uint64", "int8", "int16", "int32", "int64", "number", "string", "bytes", "enum", "bitmap", "array"]);
  if (builtins.has(itemType) || schemas.has(itemType)) return;
  throw new GeneratorError({
    code: "AXTP-GEN-1004",
    file: "registry/*.yaml",
    entry,
    field,
    message: `missing array item type: ${itemType}`
  });
}

function assertReservedReferences(spec: SpecModel): void {
  const capabilityByName = new Map(spec.capabilities.map((item) => [item.name, item]));
  const eventByName = new Map(spec.events.map((item) => [item.name, item]));
  const errorByName = new Map(spec.errors.map((item) => [item.name, item]));

  for (const method of spec.methods) {
    for (const capability of method.capabilities) {
      const item = capabilityByName.get(capability);
      if (item?.status === "reserved") {
        throw new GeneratorError({
          code: "AXTP-GEN-1005",
          file: "method_registry.yaml",
          entry: method.name,
          field: "capabilities",
          message: `method references reserved capability: ${capability}`
        });
      }
    }
    for (const event of method.events) {
      const item = eventByName.get(event);
      if (item?.status === "reserved") {
        throw new GeneratorError({
          code: "AXTP-GEN-1005",
          file: "method_registry.yaml",
          entry: method.name,
          field: "events",
          message: `method references reserved event: ${event}`
        });
      }
    }
    for (const error of method.errors) {
      const item = errorByName.get(error);
      if (item?.status === "reserved") {
        throw new GeneratorError({
          code: "AXTP-GEN-1005",
          file: "method_registry.yaml",
          entry: method.name,
          field: "errors",
          message: `method references reserved error: ${error}`
        });
      }
    }
  }
}

function assertMvpItems<T extends { name: string }>(actual: T[], required: string[], label: string): void {
  const names = new Set(actual.map((item) => item.name));
  for (const item of required) {
    if (!names.has(item)) {
      throw new GeneratorError({
        code: "AXTP-GEN-1006",
        file: "registry/mvp_profile.yaml",
        entry: item,
        field: label,
        message: `missing ${label}: ${item}`
      });
    }
  }
}

function assertLegacyMappings(spec: SpecModel): void {
  const methodById = new Map<number, Method>(spec.methods.map((item) => [item.id, item]));
  const errorByName = new Map<string, ErrorCode>(spec.errors.map((item) => [item.name, item]));
  const seen = new Map<string, string>();

  for (const item of spec.legacyMappings) {
    const key = `${item.legacyProtocol}:${item.legacyCmdValue}`;
    const existing = seen.get(key);
    if (existing) {
      throw new GeneratorError({
        code: "AXTP-GEN-1008",
        file: "legacy_mapping.yaml",
        entry: item.legacyName,
        field: "legacy_cmd_value",
        message: `duplicate legacy command ${key}: ${existing} / ${item.legacyName}`
      });
    }
    seen.set(key, item.legacyName);

    if (!methodById.has(item.axtpMethodId)) {
      throw new GeneratorError({
        code: "AXTP-GEN-1008",
        file: "legacy_mapping.yaml",
        entry: item.legacyName,
        field: "axtp_method_id",
        message: `legacy mapping target method does not exist: ${hex(item.axtpMethodId)}`
      });
    }
    for (const error of Object.values(item.statusMapping)) {
      if (!errorByName.has(error)) {
        throw new GeneratorError({
          code: "AXTP-GEN-1008",
          file: "legacy_mapping.yaml",
          entry: item.legacyName,
          field: "status_mapping",
          message: `legacy status maps to unknown errorCode: ${error}`
        });
      }
    }
  }
}

export function validateSpec(spec: SpecModel): string[] {
  const domainByHighByte = buildSourceDomainByHighByte(spec);

  assertUniqueIds(spec.methods, "methodId", "method_registry.yaml");
  assertUniqueNames(spec.methods, "method", "method_registry.yaml");
  assertUniqueIds(spec.events, "eventId", "event_registry.yaml");
  assertUniqueNames(spec.events, "event", "event_registry.yaml");
  assertRegistryDomainRanges(spec.methods, "methodId", "method_registry.yaml", [], domainByHighByte);
  assertRegistryDomainRanges(spec.events, "eventId", "event_registry.yaml", [], domainByHighByte);
  assertDomainBitOffsets(spec.methods, "method", "method_registry.yaml");
  assertDomainBitOffsets(spec.events, "event", "event_registry.yaml");
  assertUniqueIds(spec.errors, "errorCode", "error_code.yaml");
  assertUniqueNames(spec.errors, "error", "error_code.yaml");
  assertUniqueIds(spec.capabilities, "capabilityId", "capability_registry.yaml");
  assertUniqueNames(spec.capabilities, "capability", "capability_registry.yaml");
  assertRegistryDomainRanges(spec.errors, "errorCode", "error_code.yaml", ["common", "frame", "control", "rpc"], domainByHighByte);
  assertRegistryDomainRanges(spec.capabilities, "capabilityId", "capability_registry.yaml", ["protocol", "common", "reserved"], domainByHighByte);
  assertUniqueNames(spec.schemas, "schema", "schema/*.yaml");

  for (const schema of spec.schemas) assertSchemaFields(schema);

  const schemaNames = new Set(spec.schemas.map((item) => item.name));
  for (const method of spec.methods) {
    assertKnownSchema(method.requestSchema, schemaNames, method.name, "request_schema");
    assertKnownSchema(method.responseSchema, schemaNames, method.name, "response_schema");
  }
  for (const event of spec.events) {
    assertKnownSchema(event.eventSchema, schemaNames, event.name, "event_schema");
  }
  for (const capability of spec.capabilities) {
    assertKnownSchema(capability.schema, schemaNames, capability.name, "schema");
  }
  for (const schema of spec.schemas) {
    for (const field of schema.fields) {
      if (field.schema) assertKnownSchema(field.schema, schemaNames, schema.name, field.name);
      if (field.array?.itemSchema) assertKnownSchema(field.array.itemSchema, schemaNames, schema.name, `${field.name}.array.itemSchema`);
      if (field.array?.itemType) assertKnownFieldItemType(field.array.itemType, schemaNames, schema.name, `${field.name}.array.itemType`);
      if (field.type === "array" && !field.schema && !field.array?.itemType && !field.array?.itemSchema) {
        throw new GeneratorError({
          code: "AXTP-GEN-1004",
          file: "registry/*.yaml",
          entry: schema.name,
          field: field.name,
          message: "array field must declare schema, array.item_type, or array.item_schema"
        });
      }
    }
  }

  assertReservedReferences(spec);
  assertMvpItems(spec.methods, spec.mvpProfile.methods, "method");
  assertMvpItems(spec.events, spec.mvpProfile.events, "event");
  assertMvpItems(spec.errors, spec.mvpProfile.errors, "error");
  assertMvpItems(spec.capabilities, spec.mvpProfile.capabilities, "capability");
  assertLegacyMappings(spec);

  return [
    `[OK] method_registry.yaml: ${spec.methods.length} methods checked`,
    `[OK] event_registry.yaml: ${spec.events.length} events checked`,
    `[OK] error_code.yaml: ${spec.errors.length} errors checked`,
    `[OK] capability_registry.yaml: ${spec.capabilities.length} capabilities checked`,
    `[OK] schema: ${spec.schemas.length} schemas checked`,
    `[OK] legacy_mapping.yaml: ${spec.legacyMappings.length} mappings checked`
  ];
}
