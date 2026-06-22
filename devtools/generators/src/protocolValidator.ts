import { GeneratorError } from "./errors.js";
import { buildProtocolDomainByHighByte, type DomainByHighByte } from "./domainRegistry.js";
import type { ErrorDefinition, EventDefinition, MethodDefinition, ProtocolModel, SchemaDefinition } from "./protocolModel.js";
import { hex } from "./util.js";

function fail(entry: string, field: string, message: string): never {
  throw new GeneratorError({
    code: "AXTP-GEN-1004",
    file: "protocol/axtp.protocol.yaml",
    entry,
    field,
    message
  });
}

function assertUnique<T>(items: T[], key: (item: T) => string | number, label: string, field: string): void {
  const seen = new Map<string | number, string>();
  for (const item of items as Array<T & { name: string }>) {
    const value = key(item);
    const existing = seen.get(value);
    if (existing) {
      fail(item.name, field, `duplicate ${label}: ${String(value)} (${existing} / ${item.name})`);
    }
    seen.set(value, item.name);
  }
}

function assertNoForbiddenKeys(value: unknown, path = "$"): void {
  if (!value || typeof value !== "object") return;
  if (Array.isArray(value)) {
    value.forEach((item, index) => assertNoForbiddenKeys(item, `${path}[${index}]`));
    return;
  }

  for (const [key, child] of Object.entries(value)) {
    if (key === "bitmapId" || key === "requests" || key === "requiredRequests") {
      fail(path, key, `forbidden legacy Protocol Definition field: ${key}`);
    }
    assertNoForbiddenKeys(child, `${path}.${key}`);
  }
}

function assertNoUnsupportedProfileKeys(value: unknown): void {
  const profiles = (value as any)?.profiles;
  if (!Array.isArray(profiles)) return;
  profiles.forEach((profile, index) => {
    if (profile && typeof profile === "object" && "requiredCapabilities" in profile) {
      fail(`profiles[${index}]`, "requiredCapabilities", "profiles[].requiredCapabilities is not defined by docs/specs/2-registry/05-Profiles-Registry.md");
    }
  });
}

function assertDomainBits<T extends { name: string; domain: string; bitOffset: number }>(items: T[], label: string): void {
  const domains = new Map<string, T[]>();
  for (const item of items) {
    const entries = domains.get(item.domain) ?? [];
    entries.push(item);
    domains.set(item.domain, entries);
  }
  for (const [domain, entries] of domains) {
    const seen = new Map<number, string>();
    for (const item of entries) {
      if (seen.has(item.bitOffset)) {
        fail(item.name, "bitOffset", `duplicate ${label} bitOffset in domain ${domain}: ${item.bitOffset}`);
      }
      seen.set(item.bitOffset, item.name);
    }
    const bits = [...seen.keys()].sort((a, b) => a - b);
    bits.forEach((bit, index) => {
      if (bit !== index) {
        fail(domain, "bitOffset", `${label} bitOffset must be contiguous from 0 in domain ${domain}: ${bits.join(",")}`);
      }
    });
  }
}

function assertDomainName(name: string, domain: string, label: string): void {
  if (name.split(".")[0] !== domain) fail(name, "domain", `${label} domain must match name prefix`);
}

function assertMethodReferences(methods: MethodDefinition[], typeNames: Set<string>, eventNames: Set<string>, errorNames: Set<string>, capabilityNames: Set<string>): void {
  for (const method of methods) {
    assertDomainName(method.name, method.domain, "method");
    if (!method.description || method.description.trim() === "") fail(method.name, "description", "method description is required by docs/specs/2-registry/02-Methods-Registry.md");
    if (!typeNames.has(method.request.type)) fail(method.name, "request.type", `missing type: ${method.request.type}`);
    if (!typeNames.has(method.response.type)) fail(method.name, "response.type", `missing type: ${method.response.type}`);
    for (const event of method.events) {
      if (!eventNames.has(event)) fail(method.name, "events", `missing event: ${event}`);
    }
    for (const error of method.errors) {
      if (!errorNames.has(error)) fail(method.name, "errors", `missing error: ${error}`);
    }
    for (const capability of method.capabilities) {
      if (!capabilityNames.has(capability)) fail(method.name, "capabilities", `missing capability: ${capability}`);
    }
  }
}

