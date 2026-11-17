#!/bin/sh
# Adds Arcus Odyssey to your desktop's app menu (for you only; no root needed): a .desktop
# entry pointing at the program in this folder, and its icon. Run it again if you move the
# folder. To remove it: ./install.sh --remove
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
ICONS="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps"
if [ "$1" = "--remove" ]; then
    rm -f "$APPS/arcus-odyssey-dx.desktop" "$ICONS/arcus-odyssey-dx.png"
    echo "removed the app menu entry (your saves in ${XDG_DATA_HOME:-$HOME/.local/share}/Arcus Odyssey are kept)"
    exit 0
fi
mkdir -p "$APPS" "$ICONS"
[ -f "$DIR/arcus-odyssey-dx.png" ] && cp "$DIR/arcus-odyssey-dx.png" "$ICONS/"
sed "s|^Exec=.*|Exec=\"$DIR/arcus-odyssey-dx\"|" "$DIR/arcus-odyssey-dx.desktop" > "$APPS/arcus-odyssey-dx.desktop"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$APPS" || true
echo "added Arcus Odyssey DX to the app menu (runs $DIR/arcus-odyssey-dx)"
