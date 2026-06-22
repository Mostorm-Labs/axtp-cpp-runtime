import { GeneratorError } from "./errors.js";
import type { DomainRange, ErrorCode, Event, Method } from "./models.js";
import type { ErrorDefinition, EventDefinition, MethodDefinition } from "./protocolModel.js";
import { hex } from "./util.js";

export type DomainByHighByte = Map<number, string>;

interface DomainCandidate {
  highByte: number;
  domain: string;
  name: string;
  file: string;
}

function isRegistryHighByte(highByte: number): boolean {
  return highByte > 0x00 && highByte < 0x70;
}

function addDomain(mapping: DomainByHighByte, candidate: DomainCandidate): void {
  if (!Number.isInteger(candidate.highByte) || candidate.highByte < 0x00 || candidate.highByte > 0xff) {
    throw new GeneratorError({
      code: "AXTP-GEN-1004",
      file: candidate.file,
      entry: candidate.name,
      field: "highByte",
      message: `domain high byte must be a uint8 value: ${candidate.highByte}`
    });
  }

  const existing = mapping.get(candidate.highByte);
  if (existing && existing !== candidate.domain) {
    throw new GeneratorError({
      code: "AXTP-GEN-1004",
      file: candidate.file,
      entry: candidate.name,
      field: "domain",
      message: `domain high byte ${hex(candidate.highByte, 2)} is already assigned to ${existing}`
    });
  }
  mapping.set(candidate.highByte, candidate.domain);
}

function addItemDomain(mapping: DomainByHighByte, item: { id: number; name: string; domain: string }, file: string): void {
  const highByte = item.id >> 8;
  if (!isRegistryHighByte(highByte)) return;
  addDomain(mapping, {
    highByte,
    domain: item.domain,
    name: item.name,
    file
  });
}

export function buildDomainByHighByteFromRegistry(domainRegistry: DomainRange[] | undefined): DomainByHighByte {
  const mapping: DomainByHighByte = new Map();
  for (const range of domainRegistry ?? []) {
    addDomain(mapping, {
      highByte: range.highByte,
      domain: range.domain,
      name: range.domain,
      file: "domain_registry.yaml"
    });
  }
  return mapping;
}

export function buildSourceDomainByHighByte(source: {
  domainRegistry?: DomainRange[];
  methods: Method[];
  events: Event[];
  errors: ErrorCode[];
}): DomainByHighByte {
  const declared = buildDomainByHighByteFromRegistry(source.domainRegistry);
  if (declared.size > 0) return declared;

  const mapping: DomainByHighByte = new Map();
  for (const item of source.methods) addItemDomain(mapping, item, "method_registry.yaml");
  for (const item of source.events) addItemDomain(mapping, item, "event_registry.yaml");
  for (const item of source.errors) addItemDomain(mapping, item, "error_code.yaml");
  return mapping;
}

export function buildProtocolDomainByHighByte(model: {
  methods: MethodDefinition[];
  events: EventDefinition[];
  errors: ErrorDefinition[];
}): DomainByHighByte {
  const mapping: DomainByHighByte = new Map();
  for (const method of model.methods) {
    addItemDomain(mapping, { id: method.methodId, name: method.name, domain: method.domain }, "contract/protocol/axtp.protocol.yaml");
  }
  for (const event of model.events) {
    addItemDomain(mapping, { id: event.eventId, name: event.name, domain: event.domain }, "contract/protocol/axtp.protocol.yaml");
  }
  for (const error of model.errors) {
    if (mapping.has(error.code >> 8)) continue;
    addItemDomain(mapping, { id: error.code, name: error.name, domain: error.category }, "contract/protocol/axtp.protocol.yaml");
  }
  return mapping;
}
