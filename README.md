<p align="center"><img src="docs/icon.svg" width="96" alt=""></p>

<h1 align="center">resolve-linux-vst</h1>

<p align="center">
  <b>VST2 and VST3 plug-ins, AAC audio and Nikon N-RAW for DaVinci Resolve on Linux.</b><br>
  One small preload library. No changes to Resolve's files.
</p>

<p align="center">
  <a href="https://shrippen.github.io/resolve-linux-vst/">Website</a> ·
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

resolve-linux-vst fills those gaps at run time:

| | What you get |
|---|---|
| **VST2** | Native Linux VST2 plug-ins and Windows VST2 plug-ins through [yabridge](https://github.com/robbert-vdh/yabridge) show up in the Fairlight effects library. |
| **VST3** | A built-in VST3-to-VST2 adapter: native and yabridge VST3 plug-ins (iZotope, Waves, ToneBoosters, …), with editors, automation and state. |
| **LV2, CLAP** | Through [Carla](https://kx.studio/Applications:Carla), which is a VST2 plug-in itself. |
| **Categories** | Plug-ins land in Resolve's categories (Dynamics, EQ, Reverb, …) instead of "Uncategorized". |
| **HiDPI** | Plug-in editors get the right size and scale on scaled displays, including Wine plug-ins. |
| **AAC import** | MP4, MOV and M4A clips with AAC audio play with sound. |
| **AAC render** | MP4/MOV renders with an AAC encoder plug-in such as [FDK-AAC](https://github.com/hexitnz/Resolve-Linux-Studio-AAC-FDK-Encoder-plugin) get a correct AAC-LC description, so browsers play them. |
| **N-RAW** | Nikon `.nev` clips open through Resolve's own RED decoder; resolve-linux-vst decodes their frames itself, on an NVIDIA GPU, bit-exact to RED and much lighter on the CPU. |

Tested with DaVinci Resolve Studio 21.1.1 and 21.2 on Arch Linux.

> [!NOTE]
> resolve-linux-vst is an independent project. It is not affiliated with or endorsed by Blackmagic
> Design. It relies on Resolve internals, so a Resolve update can break parts of it;
> `resolve-linux-vst check` tells you which (see [After a Resolve update](#after-a-resolve-update)).

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/shrippen/resolve-linux-vst/main/install.sh | bash
```

The installer downloads the latest release from GitHub (checksum verified), installs it into
`~/.local` (no root needed), adds a menu entry **DaVinci Resolve (VST)** and checks your Resolve
installation with `resolve-linux-vst check`.

Requirements: Linux on x86_64, DaVinci Resolve in `/opt/resolve` (otherwise set `RESOLVE_BIN`),
`nm` (binutils) and `awk`. Optional: [yabridge](https://github.com/robbert-vdh/yabridge) for
Windows plug-ins, [Carla](https://kx.studio/Applications:Carla) for LV2 and CLAP,
[FDK-AAC encoder plug-in](https://github.com/hexitnz/Resolve-Linux-Studio-AAC-FDK-Encoder-plugin) for AAC renders.

Options: `install.sh --version 2.0.0`, `--prefix DIR`, `--uninstall`.

<details>
<summary>Build from source</summary>

Needs `g++` (C++17), `make`, `curl` and `nasm` or `yasm` (for the AAC decoder). For the N-RAW GPU
library also `nvcc` (CUDA 12 or newer; default `/opt/cuda/bin/nvcc`, else `make NVCC=…`); without it
the N-RAW decoder runs on the CPU only.

```sh
git clone --recursive https://github.com/shrippen/resolve-linux-vst.git
cd resolve-linux-vst
./install.sh --source        # make, make aac, make install into ~/.local
```

Or run it straight from the checkout without installing: `./start.sh` builds what is missing and
starts Resolve. Make targets: `all`, `aac` (downloads FFmpeg 6.0 and builds the AAC decoder),
`test`, `install`/`uninstall` (`PREFIX=…`), `dist`, `editor-test`, `plugin-test` (iZotope and
Waves through yabridge, local only).

</details>

## Quick start

1. Start Resolve with **DaVinci Resolve (VST)** from your application menu, or run `resolve-linux-vst`.
2. In Resolve: **Preferences → User → Audio Plugins → VST Plugin Paths**, add
   `~/.local/share/resolve-linux-vst/vst` and let Resolve scan.
3. Your plug-ins appear in the **Effects Library → Audio FX**, sorted into categories after the
   next start.

Always start Resolve through `resolve-linux-vst` (or the menu entry). Started without it, Resolve's
scanner finds nothing and marks every plug-in as failed; run `resolve-linux-vst rescan` afterwards.

AAC import and N-RAW need no setup: import the clips as usual. To render AAC, install the
[FDK-AAC encoder plug-in](https://github.com/hexitnz/Resolve-Linux-Studio-AAC-FDK-Encoder-plugin)
and pick AAC as audio codec in the Deliver page.

## Plug-ins

`resolve-linux-vst` collects plug-ins from these places and links them into
`~/.local/share/resolve-linux-vst/vst`, the one folder Resolve scans:

| Kind | Searched in |
|---|---|
| VST2 (native) | `~/.vst`, `~/.lxvst`, `/usr/local/lib/vst`, `/usr/lib/vst` (or `$VST_PATH`) |
| VST2 (Windows, yabridge) | `~/.vst/yabridge` (yabridge's default VST2 target) |
| VST3 (native and yabridge) | `~/.vst3`, `/usr/local/lib/vst3`, `/usr/lib/vst3` (or `$VST3_PATH`) |

Not supported: instruments with MIDI input, sidechain inputs, ARA, 64-bit processing.

### Commands

```text
resolve-linux-vst [Resolve arguments]   refresh the plug-in links, start Resolve
resolve-linux-vst list                  list the plug-ins found (VST3 with their classes)
resolve-linux-vst rescan                make Resolve rescan all plug-ins on the next start
resolve-linux-vst check                 check the hooks against the installed Resolve
resolve-linux-vst categories            write categories of new plug-ins (done on every start)
resolve-linux-vst restore               reset a changed Wine DPI, stop leftovers of yabridge groups
                                        (done when Resolve exits)
resolve-linux-vst link                  only refresh the links
```

### Hide plug-ins

Resolve scans every plug-in on its own; a big Waves shell (600+ plug-ins) takes about ten minutes
the first time. List what you do not need in `~/.config/resolve-linux-vst/ignore`, one shell pattern
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

On a scaled display resolve-linux-vst sizes plug-in editors to Resolve's scale factor and tells
plug-ins that support it (JUCE, VST3 content scaling) to draw at that scale. Windows plug-ins
under Wine draw with the DPI of their Wine prefix, which resolve-linux-vst sets per plug-in before
loading it and restores when Resolve exits.

Built-in defaults cover Waves (`wine-dpi = prefix`) and iZotope (`wine-zoom`, Wine scales the editor
up). Everything else can be set per plug-in in `~/.config/resolve-linux-vst/plugins.conf`
(`$XDG_CONFIG_HOME/resolve-linux-vst/plugins.conf` when that is set). A section is
the plug-in's file name without extension, the VST3 plug-in or bundle name, or `Vendor/Name`;
shell patterns work, the first match wins:

```ini
[MPC Beats64]
wine-dpi = 96          # number, auto (96 × Resolve's factor), prefix (its own DPI), 0 = leave alone
content-scale = 0      # number, auto (Resolve's factor), 0 = do not send

[WaveShell*]           # all Waves plug-ins
wine-dpi = prefix

[iZotope/*]            # all iZotope plug-ins
wine-zoom = auto       # number, auto (Resolve's factor), 0 = off
```

Details and the measurements behind these defaults: [How it works](docs/HOW-IT-WORKS.md#plug-in-windows-and-hidpi).

## Codecs

### AAC import

Resolve's bundled FFmpeg is built without the AAC decoder. resolve-linux-vst brings a small
`libavcodec` 6.0 with only the AAC decoder (built from the official FFmpeg sources) and hands it
to Resolve when it asks for AAC. For MP4 and MOV it also teaches Resolve's QuickTime reader to
pass AAC tracks on. The audio is bit-identical to `ffmpeg`'s output except for the encoder
priming at the very start of some files (the first 1024 samples, 21 ms of near silence).

### AAC render

Resolve cannot encode AAC on Linux. The
[FDK-AAC encoder plug-in](https://github.com/hexitnz/Resolve-Linux-Studio-AAC-FDK-Encoder-plugin)
(Resolve Studio) adds it; install it separately, resolve-linux-vst does not bring an encoder.
Resolve then writes the AAC track without its AudioSpecificConfig: `ffprobe` shows `profile=-1`,
VLC and mpv play the file, Firefox and other browsers play it silently. resolve-linux-vst adds the
missing AAC-LC description before Resolve writes the file. Files rendered before cannot be
repaired with `ffmpeg -c copy`; render them again.

### Nikon N-RAW

Resolve on Linux has no N-RAW decoder, but its RED decoder reads `.nev` files when they are
called `.R3D`. resolve-linux-vst shows every `X.NEV` to Resolve as `X.R3D` (read-only, a real `X.R3D`
next to it wins). Resolve then knows these clips under the `.R3D` name: started without
resolve-linux-vst, they are offline.

N-RAW is heavy for the CPU: the RED decoder unpacks every frame on all cores. resolve-linux-vst
decodes the frames itself instead, on an NVIDIA GPU (Turing or newer, driver 525 or newer), otherwise on the
CPU. The result is identical to RED's, bit for bit; RED's colour processing and the Camera RAW
settings stay as they are. The first frames and every 500th are decoded by RED as well and
compared; on any difference or error RED takes over again. Measured on a Ryzen 9 3900X with an
RTX 3090, 4.1K N-RAW to ProRes 422 UHD: 45-55 fps instead of 35-40.
`RESOLVE_LINUX_VST_NRAW_DECODE=0` leaves decoding to RED.

## Configuration

| Variable | Meaning |
|---|---|
| `RESOLVE_BIN` | Resolve executable (default `/opt/resolve/bin/resolve`) |
| `VST_PATH`, `VST3_PATH` | Plug-in search paths, separated by `:` |
| `RESOLVE_LINUX_VST_LINKS` | Link folder for Resolve (default `~/.local/share/resolve-linux-vst/vst`) |
| `RESOLVE_LINUX_VST_LOG` | Log file (default `~/.local/state/resolve-linux-vst/resolve-linux-vst.log`, `-` = stderr) |
| `RESOLVE_LINUX_VST_EDITOR_SCALE` | Fixed editor scale (default: Resolve's window factor, `1` = off) |
| `RESOLVE_LINUX_VST_CONTENT_SCALE` | Scale sent to plug-ins: `auto`, a number, `0` = do not send |
| `RESOLVE_LINUX_VST_WINE_RESTORE` | File with the Wine DPI to restore (default `~/.local/state/resolve-linux-vst/wine-dpi-restore`); the launcher passes it to Resolve, without it the library leaves the Wine DPI alone |
| `RESOLVE_LINUX_VST_WINE_DPI` | Wine DPI for yabridge prefixes: `auto` (96 × factor), a number, `prefix` (its own DPI), `0` = leave alone |
| `RESOLVE_LINUX_VST_FIX_FRAME=0` | Do not resize yabridge's embedding window |
| `RESOLVE_LINUX_VST_AAC` | `0` = no AAC decoder, or the path of another `libavcodec.so.60` (FFmpeg 6.x) |
| `RESOLVE_LINUX_VST_NEV=0` | Do not show `.nev` as `.R3D` |
| `RESOLVE_LINUX_VST_NRAW_DECODE=0` | Let the RED decoder decode N-RAW frames instead of resolve-linux-vst's own decoder; `probe` only measures |
| `RESOLVE_LINUX_VST_NRAW_THREADS` | Threads of the N-RAW decoder (default 12, `1` = none) |
| `RESOLVE_LINUX_VST_DEEPBIND=1` | Load plug-ins with `RTLD_DEEPBIND` (symbol clashes with Resolve) |

## After a Resolve update

resolve-linux-vst hooks into Resolve in a few places that a new version can move or change. Before
you start the new version, run:

```text
$ resolve-linux-vst check
/opt/resolve/libs/libFairlightPage.so
  ok       _ZN15VSTPluginLoader4LoadEPKwPFlP7AEffectiilPvfERb 0x15b2300
  …
/opt/resolve/bin/resolve
  ok       AAC track switch 0x5deaecf (alac 0x5deaedd, FFmpeg 0x5deaf15, discard 0x5deaf36)
  ok       AAC decoder open 0x5ddfdeb (extradata 0x5ddfe03, other 0x5ddff24)
  ok       AAC codec table 0x297ed7f0 (contents are checked at startup)
/opt/resolve/libs/REDR3D-x64.so
  ok       N-RAW decode function 0x237170
```

A missing hook switches off only its feature; Resolve itself keeps working, and the log says what
was skipped. Code sites without symbols are found by byte patterns, not fixed addresses, so they
usually survive updates. When one does not, `check` names the pattern and where its constants
are; [How it works](docs/HOW-IT-WORKS.md#finding-the-hooks-again) explains how to adapt it.

## Troubleshooting

- **Log:** `~/.local/state/resolve-linux-vst/resolve-linux-vst.log` lists every plug-in load, editor size,
  Wine DPI change and codec hook.
- **All plug-ins failed after starting Resolve without resolve-linux-vst:** quit Resolve,
  `resolve-linux-vst rescan`, start it through `resolve-linux-vst`.
- **A plug-in editor hangs after Resolve crashed:** yabridge plug-in groups still serve the dead
  host; `resolve-linux-vst` stops them after a crash, by hand: `pkill -f 'yabridge-host.exe.so group'`.
- **Waves plug-ins do not load ("Another process is already listening on this group's socket"):**
  `WavesLocalServer.exe` outlived the Waves group and holds its socket. `resolve-linux-vst` stops such
  processes before Resolve starts and when it exits; by hand: `resolve-linux-vst restore`.
- **Test a plug-in without Resolve:** `make editor-test`, then
  `build/editor-test <plugin.so | link-folder entry> out.png` loads it like Resolve, runs audio,
  parameters and state through it and screenshots the editor. With
  `RESOLVE_LINUX_VST_WINE_RESTORE=~/.local/state/resolve-linux-vst/wine-dpi-restore` it sets the Wine
  DPI of yabridge plug-ins like Resolve, and `resolve-linux-vst restore` sets it back.

## Uninstall

```sh
curl -fsSL https://raw.githubusercontent.com/shrippen/resolve-linux-vst/main/install.sh | bash -s -- --uninstall
```

Your settings stay in `~/.config/resolve-linux-vst`, `~/.local/share/resolve-linux-vst` and
`~/.local/state/resolve-linux-vst`. Remove the VST path from Resolve's preferences.

## License

MIT, see [LICENSE](LICENSE). Release archives include an LGPL build of FFmpeg's AAC decoder and
Steinberg's VST3 interfaces (MIT): [THIRD-PARTY.md](THIRD-PARTY.md). DaVinci Resolve is a
trademark of Blackmagic Design; VST is a trademark of Steinberg Media Technologies.