function assertEventReferences(events: EventDefinition[], typeNames: Set<string>, capabilityNames: Set<string>): void {
  for (const event of events) {
    assertDomainName(event.name, event.domain, "event");
    if (!typeNames.has(event.payload.type)) fail(event.name, "payload.type", `missing type: ${event.payload.type}`);
    for (const capability of event.capabilities) {
      if (!capabilityNames.has(capability)) fail(event.name, "capabilities", `missing capability: ${capability}`);
    }
  }
}

function assertCapabilityReferences(model: ProtocolModel, typeNames: Set<string>): void {
  for (const capability of model.capabilities) {
    assertDomainName(capability.name, capability.domain, "capability");
    if (capability.schema && !typeNames.has(capability.schema)) {
      fail(capability.name, "schema", `missing type: ${capability.schema}`);
    }
  }
}

function assertDomainIdAlignment(methods: MethodDefinition[], events: EventDefinition[]): void {
  const methodDomainIds = new Map<string, number>();
  for (const method of methods) {
    const domainId = method.methodId >> 8;
    const existing = methodDomainIds.get(method.domain);
    if (existing !== undefined && existing !== domainId) {
      fail(method.name, "methodId", `methodId high byte must be stable within domain ${method.domain}`);
    }
    methodDomainIds.set(method.domain, domainId);
  }
  for (const event of events) {
    const methodDomainId = methodDomainIds.get(event.domain);
    if (methodDomainId === undefined) continue;
    const expected = methodDomainId;
    const actual = event.eventId >> 8;
    if (actual !== expected) {
      fail(event.name, "eventId", `eventId high byte must align with domain ${event.domain}: expected ${hex(expected, 2)}`);
    }
  }
}

function assertSchemaDefinitions(schemas: SchemaDefinition[]): void {
  const allowedKinds = new Set(["object", "enum", "bitmap", "alias", "bytes"]);
  const builtins = new Set(["bool", "uint8", "uint16", "uint32", "uint64", "int8", "int16", "int32", "int64", "number", "string", "bytes", "enum", "bitmap", "array"]);
  const schemaNames = new Set(schemas.map((schema) => schema.name));
  for (const schema of schemas) {
    if (!allowedKinds.has(schema.kind)) fail(schema.name, "kind", `unsupported schema kind: ${schema.kind}`);
    if (schema.kind !== "object") continue;
    const fieldIds = new Map<number, string>();
    for (const field of schema.fields) {
      if (field.fieldId < 1 || field.fieldId > 0xff) fail(schema.name, "fieldId", `fieldId must be a 1-byte value: ${field.name}`);
      const existing = fieldIds.get(field.fieldId);
      if (existing) fail(schema.name, "fieldId", `duplicate fieldId ${hex(field.fieldId, 2)} (${existing} / ${field.name})`);
      fieldIds.set(field.fieldId, field.name);
      if (!builtins.has(field.type) && !schemaNames.has(field.type)) {
        fail(schema.name, "type", `field ${field.name} references missing schema: ${field.type}`);
      }
      if (field.schema && !schemaNames.has(field.schema)) {
        fail(schema.name, "schema", `field ${field.name} references missing schema: ${field.schema}`);
      }
      if (field.array?.itemSchema && !schemaNames.has(field.array.itemSchema)) {
        fail(schema.name, "array.itemSchema", `field ${field.name} references missing schema: ${field.array.itemSchema}`);
      }
      if (field.array?.itemType && !builtins.has(field.array.itemType) && !schemaNames.has(field.array.itemType)) {
        fail(schema.name, "array.itemType", `field ${field.name} references missing item type: ${field.array.itemType}`);
      }
    }
  }
}

function assertEmptySchemaUsage(model: ProtocolModel): void {
  const schemas = new Map(model.schemas.map((schema) => [schema.name, schema]));
  const empty = schemas.get("Empty");
  if (!empty || empty.kind !== "object" || empty.fields.length !== 0) {
    fail("schemas.Empty", "schema", "docs/specs/2-registry/02-Methods-Registry.md requires empty request/response to use Empty");
  }
  for (const method of model.methods) {
    const requestSchema = schemas.get(method.request.type);
    const responseSchema = schemas.get(method.response.type);
    if (requestSchema && requestSchema.fields.length === 0 && method.request.type !== "Empty") {
      fail(method.name, "request.type", "empty request must use Empty");
    }
    if (responseSchema && responseSchema.fields.length === 0 && method.response.type !== "Empty") {
      fail(method.name, "response.type", "empty response must use Empty");
    }
  }
}

