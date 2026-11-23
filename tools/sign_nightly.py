#!/usr/bin/env python3
"""Sign the audited Cara release products on a trusted GitHub Actions runner."""

import argparse
import base64
import contextlib
import datetime
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import secrets
import shutil
import stat
import subprocess
import tempfile
import zipfile

import mac_bundle


MAC_VARIABLES = (
    'CARA_MAC_CERTIFICATE_BASE64', 'CARA_MAC_CERTIFICATE_PASSWORD',
    'CARA_NOTARY_KEY_BASE64', 'CARA_NOTARY_KEY_ID', 'CARA_NOTARY_ISSUER_ID',
)
IOS_VARIABLES = (
    'CARA_IOS_CERTIFICATE_BASE64', 'CARA_IOS_CERTIFICATE_PASSWORD',
    'CARA_IOS_APP_STORE_PROFILE_BASE64',
)
SECURITY = '/usr/bin/security'
CODESIGN = '/usr/bin/codesign'
XCRUN = '/usr/bin/xcrun'


def run(*args, capture=False):
    # Keep commands containing ephemeral passwords out of exception messages.
    proc = subprocess.run(args, check=False, capture_output=capture)
    if proc.returncode:
        raise RuntimeError(f'{Path(args[0]).name} {args[1]} failed with status {proc.returncode}')
    return proc.stdout


