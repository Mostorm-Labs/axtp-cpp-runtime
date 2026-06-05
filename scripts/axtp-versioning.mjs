#!/usr/bin/env node
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync } from "node:fs";
import { mkdir, readFile, readdir, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(scriptDir, "..");

function parseArgs(argv) {
  const [command, ...rest] = argv;
  const options = {};
  for (let index = 0; index < rest.length; index += 1) {
    const token = rest[index];
    if (!token.startsWith("--")) {
      throw new Error(`Unexpected argument: ${token}`);
    }
    const key = token.slice(2);
    const next = rest[index + 1];
    if (next === undefined || next.startsWith("--")) {
      options[key] = "true";
    } else {
      options[key] = next;
      index += 1;
    }
  }
  return { command, options };
}

async function readText(file) {
  return readFile(file, "utf8");
}

async function readJson(file) {
  return JSON.parse(await readText(file));
}

function stripYamlValue(value) {
  const trimmed = value.trim();
  if ((trimmed.startsWith('"') && trimmed.endsWith('"')) || (trimmed.startsWith("'") && trimmed.endsWith("'"))) {
    return trimmed.slice(1, -1);
  }
  return trimmed;
}

async function readSpecLock() {
  const file = path.join(root, "AXTP_SPEC.lock.yaml");
  if (!existsSync(file)) {
    throw new Error("Missing AXTP_SPEC.lock.yaml");
  }
  const text = await readText(file);
  const fields = {};
  for (const line of text.split(/\r?\n/)) {
    const match = line.match(/^\s{2}([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.+?)\s*$/);
    if (match) fields[match[1]] = stripYamlValue(match[2]);
  }
  for (const field of ["repository", "tag", "version", "commit", "compatibility"]) {
    if (!fields[field]) throw new Error(`Missing axtp_spec.${field} in AXTP_SPEC.lock.yaml`);
  }
  if (fields.repository !== "https://github.com/Mostorm-Labs/axtp") {
    throw new Error("AXTP Spec repository must be https://github.com/Mostorm-Labs/axtp");
  }
  return fields;
}

function gitValue(args, fallback = "unknown") {
  try {
    return execFileSync("git", args, { cwd: root, encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] }).trim() || fallback;
  } catch {
    return fallback;
  }
}

async function readRuntimeVersion(runtimeName) {
  if (runtimeName === "axtp-c-runtime") {
    const text = await readText(path.join(root, "CMakeLists.txt"));
    const match = text.match(/project\s*\([^)]*?\bVERSION\s+([0-9]+\.[0-9]+\.[0-9][^\s)]*)/is);
    if (!match) throw new Error("Could not read runtime version from CMakeLists.txt project(... VERSION ...)");
    return match[1];
  }
  if (runtimeName === "axtp-cpp-runtime" || runtimeName === "axtp-mock-server") {
    const file = path.join(root, "VERSION");
    if (!existsSync(file)) throw new Error(`Missing VERSION for ${runtimeName}`);
    return (await readText(file)).trim();
  }
  if (runtimeName === "axtp-flutter-runtime") {
    const text = await readText(path.join(root, "pubspec.yaml"));
    const match = text.match(/^version:\s*([^\s]+)\s*$/m);
    if (!match) throw new Error("Could not read runtime version from pubspec.yaml");
    return match[1];
  }
  if (runtimeName === "axtp-ts-runtime") {
    return (await readJson(path.join(root, "package.json"))).version;
  }
  if (runtimeName === "axtp-python-runtime") {
    const text = await readText(path.join(root, "pyproject.toml"));
    const match = text.match(/^\[project\][\s\S]*?^version\s*=\s*"([^"]+)"/m);
    if (!match) throw new Error("Could not read runtime version from pyproject.toml [project].version");
    return match[1];
  }
  throw new Error(`Unsupported runtime: ${runtimeName}`);
}

async function readGeneratorMetadata() {
  const pkg = await readJson(path.join(root, "generators/package.json"));
  return {
    name: pkg.name,
    version: pkg.version,
    commit: gitValue(["rev-parse", "HEAD"])
  };
}

async function listFiles(dir, base = dir) {
  if (!existsSync(dir)) return [];
  const entries = await readdir(dir, { withFileTypes: true });
  const files = [];
  for (const entry of entries) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      files.push(...await listFiles(full, base));
    } else if (entry.isFile()) {
      files.push(path.relative(base, full).split(path.sep).join("/"));
    }
  }
  return files.sort();
}

