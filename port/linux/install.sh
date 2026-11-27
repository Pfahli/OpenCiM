#!/bin/sh
# OpenCiM for Linux: adds OpenCiM to the application menu and the `opencim` command for this user (no root), from the
# folder this script is in. Run it again after moving the folder.
#   ./install.sh              add the menu entry and ~/.local/bin/opencim
#   ./install.sh --uninstall  remove them again (the folder, your saves and settings stay)
set -e
here=$(cd "$(dirname "$0")" && pwd)
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
bin="$HOME/.local/bin"
desktop="$apps/opencim.desktop"

if [ "$1" = "--uninstall" ]; then
  rm -f "$desktop" "$bin/opencim"
  command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$apps" >/dev/null 2>&1 || true
  echo "OpenCiM removed from the menu. The folder $here and your saves (Documents/OpenCiM) are still there."
  exit 0
fi

if [ ! -x "$here/regs" ]; then
  echo "regs not found next to install.sh ($here)" >&2
  exit 1
fi
mkdir -p "$apps" "$bin"
ln -sf "$here/regs" "$bin/opencim"
cat > "$desktop" <<EOF
[Desktop Entry]
Type=Application
Name=OpenCiM
GenericName=Public transport simulation
Comment=Source port of Cities in Motion (needs the original game)
Exec="$here/regs"
Path=$here
Terminal=false
Categories=Game;Simulation;
Keywords=Cities in Motion;tram;metro;bus;transport;
StartupWMClass=opencim
EOF
chmod 644 "$desktop"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$apps" >/dev/null 2>&1 || true
echo "OpenCiM is in the application menu, and 'opencim' starts it from a terminal."
case ":$PATH:" in
  *":$bin:"*) ;;
  *) echo "(add $bin to your PATH for the 'opencim' command)" ;;
esac
