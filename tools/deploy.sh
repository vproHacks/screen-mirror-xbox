#!/usr/bin/env bash
# Bump the package build number, build Release, install on the Xbox and launch.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MAN="$ROOT/src/app/Package.appxmanifest"
cur=$(grep -o 'Version="[0-9.]*"' "$MAN" | head -1 | sed 's/Version="\(.*\)"/\1/')
IFS=. read -r a b c d <<< "$cur"
new="$a.$b.$((c + 1)).$d"
sed -i "0,/Version=\"$cur\"/s//Version=\"$new\"/" "$MAN"
echo "version $cur -> $new"
(cd "$ROOT" && cmd.exe //c "tools\build-app.cmd Release") > "$ROOT/build/build-app.log" 2>&1 || { grep -E " error |LNK[0-9]" "$ROOT/build/build-app.log" | head -30; exit 1; }
MSIX="$ROOT/build/AppPackages/AirServerXbox_${new}_x64_Test/AirServerXbox_${new}_x64.msix"
bash "$ROOT/tools/xbox.sh" install "$MSIX" | tail -2
bash "$ROOT/tools/xbox.sh" launch
