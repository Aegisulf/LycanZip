"""Draft generator for the file-type icons (an archive document with the LycanZip wolf, a format label and the wolf's tail).
Run: python gen_file_icon.py        Writes ../file-icons/file-<fmt>.svg and file-<fmt>-small.svg
The full version is a night-sky document (folded corner, stars), the wolf on the left, a symbol of the format on the right,
the extension label bottom left and the tail continuing the label to the right. The small version (<= 32 px) has no
document: the wolf, the label and a simple tail only.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'file-icons')
os.makedirs(OUT, exist_ok=True)
WOLF = open(os.path.join(HERE, '..', 'wolf-silhouette.path.txt'), encoding='utf-8').read().strip()
# silhouette bounds: x 100..907, y 106..864 (head looking right)

FORMATS = {
    'zip': dict(label='ZIP', main='#3aa0ff', dark='#0b5cc0', symbol='zipper'),
    '7z': dict(label='7Z', main='#34c27a', dark='#0e7a46', symbol='cube'),
    'rar': dict(label='RAR', main='#ff8f3f', dark='#c2540a', symbol='layers'),
    'tar': dict(label='TAR', main='#f2b705', dark='#a67a00', symbol='bundle'),
    'iso': dict(label='ISO', main='#b072ff', dark='#6a2fc0', symbol='disc'),
    'arc': dict(label='ARC', main='#aac0da', dark='#5f7a9c', symbol='box'),
    'comic': dict(label='CB', main='#4a4f5a', dark='#050507', symbol='comic'),   # cbz / cbr / cb7 / cbt: comic book archives, deep black
}


def wolf(x, y, w, fill, extra=''):
    s = w / 807.0
    return '<g transform="translate(%.2f %.2f) scale(%.4f) translate(-100 -106)"><path d="%s" fill="%s" %s/></g>' % (x, y, s, WOLF, fill, extra)


def stars(pts):
    out = []
    for x, y, r, o in pts:
        out.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="#fff" opacity="%.2f"/>' % (x, y, r, o))
    return ''.join(out)


def zipper(cx, y0, y1, main):
    """A vertical zipper: two rows of teeth and a pull tab."""
    parts = ['<rect x="%.1f" y="%d" width="3" height="%d" rx="1.5" fill="%s" opacity="0.35"/>' % (cx - 1.5, y0, y1 - y0, main)]
    y = y0 + 4
    left = True
    while y < y1 - 14:
        x = cx - 8 if left else cx + 1
        parts.append('<rect x="%.1f" y="%d" width="7" height="5" rx="1.2" fill="#e8f2ff"/>' % (x, y))
        left = not left
        y += 7
    ty = y1 - 14
    parts.append('<rect x="%.1f" y="%d" width="14" height="20" rx="4" fill="%s" stroke="#e8f2ff" stroke-width="2"/>' % (cx - 7, ty, main))
    parts.append('<circle cx="%.1f" cy="%d" r="3" fill="#0b1a33"/>' % (cx, ty + 12))
    return ''.join(parts)


def symbol(kind, cx, main):
    """The format's symbol, drawn in a box about 44 wide and 100 tall around (cx, 114)."""
    w = '#e8f2ff'
    if kind == 'zipper':
        return zipper(cx, 66, 162, main)
    if kind == 'cube':  # solid block
        return ('<g stroke="%s" stroke-width="3" stroke-linejoin="round"><path d="M%d 84 L%d 98 V128 L%d 142 L%d 128 V98 Z" fill="%s" fill-opacity="0.25"/>'
                '<path d="M%d 84 L%d 98 L%d 112 L%d 98 Z" fill="%s" fill-opacity="0.55"/><path d="M%d 112 V142" fill="none"/></g>'
                % (w, cx, cx + 22, cx, cx - 22, main, cx, cx + 22, cx, cx - 22, main, cx)).replace(
            'M%d 84' % cx, 'M%d 84' % cx)
    if kind == 'layers':  # three stacked plates
        out = []
        for i, y in enumerate((150, 120, 90)):
            out.append('<path d="M%d %d L%d %d L%d %d L%d %d Z" fill="%s" fill-opacity="%.2f" stroke="%s" stroke-width="3" stroke-linejoin="round"/>'
                       % (cx, y - 14, cx + 24, y, cx, y + 14, cx - 24, y, main, 0.25 + 0.2 * (2 - i) / 2 * 2, w))
        return ''.join(out)
    if kind == 'bundle':  # a bundle tied with a band
        return ('<g stroke="%s" stroke-width="3" stroke-linejoin="round"><rect x="%d" y="82" width="12" height="70" rx="3" fill="%s" fill-opacity="0.35"/>'
                '<rect x="%d" y="82" width="12" height="70" rx="3" fill="%s" fill-opacity="0.35"/><rect x="%d" y="82" width="12" height="70" rx="3" fill="%s" fill-opacity="0.35"/>'
                '<rect x="%d" y="102" width="52" height="16" rx="4" fill="%s"/></g>'
                % (w, cx - 26, main, cx - 6, main, cx + 14, main, cx - 26, main))
    if kind == 'comic':  # a comic book: cover with a spine, a page of panels and a speech bubble
        return ('<g stroke="%s" stroke-width="3" stroke-linejoin="round"><rect x="%d" y="70" width="52" height="84" rx="3" fill="#0b1a33"/>'
                '<rect x="%d" y="70" width="9" height="84" rx="2" fill="%s"/></g>'
                '<g fill="none" stroke="%s" stroke-width="2.5" stroke-linejoin="round"><rect x="%d" y="78" width="34" height="26"/>'
                '<rect x="%d" y="110" width="15" height="36"/><rect x="%d" y="110" width="16" height="17"/><rect x="%d" y="131" width="16" height="15"/></g>'
                '<g fill="%s"><ellipse cx="%d" cy="91" rx="11" ry="7"/><path d="M%d 96 L%d 104 L%d 97 Z"/></g>'
                % (w, cx - 26, cx - 26, '#7d8594', w, cx - 12, cx - 12, cx + 5, cx + 5, w, cx + 5, cx - 2, cx - 7, cx + 2))
    if kind == 'disc':
        return ('<circle cx="%d" cy="114" r="30" fill="%s" fill-opacity="0.3" stroke="%s" stroke-width="3"/>'
                '<circle cx="%d" cy="114" r="9" fill="#0b1a33" stroke="%s" stroke-width="3"/>'
                '<path d="M%d 96 A22 22 0 0 1 %d 108" fill="none" stroke="%s" stroke-width="3" stroke-linecap="round" opacity="0.9"/>'
                % (cx, main, w, cx, w, cx - 14, cx - 6, w))
    # box: an open archive crate
    return ('<g stroke="%s" stroke-width="3" stroke-linejoin="round"><path d="M%d 92 H%d V146 H%d Z" fill="%s" fill-opacity="0.3"/>'
            '<rect x="%d" y="84" width="56" height="14" rx="3" fill="%s"/><rect x="%d" y="116" width="20" height="8" rx="3" fill="#0b1a33"/></g>'
            % (w, cx - 24, cx + 24, cx - 24, main, cx - 28, main, cx - 10))


