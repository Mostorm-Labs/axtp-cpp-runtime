import { GeneratorError } from "./errors.js";

export interface PackedField { name: string; offset: number; bytes: number; type: string; }
export interface InputStreamWire {
  format: string; byteOrder: string; streamHeaderBytes: number; typeBytes: number; maxDataBytes: number;
  inputTypes: Record<string, number>;
  touch: { recordBytes: number; maxContacts: number; sampling: string; phases: Record<string, number>; fields: PackedField[] };
  mouse: { buttons: Record<string, number>; modifiers: Record<string, number>; wheelUnits: Record<string, number>;
    wheelPhases: Record<string, number>; records: Array<{name: string; kind: number; bytes: number; fields: PackedField[]}> };
}
export interface RetiredAllocation { kind: "method" | "event"; id: number; domain: string; bitOffset: number; }

function reject(message: string): never {
  throw new GeneratorError({code:"AXTP-GEN-1004",file:"contract/registry",message});
}

function exact(actual: unknown, expected: unknown): void {
  if (JSON.stringify(actual) !== JSON.stringify(expected)) reject("input wire layout differs from input-sample-v1");
}

function packed(fields: PackedField[], names: string[], types: string[], bytes: number): void {
  exact(fields.map(f => f.name), names);
  exact(fields.map(f => f.type), types);
  let offset = 0;
  const widths: Record<string, number> = {uint8:1,uint16:2,uint32:4,int32:4};
  for (const f of fields) {
    if (f.offset !== offset || f.bytes !== widths[f.type]) reject("input packed layout has gaps, overlap or invalid width");
    offset += f.bytes;
  }
  if (offset !== bytes) reject("input record layout length mismatch");
}

export function validateInputStream(wire?: InputStreamWire): void {
  if (!wire) return;
  exact(Object.keys(wire).sort(), ["byteOrder","format","inputTypes","maxDataBytes","mouse","streamHeaderBytes","touch","typeBytes"]);
  exact([wire.format,wire.byteOrder,wire.streamHeaderBytes,wire.typeBytes,wire.maxDataBytes], ["input-sample-v1","big-endian",16,1,4096]);
  exact(wire.inputTypes, {touch:1,mouse:2});
  exact([wire.touch.recordBytes,wire.touch.maxContacts,wire.touch.sampling], [7,256,"complete"]);
  exact(wire.touch.phases, {down:0,move:1,up:2,cancel:3});
  packed(wire.touch.fields, ["contactId","phase","x","y"], ["uint16","uint8","uint16","uint16"], 7);
  exact(wire.mouse.buttons,{left:1,right:2,middle:4,back:8,forward:16});
  exact(wire.mouse.modifiers,{shift:1,ctrl:2,alt:4,meta:8});
  exact(wire.mouse.wheelUnits,{detent120:0,regionQ16_16:1});
  exact(wire.mouse.wheelPhases,{none:0,began:1,changed:2,ended:3,cancelled:4});
  exact(wire.mouse.records.map(r => [r.name,r.kind,r.bytes]), [["move",0,11],["buttons",1,11],["wheel",2,21],["cancel",3,1],["modifiers",4,3]]);
  const common = ["eventKind","modifiers","buttons","x","y"];
  const types = ["uint8","uint16","uint32","uint16","uint16"];
  for (const r of wire.mouse.records) {
    const names = r.kind === 3 ? ["eventKind"] : r.kind === 4 ? ["eventKind","modifiers"] : r.kind === 2 ? [...common,"wheelUnit","wheelPhase","wheelX","wheelY"] : common;
    const rt = r.kind === 3 ? ["uint8"] : r.kind === 4 ? ["uint8","uint16"] : r.kind === 2 ? [...types,"uint8","uint8","int32","int32"] : types;
    packed(r.fields,names,rt,r.bytes);
  }
}

export function validateRetired(allocations: RetiredAllocation[] = [], methods: Array<{id?:number;methodId?:number;domain:string;bitOffset:number}>, events: Array<{id?:number;eventId?:number;domain:string;bitOffset:number}>, schemas: Array<{name:string;retiredFieldIds?:number[];fields:Array<{id?:number;fieldId?:number}>}>): void {
  const seen = new Set<string>();
  for (const a of allocations) {
    if (!["method","event"].includes(a.kind) || !Number.isInteger(a.id) || a.id < 1 || a.id > 65535 || !a.domain || !Number.isInteger(a.bitOffset) || a.bitOffset < 0)
      reject("invalid retired allocation");
    const key = `${a.kind}:${a.id}`;
    if (seen.has(key)) reject("duplicate retired allocation");
    seen.add(key);
    const live = a.kind === "method" ? methods : events;
    if (live.some(x => (x.id ?? (x as any).methodId ?? (x as any).eventId) === a.id || (x.domain === a.domain && x.bitOffset === a.bitOffset)))
      reject("retired allocation cannot be reused");
  }
  for (const schema of schemas) {
    const retired = schema.retiredFieldIds ?? [];
    if (new Set(retired).size !== retired.length || retired.some(id => !Number.isInteger(id) || id < 1 || id > 254)) reject("invalid retired field allocation");
    if (schema.fields.some(f => retired.includes((f.id ?? f.fieldId)!))) reject(`retired field allocation reused in ${schema.name}`);
  }
}

export function retiredBits(allocations: RetiredAllocation[] = [], kind: "method" | "event") {
  return allocations.filter(a => a.kind === kind).map(a => ({name:`retired:${a.id}`,domain:a.domain,bitOffset:a.bitOffset}));
}
