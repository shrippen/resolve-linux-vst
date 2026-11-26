// Editorgröße: Umrechnung der Fenstergrößen, rein gerechnet ohne Resolve und X11 (Hintergrund in
// editor_size.cpp). Eigene Datei, damit tests/editor-size-test.cpp sie ohne den Rest der
// Bibliothek baut.
#pragma once

#include <cmath>
#include <cstdint>

#include "vst2.h"

// Größe v (Pixel) durch den Skalierungsfaktor s geteilt, gerundet: Plugin-Pixel -> Resolves logische Pixel.
inline short editorScaled(short v, double s)
{
    return static_cast<short>(lround(v / s));
}

// Größe v des Plugins mit der Vergrößerung durch Wine multipliziert, gerundet: Plugin-Pixel -> Bildschirmpixel.
inline int editorZoomed(long v, double zoom)
{
    return static_cast<int>(lround(v * zoom));
}

// Größe v an Resolve: Plugin-Pixel mit Wine-Vergrößerung durch den Skalierungsfaktor, gerundet.
inline long editorToResolve(long v, double zoom, double scale)
{
    return lround(v * (zoom / scale));
}

// Das vom Plugin gemeldete Rechteck als Fenster in Bildschirmpixeln: Ursprung 0/0, nur die Größe.
inline ERect editorScreenRect(const ERect &reported, double zoom)
{
    return {0, 0, short(lround((reported.bottom - reported.top) * zoom)),
            short(lround((reported.right - reported.left) * zoom))};
}

// Fenster der Zielgröße (Bildschirmpixel) als Rechteck mit Ursprung 0/0.
inline ERect editorTargetRect(int targetW, int targetH)
{
    return {0, 0, short(targetH), short(targetW)};
}

// Meldet das Plugin nach dem Öffnen beide Seiten kleiner als die Zielgröße, gilt die Zielgröße.
inline bool editorBelowTarget(int w, int h, int targetW, int targetH)
{
    return targetW > w && targetH > h;
}

// Rechteck in Bildschirmpixeln als Rechteck an Resolve (durch den Skalierungsfaktor geteilt).
inline ERect editorToResolveRect(const ERect &r, double scale)
{
    return {editorScaled(r.top, scale), editorScaled(r.left, scale), editorScaled(r.bottom, scale),
            editorScaled(r.right, scale)};
}
