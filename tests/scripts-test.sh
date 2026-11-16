#!/usr/bin/env bash
# Test: Skripte, die README und Landing Page mit ./ aufrufen, sind ausführbar (sonst
# "Keine Berechtigung", z. B. ./install.sh --source).
set -euo pipefail

root=$(dirname "$(dirname "$(readlink -f "$0")")")
scripts=(install.sh start.sh resolve-linux-vst scripts/release-notes.sh)

fail=0
for script in "${scripts[@]}"; do
    if [[ -x $root/$script ]]; then
        echo "ok      $script ausführbar"
        continue
    fi
    echo "FEHLER  $script nicht ausführbar"
    fail=1
done
exit "$fail"
