#!/usr/bin/env bash
# Rebuild m8c and (re)install the .app into /Applications so double-clicking
# always launches the latest build. macOS only.
set -euo pipefail
cd "$(dirname "$0")/.."

export PKG_CONFIG_PATH="$(brew --prefix sdl3)/lib/pkgconfig:$(brew --prefix libserialport)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

echo "==> Building..."
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j

echo "==> Stopping running instances..."
pkill -f "m8c.app/Contents/MacOS/m8c" 2>/dev/null || true
sleep 1

echo "==> Installing to /Applications/m8c.app..."
rm -rf /Applications/m8c.app
cp -R build/m8c.app /Applications/m8c.app
codesign --force --deep --sign - --entitlements package/macos/Entitlements.plist \
  /Applications/m8c.app >/dev/null 2>&1 || true

echo "==> Done. Launch with: open /Applications/m8c.app"
