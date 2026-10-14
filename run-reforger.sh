#!/bin/zsh
# Starts Arma Reforger in a CrossOver bottle directly (without the BattlEye launcher),
# with the dxgi.dll from the game folder. Usage: ./run-reforger.sh [-window] [other game args]
# Environment: BOTTLE (default "Steam"), CXAPP (default /Applications/CrossOver.app)
set -u
BOTTLE=${BOTTLE:-Steam}
CX="${CXAPP:-/Applications/CrossOver.app}/Contents/SharedSupport/CrossOver/bin"
B="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE"
G="$B/drive_c/Program Files (x86)/Steam/steamapps/common/Arma Reforger"
EXE="$G/ArmaReforgerSteam.exe"
[ -f "$EXE" ] || { echo "Not found: $EXE"; exit 1; }
[ -f "$G/dxgi.dll" ] || { echo "dxgi.dll is missing in $G"; exit 1; }
if ! pgrep -f 'steam\.exe' >/dev/null; then
  echo "Starting Steam in bottle $BOTTLE..."
  "$CX/cxstart" --bottle "$BOTTLE" 'C:\Program Files (x86)\Steam\steam.exe' -silent &
  sleep 30
fi
echo "Starting Arma Reforger $*"
exec "$CX/cxstart" --bottle "$BOTTLE" --dll "dxgi=n,b" --workdir "$G" "$EXE" "$@"
