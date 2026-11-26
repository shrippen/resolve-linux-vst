// Decoder für Nikon N-RAW (TicoRAW, JPEG-XS-Variante), auf der GPU (nraw_gpu.h) oder der CPU.
// Kennt weder Resolve noch RED; sein GPU-Teil schreibt ins Log (log.h), Tests bauen log.cpp mit.
// Bitgenau gegen die Ausgabe von fastticolib geprüft (tests/nraw-test.cpp). Alles, was vom
// geprüften Aufbau abweicht, ergibt Unsupported.
#pragma once

#include <cstddef>
#include <cstdint>

#include "nraw_gpu.h"

// Kennlinie (Tabelle Stufe -> Wert), zur Laufzeit von fastticolib übernommen, nie mitgeliefert.
struct NrawCurve {
    const int32_t *values;
    size_t count;
};

enum class NrawResult {
    Ok,
    Unsupported, // anderer Aufbau als geprüft: RED soll selbst dekodieren
    Corrupt,     // Längen oder Grenzen stimmen nicht
};

// Gpu: die Stufen nach der Entropie laufen auf der Grafikkarte, wenn die CUDA-Bibliothek
// (nraw_gpu.h) geladen ist und lebt, sonst auf der CPU. Cpu erzwingt die CPU.
enum class NrawBackend { Cpu, Gpu };


// Codestream ab SOC (FF10). Ziel: Wf*Hf Werte (aus dem PIH), Bayer RGGB, uint16.
NrawResult decodeNraw(const uint8_t *codestream, size_t size, const NrawCurve &curve, uint16_t *bayer,
                      size_t bayerCount, NrawBackend backend = NrawBackend::Gpu);

// Zeiten des letzten GPU-Bildes; false, wenn die GPU nicht läuft.
bool nrawGpuTimes(NrawGpuTimes *times);

// Nur Tests: Zwischenergebnis des letzten GPU-Bildes (NrawGpuDump in nraw_gpu.h); gibt die Größe zurück.
size_t nrawGpuPeek(int what, void *dst, size_t bytes);
