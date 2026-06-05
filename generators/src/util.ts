import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";

export function normalizeId(value: unknown, context: string): number {
  if (typeof value === "number" && Number.isInteger(value)) return value;
  if (typeof value === "string") {
    const trimmed = value.trim();
    const parsed = trimmed.startsWith("0x") || trimmed.startsWith("0X")
      ? Number.parseInt(trimmed.slice(2), 16)
      : Number.parseInt(trimmed, 10);
    if (Number.isInteger(parsed) && !Number.isNaN(parsed)) return parsed;
  }
  throw new Error(`invalid numeric id for ${context}: ${String(value)}`);
}

export function hex(value: number, width = 4): string {
  return `0x${value.toString(16).toUpperCase().padStart(width, "0")}`;
}

export function cppName(name: string): string {
  return name
    .split(/[^A-Za-z0-9]+/)
    .filter(Boolean)
    .map((part) => {
      const normalized = part === part.toUpperCase() ? part.toLowerCase() : part;
      return normalized.charAt(0).toUpperCase() + normalized.slice(1);
    })
    .join("");
}

export function cppConstName(name: string): string {
  return name
    .replace(/([a-z0-9])([A-Z])/g, "$1_$2")
    .replace(/[^A-Za-z0-9]+/g, "_")
    .replace(/^_+|_+$/g, "")
    .toUpperCase();
}

export function sortById<T extends { id: number }>(items: T[]): T[] {
  return [...items].sort((a, b) => a.id - b.id || String((a as any).name).localeCompare(String((b as any).name)));
}

export async function writeTextFile(filePath: string, content: string): Promise<void> {
  await mkdir(path.dirname(filePath), { recursive: true });
  await writeFile(filePath, content.endsWith("\n") ? content : `${content}\n`, "utf8");
}

export function toJsonStable(value: unknown): string {
  return `${JSON.stringify(value, null, 2)}\n`;
}
