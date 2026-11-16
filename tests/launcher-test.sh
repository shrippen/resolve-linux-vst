#!/usr/bin/env bash
# Test des Launchers ohne Resolve (RESOLVE_BIN=true, eigenes HOME): nach dem Ende von Resolve
# beendet er Prozesse, die einen yabridge-Gruppen-Socket weiter belegen, wenn kein
# Gruppenprozess mehr darauf hört (WavesLocalServer.exe erbt den Socket der Waves-Gruppe und
# blockiert sonst die nächste Gruppe). Gruppenprozesse und was sie mit ihnen teilen, bleiben.
set -euo pipefail

here=$(dirname "$(readlink -f "$0")")
root=$(dirname "$here")
work=$(mktemp -d -p "${TMPDIR:-$HOME/.cache}")
pids=()
trap 'kill "${pids[@]}" 2>/dev/null || true; rm -rf "$work"' EXIT
run=$work/run
mkdir -p "$run" "$work/home" "$work/none"

# Hört auf einem Unix-Socket, mit argv wie ein Gruppenprozess (host: "yabridge-host.exe.so
# group") oder wie ein Windows-Prozess (stale). share: Gruppenprozess, dessen Kind den Socket
# erbt, wie WavesLocalServer.exe bei laufender Gruppe.
listen() {
    python3 -I - "$1" "$2" <<'EOF' &
import os, socket, subprocess, sys
sock, mode = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX)
s.bind(sock)
s.listen()
os.set_inheritable(s.fileno(), True)
sleep = ["python3", "-c", "import time; time.sleep(600)"]
if mode == "share":
    subprocess.Popen(sleep + ["WavesLocalServer.exe"], pass_fds=(s.fileno(),))
name = ["WavesLocalServer.exe"] if mode == "stale" else ["yabridge-host.exe.so", "group", sock]
os.execvp("python3", sleep + name)
EOF
    pids+=($!)
}

listen "$run/yabridge-group-stale-x64.sock" stale
stale=${pids[-1]}
listen "$run/yabridge-group-alive-x64.sock" host
alive=${pids[-1]}
listen "$run/yabridge-group-shared-x64.sock" share
sleep 1
shared=$(pgrep -P "${pids[-1]}" || true)
pids+=($shared)

launcher() {
    HOME=$work/home XDG_RUNTIME_DIR=$run VST_PATH=$work/none VST3_PATH=$work/none \
        "$root/resolve-linux-vst" >"$work/log" 2>&1
}

# Statt Resolve: scheitert, wenn der verwaiste Prozess noch lebt. Reste eines früheren Laufs
# müssen schon vor dem Start weg sein (eine Gruppe kann Resolve kurz überleben).
cat >"$work/resolve" <<EOF
#!/bin/sh
! kill -0 $stale 2>/dev/null
EOF
chmod +x "$work/resolve"
if ! RESOLVE_BIN=$work/resolve launcher; then
    cat "$work/log" >&2
    echo "launcher-test: Launcher fehlgeschlagen (verwaister Prozess beim Start noch da?): FEHLER" >&2
    exit 1
fi
sleep 1

fail=0
expect() { # Beschreibung, PID, erwartet: lebt | weg
    local state=weg
    if kill -0 "$2" 2>/dev/null; then
        state=lebt
    fi
    if [[ $state == "$3" ]]; then
        echo "launcher-test: $1 $state: ok"
        return
    fi
    echo "launcher-test: $1 $state, erwartet $3: FEHLER" >&2
    fail=1
}
if [[ -z $shared ]]; then
    echo "launcher-test: Testaufbau: geteilter Prozess fehlt" >&2
    exit 1
fi
expect "Prozess auf verwaistem Gruppen-Socket" "$stale" weg
expect "Gruppenprozess" "$alive" lebt
expect "Prozess, der den Socket mit einer laufenden Gruppe teilt" "$shared" lebt

# Ein Socket in der Liste, zu dem ss keine PID (mehr) zeigt: Gruppe gerade beendet oder
# Socket eines anderen Benutzers. Der Launcher läuft weiter (set -e).
mkdir -p "$work/bin"
cat >"$work/bin/ss" <<EOF
#!/bin/sh
case "\$*" in
*-xlH*) echo "u_str LISTEN 0 1 $run/yabridge-group-gone-x64.sock 1 * 0" ;;
esac
EOF
chmod +x "$work/bin/ss"
if PATH=$work/bin:$PATH RESOLVE_BIN=true launcher; then
    echo "launcher-test: Socket ohne PID: ok"
else
    cat "$work/log" >&2
    echo "launcher-test: Socket ohne PID bricht den Launcher ab: FEHLER" >&2
    fail=1
fi
exit "$fail"
