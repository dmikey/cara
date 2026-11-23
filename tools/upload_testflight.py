"""Upload the verified Cara IPA to App Store Connect for TestFlight processing."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from sign_nightly import decode_secret, hash_file


KEY = "CARA_ASC_UPLOAD_KEY_BASE64"
KEY_ID = "CARA_ASC_UPLOAD_KEY_ID"
ISSUER = "CARA_ASC_UPLOAD_ISSUER_ID"
IPA_NAME = "Cara-iOS-arm64-GLES3-signed.ipa"
IPA_POLICY = {
    "signing": "apple-distribution",
    "distribution": "app-store-connect",
    "bundle_id": "com.dmikey.cara",
    "renderer": "gles3",
}


def verify_ipa(ipa, revision):
    if not re.fullmatch(r"[0-9a-f]{40}", revision) or ipa.name != IPA_NAME:
        raise ValueError("Expected the Cara IPA and its full source commit")
    receipt = json.loads(ipa.with_suffix(".ipa.json").read_text())
    if (receipt.get("source_revision") != revision
            or any(receipt.get(key) != value for key, value in IPA_POLICY.items())
            or receipt.get("cpu_jit") is not False or receipt.get("shader_jit") is not False
            or receipt.get("asset_sha256") != hash_file(ipa)):
        raise ValueError("The signed IPA does not match the Cara TestFlight policy")


def upload(ipa, revision):
    verify_ipa(ipa, revision)
    key_id = os.environ.get(KEY_ID, "")
    issuer = os.environ.get(ISSUER, "")
    if not (re.fullmatch(r"[A-Z0-9]{10}", key_id)
            and re.fullmatch(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", issuer)
            and os.environ.get(KEY)):
        raise ValueError("App Store Connect upload key configuration is incomplete")
    with tempfile.TemporaryDirectory(prefix="cara-testflight-") as directory:
        folder = Path(directory)
        decode_secret(KEY, folder / f"AuthKey_{key_id}.p8")
        env = {name: value for name, value in os.environ.items()
               if name not in {KEY, KEY_ID, ISSUER}}
        env["API_PRIVATE_KEYS_DIR"] = str(folder)
        result = subprocess.run([
            "/usr/bin/xcrun", "altool", "--upload-package", ipa,
            "--api-key", key_id, "--api-issuer", issuer, "--output-format", "json",
        ], cwd=folder, env=env, timeout=1500)
        if result.returncode:
            raise RuntimeError(f"Apple rejected the IPA upload (altool exit {result.returncode})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ipa", type=Path)
    parser.add_argument("revision")
    args = parser.parse_args()
    upload(args.ipa.resolve(), args.revision)
    message = "App Store Connect accepted the Cara upload for processing."
    print(message)
    if summary := os.getenv("GITHUB_STEP_SUMMARY"):
        with open(summary, "a") as stream:
            stream.write(message + "\n")


if __name__ == "__main__":
    main()
