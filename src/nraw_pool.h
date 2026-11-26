// Threadpool des N-RAW-Decoders (kein Feature, gehört zu nraw_decode.cpp). Entsteht beim ersten
// Gebrauch, wird nie abgebaut (kein Join bei Prozessende) und läuft im Kindprozess nach fork()
// nicht mehr: dort arbeitet der Aufrufer allein.
#pragma once

#include <cstddef>

// Ruft fn(i, ctx) für i in [0, n) auf allen Threads auf; der Aufrufer arbeitet mit und kehrt
// zurück, wenn alle fertig sind. Ein Aufruf aus dem Pool heraus oder bei besetztem Pool läuft
// seriell im Aufrufer.
void parallelFor(size_t n, void (*fn)(size_t i, void *ctx), void *ctx);

// Nur für Tests: Threadzahl (1..64, 1 = seriell). Standard 12 oder RESOLVE_LINUX_VST_NRAW_THREADS.
void poolSetThreads(unsigned threads);
