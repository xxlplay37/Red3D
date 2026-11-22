#!/usr/bin/env python3
"""Génère romfs/emoji.bin (atlas RGBA8 tuilé pour la 3DS) et source/emoji_data.h
à partir de NotoColorEmoji.ttf.  Usage : python3 tools/make_emoji_atlas.py [chemin_police.ttf]"""
import sys, os
import numpy as np
from PIL import Image, ImageFont, ImageDraw
from fontTools.ttLib import TTFont

FONT = sys.argv[1] if len(sys.argv) > 1 else '/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf'
CELL, TW, TH = 18, 1024, 512
COLS, ROWS = TW // CELL, TH // CELL
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

cmap = TTFont(FONT, fontNumber=0).getBestCmap()

def wanted(cp):
    if cp < 0x203C: return False
    if 0x2190 <= cp <= 0x21FF and cp not in (0x21A9, 0x21AA): return False   # flèches texte : déjà dans la police 3DS
    if 0x1F1E6 <= cp <= 0x1F1FF: return False      # drapeaux : affichés en lettres
    if 0x1F3FB <= cp <= 0x1F3FF: return False      # teintes de peau
    if 0xE0020 <= cp <= 0xE007F: return False      # étiquettes
    if cp in (0xFE0F, 0xFE0E, 0x200D, 0x20E3, 0x2122, 0xA9, 0xAE): return False
    return True

cps = sorted(cp for cp in cmap if wanted(cp))
print("emoji candidats :", len(cps), "/ places :", COLS * ROWS)
cps = cps[:COLS * ROWS]

font = ImageFont.truetype(FONT, 109)
atlas = np.zeros((TH, TW, 4), dtype=np.uint8)
inner = CELL - 2
for i, cp in enumerate(cps):
    canvas = Image.new('RGBA', (160, 160), (0, 0, 0, 0))
    ImageDraw.Draw(canvas).text((10, 10), chr(cp), font=font, embedded_color=True)
    bbox = canvas.getbbox()
    if not bbox:
        continue
    g = canvas.crop((10, 10, 146, 138))                # case de la police : 136x128
    a = np.asarray(g, dtype=np.float32)
    a[..., :3] *= a[..., 3:4] / 255.0                  # alpha prémultiplié avant réduction
    pm = Image.fromarray(a.astype(np.float32), 'RGBA') if False else None
    # réduction canal par canal (float) pour éviter les halos
    chans = [Image.fromarray(a[..., k], 'F').resize((inner, inner), Image.LANCZOS) for k in range(4)]
    r = np.stack([np.asarray(c) for c in chans], axis=-1)
    alpha = np.clip(r[..., 3], 0, 255)
    rgb = np.where(alpha[..., None] > 0.5, r[..., :3] / np.maximum(alpha[..., None], 1) * 255.0, 0)
    out = np.dstack([np.clip(rgb, 0, 255), alpha]).astype(np.uint8)
    cx, cy = (i % COLS) * CELL + 1, (i // COLS) * CELL + 1
    atlas[cy:cy + inner, cx:cx + inner] = out

# --- tuilage PICA200 (identique à tile_index() de image.c) : u32 = R<<24 | G<<16 | B<<8 | A
ys, xs = np.mgrid[0:TH, 0:TW]
tile = ((((ys >> 3) * (TW >> 3) + (xs >> 3)) << 6) +
        ((xs & 1) | ((ys & 1) << 1) | ((xs & 2) << 1) | ((ys & 2) << 2) | ((xs & 4) << 2) | ((ys & 4) << 3)))
px = (atlas[..., 0].astype(np.uint32) << 24) | (atlas[..., 1].astype(np.uint32) << 16) | \
     (atlas[..., 2].astype(np.uint32) << 8) | atlas[..., 3].astype(np.uint32)
flat = np.zeros(TW * TH, dtype='<u4')
flat[tile.ravel()] = px.ravel()
open(os.path.join(ROOT, 'romfs', 'emoji.bin'), 'wb').write(flat.tobytes())

with open(os.path.join(ROOT, 'source', 'emoji_data.h'), 'w') as f:
    f.write('/* Généré par tools/make_emoji_atlas.py : ne pas modifier à la main */\n#pragma once\n')
    f.write('#define EMOJI_COUNT %d\n#define EMOJI_CELL %d\n#define EMOJI_TW %d\n#define EMOJI_TH %d\n#define EMOJI_COLS %d\n'
            % (len(cps), CELL, TW, TH, COLS))
    f.write('static const unsigned int EMOJI_CP[EMOJI_COUNT] = {')
    for i, cp in enumerate(cps):
        f.write(('\n    ' if i % 10 == 0 else ' ') + '0x%X,' % cp)
    f.write('\n};\n')
print("atlas :", len(cps), "emoji ->", os.path.getsize(os.path.join(ROOT, 'romfs', 'emoji.bin')), "octets")
