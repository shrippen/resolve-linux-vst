// N-RAW-Hook, gemeinsame Teile von nraw.cpp (eigener Decoder) und nraw_probe.cpp (Messmodus):
// fremden Speicher lesen, Kennlinie lesen, Zeit messen, Aufrufwerte beobachten, Bild mit RED vergleichen.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "nraw_decode.h"

// Die Dekodierfunktion von fastticolib.
using DecodeFn = int (*)(void *ctx, void *handle, void *a3, size_t size, const uint8_t *codestream, size_t dstBytes,
                         uint16_t *dst, long a8, long a9);

constexpr uint64_t kReportEvery = 200; // Statistikzeile

// Liest fremden Speicher über process_vm_readv; false bei ungültiger Adresse.
bool readMem(const void *from, void *to, size_t len);

// Kennlinie aus ctx in `values` kopieren; false, wenn Zeiger oder Anzahl nicht stimmen.
bool readCurve(const void *ctx, std::vector<int32_t> *values);

uint64_t fnv1a(const std::vector<int32_t> &values);

// Wand- und Prozess-CPU-Zeit seit der Erzeugung. Die CPU-Zeit zählt alle Threads des Prozesses.
class Stopwatch {
public:
    Stopwatch();
    uint64_t wallNs() const;
    uint64_t cpuNs() const;

private:
    std::chrono::steady_clock::time_point wall_;
    uint64_t cpu_;
};

// Ändern sich a3, a8 oder a9 gegenüber dem ersten Aufruf? Loggt jeden neuen Wert einmal, zählt
// die abweichenden Aufrufe. Aufrufe kommen aus mehreren Threads.
class ArgWatch {
public:
    void observe(uint64_t n, void *a3, long a8, long a9);
    uint64_t varying() const { return varying_.load(); }

private:
    std::atomic<uint64_t> varying_{0};
    std::atomic<bool> haveFirst_{false};
    std::atomic<bool> logged_[3]{};
    long first_[3] = {0, 0, 0};
};

// Dekodiert denselben Codestream mit dem eigenen Decoder und vergleicht mit dem Bild von RED
// (`red`, `count` Pixel) und loggt das Ergebnis. Mit `switchOff` (eigener Decoder) wird er bei einer
// Abweichung sofort abgeschaltet und erst danach das Bild für die Fehlersuche abgelegt; ohne
// (Messmodus) nur geloggt.
void compareWithRed(uint64_t n, const uint8_t *cs, size_t size, const NrawCurve &curve, const uint16_t *red,
                    size_t count, std::atomic<bool> *switchOff);
