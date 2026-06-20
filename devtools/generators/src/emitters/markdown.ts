import path from "node:path";
import type { SpecModel } from "../models.js";
import { hex, sortById, writeTextFile } from "../util.js";

export async function emitMarkdown(spec: SpecModel, outDir: string): Promise<void> {
  await emitMarkdownFiles(spec, path.join(outDir, "docs"));
}

export async function emitMarkdownFiles(spec: SpecModel, docsDir: string): Promise<void> {
  await Promise.all([
    writeTextFile(path.join(docsDir, "method_registry.generated.md"), methodTable(spec)),
    writeTextFile(path.join(docsDir, "event_registry.generated.md"), eventTable(spec)),
    writeTextFile(path.join(docsDir, "error_code.generated.md"), errorTable(spec)),
    writeTextFile(path.join(docsDir, "capability_registry.generated.md"), capabilityTable(spec)),
    writeTextFile(path.join(docsDir, "legacy_mapping.generated.md"), legacyTable(spec))
  ]);
}

function methodTable(spec: SpecModel): string {
  const rows = sortById(spec.methods).map((item) =>
    `| \`${hex(item.id)}\` | \`${item.name}\` | ${item.domain} | ${item.status} | ${item.requestSchema} | ${item.responseSchema} | ${item.legacy?.cmd_value ?? "-"} |`
  ).join("\n");
  return `# Method Registry\n\n| methodId | name | domain | status | request | response | legacy |\n|---:|---|---|---|---|---|---|\n${rows}\n`;
}

function eventTable(spec: SpecModel): string {
  const rows = sortById(spec.events).map((item) =>
    `| \`${hex(item.id)}\` | \`${item.name}\` | ${item.domain} | ${item.status} | ${item.eventSchema} |`
  ).join("\n");
  return `# Event Registry\n\n| eventId | name | domain | status | schema |\n|---:|---|---|---|---|\n${rows}\n`;
}

function errorTable(spec: SpecModel): string {
  const rows = sortById(spec.errors).map((item) =>
    `| \`${hex(item.id)}\` | \`${item.name}\` | ${item.domain} | ${item.status} | ${item.retryable} |`
  ).join("\n");
  return `# Error Code Registry\n\n| errorCode | name | domain | status | retryable |\n|---:|---|---|---|---|\n${rows}\n`;
}

function capabilityTable(spec: SpecModel): string {
  const rows = sortById(spec.capabilities).map((item) =>
    `| \`${hex(item.id)}\` | \`${item.name}\` | ${item.domain} | ${item.status} | ${item.type} | ${item.schema ?? "-"} |`
  ).join("\n");
  return `# Capability Registry\n\n| capabilityId | name | domain | status | type | schema |\n|---:|---|---|---|---|---|\n${rows}\n`;
}

function legacyTable(spec: SpecModel): string {
  const rows = [...spec.legacyMappings].sort((a, b) => a.legacyCmdValue - b.legacyCmdValue).map((item) =>
    `| ${item.legacyProtocol} | \`${hex(item.legacyCmdValue, 8)}\` | ${item.legacyName} | \`${hex(item.axtpMethodId)}\` | \`${item.axtpMethodName}\` |`
  ).join("\n");
  return `# Legacy Mapping\n\n| legacyProtocol | legacyCmdValue | legacyName | methodId | methodName |\n|---|---:|---|---:|---|${rows ? `\n${rows}` : ""}`;
}
