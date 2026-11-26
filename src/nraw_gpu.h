// Schnittstelle zwischen der Hauptbibliothek und libresolve-linux-vst-nraw-cuda.so (C-ABI, eine
// exportierte Funktion). Die Hauptbibliothek lädt die CUDA-Bibliothek per dlopen aus ihrem
// eigenen Ordner; ohne Datei, libcuda oder Karte dekodiert die CPU.
//
// Stufe A: die CPU liefert die Wavelet-Ebenen (nach Entropie und waagerechter Hebung), die GPU
// rechnet senkrechte Hebung, Star-Tetrix und Kennlinie. Stufe B ergänzt in einer höheren
// Version einen Eintrag, der den Codestream nimmt (decode); die Felder bis dahin bleiben stehen.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nraw_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NRAW_GPU_VERSION 1
#define NRAW_GPU_ENTRY "nrawGpuApi"

// Ebenen im gepinnten Speicher (allocHost), je nprec * compw Werte (int32), nacheinander:
// K0 tief, K0 hoch, K1 tief, K1 hoch, K3 tief, K3 hoch, K2 (2 * nprec Zeilen).
struct NrawGpuFrame {
    uint32_t wf, hf;
    uint32_t compw, nprec;
    const int32_t *planes;
    const uint16_t *levels; // Ausgabestufen je Tabellenstufe
    uint32_t levelCount;
};

enum NrawGpuStatus {
    NRAW_GPU_OK = 0,
    NRAW_GPU_BUSY = 1,   // Karte besetzt, nichts getan: die CPU macht das Bild
    NRAW_GPU_FAILED = 2, // Fehler, die GPU-Variante ist für den Prozess tot
};

// Kopiert count Werte von src (gepinnt, nur gültig während des Aufrufs) nach dst.
typedef void (*NrawGpuCopy)(const uint16_t *src, uint16_t *dst, size_t count);

struct NrawGpuApi {
    int version;
    void *(*allocHost)(size_t bytes);  // gepinnter Speicher, nullptr bei Fehler
    void (*freeHost)(void *p);
    // Wf*Hf Bayer-Werte nach dst; dst bleibt bei BUSY und FAILED unberührt.
    int (*bayer)(const NrawGpuFrame *frame, uint16_t *dst, NrawGpuCopy copy);
    const char *(*error)(void);        // Text des ersten Fehlers, sonst nullptr
    void (*times)(NrawGpuTimes *out);  // Zeiten des letzten Bildes
};

// Gibt nullptr zurück, wenn die Bibliothek die Version nicht kennt.
const struct NrawGpuApi *nrawGpuApi(int version);

#ifdef __cplusplus
}
#endif
