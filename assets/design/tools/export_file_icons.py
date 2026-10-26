"""Render the file-type icons (gen_file_icon.py makes the SVGs) into .ico files and PNGs with headless Edge.
Sizes <= 32 px use the small version (no document), 48 px and up the full one.
Run: python export_file_icons.py [--small-only]   (--small-only keeps the existing PNGs of the full version)
    Writes ../export/file-icons/<family>.ico and <family>-<size>.png, then copies what the program needs:
    app/Assets/FileIcons/<family>.ico (resources of LycanZip.exe) and <family>-32.png and -64.png (in-app), packaging/assets/FileIcon-<family>.png (file types of the packages the release flow makes)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from export_icons import render, write_ico  # noqa: E402

SRC = os.path.join(HERE, '..', 'file-icons')
OUT = os.path.join(HERE, '..', 'export', 'file-icons')
os.makedirs(OUT, exist_ok=True)
FAMILIES = ['zip', '7z', 'rar', 'tar', 'iso', 'arc', 'comic']
SMALL = (16, 24, 32)
FULL = (48, 64, 96, 128, 256)


def deploy():
    import shutil
    from PIL import Image
    app = os.path.join(HERE, '..', '..', '..', 'app', 'Assets', 'FileIcons')
    pkg = os.path.join(HERE, '..', '..', '..', 'packaging', 'assets')
    os.makedirs(app, exist_ok=True)
    for fam in FAMILIES:
        shutil.copyfile(os.path.join(OUT, fam + '.ico'), os.path.join(app, fam + '.ico'))
        shutil.copyfile(os.path.join(OUT, fam + '-32.png'), os.path.join(app, fam + '-32.png'))
        shutil.copyfile(os.path.join(OUT, fam + '-64.png'), os.path.join(app, fam + '-64.png'))  # the icon view
        shutil.copyfile(os.path.join(OUT, fam + '-256.png'), os.path.join(app, fam + '-256.png'))  # the big cells of the icon view
        if fam == 'iso':  # .iso / .wim are not associated, so the MSIX has no file type for them
            continue
        Image.open(os.path.join(OUT, fam + '-96.png')).convert('RGBA').resize((44, 44), Image.LANCZOS).save(os.path.join(pkg, 'FileIcon-%s.png' % fam))


def main():
    for fam in FAMILIES:
        images = []
        for size in SMALL + FULL:
            svg = os.path.abspath(os.path.join(SRC, 'file-%s%s.svg' % (fam, '-small' if size in SMALL else '')))
            png = os.path.join(OUT, '%s-%d.png' % (fam, size))
            if size in FULL and '--small-only' in sys.argv and os.path.exists(png):
                from PIL import Image
                images.append(Image.open(png).convert('RGBA'))
            else:
                images.append(render(svg, size, png))
        write_ico(os.path.join(OUT, fam + '.ico'), images)
        print(fam, [im.size for im in images])
    deploy()


if __name__ == '__main__':
    main()
