"""Render the context-menu icons (gen_menu_icons.py makes the SVGs) into .ico files with headless Edge and make a contact sheet.
Run: python export_menu_icons.py
Writes ../export/menu-icons/<name>.ico and <name>-<size>.png, ../export/menu-icons-sheet.png, and copies the .ico files to app/Assets/MenuIcons.
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from export_icons import render, write_ico  # noqa: E402
from PIL import Image  # noqa: E402

SRC = os.path.join(HERE, '..', 'menu-icons')
OUT = os.path.join(HERE, '..', 'export', 'menu-icons')
APP = os.path.join(HERE, '..', '..', '..', 'app', 'Assets', 'MenuIcons')
os.makedirs(OUT, exist_ok=True)
NAMES = ['extract', 'test', 'images', 'audio', 'encrypt', 'hash', 'template']
SIZES = (16, 20, 24, 32, 48, 64)


def main():
    os.makedirs(APP, exist_ok=True)
    sheet = Image.new('RGBA', (len(NAMES) * 130 + 10, 2 * 140), (0, 0, 0, 0))
    for bi, bg in enumerate(((243, 243, 243, 255), (32, 32, 32, 255))):
        sheet.paste(Image.new('RGBA', (sheet.width, 140), bg), (0, bi * 140))
    for i, name in enumerate(NAMES):
        images = []
        for size in SIZES:
            png = os.path.join(OUT, '%s-%d.png' % (name, size))
            if '--sheet-only' in sys.argv and os.path.exists(png):
                images.append(Image.open(png).convert('RGBA'))
            else:
                images.append(render(os.path.abspath(os.path.join(SRC, 'menu-%s.svg' % name)), size, png))
        write_ico(os.path.join(OUT, name + '.ico'), images)
        shutil.copyfile(os.path.join(OUT, name + '.ico'), os.path.join(APP, name + '.ico'))
        by = {im.width: im for im in images}
        for bi in range(2):
            x, y = 10 + i * 130, bi * 140 + 6
            sheet.alpha_composite(by[64].resize((96, 96), Image.LANCZOS), (x, y))
            xx = x
            for s in (16, 24, 32):
                sheet.alpha_composite(by[s], (xx, y + 100))
                xx += s + 6
        print(name)
    sheet.convert('RGB').save(os.path.join(HERE, '..', 'export', 'menu-icons-sheet.png'))


if __name__ == '__main__':
    main()
