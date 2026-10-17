#!/usr/bin/env bash
# Creates the self-signed certificate used to sign Dev Mode (sideload) packages.
# Its subject must equal the Publisher in src/app/Package.appxmanifest. Store uploads are not
# signed with it (Microsoft signs Store packages). Needs openssl (included with Git for Windows).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MAN="$ROOT/src/app/Package.appxmanifest"
PUBLISHER=$(grep -o 'Publisher="[^"]*"' "$MAN" | head -1 | sed 's/Publisher="\(.*\)"/\1/')
OUT="$ROOT/src/app/ScreenMirrorServer_TemporaryKey.pfx"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
MSYS_NO_PATHCONV=1 openssl req -x509 -newkey rsa:2048 -keyout "$TMP/key.pem" -out "$TMP/cert.pem" -days 3650 -nodes \
  -subj "/$PUBLISHER" -addext "extendedKeyUsage=codeSigning" -addext "basicConstraints=critical,CA:FALSE" \
  -addext "keyUsage=critical,digitalSignature" 2>/dev/null
# 3DES/SHA1 PKCS#12 so that Windows' SignTool can read it
openssl pkcs12 -export -out "$OUT" -inkey "$TMP/key.pem" -in "$TMP/cert.pem" -passout pass: \
  -certpbe PBE-SHA1-3DES -keypbe PBE-SHA1-3DES -macalg sha1
echo "created $OUT for $PUBLISHER"
