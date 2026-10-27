#!/usr/bin/env bash
# Baut libresolve-linux-vst.so bei Bedarf und startet DaVinci Resolve mit VST2-Lader,
# direkt aus diesem Ordner (ohne make install). Argumente gehen an Resolve.
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"
make --no-print-directory -s all aac
exec ./resolve-linux-vst "$@"
