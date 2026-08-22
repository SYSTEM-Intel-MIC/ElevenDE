"""Stage selected Microsoft Fluent System Icons as ElevenDE aliases.

The repository is expected at FLUENT_SOURCE and is only used at build time.
The selected SVGs are official Fluent assets; build-deb.sh rasterizes them at
16/20/22/24/32/48/64/96/128px for X11 and Qt consumers.
"""
from pathlib import Path
import os
import shutil

# Windows 11 command bars use thin regular glyphs rather than solid black
# silhouettes. The source SVGs are official Fluent assets; only their
# monochrome foreground is tinted to match the light Windows command bar.
TOOLBAR_BLUE = "#0067C0"
TOOLBAR_MUTED = "#8A929D"
NAV_GRAY = "#5F6368"

source = Path(os.environ.get("FLUENT_SOURCE", "/tmp/fluent-system-icons/assets"))
dest = Path(__file__).resolve().parent / "icons-svg"
dest.mkdir(parents=True, exist_ok=True)

mapping = {
    # Native-style tray status assets; regular icons are black #212121 SVGs.
    "tray-chevron.svg": "Chevron Up/SVG/ic_fluent_chevron_up_16_regular.svg",
    "tray-device.svg": "USB/SVG/ic_fluent_usb_20_regular.svg",
    "tray-network.svg": "WiFi 4/SVG/ic_fluent_wifi_4_20_regular.svg",
    "tray-network-off.svg": "WiFi Off/SVG/ic_fluent_wifi_off_20_regular.svg",
    "tray-volume.svg": "Speaker 2/SVG/ic_fluent_speaker_2_20_regular.svg",
    "tray-volume-muted.svg": "Speaker Mute/SVG/ic_fluent_speaker_mute_20_regular.svg",
    "tray-battery.svg": "Battery 10/SVG/ic_fluent_battery_10_20_regular.svg",
    "tray-battery-charge.svg": "Battery Charge/SVG/ic_fluent_battery_charge_20_regular.svg",
    "tray-input.svg": "Keyboard/SVG/ic_fluent_keyboard_20_regular.svg",
    # Official Win11-style status and power-menu glyphs.  These use dedicated
    # aliases so no legacy ICO conversion can ever win by sharing a filename.
    "tray-wired.svg": "Desktop Signal/SVG/ic_fluent_desktop_signal_20_regular.svg",
    "fluent-power-start.svg": "Power/SVG/ic_fluent_power_20_regular.svg",
    "fluent-power-lock.svg": "Lock Closed/SVG/ic_fluent_lock_closed_20_regular.svg",
    "fluent-power-sleep.svg": "Weather Moon/SVG/ic_fluent_weather_moon_20_regular.svg",
    "fluent-power-shutdown.svg": "Power/SVG/ic_fluent_power_20_regular.svg",
    "fluent-power-restart.svg": "Arrow Clockwise/SVG/ic_fluent_arrow_clockwise_20_regular.svg",
    # Windows 11-style default account avatar used by the login screen.
    "login-user-avatar.svg": "Person/SVG/ic_fluent_person_32_filled.svg",
    # Explorer command bar: official Fluent regular glyphs, tinted like the
    # blue/white Windows 11 command bar shown in the reference image.
    "edit-cut.svg": "Cut/SVG/ic_fluent_cut_20_regular.svg",
    "edit-copy.svg": "Copy/SVG/ic_fluent_copy_20_regular.svg",
    "edit-paste.svg": "Clipboard Paste/SVG/ic_fluent_clipboard_paste_20_regular.svg",
    "edit-rename.svg": "Rename/SVG/ic_fluent_rename_20_regular.svg",
    "explorer-share.svg": "Share/SVG/ic_fluent_share_20_regular.svg",
    "user-trash.svg": "Delete/SVG/ic_fluent_delete_20_regular.svg",
    "view-grid.svg": "Grid/SVG/ic_fluent_grid_20_regular.svg",
    "view-sort-ascending.svg": "Arrow Sort Down Lines/SVG/ic_fluent_arrow_sort_down_lines_20_regular.svg",
    "more-horizontal.svg": "More Horizontal/SVG/ic_fluent_more_horizontal_20_regular.svg",
    # Explorer navigation path-bar icons.
    "nav-back.svg": "Arrow Left/SVG/ic_fluent_arrow_left_20_regular.svg",
    "nav-forward.svg": "Arrow Right/SVG/ic_fluent_arrow_right_20_regular.svg",
    "nav-up.svg": "Arrow Up/SVG/ic_fluent_arrow_up_20_regular.svg",
    "nav-refresh.svg": "Arrow Sync/SVG/ic_fluent_arrow_sync_20_regular.svg",
    "nav-computer.svg": "Desktop/SVG/ic_fluent_desktop_20_regular.svg",
    "nav-chevron-right.svg": "Chevron Right/SVG/ic_fluent_chevron_right_20_regular.svg",
    "nav-search.svg": "Search/SVG/ic_fluent_search_20_regular.svg",
    "checkmark.svg": "Checkmark/SVG/ic_fluent_checkmark_16_regular.svg",
    "folder.svg": "Folder/SVG/ic_fluent_folder_20_filled.svg",
    "explorer-home.svg": "Home/SVG/ic_fluent_home_20_filled.svg",
    "desktop.svg": "Desktop/SVG/ic_fluent_desktop_20_filled.svg",
    "pictures.svg": "Image/SVG/ic_fluent_image_20_filled.svg",
}

missing = []
for alias, rel in mapping.items():
    src = source / rel
    if not src.exists():
        missing.append(f"{alias}: {src}")
        continue
    data = src.read_text(encoding="utf-8")
    if alias in {"user-trash.svg"}:
        data = data.replace("#212121", TOOLBAR_MUTED)
    elif alias in {"edit-cut.svg", "edit-copy.svg", "edit-rename.svg",
                   "explorer-share.svg", "edit-paste.svg", "view-grid.svg",
                   "view-sort-ascending.svg"}:
        data = data.replace("#212121", TOOLBAR_BLUE)
    elif alias in {"checkmark.svg", "login-user-avatar.svg"}:
        data = data.replace("#212121", "#FFFFFF")
    elif alias.startswith("nav-"):
        data = data.replace("#212121", NAV_GRAY)
    dest.joinpath(alias).write_text(data, encoding="utf-8")

if missing:
    raise SystemExit("Missing Fluent assets:\n" + "\n".join(missing))
print(f"staged {len(mapping)} Fluent SVG aliases into {dest}")
