# How resolve-vst works

resolve-vst is one library, `libresolve-vst.so`, that the launcher `resolve-vst` puts into
`LD_PRELOAD` for Resolve and its plug-in scanner, plus the launcher itself. It never changes
Resolve's files. Code comments in `src/` are in German; this document is the English overview.

```
resolve-vst (bash) ── links plug-ins, writes categories, sets Wine DPI back on exit
      │ LD_PRELOAD
      ▼
resolve / VstScanner ──► libresolve-vst.so
                           ├─ VSTPluginLoader::Load/Close    VST2 loader (resolve_vst.cpp)
                           ├─ VST3 adapter                    VST3 as VST2 (vst3.cpp)
                           ├─ dispatcher / host callback      HiDPI, Wine DPI, frame fix
                           ├─ avcodec_find_decoder, …         AAC decoder (aac.cpp)
                           ├─ avformat_write_header           AAC render fix (aac.cpp)
                           ├─ in-memory hooks in resolve      AAC in MP4/MOV (aac.cpp, sites.cpp)
                           └─ open, stat, readdir, …          N-RAW as R3D (nev.cpp)
```

## The missing VST loader

Resolve on Linux contains the full VST2 host of the Windows and macOS versions (`VSTHost`,
`VSTPlugin`, `VSTPluginManager` in `libFairlightPage.so`, and the scanner
`/opt/resolve/bin/VstScanner`). Only `VSTPluginLoader::Load()`, which opens the plug-in library
and calls `VSTPluginMain`, is an empty body that always returns `nullptr`.

`libresolve-vst.so` provides that function with the same mangled name:

- **resolve** calls `Load` through the PLT of `libFairlightPage.so`; the preloaded symbol wins.
- **VstScanner** is a non-PIE executable that calls it directly. At load time the library looks
  the function up in the scanner's symbol table (`/proc/self/exe`) and overwrites its first bytes
  with a jump to its own implementation.

The scanner only accepts entries ending in `.vst` (like macOS bundles), so the launcher creates a
symlink `Name.vst` for every VST2 library in the link folder.

Libraries are never unloaded (`dlclose` is left out on purpose: many plug-ins do not survive it,
and `dlopen` counts references anyway).

## VST3 adapter

Resolve has no VST3 support on Linux at all (`VST3Host` consists of empty bodies). `src/vst3.cpp`
loads a VST3 plug-in and presents it as a VST2 plug-in to Resolve's host:

- **Entries:** a bundle can contain several plug-ins (classes). The launcher lists them with
  `vst3-classes` (cached in `~/.cache/resolve-vst/vst3`, because listing a yabridge bundle starts
  Wine) and creates a folder `VST3/…/Name.vst` per class, holding `bundle` (symlink to the bundle)
  and `class` (class ID).
- **Audio:** `processReplacing` → `IAudioProcessor::process`, 32-bit. Main bus only (stereo where
  possible, `effSetSpeakerArrangement` for mono and surround); auxiliary buses such as sidechains
  get silence. Resolve's transport goes along as `ProcessContext`.
- **Parameters:** VST2 index = order in the controller, normalized values as in VST3. Editor
  changes go to Resolve as automation, host changes to processor and controller. A bypass
  parameter serves `effSetBypass`.
- **State:** one chunk with component and controller state (tag `R3VS`).
- **restartComponent** (latency, parameter values) is queued and reported to Resolve on the GUI
  thread. yabridge calls it from its own thread, often in the middle of `effMainsChanged`; an
  immediate call back into Resolve froze it (Neoverb).
- **Editor:** `IPlugView` inside Resolve's X11 window, with the same sizing, content scaling
  (`IPlugViewContentScaleSupport` instead of `'PreS'`), frame fix and Wine DPI as VST2.
- **IRunLoop:** Linux editors need timers and file descriptors from the host. Resolve's Qt runs
  on the GLib main loop; the adapter registers them there (`g_timeout_add_full`,
  `g_unix_fd_add_full`), callbacks arrive on the GUI thread.
