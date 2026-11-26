// N-RAW-Hook: Entscheidung je Aufruf, rein, damit tests/ sie ausführen kann.
#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint64_t kVerifyFirst = 3;   // eigener Decoder: so viele Prüfbilder am Anfang
constexpr uint64_t kVerifyEvery = 500; // danach jedes 500. Bild

// Muss RED dieses Bild dekodieren, ohne dass der eigene Decoder es versucht?
// a8 und a9 waren in jedem gemessenen Aufruf 0; andere Werte (ein anderer Dekodiermodus?)
// gehen an RED, ebenso eine Größe, die kein ganzes Vielfaches von uint16 ist.
// (Die lesbare Kennlinie prüft der Aufrufer danach, sie braucht fremden Speicher.)
inline bool nrawToRed(bool disabled, long a8, long a9, size_t dstBytes)
{
    return disabled || a8 != 0 || a9 != 0 || dstBytes == 0 || dstBytes % sizeof(uint16_t) != 0;
}

// Ist bei Aufruf n (ab 1) ein Prüfbild gegen RED fällig?
inline bool nrawVerifyDue(uint64_t n)
{
    return n <= kVerifyFirst || n % kVerifyEvery == 0;
}
