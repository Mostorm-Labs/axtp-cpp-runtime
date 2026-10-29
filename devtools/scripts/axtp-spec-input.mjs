import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { readFile, readdir, lstat } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const digest = (data) => createHash("sha256").update(data).digest("hex");
export async function verifyLocalSnapshot(specRoot, binding) {
  const manifest = JSON.parse(await readFile(path.join(specRoot, ".axtp-local-snapshot.json"), "utf8"));
  if (binding.sourceMode !== "local-development-snapshot" || binding.published !== false ||
      !/^\d+\.\d+\.\d+-dev\.[a-f0-9]{12}$/.test(binding.version) ||
      !/^[a-f0-9]{40}$/.test(binding.baselineCommit) ||
      !/^[a-f0-9]{64}$/.test(binding.snapshotSha256)) throw new Error("Invalid local spec binding");
  if (manifest.schemaVersion !== 1 || manifest.sourceMode !== binding.sourceMode ||
      manifest.baselineCommit !== binding.baselineCommit || manifest.repository !== binding.repository ||
      manifest.snapshotSha256 !== binding.snapshotSha256) throw new Error("Local snapshot identity mismatch");
  const entries = manifest.files;
  if (!Array.isArray(entries) || !entries.length) throw new Error("Empty snapshot manifest");
  const names = new Set();
  let listing = "";
  for (const entry of entries) {
    if (typeof entry.path !== "string" || entry.path.includes("\\") ||
        entry.path.split("/").some((part) => !part || part === "." || part === "..") ||
        path.isAbsolute(entry.path) || entry.path.includes(":") || names.has(entry.path)) {
      throw new Error("Unsafe or duplicate snapshot path");
    }
    const file = path.join(specRoot, entry.path);
    let cursor = specRoot;
    for (const part of entry.path.split("/")) {
      cursor = path.join(cursor, part);
      if ((await lstat(cursor)).isSymbolicLink()) throw new Error("Snapshot symlinks are forbidden");
    }
    const data = await readFile(file);
    if (digest(data) !== entry.sha256 || data.length !== entry.size) throw new Error(`Snapshot bytes changed: ${entry.path}`);
    names.add(entry.path);
    listing += `${entry.path}\t${entry.sha256}\n`;
  }
  if ([...names].sort().join("\n") !== [...names].join("\n") || digest(listing) !== binding.snapshotSha256) {
    throw new Error("Snapshot fingerprint mismatch");
  }
  async function checkExtra(dir, prefix = "") {
    for (const entry of await readdir(dir, { withFileTypes: true })) {
      const relative = prefix + entry.name;
      if (relative === ".axtp-local-snapshot.json") continue;
      if (entry.isDirectory()) await checkExtra(path.join(dir, entry.name), relative + "/");
      else if (!entry.isFile() || !names.has(relative)) throw new Error(`Unmanifested snapshot input: ${relative}`);
    }
  }
  await checkExtra(specRoot);
  return { repository: binding.repository, commit: binding.baselineCommit,
    tag: `local-dev/${binding.snapshotSha256.slice(0, 12)}`, version: binding.version,
    sourceMode: binding.sourceMode, snapshotSha256: binding.snapshotSha256, published: false };
}

export async function resolveSpecInput(root, { env = process.env, releaseOnly = false } = {}) {
  const mode = env.AXTP_SPEC_SOURCE_MODE || "release";
  if (mode === "local-development") {
    if (releaseOnly) throw new Error("Release verification rejects local-development spec input");
    if (!env.AXTP_SPEC_PATH) throw new Error("Local development requires explicit AXTP_SPEC_PATH");
    return verifyLocalSnapshot(path.resolve(env.AXTP_SPEC_PATH),
      JSON.parse(await readFile(path.join(root, "AXTP_SPEC.local.json"), "utf8")));
  }
  if (mode !== "release") throw new Error("Unknown AXTP_SPEC_SOURCE_MODE");
  const lockText = await readFile(path.join(root, "AXTP_SPEC.lock.yaml"), "utf8");
  const lock = {};
  for (const line of lockText.split(/\r?\n/)) {
    const m = line.match(/^\s{2}(\w+):\s*(.*?)\s*$/);
    if (m) lock[m[1]] = m[2].replace(/^["']|["']$/g, "");
  }
  if (lock.repository !== "https://github.com/Mostorm-Labs/axtp" ||
      !/^spec\/v\d+\.\d+\.\d+$/.test(lock.tag || "") ||
      lock.tag !== `spec/v${lock.version}` || !/^[a-f0-9]{40}$/.test(lock.commit || "") || !lock.compatibility) {
    throw new Error("Release spec lock must bind an exact released spec tag/commit");
  }
  if (env.AXTP_SPEC_PATH) {
    const git = (...args) => execFileSync("git", ["-C", env.AXTP_SPEC_PATH, ...args], {encoding:"utf8"}).trim();
    if (git("rev-parse", "HEAD") !== lock.commit || git("rev-parse", `${lock.tag}^{commit}`) !== lock.commit ||
        git("status", "--porcelain", "--untracked-files=all")) throw new Error("Release input does not match clean locked checkout");
  }
  return { ...lock, sourceMode: "release", published: true };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
    console.log(JSON.stringify(await resolveSpecInput(root, {releaseOnly:process.argv.includes("--release-only")})));
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}