- **Not supported:** MIDI and instruments with note input, program lists, 64-bit, sidechain, ARA.

The VST2 ID (`uniqueID`) is folded from the class ID. Resolve stores plug-ins as `Name:ID`, so
projects using VST3 plug-ins depend on the adapter.

## Categories

Resolve sorts plug-ins into its own categories, stored as a bit mask `<CategoryMask>` per plug-in
in `~/.local/share/DaVinciResolve/configs/Fairlight/Effects/FXConfiguration.xml` (bit *i* = entry
*i* of `cCategories` in `libFairlightPage.so`: 1 Channel, 2 Delay, 3 Dynamics, 4 EQ, …,
14 Reverb, …). On Linux the scanner never sets them.

The launcher fills them in before every start:

- **VST3:** like Resolve itself (`PluginHost::GetPluginCategoryMask`) from the plug-in's
  subcategories (`Fx|Dynamics`): first Analyzer → Metering, Effects → Multi Effect, Filter → EQ,
  Generator → Tools, Sound Field and Up-Downmix → Surround, then every category whose name occurs.
- **VST2:** from the VST2 category (Synth → Instrument, Analysis → Metering, RoomFx → Reverb, …).

Only masks that are 0, and once per plug-in (remembered with `<FileTime>` in
`~/.local/state/resolve-vst/categorized`), so your own changes in Resolve stay until Resolve
rescans the plug-in. Before the first change the launcher saves
`FXConfiguration.xml.resolve-vst.bak`.

Two Resolve bugs stood in the way:

- Resolve and VstScanner parse the file version (`1.4`) with the locale's number format. With a
  German locale it becomes 1, and below 1.3 they skip `<CategoryMask>`. The library sets
  `LC_NUMERIC=C` in both processes before Qt reads the locale (a set `LC_ALL` moves to `LANG`).
- VstScanner rewrites the file on every start and converts masks through tables
  (`AudioPluginHost::GetPluginCategoryMaskIn/Out`) that only Resolve fills; in the scanner they
  are empty and every mask would become 0. The library redirects both functions in the scanner:
  with an empty table the mask stays as it is.

## Plug-in windows and HiDPI

Resolve runs scaled (`QT_AUTO_SCREEN_SCALE_FACTOR=1`, set by the launcher). Plug-ins report their
editor size in screen pixels, Resolve treats it as logical pixels and multiplies by its factor,
so the window would be too large. The library sits between Resolve and the plug-in (own
dispatcher and host callback) and divides `effEditGetRect` and `audioMasterSizeWindow` by the
editor window's factor.

How large the plug-in draws is up to the plug-in:

- **Qt/Linux plug-ins** such as Carla scale themselves from the environment.
- **JUCE plug-ins** scale as VST2 only when the host says so (`effVendorSpecific` with
  `'PreS'`/`'AeCs'`, as Bitwig and Ableton do). Resolve does not; the library sends Resolve's
  factor before opening the editor.
- **Windows plug-ins through yabridge** also draw with the DPI of their Wine prefix
  (`HKCU\Control Panel\Desktop\LogPixels`).

**Wine DPI.** Before loading a yabridge plug-in the library sets the prefix's DPI (through
`$WINELOADER` like yabridge, only if it differs). Wine has no per-process DPI, so the previous
value is remembered in `~/.local/state/resolve-vst/wine-dpi-restore` and the launcher restores it
when Resolve exits, also after a crash.

**Frame fix.** yabridge embeds the Wine window in an intermediate window that gets its size from
`effEditGetRect` at open time and does not grow afterwards. JUCE plug-ins under Wine also report
their base size again after opening but draw scaled, so the editor gets cut off. The library
resizes the child windows of Resolve's editor window to the target size right after opening and
during the first seconds of `effEditIdle`, through xcb (Xlib would end the process on X errors).

**Measured plug-in behaviour** (behind the built-in defaults):

- **MPC Beats** (JUCE under Wine 9.21 and 11.11) draws with the screen factor (`Xft.dpi`), not the
  host's. A higher Wine DPI makes it smaller or blank; the host factor only grows the window.
