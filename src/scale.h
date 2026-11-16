// Skalierungsfaktor von Resolves Oberfläche (Qt-Abfrage, Erklärung in scale.cpp). Von der
// Editorgröße und von der Wine-DPI genutzt.
#pragma once

// Skalierungsfaktor des Fensters, in das das Plugin zeichnet; ohne Fenster der des Hauptbildschirms.
double editorScale(unsigned long long windowId = 0);
