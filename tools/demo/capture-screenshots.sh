#!/usr/bin/env bash
# Captures the Store screenshots (1920x1080) from a Dev Mode console with simulated senders:
#   1-ready.png    the idle screen
#   2-music.png    AirPlay speaker: a public-domain piece with cover art (make-demo-media.ps1)
#   3-website.png  portrait screen mirroring of a web page (demo-page.html)
#   4-video.png    AirPlay video: NASA's "Earth Views from the ISS - No Music" (public domain)
# Needs build/tests/fakesender.exe (tools/build-tests.cmd) and tools/xbox-address.txt.
# Output: build/screenshots/
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/screenshots"
DEMO="$ROOT/build/demo"
SENDER="$ROOT/build/tests/fakesender.exe"
XBOX_URL="${XBOX:-$(tr -d '\r' < "$ROOT/tools/xbox-address.txt")}"
IP=$(echo "$XBOX_URL" | sed 's|^[a-z]*://||; s|:.*||')
VIDEO="https://images-assets.nasa.gov/video/NHQ_2020_1221_Earth%20Views/NHQ_2020_1221_Earth%20Views~orig.mp4"
EDGE="/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"
mkdir -p "$OUT" "$DEMO"
xbox() { bash "$ROOT/tools/xbox.sh" "$@"; }
shot() { xbox screenshot "$OUT/$1"; }

# demo media: the song and cover art, and the page rendered at iPhone resolution (390x844 pt @3x)
[ -f "$DEMO/canon.wav" ] || powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$ROOT/tools/demo/make-demo-media.ps1")"
W=$(cygpath -w "$ROOT")
"$EDGE" --headless=new --disable-gpu --hide-scrollbars --no-first-run --force-device-scale-factor=3 \
  --window-size=390,844 --user-data-dir="$W\\build\\demo\\edge-profile" --screenshot="$W\\build\\demo\\page.png" \
  "file:///$(cygpath -m "$ROOT")/tools/demo/demo-page.html" 2>/dev/null

xbox launch
sleep 8
shot 1-ready.png

# speaker: start 47 s into the piece so the progress bar is under way
"$SENDER" "$IP" --name iPhone --speaker --audio-file "$DEMO/canon.wav" --cover-art "$DEMO/cover.jpg" --start-at 47 --seconds 25 &
sleep 14
shot 2-music.png
wait
sleep 4

# portrait mirroring: iOS scales a portrait screen to 1920 pixels high
"$SENDER" "$IP" --name iPhone --image "$DEMO/page.png" --size 888x1920 --no-audio --seconds 22 &
sleep 14
shot 3-website.png
wait
sleep 4

# AirPlay video, from a point with a good view
"$SENDER" "$IP" --name iPhone --cast-url "$VIDEO" --start-at 700 --just-play --seconds 30 &
sleep 22
shot 4-video.png
wait
echo "screenshots in $OUT"
