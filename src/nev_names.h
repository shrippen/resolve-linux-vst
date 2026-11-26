// N-RAW-Namen: die reine Rechnung zwischen X.R3D und X.NEV, ohne Dateisystem und Log (Hintergrund
// in nev.cpp). Eigene Datei, damit tests/nev-test.cpp sie ohne den Rest der Bibliothek baut.
#pragma once

#include <cstddef>

// Endet name (Länge n) auf ".<ext>", gleich welche Schreibung? ext klein, drei Zeichen.
inline bool hasExtension(const char *name, size_t n, const char *ext)
{
    if (n < 5 || name[n - 4] != '.') {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        char c = name[n - 3 + i];
        if (c >= 'A' && c <= 'Z') {
            c = char(c - 'A' + 'a');
        }
        if (c != ext[i]) {
            return false;
        }
    }
    return true;
}

// Tauscht die drei Zeichen nach dem Punkt, Schreibung je Buchstabe; die Ziffer 3 nimmt die
// Schreibung des ersten Buchstabens.
inline void toNev(char *ext)
{
    bool upper = ext[0] == 'R';
    ext[0] = upper ? 'N' : 'n';
    ext[1] = upper ? 'E' : 'e';
    ext[2] = ext[2] == 'D' ? 'V' : 'v';
}

inline void toR3d(char *ext)
{
    bool upper = ext[0] == 'N';
    ext[0] = upper ? 'R' : 'r';
    ext[1] = '3';
    ext[2] = ext[2] == 'V' ? 'D' : 'd';
}

// Die andere Schreibung von "nev" (ganz klein oder ganz groß), für a.R3D zu a.nev und umgekehrt.
inline void otherSpelling(char *ext)
{
    bool upper = ext[0] == 'N';
    const char *other = upper ? "nev" : "NEV";
    for (int i = 0; i < 3; ++i) {
        ext[i] = other[i];
    }
}