function allowedErrorCategories(code: number, domainByHighByte: DomainByHighByte): string[] {
  if (code <= 0x00ff) return ["common", "frame", "control", "rpc"];
  const highByte = code >> 8;
  const domain = domainByHighByte.get(highByte);
  if (domain) return [domain];
  if (highByte >= 0x70 && highByte <= 0x7e) return ["vendor"];
  if (highByte === 0x7f) return ["legacy"];
  return [];
}

function assertErrorRanges(errors: ErrorDefinition[], domainByHighByte: DomainByHighByte): void {
  for (const error of errors) {
    const allowed = allowedErrorCategories(error.code, domainByHighByte);
    if (allowed.length === 0) fail(error.name, "code", `error code must be in a registered error range`);
    if (!allowed.includes(error.category)) {
      fail(error.name, "category", `error category must be ${allowed.join(" / ")} for code ${hex(error.code)}`);
    }
  }
}

function assertStreamHeader(model: ProtocolModel): void {
  const expected = [
    ["streamId", "uint32"],
    ["seqId", "uint32"],
    ["cursor", "uint64"]
  ];
  const fields = model.stream.header.fields;
  if (model.stream.header.size !== 16) {
    fail("stream.header", "size", "STREAM header size must be 16 bytes");
  }
  for (const forbidden of ["seq", "position", "chunkLength", "flags"]) {
    if (fields.some((field) => field.name === forbidden)) {
      fail("stream.header", "fields", `STREAM header must not contain legacy field: ${forbidden}`);
    }
  }
  if (fields.length !== expected.length) {
    fail("stream.header", "fields", "STREAM header must contain streamId:uint32, seqId:uint32 and cursor:uint64");
  }
  expected.forEach(([name, type], index) => {
    const field = fields[index];
    if (!field || field.name !== name || field.type !== type) {
      fail("stream.header", "fields", "STREAM header must be streamId:uint32, seqId:uint32, cursor:uint64");
    }
  });
}

function assertWireByteOrder(model: ProtocolModel): void {
  if (model.wire.byteOrder !== "big-endian") {
    fail("wire", "byteOrder", "AXTP v1 wire byte order must be big-endian");
  }
  if (model.wire.byteOrderAlias !== "network") {
    fail("wire", "byteOrderAlias", "AXTP v1 wire byte order alias must be network");
  }
  if (model.wire.crcByteOrder !== "big-endian") {
    fail("wire", "crcByteOrder", "AXTP v1 CRC fields must be serialized big-endian");
  }
  if (!/Big-Endian|big-endian/.test(model.wire.integerEncoding)) {
    fail("wire", "integerEncoding", "AXTP v1 integerEncoding must explicitly state Big-Endian");
  }
}

function assertControlOpcodes(model: ProtocolModel): void {
  for (const opcode of ["OPEN", "ACCEPT", "HEARTBEAT", "HEARTBEAT_ACK", "CLOSE", "CLOSE_ACK"]) {
    if (!model.control.requiredOpcodes.includes(opcode)) fail("control.requiredOpcodes", opcode, `${opcode} is required by docs/specs/1-core/05-Control-Session.md`);
  }
  for (const opcode of ["READY", "ACK", "NACK"]) {
    if (model.control.requiredOpcodes.includes(opcode)) fail("control.requiredOpcodes", opcode, `${opcode} is optional/future and must not be required`);
  }
  if (!model.control.optionalOpcodes.includes("READY")) fail("control.optionalOpcodes", "READY", "READY must be optional/reserved");
  for (const opcode of ["ACK", "NACK"]) {
    if (!model.control.optionalOpcodes.includes(opcode)) fail("control.optionalOpcodes", opcode, `${opcode} must be listed as optional/future, not required`);
  }
  for (const opcode of ["ACK", "NACK", "RESUME", "HEARTBEAT", "HEARTBEAT_ACK", "CLOSE", "CLOSE_ACK"]) {
    if (model.control.reservedOpcodes.includes(opcode)) fail("control.reservedOpcodes", opcode, `${opcode} is defined by docs/specs/1-core/05-Control-Session.md and must not be listed as reserved`);
  }
}