async function hashDirectory(dir) {
  if (!existsSync(dir)) return null;
  const stats = await stat(dir);
  if (!stats.isDirectory()) return null;
  const hash = createHash("sha256");
  for (const relative of await listFiles(dir)) {
    hash.update(relative);
    hash.update("\0");
    hash.update(await readFile(path.join(dir, relative)));
    hash.update("\0");
  }
  return `sha256:${hash.digest("hex")}`;
}

function resolveSpecRoot() {
  if (process.env.AXTP_SPEC_PATH) return process.env.AXTP_SPEC_PATH;
  return path.join(root, "third_party/axtp-spec");
}

function generatedAt() {
  return process.env.AXTP_GENERATED_AT || new Date().toISOString().replace(/\.\d{3}Z$/, "Z");
}

function jsonString(value) {
  return JSON.stringify(value);
}

function constantTargets(runtimeName) {
  if (runtimeName === "axtp-c-runtime") {
    return [{ kind: "c", file: path.join(root, "include/generated/axtp_generated_version.h") }];
  }
  if (runtimeName === "axtp-cpp-runtime") {
    return [{ kind: "cpp", file: path.join(root, "core/include/generated/axtp_generated_version.hpp") }];
  }
  if (runtimeName === "axtp-flutter-runtime") {
    return [{ kind: "dart", file: path.join(root, "lib/src/generated/axtp_generated_version.dart") }];
  }
  if (runtimeName === "axtp-ts-runtime") {
    return [{ kind: "ts", file: path.join(root, "src/generated/axtpGeneratedVersion.ts") }];
  }
  if (runtimeName === "axtp-python-runtime") {
    return [{ kind: "python", file: path.join(root, "src/axtp_runtime/generated/axtp_generated_version.py") }];
  }
  if (runtimeName === "axtp-mock-server") {
    return [
      { kind: "ts", file: path.join(root, "generated/node-mock-server/src/generated/axtpGeneratedVersion.ts") },
      { kind: "cpp", file: path.join(root, "generated/cpp-mock-server/include/axtp_generated_version.hpp") }
    ];
  }
  throw new Error(`Unsupported runtime: ${runtimeName}`);
}

function versionFields(manifest) {
  return {
    runtimeName: manifest.runtime.name,
    runtimeVersion: manifest.runtime.version,
    specVersion: manifest.axtpSpec.version,
    specTag: manifest.axtpSpec.tag,
    specCommit: manifest.axtpSpec.commit,
    generatorName: manifest.generator.name,
    generatorVersion: manifest.generator.version,
    generatedAt: manifest.generatedAt
  };
}

