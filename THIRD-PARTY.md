# Third-party components

resolve-vst itself is MIT licensed (see `LICENSE`). It contains or ships the following.

## VST3 plug-in interfaces (Steinberg)

`third_party/vst3_pluginterfaces` (git submodule, v3.8.1), compiled into `libresolve-vst.so`
and `vst3-classes`. MIT License, Copyright (c) Steinberg Media Technologies GmbH.
Source: <https://github.com/steinbergmedia/vst3_pluginterfaces>. VST is a registered
trademark of Steinberg Media Technologies GmbH.

## FFmpeg 6.0 (AAC decoder only)

Release archives contain `lib/resolve-vst/ffmpeg-aac/libavcodec.so.60`, a build of FFmpeg's
libavcodec 6.0 with nothing but the AAC decoders and parsers. FFmpeg is licensed under the
GNU Lesser General Public License, version 2.1 or later (LGPL-2.1-or-later); this build uses no
GPL or non-free parts. The library is loaded at run time and can be replaced by your own build
(`RESOLVE_VST_AAC=/path/to/libavcodec.so.60`).

Corresponding source: <https://ffmpeg.org/releases/ffmpeg-6.0.tar.xz> (SHA-256
`57be87c22d9b49c112b6d24bc67d42508660e6b718b3db89c44e47e289137082`) with
`ffmpeg-binutils.patch` (upstream commit effadce6, needed for newer binutils), built by the
`aac` target in the `Makefile` of this repository, which lists the exact `configure` options.
`make aac` reproduces the library from these sources.

## Not included

DaVinci Resolve, its libraries and its plug-in SDKs are not part of this project. DaVinci
Resolve is a trademark of Blackmagic Design Pty. Ltd.; resolve-vst is not affiliated with or
endorsed by Blackmagic Design.
