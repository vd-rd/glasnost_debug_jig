#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 <serial-device> [extra idf.py flash args...]" >&2
    exit 1
fi

DEVICE="$1"
shift

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

podman run --rm -it \
    --device="$DEVICE" \
    -v "$FW_DIR:/project:Z" -w /project \
    docker.io/espressif/idf:release-v5.5 \
    idf.py -p "$DEVICE" flash "$@"
