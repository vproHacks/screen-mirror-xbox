# Source this to keep all vcpkg state inside the repository folder.
export R="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export VCPKG_ROOT="$R/tools/vcpkg"
export VCPKG_DEFAULT_BINARY_CACHE="$(cygpath -w "$R/tools/vcpkg-cache")"
export X_VCPKG_REGISTRIES_CACHE="$(cygpath -w "$R/tools/vcpkg-reg-cache")"
export VCPKG_DISABLE_METRICS=1
mkdir -p "$R/tools/vcpkg-cache" "$R/tools/vcpkg-reg-cache" "$R/tools/logs"
