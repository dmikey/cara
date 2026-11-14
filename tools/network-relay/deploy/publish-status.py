#!/usr/bin/env python3
"""Publish only Cara's static status page and exact-root nginx location.

Run on the existing relay host from a protected directory beside status.html.
No container restart or private-room access is needed. Default: read-only preflight.
"""
import argparse, datetime, fcntl, hashlib, json, os, subprocess
from pathlib import Path

SITE = Path('/etc/nginx/sites-available/carap2p.therad.ninja')
PUBLIC = Path('/var/www/cara-relay-status/index.html')
BASE = Path('/var/snap/docker/common/cara-relay')
LOCATION = '''    location = / {
        root /var/www/cara-relay-status;
        try_files /index.html =404;
        default_type text/html;
        add_header Cache-Control "no-store";
        add_header X-Content-Type-Options nosniff;
        add_header Strict-Transport-Security "max-age=31536000" always;
    }
'''
def run(args, **kw):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw).stdout

def hashes():
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in Path('/etc/nginx').rglob('*') if p.is_file()}

def containers():
    return sorted(run(['docker', 'ps', '-aq', '--no-trunc']).decode().split())

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--apply', action='store_true');a=ap.parse_args()
    source=Path(__file__).with_name('status.html').read_bytes()
    old=SITE.read_bytes();text=old.decode()
    if 'server_name carap2p.therad.ninja;' not in text or 'location = /relay {' not in text:
        raise RuntimeError('unexpected Cara vhost')
    marker='    location / { return 404; }'
    old_location=LOCATION.replace('        root /var/www/cara-relay-status;\n        try_files /index.html =404;', '        alias /var/www/cara-relay-status/index.html;')
    if old_location in text:text=text.replace(old_location, LOCATION)
    if LOCATION not in text:
        if text.count(marker)!=1 or 'location = / {' in text:raise RuntimeError('unexpected root route')
        text=text.replace(marker, LOCATION+marker)
    run(['nginx','-t'])
    if not a.apply:
        print(json.dumps({'phase':'status_preflight','page_sha256':hashlib.sha256(source).hexdigest()}));return
    lock=open('/run/lock/radops-production-backup.lock','a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    # Re-read after taking the lock: never overwrite concurrent configuration.
    if SITE.read_bytes()!=old:raise RuntimeError('Cara vhost changed during preflight')
    os.umask(0o077);before=hashes();ids=containers()
    stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    backup=BASE/'private'/('status-nginx-'+stamp+'.gpg');key=BASE/'private/config-backup.key'
    archive=run(['tar','-C','/','-czf','-','etc/nginx'])
    run(['gpg','--batch','--yes','--pinentry-mode','loopback','--passphrase-file',str(key),'--symmetric','--cipher-algo','AES256','--output',str(backup)],input=archive)
    restored=run(['gpg','--batch','--pinentry-mode','loopback','--passphrase-file',str(key),'--decrypt',str(backup)])
    if hashlib.sha256(restored).digest()!=hashlib.sha256(archive).digest():raise RuntimeError('backup verification failed')
    previous=PUBLIC.read_bytes() if PUBLIC.exists() else None
    try:
        PUBLIC.parent.mkdir(mode=0o755,parents=True,exist_ok=True);PUBLIC.parent.chmod(0o755)
        PUBLIC.write_bytes(source);PUBLIC.chmod(0o644)
        SITE.write_text(text);run(['nginx','-t']);run(['systemctl','reload','nginx'])
        after=hashes()
        if {k:v for k,v in before.items() if k!=str(SITE) and k!='/etc/nginx/sites-enabled/carap2p.therad.ninja'}!={k:v for k,v in after.items() if k!=str(SITE) and k!='/etc/nginx/sites-enabled/carap2p.therad.ninja'}:
            raise RuntimeError('unrelated nginx configuration changed')
        if ids!=containers():raise RuntimeError('container IDs changed')
        receipt={'phase':'status_published','at_utc':stamp,'page_sha256':hashlib.sha256(source).hexdigest(),'nginx_backup_verified':True,'unrelated_nginx_unchanged':True,'container_ids_unchanged':True,'relay_and_health_locations_unchanged':True}
        (BASE/('status-receipt-'+stamp+'.json')).write_text(json.dumps(receipt,indent=2))
        print(json.dumps(receipt))
    except BaseException:
        SITE.write_bytes(old)
        if previous is None:PUBLIC.unlink(missing_ok=True)
        else:PUBLIC.write_bytes(previous);PUBLIC.chmod(0o644)
        run(['nginx','-t']);run(['systemctl','reload','nginx']);raise
if __name__=='__main__':main()
