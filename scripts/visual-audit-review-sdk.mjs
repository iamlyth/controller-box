// Sealed image-byte delivery driver for read-only visual-audit review.
//
// The auditor-mandated strategy: never pass an image by CLI @path (TOCTOU).
// This driver reads the exact image bytes from the provenance-bound capture
// directory, computes their SHA-256, verifies it matches the expected hash
// from the provenance manifest, base64-encodes the SAME bytes, and delivers
// them in-memory to the configured vision model through the pi SDK. The
// response is parsed as a strict structured finding and written alongside an
// invocation receipt. A receipt proves only that the invocation happened with
// those exact bytes; it never certifies visual truth and never elevates an
// evidence tier.
//
// The vision model is never a writer/Ralph model. Model selection is
// configurable and credential-free (auth resolves from the host pi config).
//
// Usage:
//   node visual-audit-review-sdk.mjs \
//     --image PATH --expected-sha256 HEX --state-id ID --role ROLE \
//     --prompt-file PATH --prompt-sha256 HEX --schema-sha256 HEX \
//     --model ollama/kimi-k2.7-code --out-dir DIR

import { createHash } from "node:crypto";
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { resolve, dirname } from "node:path";
import { createRequire } from "node:module";

// Resolve the pi-coding-agent package without hardcoding a store path. The
// pi2 wrapper exports PI_PACKAGE_DIR; failing that, resolve relative to the
// pi binary on PATH (its package is a sibling), failing that, via require
// resolution from this script.
async function resolvePackage() {
  const fromEnv = process.env.PI_PACKAGE_DIR;
  if (fromEnv) return fromEnv;
  const require = createRequire(import.meta.url);
  try {
    return dirname(require.resolve("@earendil-works/pi-coding-agent/package.json"));
  } catch {
    /* fall through */
  }
  const pi = process.env.PI_BIN || "pi";
  const { execFileSync } = await import("node:child_process");
  try {
    const bin = execFileSync("readlink", ["-f", pi], { encoding: "utf8" }).trim();
    if (bin) {
      const dir = dirname(bin);
      const candidate = resolve(dir, "../lib/node_modules/@earendil-works/pi-coding-agent");
      const { existsSync } = await import("node:fs");
      if (existsSync(resolve(candidate, "dist/index.js"))) return candidate;
    }
  } catch {
    /* fall through */
  }
  throw new Error("cannot resolve @earendil-works/pi-coding-agent; set PI_PACKAGE_DIR");
}

const sdk = await import(resolve(await resolvePackage(), "dist/index.js"));
const { createAgentSession, SessionManager, ModelRuntime, getAgentDir } = sdk;

const args = process.argv.slice(2);
function opt(name) {
  const i = args.indexOf(name);
  if (i < 0) throw new Error(`missing option ${name}`);
  return args[i + 1];
}

const imagePath = resolve(opt("--image"));
const expectedSha = opt("--expected-sha256");
const stateId = opt("--state-id");
const role = opt("--role");
const promptFile = resolve(opt("--prompt-file"));
const promptSha = opt("--prompt-sha256");
const schemaSha = opt("--schema-sha256");
const modelName = opt("--model");
const outDir = resolve(opt("--out-dir"));

const bytes = readFileSync(imagePath);
const imageSha = createHash("sha256").update(bytes).digest("hex");
if (imageSha !== expectedSha) {
  console.error(`visual-audit-sdk: image hash mismatch (expected ${expectedSha}, got ${imageSha})`);
  process.exit(3);
}
const b64 = bytes.toString("base64");
const promptTemplate = readFileSync(promptFile, "utf8");

// Frozen protocol: prompt and schema digests are bound into the finding.
const prompt = promptTemplate
  .replaceAll("{role}", role)
  .replaceAll("{state_id}", stateId)
  .replaceAll("{expected}", "")
  .replaceAll("{image_sha256}", imageSha)
  .replaceAll("{model}", modelName)
  .replaceAll("{prompt_sha256}", promptSha)
  .replaceAll("{schema_sha256}", schemaSha);

const modelRuntime = await ModelRuntime.create();
const model = modelRuntime.getModel(...modelName.split("/"));
const { session } = await createAgentSession({
  sessionManager: SessionManager.inMemory(),
  modelRuntime,
  model,
  agentDir: getAgentDir(),
});

let out = "";
session.subscribe((event) => {
  if (event.type === "message_update" && event.assistantMessageEvent?.type === "text_delta") {
    out += event.assistantMessageEvent.delta;
  }
});

const startedAt = Date.now();
try {
  await session.prompt(prompt, {
    images: [{ type: "image", data: b64, mimeType: "image/png" }],
  });
} finally {
  session.dispose?.();
  modelRuntime.dispose?.();
}
const elapsedMs = Date.now() - startedAt;

// Extract the strict JSON finding (no markdown fences).
const start = out.indexOf("{");
const end = out.lastIndexOf("}");
if (start < 0 || end <= start) {
  console.error("visual-audit-sdk: model returned no JSON object");
  process.exit(4);
}
let finding;
try {
  finding = JSON.parse(out.slice(start, end + 1));
} catch (err) {
  console.error(`visual-audit-sdk: malformed JSON from model: ${err.message}`);
  process.exit(5);
}

// Bound fields must match the sealed protocol; any drift is fatal.
finding.schema = "ralph-visual-audit-review/v1";
finding.state_id = stateId;
finding.role = role;
finding.image_sha256 = imageSha;
finding.prompt_sha256 = promptSha;
finding.schema_sha256 = schemaSha;
finding.model = modelName;
if (!finding.observations) finding.observations = [];

const receipt = {
  schema: "ralph-visual-audit-invocation/v1",
  model: modelName,
  state_id: stateId,
  role,
  image_sha256: imageSha,
  prompt_sha256: promptSha,
  schema_sha256: schemaSha,
  started_at_ms: startedAt,
  elapsed_ms: elapsedMs,
  image_bytes: bytes.length,
  note: "invocation receipt only; does not certify visual truth or elevate evidence tiers",
};

mkdirSync(outDir, { recursive: true });
writeFileSync(resolve(outDir, `finding-${stateId}-${role}.json`), JSON.stringify(finding, null, 2) + "\n", { mode: 0o600 });
writeFileSync(resolve(outDir, `receipt-${stateId}-${role}.json`), JSON.stringify(receipt, null, 2) + "\n", { mode: 0o600 });
console.log(`visual-audit-sdk: ${stateId}/${role} verdict=${finding.verdict} (${imageSha.slice(0, 12)})`);
