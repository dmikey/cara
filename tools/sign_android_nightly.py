#!/usr/bin/env python3
"""Sign Android nightly packages with the release upload key on the trusted runner."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


KEYSTORE = "CARA_ANDROID_KEYSTORE_BASE64"
STORE_PASSWORD = "CARA_ANDROID_KEYSTORE_PASSWORD"
KEY_ALIAS = "CARA_ANDROID_KEY_ALIAS"
KEY_PASSWORD = "CARA_ANDROID_KEY_PASSWORD"
CERT_SHA = "CARA_ANDROID_UPLOAD_CERT_SHA256"
REQUIRED = (KEYSTORE, STORE_PASSWORD, KEY_ALIAS, KEY_PASSWORD, CERT_SHA)


def digest(path):
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def run(*command, capture=False):
    result = subprocess.run(command, text=True, capture_output=capture)
    if result.returncode:
        raise RuntimeError(f"{Path(command[0]).name} failed with status {result.returncode}")
    return result.stdout


def sign(artifact_dir, output_dir, revision):
    receipt = json.loads((artifact_dir / "android-build.json").read_text())
    if receipt["source_revision"] != revision or receipt["application_id"] != "org.cara.emu":
        raise RuntimeError("Android artifact source or application ID mismatch")
    if receipt["target_sdk"] != 36:
        raise RuntimeError("Expected Android API 36 release packages")
    if set(receipt["assets"]) != {f"Cara-Android-arm64-unsigned.{suffix}" for suffix in ("apk", "aab")}:
        raise RuntimeError("Unexpected unsigned Android asset names")
    for name, sha in receipt["assets"].items():
        if digest(artifact_dir / name) != sha:
            raise RuntimeError(f"Unsigned Android asset hash mismatch: {name}")

    output_dir.mkdir(parents=True, exist_ok=True)
    apk = output_dir / "Cara-Android-arm64-signed.apk"
    bundle = output_dir / "Cara-Android-arm64-signed.aab"
    manifest = output_dir / "android-signed.json"
    if any(path.exists() for path in (apk, bundle, manifest)):
        raise RuntimeError("Choose a fresh Android signing output directory")
    build_tools = Path(os.environ["ANDROID_HOME"]) / "build-tools" / "36.0.0"
    with tempfile.TemporaryDirectory(prefix="cara-android-signing-") as directory:
        keystore = Path(directory) / "upload.keystore"
        try:
            keystore.write_bytes(base64.b64decode("".join(os.environ[KEYSTORE].split()), validate=True))
        except ValueError:
            raise RuntimeError(f"Invalid base64 in {KEYSTORE}") from None
        keystore.chmod(0o600)
        cert = run("keytool", "-exportcert", "-keystore", keystore,
                   "-storepass:env", STORE_PASSWORD, "-alias", os.environ[KEY_ALIAS], "-rfc",
                   capture=True)
        match = re.search(r"-----BEGIN CERTIFICATE-----\s+(.*?)\s+-----END CERTIFICATE-----", cert, re.S)
        if not match:
            raise RuntimeError("Could not identify the Android upload certificate")
        certificate = hashlib.sha256(base64.b64decode("".join(match[1].split()))).hexdigest()

        expected = os.environ[CERT_SHA].replace(":", "").lower()
        if not re.fullmatch(r"[0-9a-f]{64}", expected) or certificate != expected:
            raise RuntimeError("Android keystore does not match the configured upload certificate")

        run(build_tools / "apksigner", "sign", "--ks", keystore, "--ks-key-alias", os.environ[KEY_ALIAS],
            "--ks-pass", f"env:{STORE_PASSWORD}", "--key-pass", f"env:{KEY_PASSWORD}",
            "--v4-signing-enabled", "false", "--out", apk, artifact_dir / "Cara-Android-arm64-unsigned.apk")
        run("jarsigner", "-keystore", keystore, "-storepass:env", STORE_PASSWORD,
            "-keypass:env", KEY_PASSWORD, "-signedjar", bundle,
            artifact_dir / "Cara-Android-arm64-unsigned.aab", os.environ[KEY_ALIAS])
        apk_signers = run(build_tools / "apksigner", "verify", "--print-certs", apk, capture=True)
        if re.findall(r"(?m)^Signer #\d+ certificate SHA-256 digest: ([0-9a-fA-F]+)$", apk_signers) != [certificate]:
            raise RuntimeError("Expected one APK signer matching the configured certificate")
        run(build_tools / "zipalign", "-c", "-P", "16", "4", apk)
        run("jarsigner", "-verify", bundle)
        # keytool reads the JAR signature certificate using the JDK verifier. Match
        # that certificate to the selected keystore alias, as for the APK.
        bundle_signer = run("keytool", "-printcert", "-jarfile", bundle, "-rfc", capture=True)
        signatures = re.findall(r"-----BEGIN CERTIFICATE-----\s+(.*?)\s+-----END CERTIFICATE-----",
                                bundle_signer, re.S)
        fingerprints = {hashlib.sha256(base64.b64decode("".join(s.split()))).hexdigest() for s in signatures}
        if certificate not in fingerprints:
            raise RuntimeError("Android App Bundle has the wrong signer")

    receipt.update(signing="android-upload-key", certificate_sha256=certificate,
                   assets={path.name: digest(path) for path in (apk, bundle)})
    manifest.write_text(json.dumps(receipt, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source-revision", required=True)
    args = parser.parse_args()
    missing = [name for name in REQUIRED if not os.environ.get(name)]
    if missing:
        message = "Android signing unavailable. Missing Actions secrets: " + ", ".join(missing)
        print(message)
        with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a") as summary:
            summary.write(message + "\n")
        raise SystemExit(1)

    sign(args.artifact_dir, args.output, args.source_revision)
    with Path(os.environ["GITHUB_OUTPUT"]).open("a") as output:
        output.write("signed=true\n")


if __name__ == "__main__":
    main()
