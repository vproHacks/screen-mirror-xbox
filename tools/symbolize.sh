#!/usr/bin/env bash
# Turns the "exe+0x<RVA>" addresses of a CRASH line in airplay.log into function names, using the
# linker map of the build that crashed (the map changes with every build).
# Usage: tools/symbolize.sh [-m build/x64/Release/ScreenMirrorServer.map] exe+0x133319 exe+0x2a10 ...
#    or: tools/xbox.sh log | grep CRASH | tools/symbolize.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MAP="$ROOT/build/x64/Release/ScreenMirrorServer.map"
if [ "$1" = "-m" ]; then MAP="$2"; shift 2; fi
[ -f "$MAP" ] || { echo "no map file: $MAP"; exit 1; }
if [ $# -gt 0 ]; then input="$*"; else input="$(cat)"; fi
addrs=$(echo "$input" | grep -o 'exe+0x[0-9a-fA-F]*' | sed 's/exe+//' | awk '!seen[$0]++')
[ -n "$addrs" ] || { echo "no exe+0x... addresses in the input"; exit 1; }
base=$((16#$(sed -n 's/.*Preferred load address is \([0-9a-fA-F]*\).*/\1/p' "$MAP")))
# public symbols as "<rva in decimal> <name> <object>", sorted by address
syms=$(awk -v base="$base" '
  /Publics by Value/ { on = 1; next }
  on && NF >= 4 && $1 ~ /^[0-9a-f]+:[0-9a-f]+$/ && $3 ~ /^[0-9a-f]+$/ {
    rva = strtonum("0x" $3) - base
    if (rva > 0) print rva, $2, $NF
  }' "$MAP" | sort -n)
for a in $addrs; do
  rva=$((a))
  echo "$syms" | awk -v rva="$rva" -v a="$a" '
    $1 <= rva { best = $2; obj = $3; off = rva - $1 }
    $1 > rva { exit }
    END { if (best != "") printf "%s  %s+0x%x  (%s)\n", a, best, off, obj; else print a "  ?" }'
done
