#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# Vater Terminal Emulator — VAXP-OS Debian (.deb) Packaging Script (Nisaba Engine)
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PKG_NAME="vater"
PKG_VERSION="$(sed -n "s/^[[:space:]]*version[[:space:]]*:[[:space:]]*'\([^']*\)'.*/\1/p" "$SCRIPT_DIR/meson.build" | head -n 1)"
PKG_VERSION="${PKG_VERSION:-0.1.0}"
PKG_ARCH="$(dpkg --print-architecture 2>/dev/null || echo "amd64")"
BUILD_DIR="$SCRIPT_DIR/build"
STAGE_DIR="$SCRIPT_DIR/pkg_stage/${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH}"
DEB_OUT="$SCRIPT_DIR/${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH}.deb"

echo "========================================================"
echo "  Packaging Vater Terminal v${PKG_VERSION} (${PKG_ARCH})"
echo "========================================================"

# 1. Configure & Compile via Meson / Ninja
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    echo "[*] Configuring Meson build directory (prefix=/usr, buildtype=release, LTO)..."
    meson setup "$BUILD_DIR" --prefix=/usr --buildtype=release -Doptimization=3 -Db_lto=true
else
    meson configure "$BUILD_DIR" --prefix=/usr
fi

echo "[*] Compiling targets (vater, vaterd)..."
meson compile -C "$BUILD_DIR" -j 4 vater vaterd

# 2. Prepare clean Debian package staging root
echo "[*] Preparing staging tree at: $STAGE_DIR"
rm -rf "$SCRIPT_DIR/pkg_stage"
mkdir -p "$STAGE_DIR/DEBIAN"

# 3. Install Meson targets & assets (assets/vater.desktop, assets/vater.svg, shell integration, terminfo source)
DESTDIR="$STAGE_DIR" meson install -C "$BUILD_DIR" --no-rebuild

# Compile terminfo/vater.ti directly into /usr/share/terminfo so TERM=vater works out of the box
if command -v tic >/dev/null 2>&1 && [ -f "$SCRIPT_DIR/terminfo/vater.ti" ]; then
    echo "[*] Compiling terminfo entry (vater.ti)..."
    mkdir -p "$STAGE_DIR/usr/share/terminfo"
    tic -x -o "$STAGE_DIR/usr/share/terminfo" "$SCRIPT_DIR/terminfo/vater.ti"
fi

# Strip binary symbols to reduce package footprint
if command -v strip >/dev/null 2>&1; then
    for bin in "$STAGE_DIR/usr/bin/vater" "$STAGE_DIR/usr/bin/vaterd"; do
        if [ -f "$bin" ]; then
            echo "[*] Stripping debug symbols from $(basename "$bin")..."
            strip --strip-unneeded "$bin"
        fi
    done
fi

INSTALLED_SIZE="$(du -sk "$STAGE_DIR/usr" | cut -f1)"

# 4. Generate DEBIAN/control metadata
cat > "$STAGE_DIR/DEBIAN/control" <<EOF
Package: ${PKG_NAME}
Version: ${PKG_VERSION}
Section: utils
Priority: optional
Architecture: ${PKG_ARCH}
Installed-Size: ${INSTALLED_SIZE}
Provides: x-terminal-emulator
Depends: libc6, libstdc++6, libgcc-s1, libgl1, libegl1, libglew2.2, libvulkan1, libgbm1, libdrm2, libwayland-client0, libwayland-egl1, libwayland-cursor0, libxkbcommon0, libx11-6, libxext6, libxrandr2, libinput10, libudev1
Maintainer: VAXP-OS Developer <developer@vaxp.org>
Homepage: https://www.vaxp.org/
Description: Vater Native Terminal Emulator and Persistent Session Daemon
 Native C++20 GPU-accelerated terminal emulator (vater) and standalone
 persistent session daemon (vaterd) built on the Nisaba Engine for VAXP-OS.
EOF

# 5. Maintainer scripts (desktop/icon cache refresh & x-terminal-emulator registration)
cat > "$STAGE_DIR/DEBIAN/postinst" <<'EOF'
#!/usr/bin/env bash
set -e
if [ "$1" = "configure" ]; then
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
    fi
    if command -v update-alternatives >/dev/null 2>&1; then
        update-alternatives --install /usr/bin/x-terminal-emulator x-terminal-emulator /usr/bin/vater 40 || true
    fi
fi
EOF

cat > "$STAGE_DIR/DEBIAN/prerm" <<'EOF'
#!/usr/bin/env bash
set -e
if [ "$1" = "remove" ] || [ "$1" = "deconfigure" ]; then
    if command -v update-alternatives >/dev/null 2>&1; then
        update-alternatives --remove x-terminal-emulator /usr/bin/vater || true
    fi
fi
EOF

cat > "$STAGE_DIR/DEBIAN/postrm" <<'EOF'
#!/usr/bin/env bash
set -e
if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
    fi
fi
EOF

chmod 0755 "$STAGE_DIR/DEBIAN/postinst" "$STAGE_DIR/DEBIAN/prerm" "$STAGE_DIR/DEBIAN/postrm"

# 6. Build the .deb archive
echo "[*] Building Debian package..."
dpkg-deb --root-owner-group --build "$STAGE_DIR" "$DEB_OUT"

rm -rf "$SCRIPT_DIR/pkg_stage"
echo "[+] Package created: $DEB_OUT"

