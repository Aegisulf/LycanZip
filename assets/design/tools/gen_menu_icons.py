"""Icons of the Explorer context-menu entries (a wolf head with a coloured badge that says what the entry does).
Run: python gen_menu_icons.py        Writes ../menu-icons/menu-<name>.svg
export_menu_icons.py renders them into .ico files and copies them to app/Assets/MenuIcons.
Entries of a format (compress to ZIP / 7z / TAR, open an archive) use the file-type icons (gen_file_icon.py) instead.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'menu-icons')
os.makedirs(OUT, exist_ok=True)
WOLF = open(os.path.join(HERE, '..', 'wolf-silhouette.path.txt'), encoding='utf-8').read().strip()
DARK = '#0b1a33'
WHITE = '#ffffff'

# glyphs are drawn in a 100 x 100 box around the badge centre (0, 0 = centre), white on the badge colour
GLYPHS = {
    'extract': '<path d="M0 -34 V10 M-22 -12 L0 12 L22 -12" fill="none" stroke="#fff" stroke-width="13" stroke-linecap="round" stroke-linejoin="round"/>'
               '<path d="M-30 34 H30" stroke="#fff" stroke-width="13" stroke-linecap="round"/>',
    'test': '<path d="M-30 2 L-10 24 L32 -26" fill="none" stroke="#fff" stroke-width="15" stroke-linecap="round" stroke-linejoin="round"/>',
    'images': '<rect x="-34" y="-28" width="68" height="56" rx="8" fill="none" stroke="#fff" stroke-width="10"/>'
              '<path d="M-26 20 L-8 -2 L4 10 L14 0 L28 20 Z" fill="#fff"/><circle cx="14" cy="-12" r="7" fill="#fff"/>',
    'audio': '<path d="M-10 -32 L26 -40 V16" fill="none" stroke="#fff" stroke-width="11" stroke-linejoin="round"/>'
             '<ellipse cx="-20" cy="22" rx="16" ry="12" fill="#fff"/><ellipse cx="16" cy="16" rx="16" ry="12" fill="#fff"/>'
             '<path d="M-10 -32 V22" stroke="#fff" stroke-width="11"/>',
    'encrypt': '<rect x="-30" y="-6" width="60" height="44" rx="9" fill="#fff"/>'
               '<path d="M-17 -6 V-18 A17 17 0 0 1 17 -18 V-6" fill="none" stroke="#fff" stroke-width="11"/>'
               '<circle cx="0" cy="14" r="7" fill="BADGE"/>',
    'hash': '<path d="M-10 -34 L-18 34 M18 -34 L10 34 M-32 -12 H34 M-34 12 H32" fill="none" stroke="#fff" stroke-width="11" stroke-linecap="round"/>',
    'template': '<path d="M0 -34 L34 -16 L0 2 L-34 -16 Z" fill="#fff"/>'
                '<path d="M-34 4 L0 22 L34 4 M-34 24 L0 42 L34 24" fill="none" stroke="#fff" stroke-width="10" stroke-linejoin="round" stroke-linecap="round"/>',
}
COLORS = {'extract': '#2f8cf0', 'test': '#22a867', 'images': '#9a5cf0', 'audio': '#ee5a96', 'encrypt': '#e0a000', 'hash': '#1fb5ae', 'template': '#f07a28'}


def icon(name):
    c = COLORS[name]
    glyph = GLYPHS[name].replace('BADGE', c)
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256" role="img">'
            '<title>LycanZip menu icon: %s</title>'
            # the wolf: a dark outline, then the white head
            '<g transform="translate(16 16) scale(0.186) translate(-100 -106)"><path d="%s" fill="%s" stroke="%s" stroke-width="58" stroke-linejoin="round"/></g>'
            '<g transform="translate(16 16) scale(0.186) translate(-100 -106)"><path d="%s" fill="#f2f8ff"/></g>'
            # the badge: bottom right
            '<circle cx="178" cy="178" r="74" fill="%s" stroke="%s" stroke-width="12"/>'
            '<g transform="translate(178 178) scale(0.95)">%s</g></svg>') % (name, WOLF, DARK, DARK, WOLF, c, DARK, glyph)


for name in GLYPHS:
    open(os.path.join(OUT, 'menu-%s.svg' % name), 'w', encoding='utf-8').write(icon(name))
    print('wrote', name)
