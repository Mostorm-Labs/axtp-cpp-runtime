export type GeneratorErrorCode =
  | "AXTP-GEN-1001"
  | "AXTP-GEN-1002"
  | "AXTP-GEN-1003"
  | "AXTP-GEN-1004"
  | "AXTP-GEN-1005"
  | "AXTP-GEN-1006"
  | "AXTP-GEN-1007"
  | "AXTP-GEN-1008";

export class GeneratorError extends Error {
  readonly code: GeneratorErrorCode;
  readonly file?: string;
  readonly entry?: string;
  readonly field?: string;

  constructor(args: {
    code: GeneratorErrorCode;
    message: string;
    file?: string;
    entry?: string;
    field?: string;
  }) {
    super(args.message);
    this.name = "GeneratorError";
    this.code = args.code;
    this.file = args.file;
    this.entry = args.entry;
    this.field = args.field;
  }
}

export function formatGeneratorError(error: unknown): string {
  if (!(error instanceof GeneratorError)) {
    return String(error instanceof Error ? error.message : error);
  }

  const lines = [`ERROR ${error.code}`];
  if (error.file) lines.push(`file: ${error.file}`);
  if (error.entry) lines.push(`entry: ${error.entry}`);
  if (error.field) lines.push(`field: ${error.field}`);
  lines.push(`message: ${error.message}`);
  return lines.join("\n");
}
