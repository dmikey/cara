#!/usr/bin/env python3
"""Install only the authorized Cara relay on an already verified RadOps host.

Default is a read-only preflight. --apply creates a scoped release, encrypted
configuration backup and dedicated vhost/container. No tenant data is accessed.
Run with sudo, from the audited release directory. Never prints room secrets.
"""
import argparse
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import tarfile
import time
import urllib.request

DOMAIN = 'carap2p.therad.ninja'
BASE = Path('/var/snap/docker/common/cara-relay')
SITE = Path('/etc/nginx/sites-available') / DOMAIN
LINK = Path('/etc/nginx/sites-enabled') / DOMAIN
HOOK = Path('/etc/letsencrypt/renewal-hooks/deploy/cara-relay-nginx')


def run(args, **kwargs):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, **kwargs).stdout


def docker_ids():
    return run(['docker', 'ps', '-aq', '--no-trunc']).decode().splitlines()


def nginx_hashes():
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in Path('/etc/nginx').rglob('*') if p.is_file()}


def public_get(url):
    request = urllib.request.Request(url, headers={'User-Agent': 'cara-relay-deployment/1.0', 'Cache-Control': 'no-cache'})
    with urllib.request.urlopen(request, timeout=15) as response:
        return response.read(8192)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    stage = Path(__file__).resolve().parent
    manifest = json.loads((stage / 'manifest.json').read_text())
    commit = manifest['source_commit']
    if len(commit) != 40 or any(c not in '0123456789abcdef' for c in commit):
        raise RuntimeError('invalid pinned revision')
    for name, expected in manifest['files'].items():
        if Path(name).name != name or hashlib.sha256((stage / name).read_bytes()).hexdigest() != expected:
            raise RuntimeError('release checksum mismatch')
    if os.geteuid() != 0:
        raise RuntimeError('run on the verified host with sudo')
    if SITE.exists() or LINK.exists() or BASE.exists() or HOOK.exists():
        raise RuntimeError('target already exists; use a reviewed update, not initial install')
    if subprocess.run(['docker', 'inspect', 'cara-relay'], stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL).returncode == 0:
        raise RuntimeError('relay container name already in use')
    try:
        sock = socket.create_connection(('127.0.0.1', 24874), timeout=1)
    except ConnectionRefusedError:
        pass
    else:
        sock.close()
        raise RuntimeError('relay loopback port is already occupied')
    run(['nginx', '-t'])
    free = shutil.disk_usage('/').free
    if free < 512 * 1024 * 1024:
        raise RuntimeError('insufficient free space on root filesystem')
    print(json.dumps({'phase': 'preflight', 'revision': commit, 'root_free_bytes': free,
                      'nginx_valid': True, 'new_target_absent': True}), flush=True)
    if not args.apply:
        return
    lock = open('/run/lock/radops-production-backup.lock', 'a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    os.umask(0o077)
    BASE.mkdir(mode=0o700)
    private = BASE / 'private'
    private.mkdir(mode=0o700)
    release = BASE / 'releases' / commit
    release.mkdir(parents=True, mode=0o700)
    for p in stage.iterdir():
        if p.is_file():
            shutil.copy2(p, release / p.name)
    before_ids = docker_ids()
    before_hashes = nginx_hashes()
    receipt = {'revision': commit, 'before_container_ids': before_ids,
               'before_nginx_sha256': before_hashes}
    (BASE / 'preflight-receipt.json').write_text(json.dumps(receipt, indent=2))
    # Only host configuration is affected: no tenant database/writer is touched.
    archive = run(['tar', '-C', '/', '-czf', '-', 'etc/nginx'])
    key = private / 'config-backup.key'
    key.write_text(secrets.token_urlsafe(48) + '\n')
    key.chmod(0o600)
    backup = private / 'nginx-before.tar.gz.gpg'
    run(['gpg', '--batch', '--yes', '--pinentry-mode', 'loopback', '--passphrase-file', str(key),
         '--symmetric', '--cipher-algo', 'AES256', '--output', str(backup)], input=archive)
    restored = run(['gpg', '--batch', '--pinentry-mode', 'loopback', '--passphrase-file', str(key),
                    '--decrypt', str(backup)])
    if hashlib.sha256(restored).digest() != hashlib.sha256(archive).digest():
        raise RuntimeError('encrypted configuration restore verification failed')
    with tarfile.open(fileobj=io.BytesIO(restored), mode='r:gz') as tar:
        if not tar.getmembers():
            raise RuntimeError('configuration backup is empty')
    receipt['encrypted_configuration_backup_verified'] = True
    print('Encrypted affected-configuration backup restored and hash-verified', flush=True)
    marker = '# Cara relay isolated deployment source: ' + commit + '\n'
    image_id = None
    try:
        tag = 'cara-relay:' + commit[:12]
        run(['docker', 'build', '--network=none', '--label', 'org.opencontainers.image.revision=' + commit,
             '--label', 'org.opencontainers.image.source=https://github.com/dmikey/cara',
             '-t', tag, str(release)])
        image_id = run(['docker', 'image', 'inspect', '--format', '{{.Id}}', tag]).decode().strip()
        rooms = private / 'rooms.json'
        rooms.write_text(json.dumps({name: secrets.token_urlsafe(32)
                                    for name in ['cara-private', 'deploy-probe-a', 'deploy-probe-b']}))
        os.chown(rooms, 65532, 65532)
        rooms.chmod(0o400)
        command = ['docker', 'run', '-d', '--name', 'cara-relay', '--restart', 'unless-stopped',
                   '--network', 'host', '--read-only', '--cap-drop', 'ALL', '--memory', '64m',
                   '--cpus', '0.25', '--pids-limit', '48', '--ulimit', 'nofile=256:256',
                   '--log-driver', 'json-file', '--log-opt', 'max-size=2m', '--log-opt', 'max-file=2',
                   '--mount', 'type=bind,src=' + str(rooms) + ',dst=/rooms.json,readonly',
                   '--health-cmd', '/cara-relay -healthcheck http://127.0.0.1:24874/healthz -public-host ' + DOMAIN,
                   '--health-interval', '15s', '--health-timeout', '5s', '--health-retries', '3',
                   '--health-start-period', '5s', image_id,
                   '-listen', '127.0.0.1:24874', '-public-host', DOMAIN,
                   '-rooms-file', '/rooms.json', '-max-peers', '32', '-queue-depth', '64']
        run(command)
        for attempt in range(15):
            health = subprocess.run(['docker', 'exec', 'cara-relay', '/cara-relay', '-healthcheck',
                                     'http://127.0.0.1:24874/healthz', '-public-host', DOMAIN],
                                    capture_output=True)
            if health.returncode == 0:
                break
            time.sleep(1)
        else:
            raise RuntimeError('new isolated relay failed local health')
        SITE.write_text(marker + (release / 'nginx-http.conf').read_text())
        SITE.chmod(0o644)
        LINK.symlink_to(SITE)
        run(['nginx', '-t'])
        run(['systemctl', 'reload', 'nginx'])
        nonce = secrets.token_hex(24)
        challenge = Path('/var/www/letsencrypt/.well-known/acme-challenge') / ('cara-preflight-' + nonce)
        challenge.parent.mkdir(parents=True, exist_ok=True)
        challenge.write_text(nonce)
        challenge.chmod(0o644)
        try:
            for attempt in range(10):
                try:
                    if public_get('http://' + DOMAIN + '/.well-known/acme-challenge/' + challenge.name).decode() == nonce:
                        break
                except Exception:
                    pass
                time.sleep(1)
            else:
                raise RuntimeError('public hostname does not reach the verified origin')
        finally:
            challenge.unlink(missing_ok=True)
        print('Public hostname reaches the verified origin; requesting its scoped certificate', flush=True)
        result = subprocess.run(['certbot', 'certonly', '--non-interactive', '--agree-tos',
                                 '--webroot', '-w', '/var/www/letsencrypt', '--cert-name', DOMAIN, '-d', DOMAIN],
                                capture_output=True, text=True, timeout=180)
        (release / 'certificate-issuance.log').write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError('certificate issuance failed; see private release log')
        SITE.write_text(marker + (release / 'nginx-http.conf').read_text() + '\n' +
                        (release / 'nginx-https.conf').read_text())
        SITE.chmod(0o644)
        shutil.copy2(release / 'renew-certificate.sh', HOOK)
        HOOK.chmod(0o755)
        run(['nginx', '-t'])
        run(['systemctl', 'reload', 'nginx'])
        public_health = json.loads(public_get('https://' + DOMAIN + '/healthz'))
        if public_health.get('status') != 'ok' or public_health.get('service') != 'cara-relay':
            raise RuntimeError('trusted public health response mismatch')
        after_hashes = nginx_hashes()
        if any(after_hashes.get(name) != digest for name, digest in before_hashes.items()):
            raise RuntimeError('an unrelated nginx file changed')
        after_ids = docker_ids()
        relay_id = run(['docker', 'inspect', '--format', '{{.Id}}', 'cara-relay']).decode().strip()
        if set(after_ids) != set(before_ids + [relay_id]):
            raise RuntimeError('unrelated container IDs changed during deployment')
        receipt.update({'image_id': image_id, 'container_id': relay_id,
                        'after_container_ids': after_ids, 'unrelated_nginx_hashes_unchanged': True,
                        'public_health': public_health, 'new_service_loopback_only': True})
        (BASE / 'receipt.json').write_text(json.dumps(receipt, indent=2))
        print(json.dumps({'phase': 'deployed', 'revision': commit, 'image_id': image_id,
                          'public_health': public_health, 'other_containers_unchanged': True}), flush=True)
    except BaseException:
        # Initial install only: no prior Cara service/vhost was overwritten.
        if image_id:
            subprocess.run(['docker', 'rm', '-f', 'cara-relay'], capture_output=True)
        for path in [LINK, SITE, HOOK]:
            if path.is_symlink() or path.exists():
                path.unlink()
        subprocess.run(['nginx', '-t'], check=True, capture_output=True)
        subprocess.run(['systemctl', 'reload', 'nginx'], check=True, capture_output=True)
        receipt.update({'rolled_back': True, 'after_rollback_container_ids': docker_ids(),
                        'after_rollback_nginx_sha256': nginx_hashes()})
        (BASE / 'failed-receipt.json').write_text(json.dumps(receipt, indent=2))
        print('Scoped install rolled back; protected backup/release files retained', flush=True)
        raise


if __name__ == '__main__':
    main()
