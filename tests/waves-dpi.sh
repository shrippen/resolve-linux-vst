#!/usr/bin/env bash
# Plugin-Test (nur lokal, braucht Waves über yabridge, Xvfb und python3 mit PIL): öffnet ein
# Waves-Plugin wie Resolve, mit der eingebauten Wine-DPI und Faktor 3.75, in einem eigenen
# X-Server und prüft, ob Waves das ganze Editorfenster füllt. Mit falscher DPI zeichnet Waves
# kleiner, oben links, und Klicks treffen nicht (Wine 9.21: Faktor 180/DPI).
#
#   tests/waves-dpi.sh [Klasse.vst]     Exit 0 = ok, 1 = Fehler, 77 = übersprungen
set -euo pipefail

here=$(dirname "$(readlink -f "$0")")
root=$(dirname "$here")
links=${XDG_DATA_HOME:-$HOME/.local/share}/resolve-linux-vst/vst
plugin=${1:-$links/VST3/yabridge/WaveShell1-VST3 14.12_x64/Butch Vig Vocals Mono.vst}
display=:97
min_fill=0.97

skip() { echo "waves-dpi: übersprungen, $*" >&2; exit 77; }

[[ -e $plugin ]] || skip "kein Plugin $plugin"
command -v Xvfb >/dev/null || skip "kein Xvfb"
python3 -I -c 'import PIL' 2>/dev/null || skip "kein python3-PIL"
# Eine laufende Gruppe hat ihre DPI schon gelesen.
pgrep -f 'yabridge-host\.exe\.so group ' >/dev/null && skip "eine yabridge-Plugingruppe läuft"

work=$(mktemp -d -p "${TMPDIR:-$HOME/.cache}")
# Aufräumen wie nach Resolve: Gruppe auslaufen lassen, dann Wine-DPI und Gruppenreste
# (WavesLocalServer.exe) zurücksetzen.
cleanup() {
    kill "${xvfb:-}" 2>/dev/null || true
    for _ in {1..30}; do
        pgrep -f 'yabridge-host\.exe\.so group ' >/dev/null || break
        sleep 1
    done
    "$root/resolve-linux-vst" restore
    rm -rf "$work"
}
trap cleanup EXIT

# Leere plugins.conf: es gilt die eingebaute Vorgabe.
mkdir -p "$work/config"
Xvfb "$display" -screen 0 3840x2400x24 >/dev/null 2>&1 &
xvfb=$!
sleep 2

DISPLAY=$display XDG_CONFIG_HOME=$work/config RESOLVE_LINUX_VST_EDITOR_SCALE=3.75 \
    timeout 150 "$root/build/editor-test" "$plugin" "$work/shot.png" 10 >"$work/log" 2>&1 \
    || { tail -20 "$work/log" >&2; echo "waves-dpi: editor-test fehlgeschlagen" >&2; exit 1; }

# Anteil von Breite und Höhe, den Waves zeichnet: letztes Pixel, das nicht Hintergrund des
# Testfensters (grau 64) oder schwarz ist, in einer Zeile und Spalte bei einem Viertel.
python3 -I - "$work/shot.png" "$min_fill" <<'EOF'
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert("RGB")
w, h = im.size
background = {(64, 64, 64), (0, 0, 0)}
xs = [x for x in range(w) if im.getpixel((x, h // 4)) not in background]
ys = [y for y in range(h) if im.getpixel((w // 4, y)) not in background]
fill_x = (max(xs) + 1) / w if xs else 0
fill_y = (max(ys) + 1) / h if ys else 0
ok = min(fill_x, fill_y) >= float(sys.argv[2])
print(f"waves-dpi: Fenster {w}x{h}, gezeichnet {fill_x:.0%} x {fill_y:.0%}: {'ok' if ok else 'FEHLER'}")
sys.exit(0 if ok else 1)
EOF
