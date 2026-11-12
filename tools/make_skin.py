#!/usr/bin/env python3
"""Create Cara's original Carbon Duo skin (requires reportlab)."""
from pathlib import Path
import json
import zipfile
from reportlab.pdfgen import canvas
from PIL import Image, ImageDraw
from reportlab.lib.colors import HexColor, Color

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / 'emulator/src/ios/skins'
W, H = 2420, 1668
TOP = dict(x=610, y=120, width=1200, height=720)
BOTTOM = dict(x=794, y=900, width=832, height=624)


def main():
    DEST.mkdir(parents=True, exist_ok=True)
    pdf = DEST / 'carbon-duo-landscape.pdf'
    c = canvas.Canvas(str(pdf), pagesize=(W, H), pageCompression=1, invariant=1)
    c.setTitle('Cara Carbon Duo - original landscape artwork')
    def box(x, y, w, h, fill, stroke=None, radius=0, line=2):
        c.setFillColor(HexColor(fill)); c.setStrokeColor(HexColor(stroke or fill)); c.setLineWidth(line)
        c.roundRect(x, H-y-h, w, h, radius, stroke=bool(stroke), fill=1)
    def text(label, x, y, size, color='#dce1df', font='Helvetica', center=False):
        c.setFillColor(HexColor(color)); c.setFont(font, size)
        (c.drawCentredString if center else c.drawString)(x, H-y, label)
    def disc(x, y, r, fill, stroke='#35413f', line=3):
        c.setFillColor(HexColor(fill)); c.setStrokeColor(HexColor(stroke)); c.setLineWidth(line)
        c.circle(x, H-y, r, stroke=1, fill=1)
    box(0, 0, W, H, '#000000')
    box(24, 24, W-48, H-48, '#000000', '#17231b', 54)
    box(52, 50, 430, 102, '#080d09', None, 28)
    text('C A R A', 88, 118, 56, '#d8cfb9', 'Helvetica-Bold')
    for f in [TOP, BOTTOM]:
        box(f['x']-18, f['y']-18, f['width']+36, f['height']+36, '#000000', '#26362a', 24, 3)
        box(**dict(x=f['x'], y=f['y'], w=f['width'], h=f['height']), fill='#000000')
    items = []
    def item(inputs, frame, edges=12, **extra):
        items.append(dict(inputs=inputs, frame=frame, extendedEdges=dict(top=edges,bottom=edges,left=edges,right=edges), **extra))
    for label, x, y, key in [('ZL',130,300,'zl'),('L',130,430,'l'),('ZR',2050,300,'zr'),('R',2050,430,'r')]:
        box(x,y,240,96,'#0e1811','#43553d',30,3)
        box(x+16,y+10,208,3,'#bdaa7c')
        text(label,x+120,y+66,45,'#dbd4bf','Helvetica-Bold',True)
        item([key],dict(x=x,y=y,width=240,height=96),18)
    for x,y,r,label,axis in [(310,750,146,'CIRCLE PAD',('circlePadX','circlePadY')),(2110,750,96,'C-STICK',('cStickX','cStickY'))]:
        disc(x,y,r+10,'#050905','#35482f'); disc(x,y,r,'#101b12','#667b52')
        disc(x,y,r*.72,'#0c140e','#2b402b')
        item(dict(x=axis[0],y=axis[1]),dict(x=x-r,y=y-r,width=r*2,height=r*2),14,thumbstick=dict(name='thumbstick.png',width=r*1.2,height=r*1.2))
    # The exact three-by-three D-pad region matches Delta's directional mapping.
    box(165,1095,290,290,'#050905','#273a28',38)
    box(165,1192,290,96,'#142317','#526346',14)
    box(262,1095,96,290,'#142317','#526346',14)
    disc(310,1240,26,'#102013','#354b31',2)
    for points in [[(310,1120),(296,1146),(324,1146)],[(310,1360),(296,1334),(324,1334)],[(190,1240),(216,1226),(216,1254)],[(430,1240),(404,1226),(404,1254)]]:
        path=c.beginPath(); path.moveTo(points[0][0],H-points[0][1])
        for x,y in points[1:]: path.lineTo(x,H-y)
        path.close(); c.setFillColor(HexColor('#a8b08e')); c.drawPath(path,stroke=0,fill=1)
    item(dict(up='up',down='down',left='left',right='right'),dict(x=165,y=1095,width=290,height=290),18)
    for label,x,y in [('X',2110,1090),('A',2260,1240),('B',2110,1390),('Y',1960,1240)]:
        disc(x,y,65,'#070d09','#30442e',4); disc(x,y,57,'#17281a','#758865',2)
        text(label,x,y+23,64,'#e4dcc6','Helvetica-Bold',True)
        item([label.lower()],dict(x=x-60,y=y-60,width=120,height=120),16)
    for label,x,key in [('SELECT',940,'select'),('MENU',1130,'menu'),('START',1320,'start')]:
        box(x,1570,160,60,'#101c13','#3f5238',24,2)
        text(label,x+80,1611,25,'#d6cfba','Helvetica-Bold',True)
        item([key],dict(x=x,y=1570,width=160,height=60),16)
    item(dict(x='touchScreenX',y='touchScreenY'),BOTTOM,0)
    c.showPage(); c.save()
    thumb = Image.new('RGBA', (512, 512), (0,0,0,0))
    draw = ImageDraw.Draw(thumb)
    draw.ellipse((8,8,504,504), fill='#17271b', outline='#657e52', width=8)
    draw.ellipse((45,45,467,467), fill='#1b2f20', outline='#3a5637', width=4)
    thumb.save(DEST/'thumbstick.png')
    rep=dict(assets=dict(resizable=pdf.name), mappingSize=dict(width=W,height=H),items=items,
             screens=[dict(inputFrame=dict(x=0,y=0,width=400,height=240),outputFrame=TOP),dict(inputFrame=dict(x=40,y=240,width=320,height=240),outputFrame=BOTTOM)],translucent=False)
    info=dict(name='Cara Carbon Duo',identifier='com.dmikey.cara.carbon-duo.ipad.landscape.v1',gameTypeIdentifier='com.dmikey.cara.game.3ds',debug=False,representations=dict(ipad=dict(standard=dict(landscape=rep))))
    (DEST/'carbon-duo.json').write_text(json.dumps(info,indent=2)+'\n')
    # Native thumbsticks are a documented Cara extension; no external assets required.
    with zipfile.ZipFile(DEST/'CarbonDuo.deltaskin','w') as z:
        for name, data in [('info.json',json.dumps(info,indent=2).encode()),(pdf.name,pdf.read_bytes()),('thumbstick.png',(DEST/'thumbstick.png').read_bytes())]:
            entry=zipfile.ZipInfo(name,(2026,1,1,0,0,0)); entry.compress_type=zipfile.ZIP_DEFLATED
            entry.external_attr=0o100644<<16; z.writestr(entry,data)
    print(DEST/'CarbonDuo.deltaskin')


if __name__ == '__main__': main()
