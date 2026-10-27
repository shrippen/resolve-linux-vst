#!/usr/bin/env bash
# resolve-linux-vst installer.
#
#   curl -fsSL https://raw.githubusercontent.com/shrippen/resolve-linux-vst/main/install.sh | bash
#
# Downloads the latest release from GitHub and installs it into ~/.local (no root needed).
#
#   install.sh [--prefix DIR] [--version X.Y.Z]   install or update
#   install.sh --source                           build and install from this checkout
#   install.sh --uninstall [--prefix DIR]         remove it again
set -euo pipefail

repo=shrippen/resolve-linux-vst
asset=resolve-linux-vst-x86_64.tar.gz
prefix=$HOME/.local
version=""
mode=release

die() { echo "resolve-linux-vst: $*" >&2; exit 1; }
say() { echo "==> $*"; }

while (($#)); do
    case $1 in
    --prefix) prefix=${2:?--prefix needs a directory}; shift ;;
    --version) version=${2#v}; shift ;;
    --source) mode=source ;;
    --uninstall) mode=uninstall ;;
    -h | --help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (see --help)" ;;
    esac
    shift
done

files=(bin/resolve-linux-vst lib/resolve-linux-vst share/applications/resolve-linux-vst.desktop share/doc/resolve-linux-vst)

if [[ $mode == uninstall ]]; then
    for f in "${files[@]}"; do rm -rf "${prefix:?}/$f"; done
    say "Removed resolve-linux-vst from $prefix."
    echo "    Your settings stay: ~/.config/resolve-linux-vst, ~/.local/share/resolve-linux-vst, ~/.local/state/resolve-linux-vst."
    exit 0
fi

[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || die "only Linux on x86_64 is supported (DaVinci Resolve's own platform)"

# Tools the launcher script needs at run time.
missing=()
for tool in nm awk sha1sum; do command -v "$tool" >/dev/null || missing+=("$tool"); done
if ((${#missing[@]})); then
    die "missing: ${missing[*]} (install binutils, gawk or mawk and coreutils with your package manager)"
fi

resolve=${RESOLVE_BIN:-/opt/resolve/bin/resolve}
[[ -x $resolve ]] || echo "warning: DaVinci Resolve not found at $resolve (set RESOLVE_BIN if it lives elsewhere)" >&2

if [[ $mode == source ]]; then
    here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
    [[ -f $here/Makefile && -d $here/src ]] || die "--source needs a checkout of the repository"
    for tool in make g++ curl; do command -v "$tool" >/dev/null || die "--source needs $tool"; done
    say "Building from $here"
    git -C "$here" submodule update --init
    make -C "$here" --no-print-directory -s all aac
    make -C "$here" --no-print-directory -s install PREFIX="$prefix"
else
    for tool in curl tar sha256sum; do command -v "$tool" >/dev/null || die "missing: $tool"; done
    if [[ -n $version ]]; then
        base=https://github.com/$repo/releases/download/v$version
    else
        base=https://github.com/$repo/releases/latest/download
    fi
    work=${XDG_CACHE_HOME:-$HOME/.cache}/resolve-linux-vst/install
    rm -rf "$work"
    mkdir -p "$work"
    trap 'rm -rf "$work"' EXIT

    say "Downloading ${version:+v$version }from github.com/$repo"
    curl -fL --proto =https --progress-bar -o "$work/$asset" "$base/$asset"
    curl -fsSL --proto =https -o "$work/$asset.sha256" "$base/$asset.sha256"
    (cd "$work" && sha256sum -c --quiet "$asset.sha256") || die "checksum mismatch, download broken"

    tar -C "$work" -xzf "$work/$asset"
    tree=$(find "$work" -mindepth 1 -maxdepth 1 -type d -name 'resolve-linux-vst-*' | head -n1)
    [[ -n $tree ]] || die "unexpected archive layout"
    for f in "${files[@]}"; do rm -rf "${prefix:?}/$f"; done
    mkdir -p "$prefix"
    cp -a "$tree"/. "$prefix"/
    sed -i "s|^Exec=.*|Exec=$prefix/bin/resolve-linux-vst %u|" "$prefix/share/applications/resolve-linux-vst.desktop"
fi

command -v update-desktop-database >/dev/null && update-desktop-database -q "$prefix/share/applications" 2>/dev/null || true
say "Installed into $prefix"

if [[ -x $resolve ]]; then
    say "Checking your DaVinci Resolve (resolve-linux-vst check)"
    "$prefix/bin/resolve-linux-vst" check || echo "warning: some hooks do not match this Resolve version, see the README (\"After a Resolve update\")" >&2
fi

case ":$PATH:" in
*":$prefix/bin:"*) ;;
*) echo "note: $prefix/bin is not in your PATH; the menu entry works anyway" >&2 ;;
esac

cat <<EOF

Next steps
  1. Start Resolve with "DaVinci Resolve (VST)" from your menu, or run: resolve-linux-vst
  2. In Resolve: Preferences → User → Audio Plugins → VST Plugin Paths → add
       ${RESOLVE_LINUX_VST_LINKS:-$HOME/.local/share/resolve-linux-vst/vst}
     then let Resolve scan. Your plug-ins appear in the Effects Library under Audio FX.

Docs: https://github.com/$repo#readme
EOF
