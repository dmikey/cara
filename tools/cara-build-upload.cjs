"use strict";

const { createHash, createPrivateKey, sign } = require("node:crypto");
const { readFileSync, appendFileSync } = require("node:fs");
const { basename } = require("node:path");

const API = "https://api.appstoreconnect.apple.com";
const APP = "6838367217";
const BUNDLE = "com.dmikey.cara";
const NAME = "Cara-iOS-arm64-GLES3-signed.ipa";
const SHA = "6a966d25036520b4312ff02c75a445db2c938f2981d1e498bb71faec875ec347";
const VERSION = "0.1.0";
const BUILD = "3";
const LIBRARY = "IOS";
const TOKEN_KEYS = ["ASC_KEY", "ASC_ID", "ASC_ISSUER"];

function requireValue(condition, message) {
  if (!condition) throw new Error(message);
}

function issueToken({ key, keyID, issuer }, now) {
  requireValue(/^[A-Z0-9]{10}$/.test(keyID) &&
    /^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i.test(issuer), "Team key metadata is invalid");
  const signingKey = createPrivateKey(key);
  requireValue(signingKey.asymmetricKeyType === "ec" &&
    signingKey.asymmetricKeyDetails.namedCurve === "prime256v1", "Expected a P-256 team key");
  const b64 = (data) => Buffer.from(JSON.stringify(data)).toString("base64url");
  const header = b64({ alg: "ES256", kid: keyID, typ: "JWT" });
  const claims = b64({ iss: issuer, iat: now, exp: now + 900, aud: "appstoreconnect-v1" });
  const payload = `${header}.${claims}`;
  const signature = sign("sha256", Buffer.from(payload), { key: signingKey, dsaEncoding: "ieee-p1363" });
  requireValue(signature.length === 64, "Unexpected ES256 signature width");
  return `${payload}.${signature.toString("base64url")}`;
}

function appleURL(value) {
  let url;
  try {
    url = new URL(value);
  } catch {
    throw new Error("Apple returned an invalid upload URL");
  }
  const allowed = ["apple.com", "mzstatic.com", "icloud-content.com"];
  requireValue(url.protocol === "https:" && !url.username && !url.password &&
    !url.port && !url.hash &&
    allowed.some((domain) => url.hostname === domain || url.hostname.endsWith(`.${domain}`)),
  "Apple returned an upload host outside the verified HTTPS domains");
  return url;
}

function headerSet(pairs, length) {
  requireValue(Array.isArray(pairs), "Apple omitted upload request headers");
  const result = {};
  const names = new Set();
  for (const { name, value } of pairs) {
    requireValue(typeof name === "string" && /^[a-zA-Z0-9-]+$/.test(name) &&
      typeof value === "string" && !/[\r\n]/.test(value), "Invalid upload header");
    const normalized = name.toLowerCase();
    requireValue(!names.has(normalized) &&
      !["authorization", "proxy-authorization", "cookie", "host", "transfer-encoding"].includes(normalized) &&
      (normalized !== "content-length" || value === String(length)), "Unexpected upload header");
    names.add(normalized);
    result[normalized] = value;
  }
  return result;
}

function partsFor(operations, size) {
  requireValue(Array.isArray(operations) && operations.length > 0 && operations.length <= 32,
    "Apple returned an invalid number of upload operations");
  const ordered = [...operations].sort((a, b) => a.offset - b.offset);
  let cursor = 0;
  const result = [];
  for (const part of ordered) {
    const { offset, length, method, url, requestHeaders } = part;
    requireValue(method === "PUT" && Number.isSafeInteger(offset) &&
      Number.isSafeInteger(length) && length > 0 && offset === cursor && length <= size - offset,
    "Apple upload operations do not exactly partition the package");
    cursor += length;
    result.push({ offset, length, url: appleURL(url), headers: headerSet(requestHeaders, length) });
  }
  requireValue(cursor === size, "Apple upload operations leave package bytes uncovered");
  return result;
}

function typed(response, type, stage) {
  const { data } = response;
  requireValue(data?.type === type && typeof data.id === "string" &&
    /^[0-9A-Za-z_-]{1,128}$/.test(data.id), `${stage} returned an invalid resource`);
  return data;
}

function createBody(type, attributes, relation, relatedType, id) {
  return { data: { type, attributes, relationships: { [relation]: { data: { type: relatedType, id } } } } };
}