- **Waves** (WaveShell, VST3 through yabridge) draws with factor 240 / Wine DPI but reports its
  size without it. At 240 DPI drawing and size match and Waves scales sharply with the host's
  content scale. Built in for `WaveShell*`.
- **iZotope** (Neoverb, Ozone, Neutron, Nectar, RX) also draws with 240 / Wine DPI but has no
  content scaling and only gets more area when the window grows. `wine-zoom` sets the DPI to
  240 / factor, scales the reported size by 240 / DPI and sends no content scale: Wine scales the
  picture up (slightly softer than native scaling). Built in for `iZotope/*`.

## AAC import

Resolve bundles FFmpeg 6.0 (`/opt/resolve/libs/libavcodec.so.60`), built with
`--disable-decoder='aac*' --disable-parser='aac*'`. That library is extended by Blackmagic (AVS3
audio, used by Resolve's `libavformat`) and cannot be swapped for a stock FFmpeg.

`make aac` downloads FFmpeg 6.0 from ffmpeg.org (SHA-256 checked) and builds a `libavcodec` with
only the AAC decoders and parsers. The library replaces `avcodec_find_decoder`,
`avcodec_find_decoder_by_name` and `av_parser_init`: when Resolve's FFmpeg finds nothing for AAC,
it loads the second library (`RTLD_LOCAL`, it uses Resolve's `libavutil`) and returns its decoder.
Resolve drives it like its own; both are the same version.

For MP4 and MOV that is not enough: Resolve's own QuickTime reader turns `mp4a` into the FourCC
`aac ` and only passes `.mp3`, `flac`, `alac` and `ac-3` on to its FFmpeg decoder class, which
looks the codec up in a table. So at the start of `main` (after Resolve's static constructors
built the table) the library changes three things in memory:

- **Codec table** FourCC → FFmpeg codec (`std::map`): adds `aac ` and `mp4a` → AAC.
- **Track switch:** sends `aac ` and `mp4a` down the path of `.mp3` and `flac`.
- **Decoder open:** like for FLAC and ALAC, it fetches the track description (`esds`) and sets the
  AudioSpecificConfig as `extradata`. It also sets `AV_CODEC_FLAG2_SKIP_MANUAL`: Resolve expects
  one frame per packet and treats a dropped priming frame as an error ("Failed to decode the
  audio samples"). As a result Resolve plays the encoder priming of libfaac files (the first 1024
  samples, near silence) where ffmpeg drops it; from there on the audio is bit-identical (checked
  with a 10-minute clip rendered as PCM), and the position relative to the picture is the same.

Before changing anything it checks the table's contents; if anything does not match it changes
nothing and logs why, and AAC tracks stay silent as without resolve-vst.

## AAC render

With an AAC encoder plug-in Resolve writes MP4, MOV and MKV through its `libavformat`, but gives
the AAC stream no `extradata`, although the plug-in supplies the AudioSpecificConfig as its magic
cookie. FFmpeg's mov muxer then writes an `esds` without DecoderSpecificInfo:

```
Resolve:        03 … 04 [40 15 …]              06 … 02    ffprobe: profile=-1
resolve-vst:    03 … 04 [40 15 …] 05 … 11 90   06 … 02    ffprobe: profile=LC
```

The library replaces `avformat_write_header` (called by Resolve through the PLT): AAC streams
without `extradata` get the AAC-LC AudioSpecificConfig computed from sample rate and channel count
(the same two bytes FDK-AAC produces, `11 90` for 48 kHz stereo). Other profiles (HE-AAC) are
left alone and logged. If Resolve's `libavformat` is not version 60, the call passes through
unchanged (the struct offsets are those of FFmpeg 6.x).

## Nikon N-RAW

Resolve on Linux lacks Nikon's N-RAW decoder (Windows and macOS get it from Nikon), but its RED
decoder reads `.nev` files when they end in `.R3D`. Instead of renaming, the library shows every
`X.NEV` as `X.R3D`:

- in directory listings (`readdir`): media page, import dialog and RED SDK see `X.R3D`;
- on access (`open`, `fopen`, `stat`, `access`, `realpath`): if `X.R3D` does not exist but
  `X.NEV` does, the access goes to `X.NEV`, read-only.

A real `X.R3D` next to it wins; case is kept (`a.nev` ↔ `a.r3d`). Only in the `resolve` process.

## Finding the hooks again

| Hook | Found by | Breaks when |
|---|---|---|
| `VSTPluginLoader::Load/Close` in `libFairlightPage.so` | dynamic symbol (PLT) | Blackmagic renames the class or changes the signature |
| the same and `AudioPluginHost::GetPluginCategoryMaskIn/Out` in `VstScanner` | the executable's symbol table | as above, or the scanner ships stripped |
| AAC track switch, decoder open, codec table in `resolve` (stripped) | byte patterns in `src/sites.cpp` | the compiler generates that code differently |
| `avcodec_find_decoder`, `av_parser_init`, `avformat_write_header` | dynamic symbols, versions `LIBAVCODEC_60` / `LIBAVFORMAT_60` | Resolve ships another FFmpeg major version or stops calling them itself |

`resolve-vst check` (`build/resolve-check`) checks all of them without starting Resolve. At start
the library runs the same pattern search (about 50 ms over Resolve's 160 MB of code) and logs the
result (`AAC: Resolve angepasst …`).

The patterns anchor on the instructions themselves, and every jump target is computed from the
instructions (not from fixed distances); each pattern must match exactly once. When one stops
matching, `check` prints the pattern and the addresses of the constants it hangs on (`'alac'`,
`'flac'`, `AV_CODEC_ID_ALAC` 0x15010, `AV_CODEC_ID_FLAC` 0x1500c):

- the **track switch** is the `cmp r12d, 0x616c6163` followed by `setne`/`or`/`jne`;
- the **decoder open** is the comparison with 0x15010 and 0x1500c right after
  `avcodec_alloc_context3`.

Look there with `objdump -d -M intel --start-address=… --stop-address=… /opt/resolve/bin/resolve`,
adapt the patterns in `findAacSites` and, if registers changed, the replacement stubs
`resolveVstSwitch` and `resolveVstCodec` in `src/aac.cpp`. The comments there and in `sites.cpp`
quote the instructions of 21.1.1. `make test` checks the search against synthetic code.

## Testing without Resolve

`make editor-test` builds `build/editor-test`. It loads a plug-in the way Resolve does (through
`libresolve-vst.so`), runs some blocks of audio, reads a parameter, writes the state back, opens
the editor and takes a screenshot. VST3 plug-ins go through their entry in the link folder.

```sh
RESOLVE_VST_EDITOR_SCALE=3.75 build/editor-test ~/.vst/yabridge/'MPC Beats64.so' mpc.png
build/editor-test ~/.local/share/resolve-vst/vst/VST3/TB/TB_Compressor_v4.vst tb.png
```

It does not set the Wine DPI; set it by hand first. Under KWin (Wayland) the test window is not
always shown; record it in its own X server instead (`Xvfb :97 -screen 0 3840x2400x24 &`,
`DISPLAY=:97 build/editor-test …`). Without a window manager some plug-ins fail on missing X
atoms (MiniMeters aborts).

## Status

- Scanner: Carla and yabridge plug-ins are recognized with name, vendor and channels; broken
  plug-ins are marked as failed cleanly.
- In Resolve: plug-ins appear, go on buses, and editors open in their own Resolve window (X11
  window via `QWidget::winId()` passed to `effEditOpen`).
- HiDPI: MPC Beats fills its window without border (3200×1920 at factor 3.75); Carla unchanged.
- VST3: native (ToneBoosters, MiniMeters) and yabridge plug-ins (iZotope, Waves 14.12 and later):
  scan, audio, parameters, state, editor in the right size.
- AAC: import of MP4/MOV/M4A from phones, cameras and Nikon proxies; renders with the FDK-AAC
  encoder play in Firefox.
