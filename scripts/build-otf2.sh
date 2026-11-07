#!/usr/bin/env bash
# Baut OTF2 (statisch) und installiert es nach <prefix>; das Trace-Modul der GUI
# (EDUMPI_TRACE=ON) findet es über -DOTF2_ROOT=<prefix>.
#   scripts/build-otf2.sh "$HOME/.local/otf2"
# Tut nichts, wenn <prefix>/bin/otf2-config schon existiert. Die Version entspricht
# dem OTF2, das Score-P 10.1 mitbringt (vendor/otf2).
set -euo pipefail

OTF2_VERSION=3.2
OTF2_SHA256=82b3a88a550cb8c3cec8fd45eca82cdcbaf945209977482471b4b5e430d64a8d
URL=https://perftools.pages.jsc.fz-juelich.de/cicd/otf2/tags/otf2-$OTF2_VERSION/otf2-$OTF2_VERSION.tar.gz

PREFIX="${1:?Aufruf: $0 <prefix>}"

if [[ -x "$PREFIX/bin/otf2-config" ]]; then
    echo "OTF2 ist schon installiert ($PREFIX)"
    exit 0
fi

BUILD="$(mktemp -d "${TMPDIR:-/tmp}/otf2-build.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT

file="$BUILD/otf2-$OTF2_VERSION.tar.gz"
curl -fL --retry 3 -o "$file" "$URL"
if command -v shasum >/dev/null; then
    echo "$OTF2_SHA256  $file" | shasum -a 256 -c --quiet
else
    echo "$OTF2_SHA256  $file" | sha256sum -c --quiet
fi
tar xzf "$file" -C "$BUILD"

JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
mkdir "$BUILD/otf2-build" && cd "$BUILD/otf2-build"
"../otf2-$OTF2_VERSION/configure" --prefix="$PREFIX" --enable-static --disable-shared
make -j"$JOBS"
make install

echo "Fertig: $PREFIX"
