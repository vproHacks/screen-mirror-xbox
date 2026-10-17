#!/usr/bin/env bash
# Xbox Device Portal helper (Device Portal authentication must be disabled).
# Usage: tools/xbox.sh <command> [args]
#   info                      OS info
#   install [msix]            upload + install the app package (with the VCLibs dependency)
#   state                     installed AirServerXbox package(s)
#   launch | stop             start / terminate the app
#   running                   is the app process running?
#   log [file]                print a file from the app's LocalState (default airplay.log)
#   put <localfile> [name]    upload a file into the app's LocalState
#   screenshot [out.png]      capture the TV picture
#   uninstall                 remove the app
set -u
# Device Portal address: $XBOX, or the first line of tools/xbox-address.txt (not committed),
# e.g. https://192.168.1.50:11443 (Dev Home shows the console's IP)
XBOX="${XBOX:-$(head -1 "$(dirname "$0")/xbox-address.txt" 2>/dev/null | tr -d '\r')}"
if [ -z "$XBOX" ]; then echo "set XBOX=https://<xbox-ip>:11443 or create tools/xbox-address.txt"; exit 2; fi
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/build/xbox"
mkdir -p "$WORK"
JAR="$WORK/cookies.txt"
PKG_NAME="${PKG_NAME:-vpro8725.ScreenMirrorServer}"

c() { curl -sk --max-time "${TIMEOUT:-60}" -b "$JAR" -c "$JAR" "$@"; }
csrf() {
  # Device Portal wants the CSRF-Token cookie echoed in X-CSRF-Token on state-changing requests
  c -o /dev/null "$XBOX/api/os/info"
  awk '$6=="CSRF-Token"{print $7}' "$JAR" | tail -1
}
b64() { printf '%s' "$1" | base64 -w0; }
urlenc() { local s="$1"; s="${s//+/%2B}"; s="${s//\//%2F}"; s="${s//=/%3D}"; printf '%s' "$s"; }

pkg_json() { c "$XBOX/api/app/packagemanager/packages"; }
full_name() { pkg_json | tr '{' '\n' | grep "\"PackageFullName\" : \"${PKG_NAME}_" | sed -n 's/.*"PackageFullName" : "\([^"]*\)".*/\1/p' | head -1; }
relative_id() { pkg_json | tr '{' '\n' | grep "\"PackageFullName\" : \"${PKG_NAME}_" | sed -n 's/.*"PackageRelativeId" : "\([^"]*\)".*/\1/p' | head -1; }

cmd="${1:-info}"; shift || true
case "$cmd" in
  info) c "$XBOX/api/os/info"; echo ;;
  install)
    MSIX="${1:-$(ls -t "$ROOT"/build/AppPackages/*/AirServerXbox_*_x64.msix | head -1)}"
    DEP="$(dirname "$MSIX")/Dependencies/x64/Microsoft.VCLibs.x64.14.00.appx"
    T=$(csrf)
    echo "installing $(basename "$MSIX") (+ $(basename "$DEP"))"
    TIMEOUT=600 c -H "X-CSRF-Token: $T" -w "\nHTTP %{http_code}\n" \
      -F "$(basename "$MSIX")=@$MSIX" -F "$(basename "$DEP")=@$DEP" \
      "$XBOX/api/app/packagemanager/package?package=$(basename "$MSIX")"
    for i in $(seq 1 120); do
      code=$(c -o "$WORK/state.json" -w "%{http_code}" "$XBOX/api/app/packagemanager/state")
      if [ "$code" != "204" ]; then echo "install state: HTTP $code $(cat "$WORK/state.json")"; break; fi
      sleep 2
    done
    echo "installed: $(full_name)" ;;
  state) echo "full name: $(full_name)"; echo "relative id: $(relative_id)" ;;
  launch)
    FN=$(full_name); RID=$(relative_id); T=$(csrf)
    c -X POST -H "X-CSRF-Token: $T" -H "Content-Length: 0" -w "HTTP %{http_code}\n" \
      "$XBOX/api/taskmanager/app?appid=$(urlenc "$(b64 "$RID")")&package=$(urlenc "$(b64 "$FN")")" ;;
  stop)
    FN=$(full_name); T=$(csrf)
    c -X DELETE -H "X-CSRF-Token: $T" -w "HTTP %{http_code}\n" "$XBOX/api/taskmanager/app?package=$(urlenc "$(b64 "$FN")")" ;;
  running)
    c "$XBOX/api/resourcemanager/processes" | tr '{' '\n' | grep -i "AirServerXbox" | sed -n 's/.*"ImageName" : "\([^"]*\)".*"PrivateWorkingSet" : \([0-9]*\).*/\1 private=\2/p; s/.*"ImageName" : "\([^"]*\)".*/\1/p' | head -3 ;;
  log)
    FN=$(full_name); F="${1:-airplay.log}"
    c "$XBOX/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$FN&filename=$F&path=%5CLocalState" ;;
  put)
    FN=$(full_name); F="$1"; NAME="${2:-$(basename "$F")}"; T=$(csrf)
    c -H "X-CSRF-Token: $T" -w "HTTP %{http_code}\n" -F "$NAME=@$F" \
      "$XBOX/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$FN&path=%5CLocalState" ;;
  screenshot)
    OUT="${1:-$WORK/screenshot.png}"
    c -o "$OUT" -w "HTTP %{http_code} %{size_download} bytes\n" "$XBOX/ext/screenshot?download=false" ;;
  uninstall)
    FN=$(full_name); T=$(csrf)
    c -X DELETE -H "X-CSRF-Token: $T" -w "HTTP %{http_code}\n" "$XBOX/api/app/packagemanager/package?package=$FN" ;;
  *) echo "unknown command $cmd"; exit 2 ;;
esac
