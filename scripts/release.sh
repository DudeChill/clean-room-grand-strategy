#!/usr/bin/env bash
# Builds a distributable release tarball and (with --publish) creates the GitHub
# release. Everything shipped is built from this repository: engine, data, client.
#
#   scripts/release.sh prepare            # verify + package into dist/
#   scripts/release.sh publish v0.1.0     # prepare, tag, push, create release
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

version="${2:-v0.1.0}"
dist="$root/dist"
name="clean-room-grand-strategy-$version"

prepare() {
  echo "== build =="
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build -j"$(nproc)"

  echo "== tests =="
  ./build/hoi_tests

  echo "== determinism / audit / save round trip =="
  ./build/game --days 3 --audit --quiet
  ./build/game --days 20 --save-every-days 10 --quiet

  echo "== package =="
  rm -rf "$dist/$name"
  mkdir -p "$dist/$name"
  install -Dm755 build/game "$dist/$name/game"
  install -Dm755 build/genmap "$dist/$name/genmap"
  cp -r data web docs src tests tools scripts "$dist/$name/"
  cp README.md ARCHITECTURE.md ROADMAP.md TODO.md DEVLOG.md LICENSE CMakeLists.txt \
     "$dist/$name/"
  rm -rf "$dist/$name"/docs/benchmarks/*.save "$dist/$name"/docs/benchmarks/*.log
  tar -C "$dist" -czf "$dist/$name.tar.gz" "$name"
  sha256sum "$dist/$name.tar.gz" > "$dist/$name.tar.gz.sha256"
  echo "packaged $dist/$name.tar.gz ($(stat -c %s "$dist/$name.tar.gz") bytes)"
}

publish() {
  prepare
  if ! git rev-parse --git-dir >/dev/null 2>&1; then
    echo "not a git repository" >&2
    exit 1
  fi
  git add -A
  if ! git diff --cached --quiet; then
    git commit -m "release $version"
  fi
  git tag -f "$version"
  git push origin HEAD --tags
  gh release create "$version" "$dist/$name.tar.gz" "$dist/$name.tar.gz.sha256" \
    --title "Clean Room Grand Strategy $version" \
    --notes-file "docs/reviews/$version.md"
}

case "${1:-prepare}" in
  prepare) prepare ;;
  publish) publish ;;
  *) echo "usage: $0 [prepare|publish] [version]" >&2; exit 2 ;;
esac
