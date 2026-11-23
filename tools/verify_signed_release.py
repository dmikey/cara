"""Check the complete signed package set before attaching any release assets."""
import argparse
import hashlib
import json
from pathlib import Path
import re


APPLE = {
    "Cara-macOS-arm64-JIT-signed.zip": {
        "signing": "developer-id", "notarized": True, "self_contained": True, "cpu_jit": True,
    },
    "Cara-iOS-arm64-GLES3-signed.ipa": {
        "signing": "apple-distribution", "distribution": "app-store-connect", "cpu_jit": False,
    },
}
ANDROID = {"Cara-Android-arm64-signed.apk", "Cara-Android-arm64-signed.aab"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def verify_digest(path, expected):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    require(digest.hexdigest() == expected, f"Package digest mismatch: {path.name}")


def verify(directory, revision, upload_cert):
    require(re.fullmatch(r"[0-9a-f]{40}", revision), "Invalid source commit")
    cert = upload_cert.lower().replace(":", "")
    require(re.fullmatch(r"[0-9a-f]{64}", cert), "Invalid Android upload certificate pin")
    names = {name + extension for name in APPLE for extension in ("", ".json")}
    names.update(ANDROID | {"android-signed.json"})
    require({p.name for p in directory.iterdir()} == names, "Unexpected signed asset set")
    require(all(p.is_file() and not p.is_symlink() for p in directory.iterdir()),
            "Every asset must be an ordinary file")

    versions = set()
    for name, requirements in APPLE.items():
        receipt = json.loads((directory / f"{name}.json").read_text())
        require(receipt["source_revision"] == revision, f"Source mismatch: {name}")
        require(receipt["shader_jit"] is False, f"Unexpected shader JIT: {name}")
        require(all(type(receipt.get(key)) is type(value) and receipt[key] == value
                    for key, value in requirements.items()), f"Signing policy mismatch: {name}")
        require(re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", receipt["build_version"]),
                f"Invalid Apple build version: {name}")
        verify_digest(directory / name, receipt["asset_sha256"])
        versions.add(receipt["build_version"])
    require(len(versions) == 1, "Apple build versions differ")

    android = json.loads((directory / "android-signed.json").read_text())
    require(android["source_revision"] == revision, "Android source mismatch")
    require(android["signing"] == "android-upload-key" and android["certificate_sha256"] == cert,
            "Unexpected Android upload signer")
    require(android["application_id"] == "org.cara.emu" and android["target_sdk"] == 36
            and android["architecture"] == "arm64-v8a" and android["renderer"] == "gles-atlas"
            and android["cpu_jit"] is True and android["shader_jit"] is False,
            "Unexpected Android build policy")
    require(type(android["version_code"]) is int and 0 < android["version_code"] <= 2_100_000_000,
            "Invalid Android version code")
    require(set(android["assets"]) == ANDROID, "Unexpected Android package names")
    for name, checksum in android["assets"].items():
        verify_digest(directory / name, checksum)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--android-cert-sha256", required=True)
    args = parser.parse_args()
    verify(args.directory, args.source_revision, args.android_cert_sha256)


if __name__ == "__main__":
    main()