TAIL = 'M168 238 C212 244 248 226 246 196 C245 184 240 176 232 168 C230 182 222 190 208 192 C196 194 182 192 168 192 Z'


def _map(path, small):
    """The small icon stretches the tail (base 176.., 150..232) so its outline keeps an even width: map the numbers, do not use a transform."""
    if not small:
        return path
    nums = re.findall(r'[MCLZ]|-?\d+\.?\d*', path)
    out, i, pair = [], 0, 0
    for t in nums:
        if t in 'MCLZ':
            out.append(t)
            pair = 0
            continue
        v = float(t)
        out.append('%.1f' % ((176 + (v - 168) * 0.9) if pair % 2 == 0 else (124 + (v - 192) * (1.2 if v < 192 else 2.6))))
        pair += 1
    return ' '.join(out)


TIP = 'M206 192 C220 188 228 182 230 170 L250 160 L250 192 Z'


def tail(fill, stroke, w, small=False):
    """Bushy tail continuing the label: its base is the label's right end; it swells outward, rises and ends in a light tip."""
    d = _map(TAIL, small)
    cid = 'tl' + ('s' if small else 'f')
    return ('<clipPath id="%s"><path d="%s"/></clipPath><path d="%s" fill="%s" stroke="%s" stroke-width="%.1f" stroke-linejoin="round"/>'
            '<g clip-path="url(#%s)"><path d="%s" fill="#e8f2ff"/></g>' % (cid, d, d, fill, stroke, w, cid, _map(TIP, small)))


