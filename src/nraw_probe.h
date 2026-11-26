// N-RAW-Messmodus (RESOLVE_LINUX_VST_NRAW_DECODE=probe): ruft immer das Original von fastticolib
// und protokolliert, womit es gerufen wird, was es verändert, wie oft und wie parallel es läuft.
// Für die Fehlersuche nach einem Resolve-Update; Teil der N-RAW-Dekodierung (nraw.cpp).
#pragma once

#include "nraw_watch.h"

int nrawProbe(DecodeFn original, void *ctx, void *handle, void *a3, size_t size, const uint8_t *cs, size_t dstBytes,
              uint16_t *dst, long a8, long a9);