function renderConstant(kind, manifest) {
  const fields = versionFields(manifest);
  if (kind === "c") {
    return `/* Generated by ${fields.runtimeName} generator. Do not edit manually. */
#ifndef AXTP_GENERATED_VERSION_H
#define AXTP_GENERATED_VERSION_H

#define AXTP_RUNTIME_NAME ${jsonString(fields.runtimeName)}
#define AXTP_RUNTIME_VERSION ${jsonString(fields.runtimeVersion)}
#define AXTP_SPEC_VERSION ${jsonString(fields.specVersion)}
#define AXTP_SPEC_TAG ${jsonString(fields.specTag)}
#define AXTP_SPEC_COMMIT ${jsonString(fields.specCommit)}
#define AXTP_GENERATOR_NAME ${jsonString(fields.generatorName)}
#define AXTP_GENERATOR_VERSION ${jsonString(fields.generatorVersion)}
#define AXTP_GENERATED_AT ${jsonString(fields.generatedAt)}

#endif
`;
  }
  if (kind === "cpp") {
    return `// Generated by ${fields.runtimeName} generator. Do not edit manually.
#pragma once

namespace axtp::generated {

inline constexpr const char* kRuntimeName = ${jsonString(fields.runtimeName)};
inline constexpr const char* kRuntimeVersion = ${jsonString(fields.runtimeVersion)};
inline constexpr const char* kSpecVersion = ${jsonString(fields.specVersion)};
inline constexpr const char* kSpecTag = ${jsonString(fields.specTag)};
inline constexpr const char* kSpecCommit = ${jsonString(fields.specCommit)};
inline constexpr const char* kGeneratorName = ${jsonString(fields.generatorName)};
inline constexpr const char* kGeneratorVersion = ${jsonString(fields.generatorVersion)};
inline constexpr const char* kGeneratedAt = ${jsonString(fields.generatedAt)};

}  // namespace axtp::generated
`;
  }
  if (kind === "dart") {
    return `// Generated by ${fields.runtimeName} generator. Do not edit manually.
class AxtpGeneratedVersion {
  static const runtimeName = '${fields.runtimeName}';
  static const runtimeVersion = '${fields.runtimeVersion}';
  static const specVersion = '${fields.specVersion}';
  static const specTag = '${fields.specTag}';
  static const specCommit = '${fields.specCommit}';
  static const generatorName = '${fields.generatorName}';
  static const generatorVersion = '${fields.generatorVersion}';
  static const generatedAt = '${fields.generatedAt}';
}
`;
  }
  if (kind === "ts") {
    return `// Generated by ${fields.runtimeName} generator. Do not edit manually.
export const AXTP_GENERATED_VERSION = {
  runtimeName: ${jsonString(fields.runtimeName)},
  runtimeVersion: ${jsonString(fields.runtimeVersion)},
  specVersion: ${jsonString(fields.specVersion)},
  specTag: ${jsonString(fields.specTag)},
  specCommit: ${jsonString(fields.specCommit)},
  generatorName: ${jsonString(fields.generatorName)},
  generatorVersion: ${jsonString(fields.generatorVersion)},
  generatedAt: ${jsonString(fields.generatedAt)}
} as const;
`;
  }
  if (kind === "python") {
    return `# Generated by ${fields.runtimeName} generator. Do not edit manually.
AXTP_GENERATED_VERSION = {
    "runtimeName": ${jsonString(fields.runtimeName)},
    "runtimeVersion": ${jsonString(fields.runtimeVersion)},
    "specVersion": ${jsonString(fields.specVersion)},
    "specTag": ${jsonString(fields.specTag)},
    "specCommit": ${jsonString(fields.specCommit)},
    "generatorName": ${jsonString(fields.generatorName)},
    "generatorVersion": ${jsonString(fields.generatorVersion)},
    "generatedAt": ${jsonString(fields.generatedAt)},
}
`;
  }
  throw new Error(`Unsupported constant kind: ${kind}`);
}

