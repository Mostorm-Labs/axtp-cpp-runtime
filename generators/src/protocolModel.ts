export type ProtocolStatus = "draft" | "experimental" | "mvp" | "stable" | "deprecated" | "reserved" | string;

export interface ProtocolMetadata {
  name: string;
  version: string;
  specVersion: number;
  registryVersion: string;
  status?: string;
}

export interface ProtocolOverview {
  title: string;
  summary: string;
  goals: string[];
  nonGoals: string[];
}

export interface ArchitectureLayer {
  name: string;
  description: string;
}

export interface LifecycleStep {
  step: string;
  from?: string;
  to?: string;
  status?: string;
  description: string;
}

export interface ProtocolArchitecture {
  layers: ArchitectureLayer[];
  lifecycle: LifecycleStep[];
  optionalLifecycleExtensions: LifecycleStep[];
}

export interface ProtocolGuide {
  quickStart: Array<{
    title: string;
    steps: string[];
  }>;
}

export interface WireDefinition {
  byteOrder: string;
  byteOrderAlias?: string;
  integerEncoding: string;
  crcByteOrder: string;
  scope?: string;
}

export interface FrameProfile {
  name: string;
  magic?: string | number;
  l1: string;
  l2: string;
  supportsMixing?: boolean;
}

export interface TransportProfile {
  name: string;
  family: string;
  mode?: string;
  frameProfile: string;
  production: boolean;
  maxFrameSize?: number;
  rpcEncodings?: string[];
  supportsControl?: boolean;
  supportsStream?: boolean;
  physicalClient?: string;
  physicalServer?: string;
  logicalClient?: string;
  logicalServer?: string;
  helloSender?: string;
  usage?: string;
  notes?: string;
}

export interface PayloadType {
  name: string;
  id: number;
  headerBytes: number;
  description: string;
  selectionRule?: string;
  headerFields?: Array<{
    name: string;
    type: string;
    bytes: number | string;
    description: string;
  }>;
}

export interface ControlDefinition {
  requiredOpcodes: string[];
  optionalOpcodes: string[];
  reservedOpcodes: string[];
  rules: string[];
}

export interface StreamDefinition {
  header: {
    name: string;
    size: number;
    fields: Array<{
      name: string;
      type: string;
    }>;
  };
  rules: string[];
}

export interface CompatibilityDefinition {
  legacySources: string[];
  rules: string[];
}

export interface SchemaField {
  fieldId: number;
  name: string;
  type: string;
  required: boolean;
  min?: number;
  max?: number;
  maxLength?: number;
  deprecated?: boolean;
  derivedFrom?: string;
  description?: string;
}

export interface SchemaDefinition {
  name: string;
  kind: string;
  description?: string;
  fields: SchemaField[];
}

/** @deprecated Use SchemaField */
export type TypeField = SchemaField;
/** @deprecated Use SchemaDefinition */
export type TypeDefinition = SchemaDefinition;

export interface MethodDefinition {
  name: string;
  description?: string;
  methodId: number;
  bitOffset: number;
  domain: string;
  since: string;
  status: ProtocolStatus;
  request: { type: string };
  response: { type: string };
  encodings: string[];
  capabilities: string[];
  events: string[];
  errors: string[];
  legacy?: Record<string, unknown>;
}

export interface EventDefinition {
  name: string;
  description?: string;
  eventId: number;
  bitOffset: number;
  domain: string;
  since: string;
  status: ProtocolStatus;
  payload: { type: string };
  severity?: string;
  trigger: string[];
  capabilities: string[];
}

export interface ErrorDefinition {
  name: string;
  code: number;
  category: string;
  since?: string;
  status: ProtocolStatus;
  severity: string;
  retryable: boolean;
  message: string;
}

export interface ProfileDefinition {
  name: string;
  since: string;
  status: ProtocolStatus;
  extends?: string;
  requiredMethods: string[];
  requiredEvents: string[];
  requiredTypes: string[];
  requiredErrors: string[];
  transportProfiles: string[];
  frameProfile?: string;
  frameProfiles: string[];
  notes?: string;
}

export interface WireExampleStep {
  direction: string;
  label: string;
  asciiLayout: string;
  hexBytes: string;
  fieldAnnotations: string[];
}

export interface WireExample {
  title: string;
  transport: string;
  frameProfile: string;
  description: string;
  steps: WireExampleStep[];
}

export interface ProtocolModel {
  specRoot: string;
  sourcePath: string;
  protocol: ProtocolMetadata;
  overview: ProtocolOverview;
  architecture: ProtocolArchitecture;
  guide: ProtocolGuide;
  wire: WireDefinition;
  frameProfiles: FrameProfile[];
  transports: TransportProfile[];
  payloadTypes: PayloadType[];
  control: ControlDefinition;
  stream: StreamDefinition;
  compatibility: CompatibilityDefinition;
  schemas: SchemaDefinition[];
  wireExamples: WireExample[];
  methods: MethodDefinition[];
  events: EventDefinition[];
  errors: ErrorDefinition[];
  profiles: ProfileDefinition[];
  raw: unknown;
}