def hash_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as file:
        for block in iter(lambda: file.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def read_plist(path):
    return plistlib.loads(path.read_bytes())


def write_plist(path, data):
    path.write_bytes(plistlib.dumps(data))


def decode_secret(name, path):
    try:
        path.write_bytes(base64.b64decode(''.join(os.environ[name].split()), validate=True))
    except ValueError:
        raise RuntimeError(f'Invalid base64 in {name}') from None
    path.chmod(0o600)


@contextlib.contextmanager
def signing_keychain(folder, cert_variable, password_variable, prefixes, allowed=None):
    password = secrets.token_urlsafe(32)
    keychain = folder / 'cara.keychain-db'
    certificate = folder / 'signing.p12'
    decode_secret(cert_variable, certificate)
    run(SECURITY, 'create-keychain', '-p', password, keychain)
    try:
        run(SECURITY, 'set-keychain-settings', '-lut', '3600', keychain)
        run(SECURITY, 'unlock-keychain', '-p', password, keychain)
        run(SECURITY, 'import', certificate, '-f', 'pkcs12', '-k', keychain,
            '-P', os.environ[password_variable], '-T', CODESIGN, capture=True)
        run(SECURITY, 'set-key-partition-list', '-S', 'apple-tool:,apple:', '-s',
            '-k', password, keychain, capture=True)
        output = run(SECURITY, 'find-identity', '-v', '-p', 'codesigning', keychain, capture=True)
        candidates = [
            digest for digest, label in re.findall(
                r'^\s*\d+\)\s+([0-9A-F]{40})\s+"([^"]+)"', output.decode(), re.M)
            if label.startswith(prefixes) and (allowed is None or digest in allowed)
        ]
        if len(candidates) != 1:
            raise RuntimeError('Expected exactly one valid signing identity for this product')
        yield keychain, candidates[0]
    finally:
        run(SECURITY, 'delete-keychain', keychain)


def sign(path, keychain, identity, *, mac, entitlements=None):
    command = [CODESIGN, '--force', '--keychain', keychain, '--sign', identity]
    command.extend(('--timestamp', '--options', 'runtime') if mac else ('--timestamp=none',))
    if entitlements is not None:
        command.extend(('--entitlements', entitlements))
    run(*command, path)


def signing_entitlements(path):
    data = run(CODESIGN, '-d', '--entitlements', '-', '--xml', path, capture=True)
    return plistlib.loads(data)


def mac_zip(app, out):
    run('/usr/bin/ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', app, out)


def mac_package(app, audit, out, revision, version, folder):
    resources = app / 'Contents/Resources'
    manifest_path = resources / 'build.json'
    manifest = json.loads(manifest_path.read_text())
    core = app / 'Contents/MacOS/cara-core'
    if (manifest.get('source_revision') != revision or manifest.get('self_contained') is not True
            or manifest.get('cpu_jit_available') is not True or manifest.get('shader_jit') is not False
            or manifest.get('binary_sha256') != hash_file(core) or audit.get('passed') is not True
            or audit.get('jit_available') is not True):
        raise RuntimeError('Expected the audited, self-contained CPU-JIT app from this commit')
    info_path = app / 'Contents/Info.plist'
    info = read_plist(info_path)
    info['CFBundleVersion'] = version
    write_plist(info_path, info)
    entitlements = resources / 'entitlements.plist'
    if read_plist(entitlements) != {'com.apple.security.cs.allow-jit': True}:
        raise RuntimeError('Unexpected Mac core entitlements')

    with signing_keychain(folder, *MAC_VARIABLES[:2], ('Developer ID Application: ',)) as identity:
        for library in sorted((app / 'Contents/Frameworks').glob('*.dylib')):
            sign(library, *identity, mac=True)
        sign(core, *identity, mac=True, entitlements=entitlements)
        if signing_entitlements(core) != {'com.apple.security.cs.allow-jit': True}:
            raise RuntimeError('Final Mac core signature lost the CPU JIT entitlement')
        manifest['binary_sha256'] = hash_file(core)
        manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
        (resources / 'bundle-audit.json').write_text(json.dumps(mac_bundle.audit(app), indent=2) + '\n')
        sign(app, *identity, mac=True)
        run(CODESIGN, '--verify', '--deep', '--strict', app)

    key = folder / 'notary.p8'
    decode_secret(MAC_VARIABLES[2], key)
    submission = folder / 'notarization.zip'
    mac_zip(app, submission)
    notary = json.loads(run(XCRUN, 'notarytool', 'submit', submission, '--key', key,
                            '--key-id', os.environ[MAC_VARIABLES[3]], '--issuer', os.environ[MAC_VARIABLES[4]],
                            '--wait', '--timeout', '20m', '--output-format', 'json', capture=True))
    key.unlink()
    if notary.get('status') != 'Accepted':
        raise RuntimeError(f"Apple notarization was not accepted (submission {notary.get('id', 'unknown')})")
    run(XCRUN, 'stapler', 'staple', app)
    run(XCRUN, 'stapler', 'validate', app)
    run(CODESIGN, '--verify', '--deep', '--strict', app)
    run('/usr/sbin/spctl', '--assess', '--type', 'execute', app)
    # Prove the final signed executable still launches using only its bundled dependencies.
    home = folder / 'home'
    home.mkdir()
    subprocess.run([app / 'Contents/MacOS/Cara', '--verify-package'], check=True, timeout=20,
                   env={'HOME': str(home), 'TMPDIR': str(home), 'PATH': '/usr/bin:/bin:/usr/sbin:/sbin'})
    if any(home.iterdir()):
        raise RuntimeError('Mac package check wrote user data')
    mac_zip(app, out)
    return dict(signing='developer-id', notarized=True, self_contained=True, cpu_jit=True, shader_jit=False)


def app_store_entitlements(profile, bundle_id, now):
    # Public release assets must never embed a list of registered devices.
    if 'ProvisionedDevices' in profile or 'ProvisionsAllDevices' in profile:
        raise ValueError('Use an App Store distribution profile with no device identifiers')
    expires = profile.get('ExpirationDate')
    # XML plist dates are returned without timezone information by Python 3.12.
    if not isinstance(expires, datetime.datetime) or expires.replace(tzinfo=datetime.timezone.utc) <= now:
        raise ValueError('App Store provisioning profile is expired or missing its expiration date')
    entitlements = profile.get('Entitlements', {})
    team = entitlements.get('com.apple.developer.team-identifier')
    prefix = profile.get('ApplicationIdentifierPrefix', [])
    app_id = entitlements.get('application-identifier')
    if (profile.get('Platform') != ['iOS'] or not team or profile.get('TeamIdentifier') != [team]
            or app_id not in {f'{value}.{bundle_id}' for value in prefix}
            or entitlements.get('get-task-allow') is not False
            or entitlements.get('beta-reports-active') is not True):
        raise ValueError('Profile must allow App Store distribution for this exact iOS bundle')
    allowed_groups = entitlements.get('keychain-access-groups', [])
    if app_id not in allowed_groups and not any(value + '.*' in allowed_groups for value in prefix):
        raise ValueError('Profile does not authorize the app keychain group')
    return {
        'application-identifier': app_id,
        'com.apple.developer.team-identifier': team,
        'get-task-allow': False,
        'beta-reports-active': True,
        'keychain-access-groups': [app_id],
    }


def ios_package(app, audit, out, revision, version, folder):
    info_path = app / 'Info.plist'
    info = read_plist(info_path)
    name = info['CFBundleExecutable']
    if Path(name).name != name:
        raise RuntimeError('Invalid iOS executable name')
    if (audit.get('source_revision') != revision or audit.get('renderer') != 'gles3'
            or audit.get('unsigned') is not True or audit.get('requires_resigning') is not True
            or audit.get('bundle_id') != info.get('CFBundleIdentifier')
            or audit.get('executable_sha256') != hash_file(app / name)):
        raise RuntimeError('Expected the audited iOS GLES3 no-JIT app from this commit')
    paths = list(app.rglob('*'))
    if any(p.is_symlink() or p.suffix in {'.framework', '.dylib', '.appex', '.app', '.mobileprovision'}
           for p in paths):
        raise RuntimeError('iOS app contains unexpected nested code, a profile, or a symlink')

    profile_path = folder / 'app-store.mobileprovision'
    decode_secret(IOS_VARIABLES[2], profile_path)
    profile = plistlib.loads(run(SECURITY, 'cms', '-D', '-i', profile_path, capture=True))
    requested = app_store_entitlements(profile, info['CFBundleIdentifier'],
                                       datetime.datetime.now(datetime.timezone.utc))
    allowed = {hashlib.sha1(cert, usedforsecurity=False).hexdigest().upper()
               for cert in profile['DeveloperCertificates']}
    info['CFBundleVersion'] = version
    write_plist(info_path, info)
    entitlements = folder / 'ios.entitlements'
    write_plist(entitlements, requested)
    shutil.copyfile(profile_path, app / 'embedded.mobileprovision')
    with signing_keychain(folder, *IOS_VARIABLES[:2], ('Apple Distribution: ', 'iPhone Distribution: '),
                          allowed) as identity:
        sign(app, *identity, mac=False, entitlements=entitlements)
    if signing_entitlements(app) != requested:
        raise RuntimeError('Final iOS signature does not match the requested distribution entitlements')
    run(CODESIGN, '--verify', '--deep', '--strict', app)
    with zipfile.ZipFile(out, 'x', compression=zipfile.ZIP_DEFLATED) as archive:
        for file in sorted(app.rglob('*')):
            if file.is_file():
                archive.write(file, 'Payload/Cara.app/' + file.relative_to(app).as_posix())
    with zipfile.ZipFile(out) as archive:
        if archive.testzip() is not None or not archive.getinfo('Payload/Cara.app/' + name).external_attr & (0o100 << 16):
            raise RuntimeError('IPA failed its archive or executable mode check')
    return dict(signing='apple-distribution', distribution='app-store-connect', cpu_jit=False,
                shader_jit=False, bundle_id=info['CFBundleIdentifier'], renderer='gles3')


def safe_archive(archive, roots):
    with zipfile.ZipFile(archive) as zipped:
        seen = set()
        for entry in zipped.infolist():
            parts = entry.filename.removesuffix('/').split('/')
            normalized = '/'.join(parts).casefold()
            mode = entry.external_attr >> 16
            if (parts[0] not in roots or any(p in {'', '.', '..'} for p in parts)
                    or normalized in seen or stat.S_ISLNK(mode)
                    or '\\' in entry.filename):
                raise ValueError('Unexpected path in unsigned package')
            seen.add(normalized)


def unpack_unsigned(platform, artifacts, folder, revision):
    if platform == 'mac':
        archive = artifacts / 'Cara-macOS-arm64-JIT.zip'
        audit = json.loads((artifacts / 'mac-release-audit.json').read_text())
        paths = {'Cara.app', '__MACOSX'}
        app = folder / 'Cara.app'
    else:
        archive = artifacts / 'Cara-iOS-arm64-GLES3-unsigned.ipa'
        audit = json.loads(archive.with_suffix('.ipa.json').read_text())
        if audit.get('source_revision') != revision or audit.get('ipa_sha256') != hash_file(archive):
            raise RuntimeError('Unsigned IPA differs from its audited package')
        paths = {'Payload'}
        app = folder / 'Payload/Cara.app'
    safe_archive(archive, paths)
    folder.mkdir()
    run('/usr/bin/ditto', '-x', '-k', archive, folder)
    if platform == 'mac':
        manifest = app / 'Contents/Resources/build.json'
        if manifest.read_bytes() != (artifacts / 'mac-build.json').read_bytes():
            raise RuntimeError('Unsigned Mac app differs from its package manifest')
        run(CODESIGN, '--verify', '--deep', '--strict', app)
    return app, audit


def build_version(run_number):
    major, tail = divmod(run_number, 10_000)
    minor, patch = divmod(tail, 100)
    return f'{1200 + major}.{minor}.{patch}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('platform', choices=['mac', 'ios'])
    parser.add_argument('--artifact-dir', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--source-revision', required=True)
    args = parser.parse_args()
    required = MAC_VARIABLES if args.platform == 'mac' else IOS_VARIABLES
    missing = [key for key in required if not os.environ.get(key)]
    if missing:
        message = f'{args.platform} signing unavailable. Missing Actions secrets: {", ".join(missing)}'
        print(message)
        if summary := os.environ.get('GITHUB_STEP_SUMMARY'):
            with open(summary, 'a') as file:
                file.write(message + '\n\n')
        raise SystemExit(1)
    revision = args.source_revision
    if not re.fullmatch(r'[0-9a-f]{40}', revision):
        raise ValueError('Expected a full source commit SHA')
    version = build_version(int(os.environ['GITHUB_RUN_NUMBER']))
    output = args.output.resolve()
    if output.exists() or output.with_suffix(output.suffix + '.json').exists():
        raise RuntimeError('Choose a fresh signed release output')
    output.parent.mkdir(parents=True, exist_ok=True)
    package = mac_package if args.platform == 'mac' else ios_package
    with tempfile.TemporaryDirectory(prefix='cara-signing-') as temporary:
        folder = Path(temporary)
        app, audit = unpack_unsigned(args.platform, args.artifact_dir.resolve(), folder / 'unsigned', revision)
        metadata = package(app, audit, output, revision, version, folder)
    metadata.update(source_revision=revision, build_version=version, asset_sha256=hash_file(output))
    output.with_suffix(output.suffix + '.json').write_text(json.dumps(metadata, indent=2) + '\n')
    with open(os.environ['GITHUB_OUTPUT'], 'a') as file:
        file.write('signed=true\n')


if __name__ == '__main__':
    main()
