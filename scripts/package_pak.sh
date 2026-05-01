#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Packaging Nimbus.pak ==="

# Copy binary
cp "$PROJECT_DIR/build/tg5040/nimbus" "$PROJECT_DIR/ports/tg5040/pak/bin/nimbus"
chmod +x "$PROJECT_DIR/ports/tg5040/pak/bin/nimbus"

# Copy CA certs
mkdir -p "$PROJECT_DIR/ports/tg5040/pak/lib"
cp "$PROJECT_DIR/build/tg5040/lib/cacert.pem" "$PROJECT_DIR/ports/tg5040/pak/lib/cacert.pem"

# Sync default location
cp "$PROJECT_DIR/assets/default_location.txt" "$PROJECT_DIR/ports/tg5040/pak/assets/default_location.txt"

# Create zip
mkdir -p "$PROJECT_DIR/dist"
cd "$PROJECT_DIR/ports/tg5040/pak"
zip -r "$PROJECT_DIR/dist/Nimbus.tg5040.pak.zip".
cd "$PROJECT_DIR"

echo "=== Package: dist/Nimbus.tg5040.pak.zip ==="
