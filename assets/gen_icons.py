#!/usr/bin/env python3
"""Generate Windows 11 (Fluent) style icons for ElevenDE as SVG, then rasterize.

Win11 icon language: rounded-rectangle tiles, soft two-stop gradients,
flat glyphs, brighter Microsoft 365 palette.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "icons-svg")
os.makedirs(OUT, exist_ok=True)

def grad(name, c1, c2, x1=0, y1=0, x2=0, y2=1):
    return (f'<linearGradient id="{name}" x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}">'
            f'<stop offset="0" stop-color="{c1}"/><stop offset="1" stop-color="{c2}"/>'
            f'</linearGradient>')

def svg(body, w=256, h=256):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
            f'viewBox="0 0 256 256">{body}</svg>')

ICONS = {}

# ---- This PC: minimal rounded monitor, Win11 style ----
ICONS['computer'] = svg(
    f'<defs>{grad("g","#6ecbff","#0f6cbd")}</defs>'
    '<rect x="30" y="40" width="196" height="124" rx="18" fill="url(#g)"/>'
    '<rect x="44" y="54" width="168" height="96" rx="10" fill="#eaf6ff" opacity="0.92"/>'
    '<rect x="58" y="70" width="64" height="9" rx="4.5" fill="#77b9e8"/>'
    '<rect x="58" y="88" width="104" height="9" rx="4.5" fill="#b9dcf4"/>'
    '<rect x="58" y="106" width="84" height="9" rx="4.5" fill="#b9dcf4"/>'
    '<path d="M118 164 h20 l6 26 h-32 Z" fill="#a9b4c4"/>'
    '<rect x="88" y="190" width="80" height="12" rx="6" fill="#c2cad6"/>'
)

# ---- Home: Win11 blue house on light tile (Quick Access style) ----
ICONS['user-home'] = svg(
    f'<defs>{grad("g","#6ecbff","#0f6cbd")}{grad("r","#d9edfb","#b5dbf5")}</defs>'
    '<rect x="36" y="36" width="184" height="184" rx="26" fill="url(#r)"/>'
    '<path d="M128 66 L56 128 h18 v62 q0 8 8 8 h36 v-42 h20 v42 h36 q8 0 8-8 v-62 h18 Z" fill="url(#g)"/>'
)

# ---- Folder: Win11 yellow ----
ICONS['folder'] = svg(
    f'<defs>{grad("g","#ffd75e","#ffb900")}{grad("b","#fff3c4","#ffd75e")}</defs>'
    '<path d="M32 70 q0-10 10-10 h52 l18 18 h102 q10 0 10 10 v100 q0 10-10 10 H42 q-10 0-10-10 Z" fill="url(#g)"/>'
    '<path d="M32 92 h192 v96 q0 10-10 10 H42 q-10 0-10-10 Z" fill="url(#b)"/>'
)

# ---- Trash: flat minimal gray ----
ICONS['user-trash'] = svg(
    f'<defs>{grad("g","#f4f7fa","#dde4ec")}</defs>'
    '<rect x="72" y="42" width="112" height="16" rx="8" fill="#aeb9c6"/>'
    '<rect x="104" y="30" width="48" height="16" rx="8" fill="#c7d1dc"/>'
    '<path d="M64 66 h128 l-10 142 q-1 10-11 10 H85 q-10 0-11-10 Z" fill="url(#g)"/>'
    '<g stroke="#aeb9c6" stroke-width="9" stroke-linecap="round">'
    '<line x1="104" y1="88" x2="106" y2="196"/>'
    '<line x1="128" y1="88" x2="128" y2="196"/>'
    '<line x1="152" y1="88" x2="150" y2="196"/>'
    '</g>'
)

# ---- Text file: Notepad page ----
ICONS['text-x-generic'] = svg(
    f'<defs>{grad("p","#ffffff","#eef2f7")}{grad("f","#cfe0f0","#9cc3e5")}</defs>'
    '<path d="M60 28 h100 l40 40 v150 q0 10-10 10 H60 q-10 0-10-10 V38 q0-10 10-10 Z" fill="url(#p)" stroke="#d3dae3" stroke-width="3"/>'
    '<path d="M160 28 l40 40 h-34 q-6 0-6-6 Z" fill="url(#f)"/>'
    '<g stroke="#8fb6dd" stroke-width="8" stroke-linecap="round">'
    '<line x1="76" y1="110" x2="180" y2="110"/>'
    '<line x1="76" y1="136" x2="180" y2="136"/>'
    '<line x1="76" y1="162" x2="150" y2="162"/>'
    '</g>'
)

# ---- Archive: folder with zipper ----
ICONS['package-x-generic'] = svg(
    f'<defs>{grad("g","#ffd75e","#ffb900")}{grad("b","#fff3c4","#ffd75e")}</defs>'
    '<path d="M32 70 q0-10 10-10 h52 l18 18 h102 q10 0 10 10 v100 q0 10-10 10 H42 q-10 0-10-10 Z" fill="url(#g)"/>'
    '<path d="M32 92 h192 v96 q0 10-10 10 H42 q-10 0-10-10 Z" fill="url(#b)"/>'
    '<g fill="#b98a00">'
    '<rect x="120" y="92" width="16" height="12"/>'
    '<rect x="112" y="106" width="16" height="12"/>'
    '<rect x="120" y="120" width="16" height="12"/>'
    '<rect x="112" y="134" width="16" height="12"/>'
    '<rect x="120" y="148" width="16" height="12"/>'
    '</g>'
    '<rect x="112" y="162" width="24" height="22" rx="5" fill="#8a6a00"/>'
)

# ---- Image: Win11 Photos blue/green landscape ----
ICONS['image-x-generic'] = svg(
    f'<defs>{grad("sky","#8fd0f7","#4aa8e8")}{grad("gr","#9be29b","#4caf50")}</defs>'
    '<rect x="36" y="44" width="184" height="168" rx="20" fill="url(#sky)"/>'
    '<circle cx="94" cy="94" r="17" fill="#ffd54f"/>'
    '<path d="M36 168 l50-52 38 38 28-24 64 52 v16 q0 20-20 20 H56 q-20 0-20-20 Z" fill="url(#gr)"/>'
)

# ---- Video: Win11 blue with play badge ----
ICONS['video-x-generic'] = svg(
    f'<defs>{grad("g","#6ecbff","#0f6cbd")}</defs>'
    '<rect x="36" y="52" width="184" height="152" rx="20" fill="url(#g)"/>'
    '<circle cx="128" cy="128" r="38" fill="#ffffff" opacity="0.95"/>'
    '<path d="M118 110 l32 18 -32 18 Z" fill="#0f6cbd"/>'
)

# ---- Audio: Groove Music style orange ----
ICONS['audio-x-generic'] = svg(
    f'<defs>{grad("g","#ffb56b","#f26522")}</defs>'
    '<rect x="36" y="36" width="184" height="184" rx="26" fill="url(#g)"/>'
    '<path d="M166 74 v86 a23 23 0 1 1-13-20.4 V100 l-46 10 v62 a23 23 0 1 1-13-20.4 V88 Z" fill="#ffffff"/>'
)

# ---- PDF ----
ICONS['application-pdf'] = svg(
    f'<defs>{grad("p","#ffffff","#eef2f7")}</defs>'
    '<path d="M60 28 h100 l40 40 v150 q0 10-10 10 H60 q-10 0-10-10 V38 q0-10 10-10 Z" fill="url(#p)" stroke="#d3dae3" stroke-width="3"/>'
    '<path d="M160 28 l40 40 h-34 q-6 0-6-6 Z" fill="#f3c8c8"/>'
    '<rect x="50" y="128" width="156" height="54" rx="10" fill="#e5484d"/>'
    '<text x="128" y="164" font-family="Segoe UI,Arial" font-size="34" font-weight="700" fill="#fff" text-anchor="middle">PDF</text>'
)

# ---- Word (M365 bright blue) ----
ICONS['application-msword'] = svg(
    f'<defs>{grad("g","#41a5ee","#185abd")}</defs>'
    '<rect x="40" y="40" width="176" height="176" rx="26" fill="url(#g)"/>'
    '<text x="128" y="166" font-family="Segoe UI,Arial" font-size="104" font-weight="700" fill="#fff" text-anchor="middle">W</text>'
)

# ---- Excel (M365 bright green) ----
ICONS['x-office-spreadsheet'] = svg(
    f'<defs>{grad("g","#33c481","#107c41")}</defs>'
    '<rect x="40" y="40" width="176" height="176" rx="26" fill="url(#g)"/>'
    '<text x="128" y="166" font-family="Segoe UI,Arial" font-size="104" font-weight="700" fill="#fff" text-anchor="middle">X</text>'
)

# ---- PowerPoint (M365 bright orange) ----
ICONS['x-office-presentation'] = svg(
    f'<defs>{grad("g","#ff8f6b","#d35230")}</defs>'
    '<rect x="40" y="40" width="176" height="176" rx="26" fill="url(#g)"/>'
    '<text x="128" y="166" font-family="Segoe UI,Arial" font-size="104" font-weight="700" fill="#fff" text-anchor="middle">P</text>'
)

# ---- Script ----
ICONS['text-x-script'] = svg(
    f'<defs>{grad("g","#334155","#0f172a")}</defs>'
    '<rect x="40" y="36" width="176" height="184" rx="18" fill="url(#g)"/>'
    '<text x="66" y="120" font-family="Consolas,monospace" font-size="46" fill="#7dd3fc">&gt;_</text>'
    '<g stroke="#94a3b8" stroke-width="7" stroke-linecap="round">'
    '<line x1="66" y1="150" x2="150" y2="150"/>'
    '<line x1="66" y1="176" x2="190" y2="176"/>'
    '</g>'
)

# ---- Executable ----
ICONS['application-x-executable'] = svg(
    f'<defs>{grad("g","#6ecbff","#0f6cbd")}</defs>'
    '<rect x="36" y="36" width="184" height="184" rx="26" fill="url(#g)"/>'
    '<g transform="translate(128 128)">'
    '<g fill="#ffffff">'
    '<path d="M-14 -84 h28 l6 22 a62 62 0 0 1 20 8 l20 -12 20 20 -12 20 a62 62 0 0 1 8 20 l22 6 v28 l-22 6 a62 62 0 0 1 -8 20 l12 20 -20 20 -20 -12 a62 62 0 0 1 -20 8 l-6 22 h-28 l-6 -22 a62 62 0 0 1 -20 -8 l-20 12 -20 -20 12 -20 a62 62 0 0 1 -8 -20 l-22 -6 v-28 l22 -6 a62 62 0 0 1 8 -20 l-12 -20 20 -20 20 12 a62 62 0 0 1 20 -8 Z" opacity="0"/>'
    '</g>'
    '<g fill="#ffffff"><path d="M0 -78 L13 -74 18 -52 38 -44 56 -58 74 -40 60 -22 68 -2 90 3 90 22 68 27 60 47 74 65 56 83 38 69 18 77 13 99 -13 99 -18 77 -38 69 -56 83 -74 65 -60 47 -68 27 -90 22 -90 3 -68 -2 -60 -22 -74 -40 -56 -58 -38 -44 -18 -52 -13 -74 Z" transform="scale(0.72)"/></g>'
    '<circle r="30" fill="url(#g)"/>'
    '<circle r="30" fill="none" stroke="#ffffff" stroke-width="13"/>'
    '</g>'
)

# ---- Optical disc ----
ICONS['media-optical'] = svg(
    f'<defs>{grad("g","#e2e8f0","#94a3b8",0,0,1,1)}</defs>'
    '<circle cx="128" cy="128" r="92" fill="url(#g)"/>'
    '<circle cx="128" cy="128" r="88" fill="none" stroke="#ffffff" stroke-opacity="0.5" stroke-width="4"/>'
    '<circle cx="128" cy="128" r="30" fill="#64748b"/>'
    '<circle cx="128" cy="128" r="14" fill="#e2e8f0"/>'
    '<path d="M128 40 a88 88 0 0 1 88 88" fill="none" stroke="#ffffff" stroke-opacity="0.65" stroke-width="8"/>'
)

# ---- App-entry aliases: Windows-inspired gradient tiles without copying
# proprietary Windows application artwork. Desktop files resolve these names
# directly, so minimal installations never fall back to mismatched system art.
ICONS['accessories-calculator'] = svg(
    f'<defs>{grad("calc","#6b8cff","#4f35d9")}{grad("screen","#eef4ff","#cdd9ff")}</defs>'
    '<rect x="28" y="28" width="200" height="200" rx="38" fill="url(#calc)"/>'
    '<rect x="57" y="54" width="142" height="52" rx="12" fill="url(#screen)"/>'
    '<text x="180" y="91" font-family="Segoe UI,Arial" font-size="34" fill="#283b70" text-anchor="end">12.4</text>'
    '<g fill="#ffffff" opacity=".96"><rect x="58" y="124" width="30" height="24" rx="8"/><rect x="102" y="124" width="30" height="24" rx="8"/><rect x="146" y="124" width="30" height="24" rx="8"/><rect x="58" y="162" width="30" height="24" rx="8"/><rect x="102" y="162" width="30" height="24" rx="8"/></g>'
    '<rect x="146" y="162" width="30" height="24" rx="8" fill="#80e0cc"/>'
)
ICONS['utilities-system-monitor'] = svg(
    f'<defs>{grad("monitor","#27b6cc","#0a6ed1")}</defs>'
    '<rect x="28" y="28" width="200" height="200" rx="38" fill="url(#monitor)"/>'
    '<rect x="51" y="55" width="154" height="118" rx="16" fill="#effcff" opacity=".96"/>'
    '<path d="M67 145 L92 119 113 132 139 89 161 112 189 76" fill="none" stroke="#13a7a3" stroke-width="12" stroke-linecap="round" stroke-linejoin="round"/>'
    '<rect x="83" y="191" width="90" height="12" rx="6" fill="#d5f3ff" opacity=".9"/>'
)
ICONS['utilities-terminal-symbolic'] = svg(
    f'<defs>{grad("term","#35445d","#111827")}</defs>'
    '<rect x="28" y="28" width="200" height="200" rx="38" fill="url(#term)"/>'
    '<text x="62" y="134" font-family="Cascadia Mono,Consolas,monospace" font-size="60" font-weight="700" fill="#76e4d1">&gt;_</text>'
    '<rect x="61" y="160" width="90" height="9" rx="4.5" fill="#b5c8dc" opacity=".8"/>'
)
# system-run is intentionally not generated here. The official WindowsIcons
# `applications/run.ico` is imported by import_windowsicons.py and must remain
# authoritative; the old generated play-button glyph looked like a media player.
ICONS['system-file-manager'] = ICONS['folder']
ICONS['accessories-text-editor'] = ICONS['text-x-generic']

for name, body in ICONS.items():
    with open(os.path.join(OUT, name + '.svg'), 'w') as f:
        f.write(body)
print(f"wrote {len(ICONS)} SVGs to {OUT}")
