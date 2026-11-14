#!/usr/bin/env python3
"""Run the opt-in native Cara relay client using private SSH-to-stdin credentials.

Only retrieves an EXISTING room from the authorized Cara server. No credential
files, command-line passwords, server changes, or original-save copying.
"""
import argparse, json, os, shlex, subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',required=True)
    parser.add_argument('--rom',required=True)
    parser.add_argument('--user-dir',required=True,help='Dedicated absolute directory; never share between running instances')
    parser.add_argument('--room',default='cara-private')
    parser.add_argument('--ssh-host',default='radadmin@167.99.14.32')
    args=parser.parse_args()
    if not os.path.isabs(args.user_dir):parser.error('--user-dir must be absolute')
    code='import json,sys; rooms=json.load(open("/var/snap/docker/common/cara-relay/private/rooms.json")); room=sys.stdin.readline().strip(); print(json.dumps({"room":room,"password":rooms[room]}))'
    fetch=subprocess.run(['ssh','-o','BatchMode=yes','-o','StrictHostKeyChecking=yes',args.ssh_host,'sudo -n python3 -c '+shlex.quote(code)],input=args.room+'\n',capture_output=True,text=True)
    if fetch.returncode:raise SystemExit('Private room credentials unavailable; no game launched')
    try:config=json.loads(fetch.stdout)
    except (ValueError,TypeError):raise SystemExit('Invalid administrative response; no game launched')
    del fetch
    if not isinstance(config,dict) or set(config)!={'room','password'} or not all(isinstance(v,str) and v for v in config.values()):
        raise SystemExit('Invalid administrative response; no game launched')
    config['endpoint']='wss://carap2p.therad.ninja/relay'
    process=subprocess.Popen([args.binary,args.rom,'--relay-stdin'],stdin=subprocess.PIPE,env={**os.environ,'CARA_USER_DIR':args.user_dir},text=True)
    try:
        process.stdin.write(json.dumps(config)+'\n');process.stdin.close();config.clear()
        return process.wait()
    finally:
        config.clear()
        if process.poll() is None:
            process.terminate()
            try:process.wait(timeout=30)
            except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)

if __name__=='__main__':raise SystemExit(main())