function assertCurrentTransportPolicy(model: ProtocolModel): void {
  for (const frameProfile of model.frameProfiles) {
    if (frameProfile.name.startsWith("COMPACT_")) {
      fail(frameProfile.name, "frameProfiles", "COMPACT_FRAME is documented only in the low-bandwidth degradation spec, not current Protocol IR");
    }
  }

  for (const transport of model.transports) {
    if (transport.name.includes("HID-64")) {
      fail(transport.name, "transports", "AXTP-HID-64 must not be exposed as a current v1 Core transport");
    }

    const rpcEncodings = transport.rpcEncodings ?? [];
    if (transport.frameProfile === "none") {
      if (transport.supportsControl !== false || transport.supportsStream !== false) {
        fail(transport.name, "transports", "WebSocket Unframed JSON transports must not support CONTROL or STREAM");
      }
      if (rpcEncodings.length !== 1 || rpcEncodings[0] !== "JSON") {
        fail(transport.name, "rpcEncodings", "WebSocket Unframed JSON transports must use rpcEncodings=[JSON]");
      }
      continue;
    }

    if (transport.frameProfile !== "STANDARD_FRAME") {
      fail(transport.name, "frameProfile", "current Standard Framed transports must use STANDARD_FRAME");
    }
    if (transport.supportsControl !== true || transport.supportsStream !== true) {
      fail(transport.name, "transports", "Standard Framed transports must support CONTROL and STREAM");
    }
    for (const encoding of ["JSON", "CBOR", "MSGPACK", "JSON_BINARY"]) {
      if (!rpcEncodings.includes(encoding)) {
        fail(transport.name, "rpcEncodings", `Standard Framed transports must declare rpcEncoding ${encoding}`);
      }
    }
  }

  for (const profile of model.profiles) {
    if (profile.transportProfiles.includes("AXTP-HID-64")) {
      fail(profile.name, "transportProfiles", "profiles must use AXTP-USB-HID instead of AXTP-HID-64");
    }
    if (profile.frameProfile?.startsWith("COMPACT_") || profile.frameProfiles.some((frameProfile) => frameProfile.startsWith("COMPACT_"))) {
      fail(profile.name, "frameProfile", "profiles must not reference COMPACT_FRAME in the current Protocol IR");
    }
  }
}

