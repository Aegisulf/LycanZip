"""Render the SVG logos with headless Edge (transparent background) and build every raster asset the app and the
packages need: PNG sizes, a multi-size .ico (small-icon version <= 32px, full version >= 48px), MSIX tile images.
Run: python export_icons.py
Writes: ../export/  (all sizes)   app/Assets/  (exe icon + in-app logos)   packaging/assets/  (images of the packages the release flow makes)
"""
import os
import shutil
import struct
import subprocess
import tempfile
from io import BytesIO

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
DESIGN = os.path.abspath(os.path.join(HERE, '..'))
ROOT = os.path.abspath(os.path.join(DESIGN, '..', '..'))
EXPORT = os.path.join(DESIGN, 'export')
APP_ASSETS = os.path.join(ROOT, 'app', 'Assets')
PKG_ASSETS = os.path.join(ROOT, 'packaging', 'assets')
for d in (EXPORT, APP_ASSETS, PKG_ASSETS):
    os.makedirs(d, exist_ok=True)

EDGE = next(p for p in [
    os.path.join(os.environ.get('ProgramFiles(x86)', ''), r'Microsoft\Edge\Application\msedge.exe'),
    os.path.join(os.environ.get('ProgramFiles', ''), r'Microsoft\Edge\Application\msedge.exe')] if os.path.exists(p))


def render(svg, size, out_png):
    tmp = tempfile.mkdtemp()
    html = os.path.join(tmp, 'p.html')
    open(html, 'w', encoding='utf-8').write(
        '<!doctype html><style>html,body{margin:0;background:transparent;overflow:hidden}img{display:block}</style>'
        '<img src="file:///%s" width="%d" height="%d">' % (svg.replace('\\', '/'), size, size))
    subprocess.run([EDGE, '--headless=new', '--disable-gpu', '--hide-scrollbars', '--default-background-color=00000000',
                    '--window-size=%d,%d' % (size, size), '--force-device-scale-factor=1',
                    '--screenshot=' + out_png, 'file:///' + html.replace('\\', '/')],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60)
    shutil.rmtree(tmp, ignore_errors=True)
    return Image.open(out_png).convert('RGBA')


def write_ico(path, images):
    """images: list of RGBA PIL images (each a different size). PNG-compressed entries (Vista+)."""
    blobs = []
    for im in images:
        b = BytesIO()
        im.save(b, format='PNG')
        blobs.append((im.width, im.height, b.getvalue()))
    out = struct.pack('<HHH', 0, 1, len(blobs))
    offset = 6 + 16 * len(blobs)
    entries = b''
    data = b''
    for w, h, blob in blobs:
        entries += struct.pack('<BBBBHHII', 0 if w >= 256 else w, 0 if h >= 256 else h, 0, 0, 1, 32, len(blob), offset + len(data))
        data += blob
    open(path, 'wb').write(out + entries + data)


def main():
    icon_svg = os.path.join(DESIGN, 'logo-icon.svg')
    small_svg = os.path.join(DESIGN, 'logo-icon-small.svg')

    full = render(icon_svg, 1024, os.path.join(EXPORT, 'icon-1024.png'))
    sizes = {}
    for s in (16, 20, 24, 32):  # small version rendered natively at each size
        sizes[s] = render(small_svg, s, os.path.join(EXPORT, 'icon-%d.png' % s))
    for s in (40, 44, 48, 50, 64, 128, 150, 256, 512):
        sizes[s] = full.resize((s, s), Image.LANCZOS)
        sizes[s].save(os.path.join(EXPORT, 'icon-%d.png' % s))

    # exe / file icon
    ico_sizes = (16, 24, 32, 48, 64, 128, 256)
    write_ico(os.path.join(EXPORT, 'LycanZip.ico'), [sizes[s] for s in ico_sizes])
    shutil.copy(os.path.join(EXPORT, 'LycanZip.ico'), os.path.join(APP_ASSETS, 'LycanZip.ico'))

    # in-app logos
    sizes[32].save(os.path.join(APP_ASSETS, 'logo-small-32.png'))
    sizes[128].save(os.path.join(APP_ASSETS, 'logo-icon-128.png'))
    sizes[256].save(os.path.join(APP_ASSETS, 'logo-icon-256.png'))   # the start page shows the same icon as the program

    # MSIX / sparse package images
    sizes[50].save(os.path.join(PKG_ASSETS, 'StoreLogo.png'))
    sizes[44].save(os.path.join(PKG_ASSETS, 'Square44x44Logo.png'))
    sizes[44].save(os.path.join(PKG_ASSETS, 'FileIcon.png'))
    sizes[150].save(os.path.join(PKG_ASSETS, 'Square150x150Logo.png'))
    wide = Image.new('RGBA', (310, 150), (0, 0, 0, 0))
    wide.paste(sizes[150], (80, 0), sizes[150])
    wide.save(os.path.join(PKG_ASSETS, 'Wide310x150Logo.png'))
    print('exported:', sorted(os.listdir(EXPORT)), '| app:', sorted(os.listdir(APP_ASSETS)), '| packaging:', sorted(os.listdir(PKG_ASSETS)))


if __name__ == '__main__':
    main()
