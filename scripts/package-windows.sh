#!/usr/bin/env bash
# Packages the cross-compiled Windows build into the same zip the Windows CI
# job produces, so the release layout can be checked locally without CI.
#
#   scripts/package-windows.sh [version]
#
# version defaults to `git describe --tags --always` (e.g. v0.11.0), which is the
# tag GitHub Actions would use, so the two zips come out with the same name.
# Requires build-win/game.exe (see .github/workflows/windows.yml for the CI
# build; a local cross-build puts it in the same place).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

version="${1:-}"
if [ -z "$version" ]; then
    version="$(git describe --tags --always 2>/dev/null || echo dev)"
fi

name="clean-room-grand-strategy-$version-windows-x64"
stage="dist/$name"
zip="dist/$name.zip"
exe="build-win/game.exe"

if [ ! -f "$exe" ]; then
    echo "error: $exe not found, so there is nothing to package." >&2
    echo "Build the Windows target first; a cross-compiled build puts game.exe there." >&2
    exit 1
fi

# Everything the zip promises to contain, checked before anything is written so a
# missing file fails loudly instead of producing an empty archive.
for path in data web play.bat START_HERE.md README.md LICENSE; do
    if [ ! -e "$path" ]; then
        echo "error: $path is missing from $root" >&2
        exit 1
    fi
done

if ! command -v zip >/dev/null 2>&1; then
    echo "error: the 'zip' command is required to build the archive" >&2
    exit 1
fi

rm -rf "$stage" "$zip"
mkdir -p "$stage"

cp "$exe" "$stage/game.exe"
cp -r data "$stage/data"
cp -r web "$stage/web"
cp play.bat START_HERE.md README.md LICENSE "$stage/"

# Zip from dist/ so the archive holds one top-level folder, matching CI.
( cd dist && zip -qr "$name.zip" "$name" )

echo "packaged $zip"
unzip -l "$zip"