def full(fmt, d):
    main, dark, label = d['main'], d['dark'], d['label']
    page = 'M34 10 H190 L226 46 V236 Q226 246 216 246 H44 Q34 246 34 236 Z'
    fold = 'M190 10 V36 Q190 46 200 46 H226 Z'
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256" role="img">',
           '<title>LycanZip %s file icon (draft)</title>' % label,
           '<defs>',
           '<linearGradient id="sky" x1="0" y1="0" x2="0.4" y2="1"><stop offset="0" stop-color="#16294f"/><stop offset="0.6" stop-color="#0d1a38"/><stop offset="1" stop-color="#070d1f"/></linearGradient>',
           '<linearGradient id="lab" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient>' % (dark, main),
           '<radialGradient id="glow" cx="0.3" cy="0.5" r="0.7"><stop offset="0" stop-color="%s" stop-opacity="0.35"/><stop offset="1" stop-color="%s" stop-opacity="0"/></radialGradient>' % (main, main),
           '<clipPath id="pg"><path d="%s"/></clipPath>' % page,
           '</defs>',
           '<path d="%s" fill="url(#sky)"/>' % page,
           '<g clip-path="url(#pg)"><rect x="34" y="40" width="192" height="170" fill="url(#glow)"/>',
           stars([(60, 34, 1.6, .9), (96, 24, 1.1, .7), (140, 30, 1.4, .8), (172, 22, 1, .6), (206, 70, 1.5, .9), (212, 104, 1, .6),
                  (54, 92, 1.2, .7), (78, 54, 1, .5), (156, 62, 1.1, .6), (200, 140, 1.2, .7), (66, 150, 1, .5), (120, 48, 0.9, .5)]),
           '</g>',
           '<path d="%s" fill="%s"/>' % (fold, '#2c4a86'),
           '<path d="%s" fill="none" stroke="%s" stroke-width="3" stroke-linejoin="round"/>' % (page, main),
           # the wolf, left; a soft light edge so it reads on the dark sky
           wolf(42, 70, 104, '#dfeeff'),
           # format symbol, right
           '<g transform="translate(182 114) scale(1.25) translate(-182 -114)">' + symbol(d['symbol'], 182, main) + '</g>',
           # label bottom left and the tail behind it
           tail('url(#lab)', '#cfe6ff', 1.5),
           '<rect x="34" y="192" width="140" height="46" rx="8" fill="url(#lab)" stroke="#cfe6ff" stroke-width="1.5"/>',
           '<text x="104" y="226" text-anchor="middle" font-family="Segoe UI, Arial, sans-serif" font-weight="800" font-size="38" fill="#fff" letter-spacing="2">%s</text>' % label,
           '</svg>']
    return '\n'.join(svg)


def small(fmt, d):
    main, dark, label = d['main'], d['dark'], d['label']
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256" role="img">',
           '<title>LycanZip %s file icon, small (draft)</title>' % label,
           '<defs><linearGradient id="lab" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient></defs>' % (dark, main),
           tail('url(#lab)', '#0b1a33', 7, True),
           # wolf with a dark outline so it reads on any background
           '<g transform="translate(26 0) scale(0.186) translate(-100 -106)"><path d="%s" fill="#0b1a33" stroke="#0b1a33" stroke-width="60" stroke-linejoin="round"/></g>' % WOLF,
           wolf(26, 0, 150, '#f2f8ff'),
           '<rect x="6" y="124" width="178" height="120" rx="14" fill="url(#lab)" stroke="#0b1a33" stroke-width="8"/>',
           '<text x="95" y="212" text-anchor="middle" font-family="Segoe UI, Arial, sans-serif" font-weight="900" font-size="92" textLength="150" lengthAdjust="spacingAndGlyphs" fill="#fff" stroke="#0b1a33" stroke-width="3" paint-order="stroke">%s</text>' % label,
           '</svg>']
    return '\n'.join(svg)


for fmt, d in FORMATS.items():
    open(os.path.join(OUT, 'file-%s.svg' % fmt), 'w', encoding='utf-8').write(full(fmt, d))
    open(os.path.join(OUT, 'file-%s-small.svg' % fmt), 'w', encoding='utf-8').write(small(fmt, d))
    print('wrote', fmt)