async function writeVersionMetadata(runtimeName) {
  const lock = await readSpecLock();
  const generator = await readGeneratorMetadata();
  const specRoot = resolveSpecRoot();
  const manifest = {
    generatedAt: generatedAt(),
    generator,
    axtpSpec: {
      repository: lock.repository,
      tag: lock.tag,
      version: lock.version,
      commit: lock.commit
    },
    runtime: {
      name: runtimeName,
      version: await readRuntimeVersion(runtimeName),
      commit: gitValue(["rev-parse", "HEAD"])
    },
    inputs: {
      registryHash: await hashDirectory(path.join(specRoot, "registry")),
      schemasHash: await hashDirectory(path.join(specRoot, "schemas")),
      conformanceHash: (await hashDirectory(path.join(specRoot, "docs", "conformance"))) ?? await hashDirectory(path.join(specRoot, "conformance"))
    }
  };
  const manifestPath = path.join(root, "generated/axtp_generated_manifest.json");
  await mkdir(path.dirname(manifestPath), { recursive: true });
  await writeFile(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`, "utf8");
  for (const target of constantTargets(runtimeName)) {
    await mkdir(path.dirname(target.file), { recursive: true });
    await writeFile(target.file, renderConstant(target.kind, manifest), "utf8");
  }
  console.log(`[OK] generated version manifest: ${manifestPath}`);
}

async function readManifest() {
  const file = path.join(root, "generated/axtp_generated_manifest.json");
  if (!existsSync(file)) throw new Error("Missing generated/axtp_generated_manifest.json");
  return readJson(file);
}

function assertEqual(label, actual, expected) {
  if (actual !== expected) {
    throw new Error(`${label} mismatch: expected ${expected}, got ${actual}`);
  }
}

async function checkVersionMetadata(runtimeName, { release = false, tagVersion = null } = {}) {
  const lock = await readSpecLock();
  const manifest = await readManifest();
  const runtimeVersion = await readRuntimeVersion(runtimeName);
  const generator = await readGeneratorMetadata();

  if (release && lock.tag === "unreleased") {
    throw new Error("Release builds must not use AXTP Spec tag: unreleased");
  }
  if (tagVersion !== null) {
    assertEqual("runtime tag version", runtimeVersion, tagVersion);
    assertEqual("AXTP Spec lock version", lock.version, tagVersion);
    assertEqual("AXTP Spec lock tag", lock.tag, "spec/v" + tagVersion);
  }
  assertEqual("runtime.name", manifest.runtime.name, runtimeName);
  assertEqual("runtime.version", manifest.runtime.version, runtimeVersion);
  assertEqual("generator.name", manifest.generator.name, generator.name);
  assertEqual("generator.version", manifest.generator.version, generator.version);
  assertEqual("axtpSpec.repository", manifest.axtpSpec.repository, lock.repository);
  assertEqual("axtpSpec.tag", manifest.axtpSpec.tag, lock.tag);
  assertEqual("axtpSpec.version", manifest.axtpSpec.version, lock.version);
  assertEqual("axtpSpec.commit", manifest.axtpSpec.commit, lock.commit);

  const fields = versionFields(manifest);
  for (const target of constantTargets(runtimeName)) {
    if (!existsSync(target.file)) throw new Error(`Missing generated version constant: ${path.relative(root, target.file)}`);
    const text = await readText(target.file);
    for (const [key, value] of Object.entries(fields)) {
      if (!text.includes(value)) {
        throw new Error(`${path.relative(root, target.file)} does not contain ${key}=${value}`);
      }
    }
  }

  console.log("AXTP generated version");
  console.log(`  runtime: ${manifest.runtime.name} ${manifest.runtime.version}`);
  console.log(`  spec: ${manifest.axtpSpec.tag} (${manifest.axtpSpec.commit})`);
  console.log(`  generator: ${manifest.generator.name} ${manifest.generator.version}`);
  console.log(`  generatedAt: ${manifest.generatedAt}`);
}

async function writeReleaseNotes(runtimeName, outFile) {
  const manifest = await readManifest();
  const runtimeCommit = process.env.GITHUB_SHA || gitValue(["rev-parse", "HEAD"]);
  const templatePath = path.join(root, ".github/release-notes-template.md");
  const template = existsSync(templatePath)
    ? await readText(templatePath)
    : `# {{RUNTIME_NAME}} v{{RUNTIME_VERSION}}

## AXTP Spec Compatibility

- AXTP Spec: \`{{SPEC_TAG}}\`
- AXTP Spec Version: \`{{SPEC_VERSION}}\`
- AXTP Spec Commit: \`{{SPEC_COMMIT}}\`
- Generator: \`{{GENERATOR_NAME}} {{GENERATOR_VERSION}}\`
- Generated Manifest: \`generated/axtp_generated_manifest.json\`

## Runtime

- Runtime Version: \`{{RUNTIME_VERSION}}\`
- Runtime Commit: \`{{RUNTIME_COMMIT}}\`

## Artifacts

- Source package
- Generated manifest
- Generated bindings/types

## Changes

Please see commit history or changelog for details.
`;
  const replacements = {
    RUNTIME_NAME: runtimeName,
    RUNTIME_VERSION: manifest.runtime.version,
    SPEC_TAG: manifest.axtpSpec.tag,
    SPEC_VERSION: manifest.axtpSpec.version,
    SPEC_COMMIT: manifest.axtpSpec.commit,
    GENERATOR_NAME: manifest.generator.name,
    GENERATOR_VERSION: manifest.generator.version,
    RUNTIME_COMMIT: runtimeCommit
  };
  let output = template;
  for (const [key, value] of Object.entries(replacements)) {
    output = output.replaceAll(`{{${key}}}`, value);
  }
  await mkdir(path.dirname(outFile), { recursive: true });
  await writeFile(outFile, output, "utf8");
  console.log(`[OK] wrote release notes: ${outFile}`);
}

const { command, options } = parseArgs(process.argv.slice(2));
const runtimeName = options["runtime-name"] || path.basename(root);

try {
  if (command === "generate") {
    await writeVersionMetadata(runtimeName);
  } else if (command === "check") {
    await checkVersionMetadata(runtimeName);
  } else if (command === "release-check") {
    const tagVersion = options["tag-version"];
    if (!tagVersion) throw new Error("release-check requires --tag-version X.Y.Z");
    await checkVersionMetadata(runtimeName, { release: true, tagVersion });
  } else if (command === "release-notes") {
    const outFile = options.out;
    if (!outFile) throw new Error("release-notes requires --out <file>");
    await writeReleaseNotes(runtimeName, outFile);
  } else {
    throw new Error("Usage: axtp-versioning.mjs <generate|check|release-check|release-notes> [--runtime-name NAME]");
  }
} catch (error) {
  console.error(error instanceof Error ? error.message : String(error));
  process.exit(1);
}
