#!/usr/bin/env python3
"""Show an existing Cara room privately in the user's own terminal.

Requires an interactive terminal, authenticated administrative SSH, and an
explicit keypress before revealing. Uses an alternate screen cleared on exit;
never run this through a captured tool output or redirect it to a file.
"""
import argparse, curses, json, shlex, subprocess, sys, textwrap


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--room',default='cara-private')
    parser.add_argument('--ssh-host',default='radadmin@167.99.14.32')
    args=parser.parse_args()
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        raise SystemExit('Open this helper directly in your own Terminal; captured output is refused.')
    code='import json,sys; rooms=json.load(open("/var/snap/docker/common/cara-relay/private/rooms.json")); room=sys.stdin.readline().strip(); print(json.dumps({"room":room,"password":rooms[room]}))'
    result=subprocess.run(['ssh','-o','BatchMode=yes','-o','StrictHostKeyChecking=yes',args.ssh_host,'sudo -n python3 -c '+shlex.quote(code)],input=args.room+'\n',capture_output=True,text=True)
    if result.returncode:raise SystemExit('Private room credentials unavailable.')
    try:config=json.loads(result.stdout)
    except ValueError:raise SystemExit('Invalid administrative response.')
    del result
    if not isinstance(config,dict) or set(config)!={'room','password'} or not all(isinstance(v,str) and v for v in config.values()):
        raise SystemExit('Invalid administrative response.')
    def screen(window):
        window.erase();window.addstr(0,0,'Cara private room. Press Enter to reveal, or any other key to cancel.')
        window.refresh()
        if window.getch() not in (10,13):return
        window.erase();height,width=window.getmaxyx()
        text=('In Cara on your iPad: Settings > Local multiplayer > Private room.\n\n'
              'Server: wss://carap2p.therad.ninja/relay\nRoom: '+config['room']+'\nPassword: '+config['password']+
              '\n\nEnter these in Cara, then choose Join room and wait for Connected.\nPress any key here to hide and exit. This screen closes after two minutes.')
        lines=[]
        for paragraph in text.split('\n'):lines.extend(textwrap.wrap(paragraph,max(10,width-2)) or [''])
        for row,line in enumerate(lines[:max(0,height-1)]):window.addnstr(row,0,line,max(0,width-1))
        window.timeout(120000);window.refresh();window.getch();window.erase();window.refresh()
    try:curses.wrapper(screen)
    finally:config.clear()

if __name__=='__main__':main()
