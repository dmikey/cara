#!/usr/bin/env python3
"""Run the opt-in native Cara relay client using private SSH-to-stdin credentials.

Only retrieves an EXISTING room from the authorized Cara server. No credential
files, command-line passwords, server changes, or original-save copying.
"""
import argparse, configparser, json, os, shlex, subprocess
from pathlib import Path


def prepare_gpu_config(user_dir, initialize=False):
    """Require an explicit native GPU preset without overwriting existing settings."""
    target=Path(user_dir)/'config/sdl2-config.ini'
    if not target.exists():
        if not initialize:
            raise ValueError('No native GPU configuration. Use --initialize-gpu-config once for this dedicated test directory.')
        target.parent.mkdir(parents=True,exist_ok=True)
        try:
            with target.open('x') as output:
                output.write('[Renderer]\nuse_hw_renderer=true\nuse_hw_shader=true\n')
        except FileExistsError:
            pass  # Concurrent creation is validated below, never replaced.
    if target.is_symlink():raise ValueError('Native GPU configuration must be a regular file.')
    parser=configparser.ConfigParser()
    try:
        with target.open() as source:parser.read_file(source)
        enabled=all(parser.getboolean('Renderer',name,fallback=False)
                    for name in ['use_hw_renderer','use_hw_shader'])
    except (OSError,configparser.Error,ValueError):
        raise ValueError('Cannot validate native GPU configuration; existing settings were kept.') from None
    if not enabled:
        raise ValueError('Native relay tests require use_hw_renderer=true and use_hw_shader=true. Existing settings were kept.')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',required=True)
    parser.add_argument('--rom',required=True)
    parser.add_argument('--user-dir',required=True,help='Dedicated absolute directory; never share between running instances')
    parser.add_argument('--initialize-gpu-config',action='store_true',help='Create an explicit native GPU preset only when this dedicated directory has no config')
    parser.add_argument('--room',default='cara-private')
    parser.add_argument('--ssh-host',default='radadmin@167.99.14.32')
    args=parser.parse_args()
    if not os.path.isabs(args.user_dir):parser.error('--user-dir must be absolute')
    try:prepare_gpu_config(args.user_dir,args.initialize_gpu_config)
    except ValueError as error:raise SystemExit(str(error))
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
