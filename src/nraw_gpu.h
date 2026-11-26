// Schnittstelle zwischen der Hauptbibliothek und libresolve-linux-vst-nraw-cuda.so (C-ABI, eine
// exportierte Funktion). Die Hauptbibliothek lädt die CUDA-Bibliothek per dlopen aus ihrem
// eigenen Ordner; ohne Datei, libcuda oder Karte dekodiert die CPU.
//
// Stufe A (bayer): die CPU liefert die Wavelet-Ebenen (nach Entropie und waagerechter Hebung), die
// GPU rechnet senkrechte Hebung, Star-Tetrix und Kennlinie.
// Stufe B (decode): die GPU nimmt den Codestream und rechnet auch Entropie, Dequantisierung und
// waagerechte Hebung. Die CPU liest vorab nur die Lagen der Pakete und die Abschneidestufen.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Zeiten der GPU-Stufen des letzten Bildes in ms (nur Tests und Messung).
// kernel = senkrechte Hebung, Star-Tetrix und Kennlinie; codes, gcli, coefs = Stufen der Entropie
// auf der Karte (nur Stufe B); scan = Vorabsuche der CPU; flags = Bits der eingebauten Prüfung.
struct NrawGpuTimes {
    float upload, kernel, download, copy;
    float scan, codes, gcli, coefs;
    unsigned flags;
};

#ifdef __cplusplus
extern "C" {
#endif

#define NRAW_GPU_VERSION 2
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
    NRAW_GPU_CHECK = 3,  // eingebaute Prüfung schlug an (Strom beschädigt): die CPU macht das Bild
};

// Paketarten: tief (LL5, HL5..HL1), hoch (LH1, HH1), eins (K2 ohne Wavelet).
enum NrawGpuKind { NRAW_GPU_LOW, NRAW_GPU_HIGH, NRAW_GPU_ONE, NRAW_GPU_KINDS };
#define NRAW_GPU_BANDS 6
#define NRAW_GPU_PACKETS 8   // Pakete je Precinct
#define NRAW_GPU_WGT 25      // Einträge der Abschneidetabelle je Precinct

// Bänder je Paketart: Gruppenzahl (4 Koeffizienten) und gültige Koeffizienten.
struct NrawGpuLayout {
    uint32_t bands[NRAW_GPU_KINDS];
    uint32_t groups[NRAW_GPU_KINDS][NRAW_GPU_BANDS];
    uint32_t width[NRAW_GPU_KINDS][NRAW_GPU_BANDS];
};

// Lage eines Precincts im Codestream (Byte-Positionen ab SOC), vorab geprüft.
struct NrawGpuPrec {
    uint32_t pkt[NRAW_GPU_PACKETS]; // Anfang der Pakete
    uint32_t end;                   // Ende des Precincts
};

// Ein Bild als Codestream. precs: nprec Einträge, gtli: nprec * NRAW_GPU_WGT Abschneidestufen.
struct NrawGpuStream {
    uint32_t wf, hf;
    uint32_t compw, nprec;
    const uint8_t *cs;
    size_t size;
    const NrawGpuPrec *precs;
    const uint8_t *gtli;
    const uint16_t *levels; // Ausgabestufen je Tabellenstufe
    uint32_t levelCount;
    struct NrawGpuLayout layout;
    float scanMs;           // Zeit der Vorabsuche (nur zur Messung)
};

// Zwischenergebnisse zum Vergleich im Test (peek): Zählcodes je Gruppe, GCLI je Gruppe (je Paket
// NRAW_GPU_GROUP_STRIDE Bytes, 255 = Gruppe ohne Code), Wavelet-Ebenen (int32, Aufbau von NrawGpuFrame).
enum NrawGpuDump { NRAW_GPU_DUMP_CODES, NRAW_GPU_DUMP_GCLI, NRAW_GPU_DUMP_PLANES };
#define NRAW_GPU_GROUP_STRIDE 528
#define NRAW_GPU_QUIET 255

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
    // Stufe B: Bayer-Werte aus dem Codestream; dst bleibt bei BUSY, FAILED und CHECK unberührt.
    int (*decode)(const NrawGpuStream *stream, uint16_t *dst, NrawGpuCopy copy);
    // Test: Zwischenergebnis des letzten Bildes nach dst (höchstens bytes); gibt die Größe zurück.
    size_t (*peek)(int what, void *dst, size_t bytes);
};

// Gibt nullptr zurück, wenn die Bibliothek die Version nicht kennt.
const struct NrawGpuApi *nrawGpuApi(int version);

#ifdef __cplusplus
}
#endif
