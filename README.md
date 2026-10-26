<p align="center"><img src="docs/icon.svg" width="96" alt=""></p>

<h1 align="center">resolve-vst</h1>

<p align="center">
  <b>VST2 and VST3 plug-ins, AAC audio and Nikon N-RAW for DaVinci Resolve on Linux.</b><br>
  One small preload library. No changes to Resolve's files.
</p>

<p align="center">
  <a href="https://shrippen.github.io/resolve-vst/">Website</a> ·
  <a href="#install">Install</a> ·
  <a href="#quick-start">Quick start</a> ·
  <a href="docs/HOW-IT-WORKS.md">How it works</a> ·
  <a href="#after-a-resolve-update">After a Resolve update</a>
</p>

---

DaVinci Resolve on Linux ships the complete VST host of the Windows and macOS versions, but the
one function that loads a plug-in is an empty stub, so Resolve never finds a single plug-in. It
also refuses AAC audio (phones, cameras, screen recordings) and Nikon's N-RAW video, and its MP4
renders with an AAC encoder plug-in miss the AAC profile, so browsers play them without sound.

resolve-vst fills those gaps at run time:

| | What you get |
|---|---|
| **VST2** | Native Linux VST2 plug-ins and Windows VST2 plug-ins through [yabridge](https://github.com/robbert-vdh/yabridge) show up in the Fairlight effects library. |
| **VST3** | A built-in VST3-to-VST2 adapter: native and yabridge VST3 plug-ins (iZotope, Waves, ToneBoosters, …), with editors, automation and state. |
| **LV2, CLAP** | Through [Carla](https://kx.studio/Applications:Carla), which is a VST2 plug-in itself. |
| **Categories** | Plug-ins land in Resolve's categories (Dynamics, EQ, Reverb, …) instead of "Uncategorized". |
| **HiDPI** | Plug-in editors get the right size and scale on scaled displays, including Wine plug-ins. |
| **AAC import** | MP4, MOV and M4A clips with AAC audio play with sound. |
| **AAC render** | MP4/MOV renders with an AAC encoder plug-in get a correct AAC-LC description, so browsers play them. |
| **N-RAW** | Nikon `.nev` clips open through Resolve's own RED decoder. |

Tested with DaVinci Resolve Studio 21.1.1 on Arch Linux.

> [!NOTE]
> resolve-vst is an independent project. It is not affiliated with or endorsed by Blackmagic
> Design. It relies on Resolve internals, so a Resolve update can break parts of it;
> `resolve-vst check` tells you which (see [After a Resolve update](#after-a-resolve-update)).

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/shrippen/resolve-vst/main/install.sh | bash
```

The installer downloads the latest release from GitHub (checksum verified), installs it into
`~/.local` (no root needed), adds a menu entry **DaVinci Resolve (VST)** and checks your Resolve
installation with `resolve-vst check`.

Requirements: Linux on x86_64, DaVinci Resolve in `/opt/resolve` (otherwise set `RESOLVE_BIN`),
`nm` (binutils) and `awk`. Optional: [yabridge](https://github.com/robbert-vdh/yabridge) for
Windows plug-ins, [Carla](https://kx.studio/Applications:Carla) for LV2 and CLAP.

Options: `install.sh --version 0.1.0`, `--prefix DIR`, `--uninstall`.

<details>
<summary>Build from source</summary>

Needs `g++` (C++17), `make`, `curl` and `nasm` or `yasm` (for the AAC decoder).

```sh
git clone --recursive https://github.com/shrippen/resolve-vst.git
cd resolve-vst
./install.sh --source        # make, make aac, make install into ~/.local
```

Or run it straight from the checkout without installing: `./start.sh` builds what is missing and
starts Resolve. Make targets: `all`, `aac` (downloads FFmpeg 6.0 and builds the AAC decoder),
`test`, `install`/`uninstall` (`PREFIX=…`), `dist`, `editor-test`.

</details>

## Quick start

1. Start Resolve with **DaVinci Resolve (VST)** from your application menu, or run `resolve-vst`.
2. In Resolve: **Preferences → User → Audio Plugins → VST Plugin Paths**, add
   `~/.local/share/resolve-vst/vst` and let Resolve scan.
3. Your plug-ins appear in the **Effects Library → Audio FX**, sorted into categories after the
   next start.

Always start Resolve through `resolve-vst` (or the menu entry). Started without it, Resolve's
scanner finds nothing and marks every plug-in as failed; run `resolve-vst rescan` afterwards.

AAC and N-RAW need no setup: import the clips as usual.

## Plug-ins

`resolve-vst` collects plug-ins from these places and links them into
`~/.local/share/resolve-vst/vst`, the one folder Resolve scans:

| Kind | Searched in |
|---|---|
| VST2 (native) | `~/.vst`, `~/.lxvst`, `/usr/local/lib/vst`, `/usr/lib/vst` (or `$VST_PATH`) |
| VST2 (Windows, yabridge) | `~/.vst/yabridge` (yabridge's default VST2 target) |
| VST3 (native and yabridge) | `~/.vst3`, `/usr/local/lib/vst3`, `/usr/lib/vst3` (or `$VST3_PATH`) |

Not supported: instruments with MIDI input, sidechain inputs, ARA, 64-bit processing.

### Commands

```text
resolve-vst [Resolve arguments]   refresh the plug-in links, start Resolve
resolve-vst list                  list the plug-ins found (VST3 with their classes)
resolve-vst rescan                make Resolve rescan all plug-ins on the next start
resolve-vst check                 check the hooks against the installed Resolve
resolve-vst categories            write categories of new plug-ins (done on every start)
resolve-vst restore               reset a changed Wine DPI (done when Resolve exits)
resolve-vst link                  only refresh the links
```

### Hide plug-ins

Resolve scans every plug-in on its own; a big Waves shell (600+ plug-ins) takes about ten minutes
the first time. List what you do not need in `~/.config/resolve-vst/ignore`, one shell pattern
per line, matched against the paths in the link folder:

```text
carla/CarlaPatchbay[0-9]*
VST3/yabridge/WaveShell1-VST3 14.12_x64/Abbey Road*
```

### Waves: load faster

Put all WaveShells into one [yabridge plug-in group](https://github.com/robbert-vdh/yabridge#plugin-groups)
so Wine starts once and stays warm (a further Waves plug-in then opens in 30 ms instead of
0.5–1 s). In `~/.vst3/yabridge/yabridge.toml`:

```toml
["WaveShell*.vst3"]
group = "waves"
```

## Plug-in windows on HiDPI screens

On a scaled display resolve-vst sizes plug-in editors to Resolve's scale factor and tells
plug-ins that support it (JUCE, VST3 content scaling) to draw at that scale. Windows plug-ins
under Wine draw with the DPI of their Wine prefix, which resolve-vst sets per plug-in before
loading it and restores when Resolve exits.

Built-in defaults cover Waves (Wine DPI 240) and iZotope (`wine-zoom`, Wine scales the editor
up). Everything else can be set per plug-in in `~/.config/resolve-vst/plugins.conf`. A section is
the plug-in's file name without extension, the VST3 plug-in or bundle name, or `Vendor/Name`;
shell patterns work, the first match wins:

```ini
[MPC Beats64]
wine-dpi = 96          # number, auto (96 × Resolve's factor), 0 = leave alone
content-scale = 0      # number, auto (Resolve's factor), 0 = do not send

[WaveShell*]           # all Waves plug-ins
wine-dpi = 240

[iZotope/*]            # all iZotope plug-ins
wine-zoom = auto       # number, auto (Resolve's factor), 0 = off
```

Details and the measurements behind these defaults: [How it works](docs/HOW-IT-WORKS.md#plug-in-windows-and-hidpi).

## Codecs

### AAC import

Resolve's bundled FFmpeg is built without the AAC decoder. resolve-vst brings a small
`libavcodec` 6.0 with only the AAC decoder (built from the official FFmpeg sources) and hands it
to Resolve when it asks for AAC. For MP4 and MOV it also teaches Resolve's QuickTime reader to
pass AAC tracks on. The audio is bit-identical to `ffmpeg`'s output except for the encoder
priming at the very start of some files (the first 1024 samples, 21 ms of near silence).

### AAC render

Resolve cannot encode AAC on Linux; an encoder plug-in such as
[FDK-AAC](https://github.com/hexitnz/Resolve-Linux-Studio-AAC-FDK-Encoder-plugin) adds it. Resolve
then writes the AAC track without its AudioSpecificConfig: `ffprobe` shows `profile=-1`, VLC and
mpv play the file, Firefox and other browsers play it silently. resolve-vst adds the missing
AAC-LC description before Resolve writes the file. Files rendered before cannot be repaired with
`ffmpeg -c copy`; render them again.

### Nikon N-RAW

Resolve on Linux has no N-RAW decoder, but its RED decoder reads `.nev` files when they are
called `.R3D`. resolve-vst shows every `X.NEV` to Resolve as `X.R3D` (read-only, a real `X.R3D`
next to it wins). Resolve then knows these clips under the `.R3D` name: started without
resolve-vst, they are offline.

## Configuration

| Variable | Meaning |
|---|---|
| `RESOLVE_BIN` | Resolve executable (default `/opt/resolve/bin/resolve`) |
| `VST_PATH`, `VST3_PATH` | Plug-in search paths, separated by `:` |
| `RESOLVE_VST_LINKS` | Link folder for Resolve (default `~/.local/share/resolve-vst/vst`) |
| `RESOLVE_VST_LOG` | Log file (default `~/.local/state/resolve-vst/resolve-vst.log`, `-` = stderr) |
| `RESOLVE_VST_EDITOR_SCALE` | Fixed editor scale (default: Resolve's window factor, `1` = off) |
| `RESOLVE_VST_CONTENT_SCALE` | Scale sent to plug-ins: `auto`, a number, `0` = do not send |
| `RESOLVE_VST_WINE_DPI` | Wine DPI for yabridge prefixes: `auto` (96 × factor), a number, `0` = leave alone |
| `RESOLVE_VST_FIX_FRAME=0` | Do not resize yabridge's embedding window |
| `RESOLVE_VST_AAC` | `0` = no AAC decoder, or the path of another `libavcodec.so.60` (FFmpeg 6.x) |
| `RESOLVE_VST_NEV=0` | Do not show `.nev` as `.R3D` |
| `RESOLVE_VST_DEEPBIND=1` | Load plug-ins with `RTLD_DEEPBIND` (symbol clashes with Resolve) |

## After a Resolve update

resolve-vst hooks into Resolve in a few places that a new version can move or change. Before
you start the new version, run:

```text
$ resolve-vst check
/opt/resolve/libs/libFairlightPage.so
  ok       _ZN15VSTPluginLoader4LoadEPKwPFlP7AEffectiilPvfERb 0x15b2300
  …
/opt/resolve/bin/resolve
  ok       AAC track switch 0x5deaecf (alac 0x5deaedd, FFmpeg 0x5deaf15, discard 0x5deaf36)
  ok       AAC decoder open 0x5ddfdeb (extradata 0x5ddfe03, other 0x5ddff24)
  ok       AAC codec table 0x297ed7f0 (contents are checked at startup)
```

A missing hook switches off only its feature; Resolve itself keeps working, and the log says what
was skipped. Code sites without symbols are found by byte patterns, not fixed addresses, so they
usually survive updates. When one does not, `check` names the pattern and where its constants
are; [How it works](docs/HOW-IT-WORKS.md#finding-the-hooks-again) explains how to adapt it.

## Troubleshooting

- **Log:** `~/.local/state/resolve-vst/resolve-vst.log` lists every plug-in load, editor size,
  Wine DPI change and codec hook.
- **All plug-ins failed after starting Resolve without resolve-vst:** quit Resolve,
  `resolve-vst rescan`, start it through `resolve-vst`.
- **A plug-in editor hangs after Resolve crashed:** yabridge plug-in groups still serve the dead
  host; `resolve-vst` stops them after a crash, by hand: `pkill -f 'yabridge-host.exe.so group'`.
- **Test a plug-in without Resolve:** `make editor-test`, then
  `build/editor-test <plugin.so | link-folder entry> out.png` loads it like Resolve, runs audio,
  parameters and state through it and screenshots the editor.

## Uninstall

```sh
curl -fsSL https://raw.githubusercontent.com/shrippen/resolve-vst/main/install.sh | bash -s -- --uninstall
```

Your settings stay in `~/.config/resolve-vst`, `~/.local/share/resolve-vst` and
`~/.local/state/resolve-vst`. Remove the VST path from Resolve's preferences.

## License

MIT, see [LICENSE](LICENSE). Release archives include an LGPL build of FFmpeg's AAC decoder and
Steinberg's VST3 interfaces (MIT): [THIRD-PARTY.md](THIRD-PARTY.md). DaVinci Resolve is a
trademark of Blackmagic Design; VST is a trademark of Steinberg Media Technologies.