async function transmit({ ipa, token, fetcher = fetch, journal = () => {} }) {
  async function api(method, path, status, stage, body) {
    let response;
    try {
      response = await fetcher(API + path, {
        method, headers: { Authorization: `Bearer ${token}`, Accept: "application/json",
          ...(body ? { "Content-Type": "application/json" } : {}) },
        body: body ? JSON.stringify(body) : undefined, redirect: "error",
        signal: AbortSignal.timeout(60_000),
      });
    } catch {
      throw new Error(`${stage} request failed without a verified response. Inspect App Store Connect before retrying.`);
    }
    requireValue(response.status === status, `${stage} returned HTTP ${response.status}`);
    try {
      return await response.json();
    } catch {
      throw new Error(`${stage} returned an invalid body. Inspect App Store Connect before retrying.`);
    }
  }

  const app = typed(await api("GET", `/v1/apps/${APP}`, 200, "App identity"), "apps", "App identity");
  requireValue(app.id === APP && app.attributes?.bundleId === BUNDLE, "App identity does not match Cara");

  const previousAttempt = typed(await api("GET",
    "/v1/buildUploads/b80af088-2162-44f7-af1c-ad4df5e0b5c9", 200, "Prior build status"),
    "buildUploads", "Prior build status");
  requireValue(previousAttempt.attributes?.state?.state === "FAILED" &&
    previousAttempt.attributes.cfBundleVersion === "2" &&
    previousAttempt.attributes.state.errors?.some(error => error.code === "90502"),
    "The prior Cara build must have the verified terminal ARM64 validation failure");

  const duplicates = new URLSearchParams({
    "filter[cfBundleShortVersionString]": VERSION, "filter[cfBundleVersion]": BUILD,
    "filter[platform]": LIBRARY,
  });
  const previous = await api("GET", `/v1/apps/${APP}/buildUploads?${duplicates}`, 200, "Upload duplicate check");
  requireValue(Array.isArray(previous.data) && !previous.data.length && !previous.links?.next,
    "This Cara build already has an upload record or duplicate status could not be verified");
  const oldBuild = new URLSearchParams({ "filter[app]": APP, "filter[version]": BUILD });
  const priorBuild = await api("GET", `/v1/builds?${oldBuild}`, 200, "Build duplicate check");
  requireValue(Array.isArray(priorBuild.data) && !priorBuild.data.length && !priorBuild.links?.next,
    "This Cara build number already exists or duplicate status could not be verified");

  const upload = typed(await api("POST", "/v1/buildUploads", 201, "Build upload creation",
    createBody("buildUploads", { cfBundleShortVersionString: VERSION, cfBundleVersion: BUILD, platform: LIBRARY },
      "app", "apps", APP)), "buildUploads", "Build upload creation");
  journal(`Apple build upload created: ${upload.id}`);

  const file = typed(await api("POST", "/v1/buildUploadFiles", 201, "Build file reservation",
    createBody("buildUploadFiles", { assetType: "ASSET", fileName: NAME, fileSize: ipa.length, uti: "com.apple.ipa" },
      "buildUpload", "buildUploads", upload.id)), "buildUploadFiles", "Build file reservation");
  requireValue(file.attributes?.fileName === NAME && file.attributes.fileSize === ipa.length &&
    file.attributes.assetType === "ASSET" && file.attributes.uti === "com.apple.ipa",
  "Apple file reservation differs from the signed Cara package");
  journal(`Apple build file reserved: ${file.id}`);

  const parts = partsFor(file.attributes.uploadOperations, ipa.length);
  for (const [index, part] of parts.entries()) {
    let response;
    try {
      response = await fetcher(part.url, { method: "PUT", headers: part.headers,
        body: ipa.subarray(part.offset, part.offset + part.length), redirect: "error",
        signal: AbortSignal.timeout(75_000) });
    } catch {
      throw new Error(`Apple package part ${index + 1} did not yield a verified response`);
    }
    requireValue(response.status >= 200 && response.status < 300,
      `Apple package part ${index + 1} returned HTTP ${response.status}`);
    if (response.body) await response.body.cancel();
  }
  const hash = createHash("md5").update(ipa).digest("hex");
  const fileResult = typed(await api("PATCH", `/v1/buildUploadFiles/${file.id}`, 200, "Build file commit",
    { data: { type: "buildUploadFiles", id: file.id, attributes: { uploaded: true,
      sourceFileChecksums: { file: { algorithm: "MD5", hash } } } } }),
  "buildUploadFiles", "Build file commit");
  const state = fileResult.attributes?.assetDeliveryState?.state;
  requireValue(fileResult.id === file.id && (state === "UPLOAD_COMPLETE" || state === "COMPLETE"),
    "Apple returned a different or incomplete file commit. Inspect the existing upload in App Store Connect.");
  journal(`Apple accepted Cara ${VERSION} (${BUILD}) for processing. Build file state: ${state}.`);
  return { upload: upload.id, file: file.id, state };
}

async function main() {
  const path = process.argv[2];
  requireValue(process.argv.length === 3 && basename(path) === NAME, "Expected the pinned Cara IPA filename");
  const ipa = readFileSync(path);
  requireValue(createHash("sha256").update(ipa).digest("hex") === SHA,
    "The Cara IPA does not match the verified release hash");
  const [encoded, keyID, issuer] = TOKEN_KEYS.map((key) => process.env[key] || "");
  TOKEN_KEYS.forEach((key) => delete process.env[key]);
  requireValue(/^[A-Za-z0-9+/]+={0,2}$/.test(encoded), "App Store Connect key configuration is incomplete");
  const key = Buffer.from(encoded, "base64");
  try {
    const token = issueToken({ key, keyID, issuer }, Math.floor(Date.now() / 1000));
    await transmit({ ipa, token, journal: (message) => {
      console.log(message);
      if (process.env.GITHUB_STEP_SUMMARY) appendFileSync(process.env.GITHUB_STEP_SUMMARY, message + "\n");
    }});
  } finally {
    key.fill(0);
  }
}

module.exports = { issueToken, appleURL, partsFor, transmit };
if (require.main === module) {
  main().catch((error) => {
    const message = typeof error.message === "string" && error.message.startsWith("error:")
      ? "App Store Connect signing key initialization failed" : error.message;
    console.error(message);
    process.exitCode = 1;
  });
}
