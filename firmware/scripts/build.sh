#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

podman run --rm -it \
    -v "$FW_DIR:/project:Z" -w /project \
    docker.io/espressif/idf:release-v5.5 \
    idf.py set-target esp32s3 build
