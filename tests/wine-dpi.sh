#!/usr/bin/env bash
# Plugin-Test (nur lokal, braucht yabridge-Plugins, Xvfb und python3 mit PIL): öffnet Plugins
# nacheinander wie Resolve, mit den eingebauten Einstellungen und Faktor 3.75, in einem eigenen
# X-Server und prüft, ob jedes das ganze Editorfenster füllt. Wine vergrößert mit
# Basis-DPI / DPI des Plugin-Prozesses (Basis: DPI beim Start der Wine-Sitzung); stimmt die
# Rechnung nicht, zeichnet das Plugin kleiner, oben links, und Klicks treffen nicht.
# Vorgabe: iZotope Neoverb dreimal (wine-zoom), dazwischen endet die Wine-Sitzung jedes Mal
# (die Basis darf nicht vom zuletzt gesetzten Wert ausgehen, sonst sinkt sie), dann Waves
# Butch Vig (die Gruppe läuft am längsten).
#
#   tests/wine-dpi.sh [Klasse.vst …]    Exit 0 = ok, 1 = Fehler, 77 = übersprungen
set -euo pipefail

here=$(dirname "$(readlink -f "$0")")
root=$(dirname "$here")
links=${XDG_DATA_HOME:-$HOME/.local/share}/resolve-linux-vst/vst
plugins=("$@")
if ((${#plugins[@]} == 0)); then
    neoverb=$links/VST3/yabridge/iZotope/Neoverb.vst
    plugins=("$neoverb" "$neoverb" "$neoverb"
        "$links/VST3/yabridge/WaveShell1-VST3 14.12_x64/Butch Vig Vocals Mono.vst")
fi
display=:97
min_fill=0.97

skip() { echo "wine-dpi: übersprungen, $*" >&2; exit 77; }

for plugin in "${plugins[@]}"; do
    [[ -e $plugin ]] || skip "kein Plugin $plugin"
done
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

# Anteil von Breite und Höhe, den das Plugin zeichnet: letztes Pixel, das nicht Hintergrund des
# Testfensters (grau 64) oder schwarz ist, in einer Zeile und Spalte bei einem Viertel.
measure() {
    python3 -I - "$1" "$min_fill" <<'EOF'
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
print(f"Fenster {w}x{h}, gezeichnet {fill_x:.0%} x {fill_y:.0%}: {'ok' if ok else 'FEHLER'}")
sys.exit(0 if ok else 1)
EOF
}

# Leere plugins.conf: es gelten die eingebauten Vorgaben.
mkdir -p "$work/config"
Xvfb "$display" -screen 0 3840x2400x24 >/dev/null 2>&1 &
xvfb=$!
sleep 2

# Wartet, bis keine Wine-Sitzung mehr läuft (wineserver endet einige Sekunden nach dem letzten
# Wine-Prozess). Hält ein Prozess sie offen (z. B. MicrosoftEdgeUpdate.exe im Prefix), prüft der
# Test den Neustart der Sitzung nicht: übersprungen.
await_session_end() {
    for _ in {1..30}; do
        pgrep -x wineserver >/dev/null || return 0
        sleep 1
    done
    # Ein schon gemessener Fehler geht vor.
    if ((fail)); then
        echo "wine-dpi: eine Wine-Sitzung endet nicht, Abbruch nach Fehler" >&2
        exit 1
    fi
    skip "eine Wine-Sitzung endet nicht ($(pgrep -a -x wineserver | head -1))"
}

fail=0
for plugin in "${plugins[@]}"; do
    await_session_end
    name=$(basename "$plugin")
    rm -f "$work/shot.png"
    status=0
    DISPLAY=$display XDG_CONFIG_HOME=$work/config RESOLVE_LINUX_VST_EDITOR_SCALE=3.75 \
        timeout 150 "$root/build/editor-test" "$plugin" "$work/shot.png" 10 >"$work/log" 2>&1 || status=$?
    # Geprüft wird die Zeichnung; manche Plugins brechen unter Xvfb erst beim Abbau ab.
    if [[ ! -s $work/shot.png ]]; then
        tail -20 "$work/log" >&2
        echo "wine-dpi: $name: keine Aufnahme (editor-test Exit $status): FEHLER" >&2
        fail=1
        continue
    fi
    if ((status != 0)); then
        echo "wine-dpi: $name: Hinweis: editor-test Exit $status nach der Aufnahme" >&2
    fi
    echo -n "wine-dpi: $name: "
    measure "$work/shot.png" || fail=1
done
exit "$fail"
