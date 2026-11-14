#!/usr/bin/env python3
"""Update only an existing Cara relay, preserving its private room file and vhost.

Default preflight is read-only. With --apply, retain the prior stopped container
until the new image is healthy; restore only that container on any failure.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
from install import BASE, DOMAIN, run, docker_ids, nginx_hashes


def inspect(name='cara-relay'):
    return json.loads(run(['docker', 'inspect', name]))[0]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply',action='store_true')
    args=parser.parse_args()
    stage=Path(__file__).resolve().parent
    manifest=json.loads((stage/'manifest.json').read_text())
    for name,digest in manifest['files'].items():
        if Path(name).name!=name or hashlib.sha256((stage/name).read_bytes()).hexdigest()!=digest:
            raise RuntimeError('release checksum mismatch')
    commit=manifest['source_commit']
    if len(commit)!=40 or any(c not in '0123456789abcdef' for c in commit):raise RuntimeError('invalid revision')
    receipt=json.loads((BASE/'receipt.json').read_text())
    old=inspect()
    if old['Config']['Labels'].get('org.opencontainers.image.revision')!=receipt['revision']:
        raise RuntimeError('existing Cara service differs from its receipt')
    if old['Config']['User']!='65532:65532' or old['HostConfig']['NetworkMode']!='host':
        raise RuntimeError('unexpected existing service identity')
    if not args.apply:
        print(json.dumps({'phase':'update_preflight','from':receipt['revision'],'to':commit}));return
    lock=open('/run/lock/radops-production-backup.lock','a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    os.umask(0o077)
    old_ids=docker_ids();old_nginx=nginx_hashes()
    rooms=BASE/'private/rooms.json';rooms_hash=hashlib.sha256(rooms.read_bytes()).digest()
    release=BASE/'releases'/commit;release.mkdir(parents=True,mode=0o700)
    for p in stage.iterdir():
        if p.is_file():shutil.copy2(p,release/p.name)
    archive=json.dumps(old,sort_keys=True).encode()
    backup=release/'previous-container.json.gpg';key=BASE/'private/config-backup.key'
    run(['gpg','--batch','--yes','--pinentry-mode','loopback','--passphrase-file',str(key),'--symmetric','--cipher-algo','AES256','--output',str(backup)],input=archive)
    restored=run(['gpg','--batch','--pinentry-mode','loopback','--passphrase-file',str(key),'--decrypt',str(backup)])
    if hashlib.sha256(restored).digest()!=hashlib.sha256(archive).digest():raise RuntimeError('container config backup verification failed')
    tag='cara-relay:'+commit[:12]
    run(['docker','build','--network=none','--label','org.opencontainers.image.revision='+commit,'--label','org.opencontainers.image.source=https://github.com/dmikey/cara','-t',tag,str(release)])
    image=run(['docker','image','inspect','--format','{{.Id}}',tag]).decode().strip()
    health=json.loads(run(['docker','image','inspect',image]))[0]['Config'].get('Healthcheck',{})
    if health.get('Test',[''])[0]!='CMD':raise RuntimeError('scratch image requires an exec-form health check')
    previous='cara-relay-rollback-'+receipt['revision'][:12]
    run(['docker','stop','--time','5','cara-relay']);run(['docker','rename','cara-relay',previous])
    try:
        run(['docker','run','-d','--name','cara-relay','--restart','unless-stopped','--network','host','--read-only','--cap-drop','ALL','--memory','64m','--cpus','0.25','--pids-limit','48','--ulimit','nofile=256:256','--log-driver','json-file','--log-opt','max-size=2m','--log-opt','max-file=2','--mount','type=bind,src='+str(rooms)+',dst=/rooms.json,readonly',image,*old['Config']['Cmd']])
        for attempt in range(35):
            current=inspect()
            if current['State']['Running'] and current['State'].get('Health',{}).get('Status')=='healthy':break
            time.sleep(1)
        else:raise RuntimeError('new Cara relay did not become healthy')
        if rooms_hash!=hashlib.sha256(rooms.read_bytes()).digest() or old_nginx!=nginx_hashes():raise RuntimeError('private config or nginx changed')
        # Keep exactly this prior Cara container stopped for immediate rollback.
        # Its unless-stopped policy prevents a reboot from rebinding the port.
        new_ids=docker_ids()
        if set(new_ids)!=(set(old_ids)|{current['Id']}):raise RuntimeError('unrelated container IDs changed')
        (release/'previous-receipt.json').write_text(json.dumps(receipt,indent=2))
        receipt.update({'previous_revision':receipt['revision'],'revision':commit,'image_id':image,'container_id':current['Id'],'after_container_ids':new_ids,'room_configuration_preserved':True,'docker_exec_health_verified':True,'rollback_container':previous})
        (BASE/'receipt.json').write_text(json.dumps(receipt,indent=2))
        print(json.dumps({'phase':'updated','revision':commit,'image_id':image,'healthy':True,'private_rooms_preserved':True,'nginx_unchanged':True,'unrelated_container_ids_unchanged':True}))
    except BaseException:
        subprocess.run(['docker','rm','-f','cara-relay'],capture_output=True)
        subprocess.run(['docker','rename',previous,'cara-relay'],check=True,capture_output=True)
        subprocess.run(['docker','start','cara-relay'],check=True,capture_output=True)
        raise

if __name__=='__main__':main()
