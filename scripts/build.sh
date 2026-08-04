#!/usr/bin/env bash
# Cross-compile iperf-amigaos4 in the walkero AmigaOS 4 docker image.
#
# Usage:
#   ./scripts/build.sh           # build
#   ./scripts/build.sh clean     # rm build/
#   ./scripts/build.sh shell     # interactive shell in container
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="walkero/amigagccondocker:os4-gcc11"

ARCH="$(uname -m)"
case "$ARCH" in
  arm64|aarch64) IMAGE="walkero/amigagccondocker:os4-gcc11-arm64" ;;
esac

case "${1:-all}" in
  clean) rm -rf "$HERE/build"; exit 0 ;;
  shell) exec docker run --rm -it -v "$HERE:/work" -w /work "$IMAGE" bash ;;
esac

exec docker run --rm -v "$HERE:/work" -w /work "$IMAGE" make "$@"
