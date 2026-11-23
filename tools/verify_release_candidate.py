"""Check the complete Mac, iOS and Android package set before publishing a numbered candidate."""
import argparse
import hashlib
import json
from pathlib import Path
import re


MAC = "Cara-macOS-arm64-JIT-signed.zip"
IOS = "Cara-iOS-arm64-GLES3-unsigned.ipa"
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
    names = ANDROID | {"android-signed.json", MAC, f"{MAC}.json", IOS, f"{IOS}.json"}
    require({p.name for p in directory.iterdir()} == names, "Unexpected release candidate asset set")
    require(all(p.is_file() and not p.is_symlink() for p in directory.iterdir()),
            "Every asset must be an ordinary file")

    mac = json.loads((directory / f"{MAC}.json").read_text())
    require(mac["source_revision"] == revision, f"Source mismatch: {MAC}")
    require(mac.get("signing") == "developer-id" and mac.get("notarized") is True
            and mac.get("self_contained") is True and mac.get("cpu_jit") is True
            and mac.get("shader_jit") is False, "Unexpected macOS signing policy")
    require(re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", mac["build_version"]),
            "Invalid macOS build version")
    verify_digest(directory / MAC, mac["asset_sha256"])

    ios = json.loads((directory / f"{IOS}.json").read_text())
    require(ios["source_revision"] == revision, f"Source mismatch: {IOS}")
    require(ios.get("unsigned") is True and ios.get("requires_resigning") is True
            and ios.get("renderer") == "gles3" and ios.get("bundle_id") == "com.dmikey.cara",
            "Unexpected iOS package policy")
    verify_digest(directory / IOS, ios["ipa_sha256"])

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
