// Was der Lader (resolve_linux_vst.cpp) seinen Teilen bietet: den Skalierungsfaktor. Der Log
// steht in log.h, das laufende Programm in process.h, die Einstellungen in plugin_conf.h.
#pragma once

// Skalierungsfaktor des Fensters, in das das Plugin zeichnet; ohne Fenster der des Hauptbildschirms.
double editorScale(unsigned long long windowId = 0);
