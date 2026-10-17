#!/usr/bin/env bash
# Builds the third-party libraries with vcpkg (tools/vcpkg, a submodule) into tools/vcpkg_installed.
#   app (UWP):          openssl, libplist, ffmpeg (libavcodec: AAC-ELD and ALAC decoders; LGPL build)
#   test tools (Win32): openssl, libplist, openh264 (test video), ffmpeg (reads audio files)
# Run from Git Bash after installing the VS 2022 Build Tools (UWP + C++ workloads).
set -e
source "$(dirname "$0")/vcpkg-env.sh"
cd "$VCPKG_ROOT"
[ -x ./vcpkg.exe ] || cmd.exe //c "bootstrap-vcpkg.bat -disableMetrics"

# FFmpeg's build refuses paths that contain spaces: build through a temporary drive letter then
run_vcpkg() {
  local root_win; root_win="$(cygpath -w "$R")"
  if [[ "$root_win" == *" "* ]]; then
    local drive=""
    for d in W V U T S; do [ -d "/$(echo $d | tr A-Z a-z)" ] || { drive=$d; break; }; done
    [ -n "$drive" ] || { echo "no free drive letter for subst"; exit 1; }
    cmd.exe //c "subst $drive: \"$root_win\""
    trap "cmd.exe //c 'subst $drive: /d' >/dev/null" EXIT
    (cd "/$(echo $drive | tr A-Z a-z)/tools/vcpkg" && \
      VCPKG_ROOT="$drive:\\tools\\vcpkg" ./vcpkg.exe install "$@" --x-install-root="$drive:\\tools\\vcpkg_installed" --clean-after-build)
  else
    ./vcpkg.exe install "$@" --x-install-root="$(cygpath -w "$R/tools/vcpkg_installed")" --clean-after-build
  fi
}

run_vcpkg openssl:x64-uwp-static-md libplist:x64-uwp-static-md "ffmpeg[core,avcodec]:x64-uwp-static-md"
run_vcpkg openssl:x64-windows-static-md libplist:x64-windows-static-md openh264:x64-windows-static-md \
          "ffmpeg[core,avcodec,avformat,swresample]:x64-windows-static-md"
echo "dependencies installed in tools/vcpkg_installed"
