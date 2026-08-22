#!/usr/bin/env python3
"""Import a curated WindowsIcons set into ElevenDE's X11 icon theme.

The source repository is kept outside the build tree. Only converted PNG
assets are staged into the local package, with provenance documented in
WINDOWSICONS-NOTICE.md.
"""
from pathlib import Path
from PIL import Image
import os
import shutil

# Point this at a checked-out HaydenReeve/WindowsIcons tree. The build remains
# offline; only converted PNGs are committed/staged into the package.
SOURCE = Path(os.environ.get('WINDOWSICONS_SOURCE', '/tmp/windowsicons-current/Icons'))
DEST = Path(__file__).resolve().parent / 'icons'
SIZES = (16, 22, 24, 32, 48, 64, 96, 128)

MAPPING = {
    'apps': {
        'utilities-terminal': 'applications/terminal.ico',
        'accessories-calculator': 'applications/calculator.ico',
        'preferences-system': 'applications/settings.ico',
        'utilities-system-monitor': 'applications/taskmanager2.ico',
        'accessories-text-editor': 'applications/notepad.ico',
        'web-browser': 'applications/edge.ico',
        'system-file-manager': 'folders/explorer2.ico',
        'system-run': 'applications/run.ico',
        'edit-cut': 'objects/scissors.ico',
        'edit-copy': 'objects/clipboard.ico',
        'edit-paste': 'objects/clipboard2.ico',
        'edit-rename': 'files/text2.ico',
        'user-trash': 'objects/recyclebinempty.ico',
        'view-grid': 'files/window.ico',
        'view-list': 'files/windowtasks.ico',
        'view-sort-ascending': 'files/select.ico',
        'view-refresh': 'emblems/refresh.ico',
        'document-properties': 'files/configuration.ico',
        # CommandBar and task buttons also ask for these names in the apps
        # category; duplicate the exact ICO mapping to avoid Qt fallbacks.
        'folder': 'folders/folder.ico',
        'computer': 'devices/computercase.ico',
        # Use the clean modern drive body; windows.ico carries the older Win10 flag overlay.
        'drive-windows': 'devices/drives/drive.ico',
        'drive': 'devices/drives/drive.ico',
        'documents': 'folders/documents.ico',
        # Windows 11 Settings navigation: explicit aliases so the Qt settings
        # shell never falls back to monochrome text glyphs.
        'settings-nav-home': 'emblems/homegroup.ico',
        'settings-nav-system': 'devices/computercase.ico',
        'settings-nav-personalization': 'devices/monitors/personalization.ico',
        'settings-nav-display': 'devices/monitors/monitor.ico',
        'settings-nav-network': 'devices/monitors/network.ico',
        'settings-nav-sound': 'emblems/soundsmall.ico',
        'settings-nav-shortcuts': 'devices/keyboard.ico',
        'settings-nav-time': 'emblems/time.ico',
        'settings-nav-apps': 'applications/setup.ico',
        'settings-nav-mouse': 'applications/interface.ico',
        'settings-nav-power': 'devices/battery.ico',
        'settings-nav-users': 'emblems/users.ico',
        # Explicit Explorer/Desktop resources. These remain in the apps
        # namespace so Shell and Qt never fall through to a generic SVG alias.
        'desktop-this-pc': 'devices/monitors/monitor.ico',
        'desktop-home': 'folders/user.ico',
        'explorer-home': 'emblems/homegroup.ico',
        'explorer-new': 'folders/folder.ico',
        'explorer-share': 'folders/share.ico',
        # System-tray assets: use packaged WindowsIcons PNGs, never Shell-drawn glyphs.
        'tray-chevron': 'emblems/up.ico',
        'tray-device': 'devices/device.ico',
        'tray-network': 'devices/monitors/network.ico',
        'tray-network-off': 'devices/monitors/network.ico',
        'tray-wired': 'devices/monitors/network2.ico',
        'tray-volume': 'emblems/soundsmall.ico',
        'tray-battery': 'devices/battery.ico',
        'tray-input': 'devices/keyboard.ico',
        'power-sleep': 'devices/battery.ico',
        'power-shutdown': 'applications/powerapps.ico',
        'power-restart': 'emblems/refresh.ico',
        'power-logout': 'folders/user.ico',
    },
    'places': {
        'computer': 'devices/computercase.ico',
        'desktop-this-pc': 'devices/monitors/monitor.ico',
        'user-home': 'folders/user.ico',
        'desktop': 'folders/desktop.ico',
        'folder': 'folders/folder.ico',
        'documents': 'folders/documents.ico',
        'downloads': 'folders/downloads.ico',
        'pictures': 'folders/pictures.ico',
        'music': 'folders/music.ico',
        'videos': 'folders/videos.ico',
        'user-trash': 'objects/recyclebinempty.ico',
        'network': 'folders/network.ico',
        'onedrive': 'folders/onedrive.ico',
    },
    'devices': {
        'drive': 'devices/drives/drive.ico',
        # Use the clean modern drive body; windows.ico carries the older Win10 flag overlay.
        'drive-windows': 'devices/drives/drive.ico',
        'drive-network': 'devices/drives/network.ico',
    },
    'mimetypes': {
        'text-x-generic': 'files/document.ico',
        'image': 'files/image.ico',
        'video-x-generic': 'files/video.ico',
    },
}

# Native Windows 11 power/network glyphs are monochrome even when the source
# ICO contains colored artwork. Preserve alpha and normalize the foreground.
MONO_ALIASES = {
    'tray-network', 'tray-network-off', 'tray-wired',
    'power-sleep', 'power-shutdown', 'power-restart', 'power-logout',
}


def convert(src: Path, dst: Path, size: int, monochrome: bool = False) -> None:
    with Image.open(src) as im:
        # ICO files contain multiple embedded resolutions. Pick the largest
        # available frame, then use high-quality Lanczos downsampling.
        frames = []
        for i in range(getattr(im, 'n_frames', 1)):
            im.seek(i)
            frame = im.convert('RGBA')
            frames.append(frame.copy())
        frame = max(frames, key=lambda x: x.width * x.height)
        if monochrome:
            alpha = frame.getchannel('A')
            frame = Image.new('RGBA', frame.size, (32, 32, 32, 0))
            frame.putalpha(alpha)
        frame.thumbnail((size, size), Image.Resampling.LANCZOS)
        canvas = Image.new('RGBA', (size, size), (0, 0, 0, 0))
        canvas.alpha_composite(frame, ((size - frame.width) // 2,
                                       (size - frame.height) // 2))
        dst.parent.mkdir(parents=True, exist_ok=True)
        canvas.save(dst, 'PNG', optimize=True)


def main() -> None:
    if not SOURCE.is_dir():
        raise SystemExit(f'missing source: {SOURCE}')
    if DEST.exists():
        shutil.rmtree(DEST)
    count = 0
    for category, names in MAPPING.items():
        for name, rel in names.items():
            src = SOURCE / rel
            if not src.exists():
                raise SystemExit(f'missing mapped icon: {src}')
            for size in SIZES:
                convert(src, DEST / f'{size}x{size}' / category / f'{name}.png', size,
                        monochrome=(name in MONO_ALIASES))
                count += 1
    print(f'imported {len(sum((list(x) for x in MAPPING.values()), []))} ICOs as {count} PNG assets into {DEST}')


if __name__ == '__main__':
    main()
