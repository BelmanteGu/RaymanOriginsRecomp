#!/usr/bin/env python3
"""Packs YOUR OWN game files (private/game) into one .zip that the Android
app's launcher imports ("Import .zip"). For carrying your copy to your own
devices: it contains the game, so never publish or share it.

Entries are stored (no compression: the bundles barely compress and stored
entries extract fast) with Zip64, since the game is over 4 GB.

Usage: python tools/make_game_pack.py [game dir] [out.zip]
"""
import os
import sys
import zipfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
game = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'private', 'game')
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, 'private', 'dist', 'RaymanOrigins-game.zip')

if not os.path.isfile(os.path.join(game, 'default.xex')):
    sys.exit(f'{game}: default.xex not found')
os.makedirs(os.path.dirname(out), exist_ok=True)
files = []
for dirpath, _, names in os.walk(game):
    for name in names:
        path = os.path.join(dirpath, name)
        files.append((path, os.path.relpath(path, game).replace(os.sep, '/')))
total = sum(os.path.getsize(p) for p, _ in files)
done = 0
tmp = out + '.tmp'
with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_STORED, allowZip64=True) as z:
    for path, arc in sorted(files, key=lambda f: f[1]):
        z.write(path, arc)
        done += os.path.getsize(path)
        print(f'\r{done >> 20} / {total >> 20} MB', end='', flush=True)
os.replace(tmp, out)
print(f'\n{out}: {len(files)} files, {os.path.getsize(out) / 2**30:.2f} GB')
