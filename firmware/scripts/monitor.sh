#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 <serial-device>" >&2
    exit 1
fi

DEVICE="$1"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

podman run --rm -it \
    --device="$DEVICE" \
    -e "TERM=${TERM:-xterm}" \
    -v "$FW_DIR:/project:Z" -w /project \
    docker.io/espressif/idf:release-v5.5 \
    idf.py -p "$DEVICE" monitor