export function validateProtocolDefinition(model: ProtocolModel): string[] {
  const domainByHighByte = buildProtocolDomainByHighByte(model);

  assertNoForbiddenKeys(model.raw);
  assertNoUnsupportedProfileKeys(model.raw);
  assertWireByteOrder(model);
  assertStreamHeader(model);
  assertControlOpcodes(model);

  assertUnique(model.methods, (item) => item.name, "method name", "name");
  assertUnique(model.methods, (item) => item.methodId, "methodId", "methodId");
  assertUnique(model.events, (item) => item.name, "event name", "name");
  assertUnique(model.events, (item) => item.eventId, "eventId", "eventId");
  assertUnique(model.errors, (item) => item.name, "error name", "name");
  assertUnique(model.errors, (item) => item.code, "error code", "code");
  assertUnique(model.capabilities, (item) => item.name, "capability name", "name");
  assertUnique(model.capabilities, (item) => item.capabilityId, "capabilityId", "capabilityId");
  assertUnique(model.schemas, (item) => item.name, "schema name", "name");
  assertUnique(model.transports, (item) => item.name, "transport name", "name");
  assertUnique(model.profiles, (item) => item.name, "profile name", "name");

  assertDomainBits(model.methods, "method");
  assertDomainBits(model.events, "event");
  assertDomainIdAlignment(model.methods, model.events);
  assertSchemaDefinitions(model.schemas);
  assertEmptySchemaUsage(model);
  assertErrorRanges(model.errors, domainByHighByte);
  assertCurrentTransportPolicy(model);

  const typeNames = new Set(model.schemas.map((item) => item.name));
  const methodNames = new Set(model.methods.map((item) => item.name));
  const eventNames = new Set(model.events.map((item) => item.name));
  const errorNames = new Set(model.errors.map((item) => item.name));
  const capabilityNames = new Set(model.capabilities.map((item) => item.name));
  const transportNames = new Set(model.transports.map((item) => item.name));
  const frameProfileNames = new Set(model.frameProfiles.map((item) => item.name));

  assertCapabilityReferences(model, typeNames);
  assertMethodReferences(model.methods, typeNames, eventNames, errorNames, capabilityNames);
  assertEventReferences(model.events, typeNames, capabilityNames);

  const supportedMethodsResponse = model.schemas.find((item) => item.name === "CapabilitySupportedMethodsResponse");
  if (supportedMethodsResponse) {
    const methodMasks = supportedMethodsResponse.fields.find((field) => field.name === "methodMasks");
    if (!methodMasks || methodMasks.derivedFrom !== "methods[].bitOffset") {
      fail("CapabilitySupportedMethodsResponse", "methodMasks", "optional methodMasks must derive from methods[].bitOffset");
    }
  }

  for (const transport of model.transports) {
    if (transport.frameProfile !== "none" && !frameProfileNames.has(transport.frameProfile)) {
      fail(transport.name, "frameProfile", `missing frame profile: ${transport.frameProfile}`);
    }
  }

  for (const profile of model.profiles) {
    for (const method of profile.requiredMethods) {
      if (!methodNames.has(method)) fail(profile.name, "requiredMethods", `missing method: ${method}`);
    }
    for (const event of profile.requiredEvents) {
      if (!eventNames.has(event)) fail(profile.name, "requiredEvents", `missing event: ${event}`);
    }
    for (const error of profile.requiredErrors) {
      if (!errorNames.has(error)) fail(profile.name, "requiredErrors", `missing error: ${error}`);
    }
    for (const type of profile.requiredTypes) {
      if (!typeNames.has(type)) fail(profile.name, "requiredTypes", `missing type: ${type}`);
    }
    for (const transport of profile.transportProfiles) {
      if (!transportNames.has(transport)) fail(profile.name, "transportProfiles", `missing transport: ${transport}`);
    }
    const usedFrameProfiles = new Set(
      profile.transportProfiles
        .map((transportName) => model.transports.find((transport) => transport.name === transportName)?.frameProfile)
        .filter((frameProfile): frameProfile is string => Boolean(frameProfile) && frameProfile !== "none")
    );
    if (profile.frameProfile) {
      for (const frameProfile of usedFrameProfiles) {
        if (frameProfile !== profile.frameProfile) {
          fail(profile.name, "frameProfile", `frameProfile ${profile.frameProfile} does not match transport frame profile ${frameProfile}`);
        }
      }
    }
    for (const frameProfile of profile.frameProfiles) {
      if (!frameProfileNames.has(frameProfile)) fail(profile.name, "frameProfiles", `missing frame profile: ${frameProfile}`);
    }
    if (profile.frameProfiles.length > 0) {
      const declared = new Set(profile.frameProfiles);
      for (const frameProfile of usedFrameProfiles) {
        if (!declared.has(frameProfile)) fail(profile.name, "frameProfiles", `missing transport frame profile: ${frameProfile}`);
      }
      for (const frameProfile of declared) {
        if (!usedFrameProfiles.has(frameProfile)) fail(profile.name, "frameProfiles", `frame profile is not used by transportProfiles: ${frameProfile}`);
      }
    }
    if (profile.frameProfile && !frameProfileNames.has(profile.frameProfile)) {
      fail(profile.name, "frameProfile", `missing frame profile: ${profile.frameProfile}`);
    }
  }

  return [
    `[OK] protocol/axtp.protocol.yaml: ${model.methods.length} methods checked`,
    `[OK] protocol/axtp.protocol.yaml: ${model.events.length} events checked`,
    `[OK] protocol/axtp.protocol.yaml: ${model.errors.length} errors checked`,
    `[OK] protocol/axtp.protocol.yaml: ${model.capabilities.length} capabilities checked`,
    `[OK] protocol/axtp.protocol.yaml: ${model.schemas.length} schemas checked`,
    `[OK] protocol/axtp.protocol.yaml: ${model.profiles.length} profiles checked`
  ];
}
