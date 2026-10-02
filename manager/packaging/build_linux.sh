#!/usr/bin/env bash
# build_linux.sh - the Linux build of atmt_manager, run inside manager/packaging/Dockerfile's image
# (tools\build_manager.ps1 -Linux mounts the repository at /src; CI does the same).
#
#   bash manager/packaging/build_linux.sh [<payload dir>] [<app version>]
#
# Builds the app and its tests, runs the tests, and - with a payload - makes the AppImage:
#
#   AppDir/AppRun                                  starts usr/bin/atmt_manager
#   AppDir/usr/bin/atmt_manager                    GUI + CLI
#   AppDir/usr/share/atmt_manager/payload/         the bundled payload, a folder per component (PayloadRoots)
#   AppDir/atmt-manager.desktop, atmt-manager.png
#
# Output: dist/linux/atmt_manager (the bare binary, which tools\deploy_deck.ps1 runs over ssh) and,
# with a payload, dist/linux/atmt-manager-<version>-x86_64.AppImage. The build tree is
# build/manager-linux.
set -euo pipefail

PAYLOAD=${1:-}
VERSION=${2:-}
SRC=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$SRC/build/manager-linux"
DIST="$SRC/dist/linux"
mkdir -p "$DIST"

# The third-party sources are git submodules (third_party/); they have to be checked out on the host,
# which tools/build_manager.ps1 and `git clone --recursive` both do.
for dep in monocypher zstd SDL2 imgui; do
    [ -n "$(ls -A "$SRC/third_party/$dep" 2>/dev/null)" ] || {
        echo "third_party/$dep is empty - run: git submodule update --init --depth 1" >&2; exit 1; }
done

cmake -S "$SRC/manager" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release ${ATMT_CMAKE_ARGS:-}
cmake --build "$OUT" --target atmt_manager atmt_manager_test
"$OUT/atmt_manager_test"
cp "$OUT/atmt_manager" "$DIST/atmt_manager"

if [ -z "$PAYLOAD" ]; then
    echo "built $DIST/atmt_manager (no payload given: no AppImage)"
    exit 0
fi
if [ -z "$VERSION" ]; then
    VERSION=$(sed -n 's/.*set(ATMT_MANAGER_VERSION "\([^"]*\)".*/\1/p' "$SRC/manager/CMakeLists.txt" | head -1)
fi

APPDIR="$OUT/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/atmt_manager"
cp "$OUT/atmt_manager" "$APPDIR/usr/bin/"
strip "$APPDIR/usr/bin/atmt_manager"
cp -r "$PAYLOAD" "$APPDIR/usr/share/atmt_manager/payload"
cp "$SRC/manager/packaging/atmt-manager.desktop" "$APPDIR/"
cp "$SRC/manager/packaging/atmt-manager.png" "$APPDIR/"
cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
exec "$HERE/usr/bin/atmt_manager" "$@"
EOF
chmod +x "$APPDIR/AppRun"

IMAGE="$DIST/atmt-manager-$VERSION-x86_64.AppImage"
rm -f "$IMAGE"
ARCH=x86_64 appimagetool --appimage-extract-and-run --no-appstream "$APPDIR" "$IMAGE"
echo "built $IMAGE"
