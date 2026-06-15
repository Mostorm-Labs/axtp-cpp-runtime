export type Status = "draft" | "mvp" | "stable" | "deprecated" | "reserved" | string;

export interface RegistryItem {
  id: number;
  name: string;
  domain: string;
  status: Status;
  description?: string;
  since?: string;
  deprecated?: boolean;
}

export interface Method extends RegistryItem {
  bitOffset: number;
  rpcOp: string;
  requestSchema: string;
  responseSchema: string;
  recommendedEncoding: string[];
  capabilities: string[];
  events: string[];
  errors: string[];
  legacy?: Record<string, unknown>;
}

export interface Event extends RegistryItem {
  bitOffset: number;
  eventSchema: string;
  severity?: string;
  trigger: string[];
  capabilities: string[];
}

export interface ErrorCode extends RegistryItem {
  retryable: boolean;
  category?: string;
  severity?: string;
  message?: string;
}

export interface Capability extends RegistryItem {
  type: string;
  schema?: string;
}

export interface LegacyMapping {
  legacyProtocol: string;
  legacyCmdValue: number;
  legacyName: string;
  axtpMethodId: number;
  axtpMethodName: string;
  direction: string;
  statusMapping: Record<string, string>;
}

export interface Field {
  id: number;
  name: string;
  type: string;
  required: boolean;
  deprecated: boolean;
  min?: number;
  max?: number;
  maxLength?: number;
  default?: unknown;
  schema?: string;
  enum?: string;
  repeated?: boolean;
  derivedFrom?: string;
  description?: string;
}

export interface Schema {
  name: string;
  type: string;
  description?: string;
  fields: Field[];
}

export interface CommonRegistryItem extends RegistryItem {
  value?: number;
}

export interface SpecModel {
  specRoot: string;
  version: Record<string, unknown>;
  config: Record<string, any>;
  payloadTypes: CommonRegistryItem[];
  controlOpcodes: CommonRegistryItem[];
  rpcEncodings: CommonRegistryItem[];
  rpcBodyEncodings: CommonRegistryItem[];
  rpcOps: CommonRegistryItem[];
  streamProfiles: CommonRegistryItem[];
  methods: Method[];
  events: Event[];
  errors: ErrorCode[];
  capabilities: Capability[];
  legacyMappings: LegacyMapping[];
  schemas: Schema[];
  mvpProfile: {
    methods: string[];
    events: string[];
    errors: string[];
    capabilities: string[];
  };
}
