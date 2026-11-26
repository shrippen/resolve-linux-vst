// Editorgröße: Zugriff auf das Fenstersystem (xcb) für den Rahmen eines Editors; Hintergrund in
// editor_size.cpp. Von der Editorgröße (editor_size.cpp) genutzt.
#pragma once

#include <cstdint>

// Bringt alle Unterfenster des Fensters `window` auf die Größe w x h (Bildschirmpixel). Ohne
// X11-Verbindung oder ohne Fenster tut sie nichts.
void resizeChildWindows(uint32_t window, int w, int h);
