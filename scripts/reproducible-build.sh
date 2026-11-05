#!/bin/bash
# Oeeoo Coin — reproducible build + release packaging.
# Builds the three binaries twice (in two different directories) and
# compares their SHA-256 hashes: if they match, the build is
# reproducible — anyone with the same toolchain gets the same
# binaries from the same source. Requires: g++, CMake, OpenSSL,
# SQLite3 (see docs/BUILD_GUIDE.md).
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:-0.1.0}"
if git rev-parse --git-dir >/dev/null 2>&1; then
  SOURCE_DATE_EPOCH="$(git log -1 --format=%ct)"
else
  SOURCE_DATE_EPOCH="$(date +%s)"
  echo "warning: not a git checkout — SOURCE_DATE_EPOCH is today, not the commit date" >&2
fi
export SOURCE_DATE_EPOCH
export ZERO_AR_DATE=1
# No absolute build paths in the binaries.
FLAGS="-ffile-prefix-map=$PWD=. -Wdate-time -D_FORTIFY_SOURCE=0"

build_one() {
  local dir="$1"
  rm -rf "$dir"
  # NOTE: only CXX flags are passed. The project has no C sources,
  # so -DCMAKE_C_FLAGS was an unused variable (CMake warned about
  # it); it was removed in Milestone 13.
  cmake -S . -B "$dir" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="$FLAGS" >/dev/null
  cmake --build "$dir" >/dev/null
}

echo "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH"
build_one build-repro-a
build_one build-repro-b

REPRODUCIBLE=1
for bin in oeeoo-node oeeoo-genesis oeeoo-cli; do
  a="$(sha256sum "build-repro-a/$bin" | cut -d' ' -f1)"
  b="$(sha256sum "build-repro-b/$bin" | cut -d' ' -f1)"
  if [ "$a" != "$b" ]; then
    echo "NOT REPRODUCIBLE: $bin differs ($a vs $b)" >&2
    REPRODUCIBLE=0
  fi
done
if [ "$REPRODUCIBLE" -eq 1 ]; then
  echo "REPRODUCIBLE: all three binaries are byte-identical across two builds"
fi

# Release package (from the first build).
rm -rf "release-$VERSION" "oeeoo-coin-$VERSION.tar.gz" "oeeoo-coin-$VERSION-2.tar.gz"
mkdir -p "release-$VERSION"
cp build-repro-a/oeeoo-node build-repro-a/oeeoo-genesis build-repro-a/oeeoo-cli "release-$VERSION/"
cp README.md LICENSE "release-$VERSION/"
cp docs/MAINNET_LAUNCH.md docs/BUILD_GUIDE.md "release-$VERSION/"
if git rev-parse --git-dir >/dev/null 2>&1; then
  git rev-parse HEAD > "release-$VERSION/SOURCE.txt"
  echo "source commit: $(cat release-$VERSION/SOURCE.txt)" >> "release-$VERSION/SOURCE.txt"
else
  echo "source commit: (not a git checkout)" > "release-$VERSION/SOURCE.txt"
fi
( cd "release-$VERSION" && sha256sum oeeoo-node oeeoo-genesis oeeoo-cli > checksums.txt )
# Reproducible archive: sorted entries, fixed mtime/owner/group,
# and no timestamp in the gzip header (GZIP=-n). Build it twice
# and compare, so the archive itself is verified reproducible.
export GZIP=-n
tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 \
  --numeric-owner -czf "oeeoo-coin-$VERSION.tar.gz" "release-$VERSION"
tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 \
  --numeric-owner -czf "oeeoo-coin-$VERSION-2.tar.gz" "release-$VERSION"
TAR_A="$(sha256sum "oeeoo-coin-$VERSION.tar.gz" | cut -d' ' -f1)"
TAR_B="$(sha256sum "oeeoo-coin-$VERSION-2.tar.gz" | cut -d' ' -f1)"
rm -f "oeeoo-coin-$VERSION-2.tar.gz"
if [ "$TAR_A" != "$TAR_B" ]; then
  echo "NOT REPRODUCIBLE: archive differs ($TAR_A vs $TAR_B)" >&2
  REPRODUCIBLE=0
else
  echo "REPRODUCIBLE: archive is byte-identical across two builds"
fi
echo "release package: oeeoo-coin-$VERSION.tar.gz"
sha256sum "oeeoo-coin-$VERSION.tar.gz"
[ "$REPRODUCIBLE" -eq 1 ]
