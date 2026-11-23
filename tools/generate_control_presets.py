#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate original enhanced Delta skins and their explicit selection registry.

These are conservative, button-labeled starting layouts, not gameplay-validated
semantic mappings or input macros. No game assets are included.
"""
import argparse
import struct
import zlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / 'emulator/src/ios/ControlPresets'
def png_chunk(kind, data):
    return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
PNG = (b'\x89PNG\r\n\x1a\n'+png_chunk(b'IHDR',struct.pack('>IIBBBBB',2,2,8,6,0,0,0))+
       png_chunk(b'IDAT',zlib.compress(bytes(18)))+png_chunk(b'IEND',b''))


def rect(x, y, width, height):
    return dict(x=x, y=y, width=width, height=height)


def create(name, identifier, filename, title_id=None):
    variants = {}
    representations = {}
    for landscape in (False, True):
        orientation = 'landscape' if landscape else 'portrait'
        width, height = (1200, 900) if landscape else (900, 1200)
        top = rect(0, 0, 1200, 720) if landscape else rect(0, 0, 900, 540)
        bottom = rect(816, 588, 384, 288) if landscape else rect(90, 550, 720, 540)
        controls = []
        # Explicit 3DS labels deliberately avoid assuming a game's action semantics.
        definitions = [('Move', 'floating', 'circle', .02, .64, .25, .28, 'move.3d'),
                       ('A', 'button', 'a', .83, .56, .12, .12, 'a.circle'),
                       ('B', 'button', 'b', .72, .70, .12, .12, 'b.circle'),
                       ('X', 'button', 'x', .72, .42, .12, .12, 'x.circle'),
                       ('Y', 'button', 'y', .60, .56, .12, .12, 'y.circle'),
                       ('L', 'button', 'l', .02, .12, .16, .10, 'scope'),
                       ('R', 'button', 'r', .82, .12, .16, .10, 'bolt.fill'),
                       ('Start', 'button', 'start', .40, .88, .16, .08, 'map')]
        for n, kind, binding, x, y, w, h, glyph in definitions:
            control = dict(id=orientation+'-'+n, label=n, kind=kind, binding=binding,
                           toggle=False, hidden=False, travel=64, glyph=glyph,
                           opacity=.45, frame=rect(x,y,w,h))
            controls.append(control)
        variants[orientation] = controls
        items = [dict(frame=bottom, inputs=dict(x='touchScreenX', y='touchScreenY'))]
        for control in controls:
            frame = control['frame']
            inputs = [control['binding']] if control['kind']=='button' else dict(x='circlePadX', y='circlePadY')
            items.append(dict(inputs=inputs,frame=rect(frame['x']*width,frame['y']*height,frame['width']*width,frame['height']*height)))
        representations[orientation] = dict(mappingSize=dict(width=width,height=height),
                screens=[dict(outputFrame=top),dict(outputFrame=bottom)],items=items,
                assets=dict(resizable='transparent.png'))
    layout = dict(version=2,enabled=True,representations=representations,**variants)
    info = dict(name=name, identifier=identifier, gameTypeIdentifier='com.dmikey.cara.game.3ds',
                representations={device:dict(standard=representations) for device in ('ipad','iphone')},
                caraActionLayout=layout)
    with zipfile.ZipFile(DEST/filename,'w',zipfile.ZIP_DEFLATED) as archive:
        for path, data in [('info.json',json.dumps(info,indent=2).encode()),('transparent.png',PNG)]:
            entry=zipfile.ZipInfo(path,date_time=(2026,11,20,0,0,0)); entry.compress_type=zipfile.ZIP_DEFLATED
            archive.writestr(entry,data)
    return dict(name=name,file=filename,identifier=identifier,title_id=title_id,
                validation='starting-layout; gameplay-unverified')


def generate():
    DEST.mkdir(parents=True,exist_ok=True)
    entries=[]
    for name, slug, title in [('3DS transparent controls','standard',None),
          ("Luigi’s Mansion: Dark Moon · 3DS buttons",'luigi-dark-moon','0004000000055F00'),
          ('Zelda · 3DS buttons','zelda',None),('Xenoblade · 3DS buttons','xenoblade',None)]:
        entries.append(create(name,'com.dmikey.cara.demap.'+slug,slug+'.deltaskin',title))
    (DEST/'registry.json').write_text(json.dumps(entries,indent=2)+'\n')
    (DEST/'NOTICE').write_text('Copyright 2026 Cara contributors. Original controller layouts, GPL-2.0-or-later.\nNo game artwork included. Glyph names refer to system-provided SF Symbols, rendered by iOS.\n')


if __name__=='__main__':
    generate()
