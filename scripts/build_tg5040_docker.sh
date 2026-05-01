#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Building Nimbus for tg5040 ==="

docker run --rm \
    -v "$PROJECT_DIR":/workspace \
    ghcr.io/loveretro/tg5040-toolchain \
    make -C /workspace/ports/tg5040 -f Makefile

echo "=== Build complete ==="
echo "Binary: build/tg5040/nimbus"
