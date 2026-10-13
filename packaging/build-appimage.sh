#!/usr/bin/env bash
# Builds dist/msfs-linuxrebind-x86_64.AppImage.
# Bundles libglfw (>= 3.4 is rare on older distros) and links libstdc++/libgcc statically; everything
# else (libGL, X11/Wayland client libs, glibc) comes from the host like any AppImage.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-appimage"
APPDIR="$BUILD/AppDir"
DIST="$ROOT/dist"
TOOL="$ROOT/packaging/tools/appimagetool-x86_64.AppImage"
ARCH="${ARCH:-x86_64}"
VERSION="$(sed -n 's/^project(msfs-linuxrebind VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"

RUNTIME="$ROOT/packaging/tools/runtime-$ARCH"
if [ ! -f "$RUNTIME" ]; then
    mkdir -p "$(dirname "$RUNTIME")"
    echo "downloading AppImage runtime..."
    curl -sS -L -o "$RUNTIME" "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-$ARCH"
fi
if [ ! -x "$TOOL" ]; then
    mkdir -p "$(dirname "$TOOL")"
    echo "downloading appimagetool..."
    curl -sS -L -o "$TOOL" "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage"
    chmod +x "$TOOL"
fi

rm -rf "$APPDIR"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DLINUXREBIND_STATIC_CXX=ON
ninja -C "$BUILD"
DESTDIR="$APPDIR" ninja -C "$BUILD" install

# Bundle GLFW next to the binary (it dlopens the platform libraries itself at runtime).
mkdir -p "$APPDIR/usr/lib"
GLFW="$(ldd "$APPDIR/usr/bin/msfs-linuxrebind" | awk '/libglfw\.so/ {print $3}')"
[ -n "$GLFW" ] && cp -L "$GLFW" "$APPDIR/usr/lib/"

cat > "$APPDIR/AppRun" <<'RUN'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/usr/bin/msfs-linuxrebind" "$@"
RUN
chmod +x "$APPDIR/AppRun"
cp "$ROOT/packaging/msfs-linuxrebind.desktop" "$APPDIR/"
cp "$ROOT/packaging/icons/msfs-linuxrebind-256.png" "$APPDIR/msfs-linuxrebind.png"
cp "$ROOT/packaging/icons/msfs-linuxrebind.svg" "$APPDIR/.DirIcon" 2>/dev/null || cp "$APPDIR/msfs-linuxrebind.png" "$APPDIR/.DirIcon"

mkdir -p "$DIST"
OUT="$DIST/msfs-linuxrebind-$VERSION-$ARCH.AppImage"
ARCH="$ARCH" "$TOOL" --appimage-extract-and-run --runtime-file "$RUNTIME" "$APPDIR" "$OUT"
echo "built $OUT"
